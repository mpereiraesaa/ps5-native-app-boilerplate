/*
 * ps5-native-app-boilerplate - Minimal PS5 dynamic-module sample.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Exports two functions and one variable, imports one kernel call, and keeps
 * private state, so a hardware run can prove export resolution, module-local
 * data, and system imports from inside a packaged PRX.
 */

#include <stddef.h>
#include <stdint.h>

int sceKernelUsleep(unsigned int microseconds);

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

int module_start(size_t argc, const void *argv)
{
    (void)argc;
    (void)argv;
    hello_started = 1;
    return 0;
}

int module_stop(size_t argc, const void *argv)
{
    (void)argc;
    (void)argv;
    hello_stopped = 1;
    return 0;
}
