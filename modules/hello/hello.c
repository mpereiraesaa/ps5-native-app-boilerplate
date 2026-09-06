/*
 * ps5-native-app-boilerplate - Minimal PS5 dynamic-module sample.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Exports functions and data through a prx_loader.h descriptor, imports one
 * kernel call, and keeps private state, so a hardware run can prove export
 * resolution, module-local data, and system imports from inside a packaged PRX.
 */

#include "prx_loader.h"

#include <stddef.h>
#include <stdint.h>

int sceKernelUsleep(unsigned int microseconds);
int module_start(size_t argc, const void *argv);

static uint32_t call_count;

/* Loader-visible export. The application imports it by NID. */
int hello_add(int left, int right)
{
    ++call_count;
    return left + right;
}

/* Exercises a system import from module code and reports module-local state. */
uint32_t hello_sleep_and_count(unsigned int microseconds)
{
    if (microseconds != 0)
        (void)sceKernelUsleep(microseconds);
    return ++call_count;
}

/* Exported data object; its address must land inside the module's data LOAD. */
const uint32_t hello_version = 0x00010000;

/* Set by module_start. The application reads it to learn whether the loader
 * invokes the conventional entry points of a packaged module on this
 * firmware; both answers are useful evidence and neither is assumed. */
uint32_t hello_started;
uint32_t hello_stopped;

/* Handshake filled for the caller of sceKernelLoadStartModule: the loader
 * forwards argc/argv to module_start, so a module can publish its entry points
 * without depending on the kernel's symbol lookup. The caller passes a table
 * whose magic and size it owns; anything else is left untouched. */
typedef struct hello_api
{
    uint32_t magic;   /* 'PRX1' */
    uint32_t version; /* hello_version */
    int (*add)(int, int);
    uint32_t (*sleep_and_count)(unsigned int);
    const uint32_t *started;
    const uint32_t *stopped;
} hello_api;

/* Export descriptor the host locates through prx_loader.h: neither kernel
 * symbol lookup nor module_start arguments are involved, and the loader's
 * RELATIVE relocations make every pointer valid once the module is mapped. */
PRX_DEFINE_DESCRIPTOR(hello_exports, PRX_EXPORT(hello_add), PRX_EXPORT(hello_sleep_and_count),
                      PRX_EXPORT(hello_version), PRX_EXPORT(hello_started),
                      PRX_EXPORT(hello_stopped), PRX_EXPORT(module_start));

int module_start(size_t argc, const void *argv)
{
    hello_started = 1;
    if (argc == sizeof(hello_api) && argv)
    {
        hello_api *api = (hello_api *)(uintptr_t)argv;
        if (api->magic == 0x31585250u)
        {
            api->version = hello_version;
            api->add = hello_add;
            api->sleep_and_count = hello_sleep_and_count;
            api->started = &hello_started;
            api->stopped = &hello_stopped;
        }
    }
    return 0;
}

int module_stop(size_t argc, const void *argv)
{
    (void)argc;
    (void)argv;
    hello_stopped = 1;
    return 0;
}
