/*
 * ps5-native-app-boilerplate - Application-owned module loader and export descriptor.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Single-header library shared by a module and the title that loads it.
 *
 * On firmware 12.02 the loader accepts an application-owned PRX through
 * sceKernelLoadStartModule, but sceKernelDlsym does not resolve its exports
 * and module_start is not invoked. A module therefore publishes a static,
 * relocated export descriptor, and the host locates it by scanning the module
 * segments reported by sceKernelGetModuleInfo. Both halves live here:
 *
 *   module side:  PRX_DEFINE_DESCRIPTOR(prx_exports, PRX_EXPORT(fn), ...);
 *   host side:    prx_load / prx_get_proc / prx_unload
 *
 * The pure parts (descriptor validation, segment scan, name lookup) take
 * plain memory ranges and are host-testable; the kernel calls are injected
 * through prx_loader_ops so a test can substitute fakes. Define
 * PRX_LOADER_IMPLEMENTATION in exactly one translation unit of the host.
 */

#ifndef PS5_PRX_LOADER_H
#define PS5_PRX_LOADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* "PRXDESC1" read as a little-endian 64-bit word. */
#define PRX_DESCRIPTOR_MAGIC UINT64_C(0x3143534544585250)
#define PRX_DESCRIPTOR_VERSION 1u
#define PRX_DESCRIPTOR_ALIGNMENT 16u
#define PRX_MAX_EXPORTS 4096u
#define PRX_MAX_SEGMENTS 4u
#define PRX_MODULE_INFO_BYTES 0x160u

    typedef struct prx_export
    {
        const char *name;
        const void *address;
    } prx_export;

    typedef struct prx_descriptor_header
    {
        uint64_t magic;
        uint32_t version;
        uint32_t count;
    } prx_descriptor_header;

    typedef struct prx_descriptor
    {
        prx_descriptor_header header;
        prx_export exports[1];
    } prx_descriptor;

#define PRX_EXPORT(symbol) {#symbol, (const void *)&(symbol)}

/* Defines a descriptor named `symbol` holding the listed PRX_EXPORT entries.
 * The object is const, 16-byte aligned and kept even when unreferenced; its
 * pointers become valid through the loader's RELATIVE relocations. */
#define PRX_DEFINE_DESCRIPTOR(symbol, ...)                                                         \
    __attribute__((used, aligned(PRX_DESCRIPTOR_ALIGNMENT))) const struct                          \
    {                                                                                              \
        prx_descriptor_header header;                                                              \
        prx_export exports[sizeof((prx_export[]){__VA_ARGS__}) / sizeof(prx_export)];              \
    } symbol = {{PRX_DESCRIPTOR_MAGIC, PRX_DESCRIPTOR_VERSION,                                     \
                 (uint32_t)(sizeof((prx_export[]){__VA_ARGS__}) / sizeof(prx_export))},            \
                {__VA_ARGS__}}

    typedef struct prx_segment
    {
        const void *address;
        uint32_t size;
        uint32_t protection; /* 1 read, 2 write, 4 execute */
    } prx_segment;

    typedef struct prx_loader_ops
    {
        int32_t (*load_start)(const char *path, size_t argc, const void *argv, uint32_t flags,
                              const void *option, int *result);
        int (*module_info)(int32_t handle, void *info);
        int (*stop_unload)(int32_t handle, size_t argc, const void *argv, uint32_t flags,
                           const void *option, int *result);
    } prx_loader_ops;

    typedef struct prx_module
    {
        int32_t handle;
        int start_result;
        char name[64];
        uint32_t segment_count;
        prx_segment segments[PRX_MAX_SEGMENTS];
        const prx_descriptor *descriptor;
    } prx_module;

    enum prx_result
    {
        PRX_OK = 0,
        PRX_ERROR_ARGUMENT = -1,
        PRX_ERROR_LOAD = -2,
        PRX_ERROR_MODULE_INFO = -3,
        PRX_ERROR_NO_DESCRIPTOR = -4,
        PRX_ERROR_DESCRIPTOR_INVALID = -5,
        PRX_ERROR_UNLOAD = -6,
    };

    /* Parses a SceKernelModuleInfo record (PRX_MODULE_INFO_BYTES) into name and
     * segments. Returns PRX_OK or PRX_ERROR_MODULE_INFO on an implausible record. */
    int prx_parse_module_info(const void *info, char *name, size_t name_capacity,
                              prx_segment *segments, uint32_t *segment_count);

    /* Scans readable segments for a valid descriptor. Every export name and
     * address must lie inside one of the segments; otherwise the candidate is
     * rejected and scanning continues. */
    int prx_find_descriptor(const prx_segment *segments, uint32_t segment_count,
                            const prx_descriptor **descriptor);

    /* Looks a name up in a validated descriptor. Returns NULL when absent. */
    const void *prx_descriptor_lookup(const prx_descriptor *descriptor, const char *name);

    /* Loads, starts and describes a module at `path` using the given ops. On any
     * failure after a successful load the module is unloaded again so the caller
     * never holds a half-initialised handle. */
    int prx_load(prx_module *module, const char *path, const prx_loader_ops *ops);
    const void *prx_get_proc(const prx_module *module, const char *name);
    int prx_unload(prx_module *module, const prx_loader_ops *ops);

#ifdef PRX_LOADER_NATIVE
    /* Kernel entry points as exported by libkernel on firmware 12.02. */
    int32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                                     uint32_t flags, const void *option, int *result);
    int sceKernelGetModuleInfo(int32_t handle, void *info);
    int sceKernelStopUnloadModule(int32_t handle, size_t argc, const void *argv, uint32_t flags,
                                  const void *option, int *result);
    const prx_loader_ops *prx_native_ops(void);
#endif

#ifdef __cplusplus
}
#endif

#ifdef PRX_LOADER_IMPLEMENTATION

#include <string.h>

static int prx__contains(const prx_segment *segments, uint32_t segment_count, const void *pointer,
                         size_t bytes)
{
    const uintptr_t address = (uintptr_t)pointer;
    uint32_t index;
    if (pointer == NULL || bytes == 0 || address > UINTPTR_MAX - bytes)
        return 0;
    for (index = 0; index < segment_count; ++index)
    {
        const uintptr_t base = (uintptr_t)segments[index].address;
        const size_t size = segments[index].size;
        if (base == 0 || size == 0)
            continue;
        if (address >= base && address + bytes <= base + size)
            return 1;
    }
    return 0;
}

static int prx__name_inside(const prx_segment *segments, uint32_t segment_count, const char *name)
{
    size_t length = 0;
    if (!prx__contains(segments, segment_count, name, 1))
        return 0;
    /* Bound the string walk by the segment that holds its first byte. */
    while (prx__contains(segments, segment_count, name + length, 1))
    {
        if (name[length] == '\0')
            return length > 0;
        if (++length > 256)
            return 0;
    }
    return 0;
}

static int prx__validate(const prx_descriptor *descriptor, const prx_segment *segments,
                         uint32_t segment_count)
{
    uint32_t index;
    size_t bytes;
    if (descriptor->header.magic != PRX_DESCRIPTOR_MAGIC ||
        descriptor->header.version != PRX_DESCRIPTOR_VERSION || descriptor->header.count == 0 ||
        descriptor->header.count > PRX_MAX_EXPORTS)
        return 0;
    bytes = sizeof(prx_descriptor_header) + (size_t)descriptor->header.count * sizeof(prx_export);
    if (!prx__contains(segments, segment_count, descriptor, bytes))
        return 0;
    for (index = 0; index < descriptor->header.count; ++index)
    {
        const prx_export *entry = &descriptor->exports[index];
        if (!prx__name_inside(segments, segment_count, entry->name) ||
            !prx__contains(segments, segment_count, entry->address, 1))
            return 0;
    }
    return 1;
}

int prx_parse_module_info(const void *info, char *name, size_t name_capacity, prx_segment *segments,
                          uint32_t *segment_count)
{
    const uint8_t *raw = (const uint8_t *)info;
    uint64_t size;
    uint32_t count;
    uint32_t index;
    if (info == NULL || segments == NULL || segment_count == NULL)
        return PRX_ERROR_ARGUMENT;
    memcpy(&size, raw, sizeof(size));
    memcpy(&count, raw + 0x148, sizeof(count));
    if (size != PRX_MODULE_INFO_BYTES || count == 0 || count > PRX_MAX_SEGMENTS)
        return PRX_ERROR_MODULE_INFO;
    if (name != NULL && name_capacity != 0)
    {
        size_t copy = name_capacity - 1;
        if (copy > 255)
            copy = 255;
        memcpy(name, raw + 8, copy);
        name[copy] = '\0';
    }
    for (index = 0; index < count; ++index)
    {
        const uint8_t *segment = raw + 0x108 + index * 16;
        uintptr_t address;
        memcpy(&address, segment, sizeof(address));
        segments[index].address = (const void *)address;
        memcpy(&segments[index].size, segment + 8, sizeof(uint32_t));
        memcpy(&segments[index].protection, segment + 12, sizeof(uint32_t));
        if (address == 0 || segments[index].size == 0)
            return PRX_ERROR_MODULE_INFO;
    }
    *segment_count = count;
    return PRX_OK;
}

int prx_find_descriptor(const prx_segment *segments, uint32_t segment_count,
                        const prx_descriptor **descriptor)
{
    uint32_t index;
    if (segments == NULL || descriptor == NULL || segment_count == 0 ||
        segment_count > PRX_MAX_SEGMENTS)
        return PRX_ERROR_ARGUMENT;
    *descriptor = NULL;
    for (index = 0; index < segment_count; ++index)
    {
        const uint8_t *base = (const uint8_t *)segments[index].address;
        size_t offset;
        if (base == NULL || (segments[index].protection & 1u) == 0 ||
            segments[index].size < sizeof(prx_descriptor_header))
            continue;
        for (offset = 0; offset + sizeof(prx_descriptor_header) <= segments[index].size;
             offset += PRX_DESCRIPTOR_ALIGNMENT)
        {
            uint64_t magic;
            memcpy(&magic, base + offset, sizeof(magic));
            if (magic != PRX_DESCRIPTOR_MAGIC)
                continue;
            {
                const prx_descriptor *candidate = (const prx_descriptor *)(base + offset);
                if (prx__validate(candidate, segments, segment_count))
                {
                    *descriptor = candidate;
                    return PRX_OK;
                }
            }
        }
    }
    return PRX_ERROR_NO_DESCRIPTOR;
}

const void *prx_descriptor_lookup(const prx_descriptor *descriptor, const char *name)
{
    uint32_t index;
    if (descriptor == NULL || name == NULL)
        return NULL;
    for (index = 0; index < descriptor->header.count; ++index)
        if (strcmp(descriptor->exports[index].name, name) == 0)
            return descriptor->exports[index].address;
    return NULL;
}

int prx_load(prx_module *module, const char *path, const prx_loader_ops *ops)
{
    uint64_t info[PRX_MODULE_INFO_BYTES / 8 + 8];
    int result;
    if (module == NULL || path == NULL || ops == NULL || ops->load_start == NULL ||
        ops->module_info == NULL || ops->stop_unload == NULL)
        return PRX_ERROR_ARGUMENT;
    memset(module, 0, sizeof(*module));
    module->handle = ops->load_start(path, 0, NULL, 0, NULL, &module->start_result);
    if (module->handle <= 0)
        return PRX_ERROR_LOAD;
    memset(info, 0, sizeof(info));
    info[0] = PRX_MODULE_INFO_BYTES;
    if (ops->module_info(module->handle, info) != 0)
        result = PRX_ERROR_MODULE_INFO;
    else
        result = prx_parse_module_info(info, module->name, sizeof(module->name), module->segments,
                                       &module->segment_count);
    if (result == PRX_OK)
        result = prx_find_descriptor(module->segments, module->segment_count, &module->descriptor);
    if (result != PRX_OK)
    {
        int ignored = 0;
        (void)ops->stop_unload(module->handle, 0, NULL, 0, NULL, &ignored);
        module->handle = 0;
        module->descriptor = NULL;
    }
    return result;
}

const void *prx_get_proc(const prx_module *module, const char *name)
{
    if (module == NULL || module->handle <= 0)
        return NULL;
    return prx_descriptor_lookup(module->descriptor, name);
}

int prx_unload(prx_module *module, const prx_loader_ops *ops)
{
    int result = 0;
    if (module == NULL || ops == NULL || ops->stop_unload == NULL)
        return PRX_ERROR_ARGUMENT;
    if (module->handle <= 0)
        return PRX_OK;
    if (ops->stop_unload(module->handle, 0, NULL, 0, NULL, &result) != 0)
        return PRX_ERROR_UNLOAD;
    module->handle = 0;
    module->descriptor = NULL;
    return PRX_OK;
}

#ifdef PRX_LOADER_NATIVE
const prx_loader_ops *prx_native_ops(void)
{
    static const prx_loader_ops ops = {sceKernelLoadStartModule, sceKernelGetModuleInfo,
                                       sceKernelStopUnloadModule};
    return &ops;
}
#endif

#endif /* PRX_LOADER_IMPLEMENTATION */
#endif /* PS5_PRX_LOADER_H */
