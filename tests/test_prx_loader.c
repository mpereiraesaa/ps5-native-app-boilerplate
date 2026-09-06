/*
 * ps5-native-app-boilerplate - Host tests for the module loader header.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Exercises the pure descriptor scan and lookup on synthetic segments, the
 * module-info parser on a record shaped like the firmware 12.02 result, and
 * prx_load/prx_unload against fake kernel calls including the unload-on-failure
 * path. Uses a CHECK macro that survives NDEBUG.
 */

#define PRX_LOADER_IMPLEMENTATION
#include "../modules/prx_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static int fake_add(int left, int right)
{
    return left + right;
}
static const uint32_t fake_version = 0x00010000u;

/* A module image laid out like a real one: code, rodata with the descriptor,
 * and a data segment; strings live in rodata. */
static unsigned char code[64];
static struct
{
    char names[4][32];
    unsigned char pad[16];
    prx_descriptor_header header;
    prx_export exports[2];
} rodata __attribute__((aligned(16)));
static uint32_t data[8];

static prx_segment segments[3];

static void build_image(void)
{
    strcpy(rodata.names[0], "fake_add");
    strcpy(rodata.names[1], "fake_version");
    rodata.header.magic = PRX_DESCRIPTOR_MAGIC;
    rodata.header.version = PRX_DESCRIPTOR_VERSION;
    rodata.header.count = 2;
    rodata.exports[0].name = rodata.names[0];
    rodata.exports[0].address = (const void *)code; /* pretend code lives in the code segment */
    rodata.exports[1].name = rodata.names[1];
    rodata.exports[1].address = &data[1];
    segments[0].address = code;
    segments[0].size = sizeof(code);
    segments[0].protection = 4;
    segments[1].address = &rodata;
    segments[1].size = sizeof(rodata);
    segments[1].protection = 1;
    segments[2].address = data;
    segments[2].size = sizeof(data);
    segments[2].protection = 3;
}

static void test_scan_and_lookup(void)
{
    const prx_descriptor *descriptor = NULL;
    build_image();
    CHECK(prx_find_descriptor(segments, 3, &descriptor) == PRX_OK);
    CHECK((const void *)descriptor == (const void *)&rodata.header);
    CHECK(prx_descriptor_lookup(descriptor, "fake_add") == (const void *)code);
    CHECK(prx_descriptor_lookup(descriptor, "fake_version") == (const void *)&data[1]);
    CHECK(prx_descriptor_lookup(descriptor, "missing") == NULL);
    CHECK(prx_descriptor_lookup(descriptor, NULL) == NULL);
}

static void test_rejects_invalid_candidates(void)
{
    const prx_descriptor *descriptor = NULL;
    build_image();
    /* Wrong version. */
    rodata.header.version = 2;
    CHECK(prx_find_descriptor(segments, 3, &descriptor) == PRX_ERROR_NO_DESCRIPTOR);
    build_image();
    /* An export address outside every segment. */
    rodata.exports[1].address = (const void *)(uintptr_t)0x10;
    CHECK(prx_find_descriptor(segments, 3, &descriptor) == PRX_ERROR_NO_DESCRIPTOR);
    build_image();
    /* A name pointer outside the module. */
    rodata.exports[0].name = "not inside";
    CHECK(prx_find_descriptor(segments, 3, &descriptor) == PRX_ERROR_NO_DESCRIPTOR);
    build_image();
    /* Count that would run past the segment. */
    rodata.header.count = 4000;
    CHECK(prx_find_descriptor(segments, 3, &descriptor) == PRX_ERROR_NO_DESCRIPTOR);
    build_image();
    /* Non-readable segment holding the descriptor is skipped. */
    segments[1].protection = 0;
    CHECK(prx_find_descriptor(segments, 3, &descriptor) == PRX_ERROR_NO_DESCRIPTOR);
    CHECK(prx_find_descriptor(NULL, 3, &descriptor) == PRX_ERROR_ARGUMENT);
    CHECK(prx_find_descriptor(segments, 0, &descriptor) == PRX_ERROR_ARGUMENT);
}

static void fill_info(unsigned char *info, const char *name)
{
    uint64_t size = PRX_MODULE_INFO_BYTES;
    uint32_t count = 3;
    unsigned index;
    memset(info, 0, PRX_MODULE_INFO_BYTES);
    memcpy(info, &size, sizeof(size));
    strcpy((char *)info + 8, name);
    for (index = 0; index < 3; ++index)
    {
        uintptr_t address = (uintptr_t)segments[index].address;
        memcpy(info + 0x108 + index * 16, &address, sizeof(address));
        memcpy(info + 0x108 + index * 16 + 8, &segments[index].size, 4);
        memcpy(info + 0x108 + index * 16 + 12, &segments[index].protection, 4);
    }
    memcpy(info + 0x148, &count, sizeof(count));
}

static void test_module_info_parser(void)
{
    unsigned char info[PRX_MODULE_INFO_BYTES];
    char name[64];
    prx_segment parsed[PRX_MAX_SEGMENTS];
    uint32_t count = 0;
    build_image();
    fill_info(info, "hello.prx");
    CHECK(prx_parse_module_info(info, name, sizeof(name), parsed, &count) == PRX_OK);
    CHECK(strcmp(name, "hello.prx") == 0);
    CHECK(count == 3 && parsed[1].address == (const void *)&rodata && parsed[2].protection == 3);
    /* Wrong record size fails closed. */
    info[0] = 0x50;
    CHECK(prx_parse_module_info(info, name, sizeof(name), parsed, &count) == PRX_ERROR_MODULE_INFO);
}

/* Fake kernel. */
static int fake_load_calls, fake_unload_calls, fake_info_calls;
static int fake_fail_info, fake_hide_descriptor;
static int32_t fake_load_start(const char *path, size_t argc, const void *argv, uint32_t flags,
                               const void *option, int *result)
{
    (void)argc;
    (void)argv;
    (void)flags;
    (void)option;
    ++fake_load_calls;
    *result = 0;
    return strcmp(path, "/app0/sce_module/hello.prx") == 0 ? 0xd0 : (int32_t)0x80020002;
}
static int fake_module_info(int32_t handle, void *info)
{
    ++fake_info_calls;
    CHECK(handle == 0xd0);
    if (fake_fail_info)
        return (int)0x80020016;
    fill_info((unsigned char *)info, "hello.prx");
    if (fake_hide_descriptor)
        rodata.header.magic = 0;
    return 0;
}
static int fake_stop_unload(int32_t handle, size_t argc, const void *argv, uint32_t flags,
                            const void *option, int *result)
{
    (void)argc;
    (void)argv;
    (void)flags;
    (void)option;
    CHECK(handle == 0xd0);
    ++fake_unload_calls;
    *result = 0;
    return 0;
}

static void test_load_and_unload(void)
{
    const prx_loader_ops ops = {fake_load_start, fake_module_info, fake_stop_unload};
    prx_module module;
    build_image();
    fake_load_calls = fake_unload_calls = fake_info_calls = 0;
    CHECK(prx_load(&module, "/app0/sce_module/hello.prx", &ops) == PRX_OK);
    CHECK(module.handle == 0xd0 && module.segment_count == 3);
    CHECK(strcmp(module.name, "hello.prx") == 0);
    CHECK(prx_get_proc(&module, "fake_add") == (const void *)code);
    CHECK(prx_get_proc(&module, "absent") == NULL);
    CHECK(prx_unload(&module, &ops) == PRX_OK);
    CHECK(module.handle == 0 && prx_get_proc(&module, "fake_add") == NULL);
    CHECK(fake_load_calls == 1 && fake_info_calls == 1 && fake_unload_calls == 1);

    /* Load failure: no unload, no handle. */
    CHECK(prx_load(&module, "/app0/sce_module/missing.prx", &ops) == PRX_ERROR_LOAD);
    CHECK(module.handle <= 0 && fake_unload_calls == 1);

    /* Module info failure after a successful load unloads again. */
    fake_fail_info = 1;
    CHECK(prx_load(&module, "/app0/sce_module/hello.prx", &ops) == PRX_ERROR_MODULE_INFO);
    CHECK(module.handle == 0 && fake_unload_calls == 2);
    fake_fail_info = 0;

    /* Missing descriptor after a successful load unloads again. */
    fake_hide_descriptor = 1;
    CHECK(prx_load(&module, "/app0/sce_module/hello.prx", &ops) == PRX_ERROR_NO_DESCRIPTOR);
    CHECK(module.handle == 0 && fake_unload_calls == 3);
    fake_hide_descriptor = 0;
    build_image();

    CHECK(prx_load(NULL, "x", &ops) == PRX_ERROR_ARGUMENT);
    CHECK(prx_unload(&module, NULL) == PRX_ERROR_ARGUMENT);
}

/* The producer macro must yield a valid, discoverable descriptor. */
PRX_DEFINE_DESCRIPTOR(macro_descriptor, PRX_EXPORT(fake_add), PRX_EXPORT(fake_version));

static void test_define_descriptor_macro(void)
{
    CHECK(macro_descriptor.header.magic == PRX_DESCRIPTOR_MAGIC);
    CHECK(macro_descriptor.header.version == PRX_DESCRIPTOR_VERSION);
    CHECK(macro_descriptor.header.count == 2);
    CHECK(strcmp(macro_descriptor.exports[0].name, "fake_add") == 0);
    CHECK(macro_descriptor.exports[0].address == (const void *)&fake_add);
    CHECK(macro_descriptor.exports[1].address == (const void *)&fake_version);
    CHECK(((uintptr_t)&macro_descriptor % PRX_DESCRIPTOR_ALIGNMENT) == 0);
    CHECK(prx_descriptor_lookup((const prx_descriptor *)&macro_descriptor, "fake_version") ==
          (const void *)&fake_version);
}

int main(void)
{
    test_scan_and_lookup();
    test_rejects_invalid_candidates();
    test_module_info_parser();
    test_load_and_unload();
    test_define_descriptor_macro();
    puts("prx_loader tests passed");
    return 0;
}
