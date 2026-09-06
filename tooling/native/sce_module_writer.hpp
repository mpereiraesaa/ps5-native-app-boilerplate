/*
 * ps5-native-app-boilerplate - Native PS5 dynamic-module writer interface.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Declares conversion of an ordinary LLVM-linked PIE into the PS5 application
 * ELF layout consumed by the FSELF wrapper, and of an LLVM-linked shared
 * object into a PS5 dynamic module with NID exports.
 */

#pragma once

#include "elf_object.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps5::module
{

struct Options
{
    std::string entry = "_start";
    std::string file_name = "eboot.elf";
    std::uint32_t module_sdk = 0x02000009;
    std::uint32_t companion_sdk = 0x08050001;
    std::vector<std::string> version_components;
};

[[nodiscard]] elf::Bytes write_executable(const elf::Image &image, std::span<const elf::Stub> stubs,
                                          const Options &options = {});

struct ModuleOptions
{
    // Loader-visible file name; also the DT_SONAME other modules import.
    std::string file_name = "libmodule.prx";
    // DT_SCE_MODULE_INFO name. Defaults to file_name without its extension.
    std::string module_name;
    // DT_SCE_EXPORT_LIB name. Defaults to module_name, matching how the
    // stub reader derives import library names from a SONAME.
    std::string export_library;
    std::uint32_t module_sdk = 0x02000009;
    std::uint32_t companion_sdk = 0x08050001;
    std::vector<std::string> version_components;
};

// Converts an lld shared object (linked with the repository layout script and
// -Bsymbolic) into a PS5 dynamic module (e_type 0xFE18) that publishes its
// global dynamic symbols as NID exports and imports SDK symbols like an
// executable does.
[[nodiscard]] elf::Bytes write_module(const elf::Image &image, std::span<const elf::Stub> stubs,
                                      const ModuleOptions &options = {});

} // namespace ps5::module
