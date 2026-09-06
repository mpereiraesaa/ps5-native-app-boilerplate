# Application-owned dynamic modules (PRX)

The converter can produce PS5 dynamic modules from ordinary Clang/lld shared
objects. A module is packaged next to the runtime shim under
`sce_module/`, publishes its global symbols as NID exports, and imports public
SDK symbols exactly like the executable does. This page documents the build
flow, the loader-visible shape the converter emits, and what is and is not
validated.

## Build flow

Place module sources under `modules/<name>/` together with an lld version
script named `exports.map` that lists the loader-visible symbols:

```text
{
    global:
        hello_add;
        hello_version;
    local:
        *;
};
```

Every `global` name must be defined by the module; lld rejects a version
script that names an undefined symbol. Then run:

```sh
bash tools/build-module.sh hello
```

The script compiles with `-fPIC`, links with
`prospero-lld --shared -Bsymbolic -T tooling/native/ps5-pie.ld
--version-script modules/hello/exports.map -soname hello.prx`, converts the
result with `ps5-native-tool link --module`, signs it, and leaves two files:

| Output | Purpose |
| --- | --- |
| `.local/runtime/hello.prx` | Signed module for `sce_module/`; select it with `APP_RUNTIME_MODULES`. |
| `.local/stubs/hello.so` | The lld shared object, reused as the import stub the application links against; select it with `APP_IMPORT_STUBS`. |

Build the application against the module with:

```sh
make APP_RUNTIME_MODULES=.local/runtime/hello.prx APP_IMPORT_STUBS=.local/stubs/hello.so
```

The application then records `DT_NEEDED hello.prx` and imports `hello_add`
as `NID#lib#module`, and the loader resolves it against the packaged module at
startup. Runtime loading with `sceKernelLoadStartModule` and `sceKernelDlsym`
uses the same packaged file at `/app0/sce_module/hello.prx`; the stub is not
needed for that path.

## What the converter emits

`ps5-native-tool link --module` mirrors the executable converter with these
differences, all taken from the hardware-validated runtime shim that
`libc_builder.cpp` emits:

- ELF type `0xFE18` (dynamic module) and a zero entry point. Initializers run
  through `DT_INIT`, the init array, and `module_start` when the loader
  invokes it; nothing here assumes which of those the firmware calls.
- `PT_SCE_MODULE_PARAM` (`0x61000002`) replaces `PT_SCE_PROCPARAM`: a 32-byte
  record with magic `0x3C13F4BF`, version 3, the companion and module SDK
  versions, and flag 1, placed inside the RELRO load.
- `DT_SONAME` and `DT_SCE_ORIGINAL_FILENAME` carry `--file-name`.
- `DT_SCE_MODULE_INFO` carries `--module-name` (default: the file name without
  extension) with version `0x0101` and module id 0.
- `DT_SCE_EXPORT_LIB` and its attribute record carry `--export-library`
  (default: the module name). The export library takes the library id after
  the last import library; the module itself is module id 0.
- Every defined global or weak dynamic symbol becomes an export named
  `<NID>#<export-lib-id>#A`, keeping its address, size, binding, and type.
- Imports are named `<NID>#<import-lib-id>#<module-id>` as in executables.
- The SysV hash table hashes the exact encoded names stored in `.dynstr`,
  because the loader resolves exports by looking up those strings.

The default export-library name equals the module name so that the stub reader
in `elf_object.cpp`, which derives an import library name from a SONAME,
produces the same `NID#lib#module` identity on the importing side.

## Loader contracts learned on hardware

Two facts about the FW 12.02 loader were established with `sceKernelLoadStartModule`
on real hardware and are now enforced by the converter:

- **`DT_PLTGOT` is a three-entry table.** The loader writes its link map and
  resolver into entries 1 and 2 regardless of whether the module has a PLT.
  lld emits `.got.plt` only when a PLT exists, so a `-fno-plt` module can end
  up with a one-entry GOT and the module parameters directly behind it. The
  loader then corrupts the parameters before validating them and reports
  `0x80020063` (SDK version). The converter reserves its own table inside the
  RELRO load, places the module parameters after it, and points `DT_PLTGOT`
  at the table.
- **`e_entry` is the module's start routine and must return 0.** The loader
  calls it after mapping; a non-zero return makes the load fail with
  `0x80020016`. The layout script's `int3` filler at the base becomes a
  `xor eax, eax; ret` stub; `e_entry` is the exported `module_start` when
  present and the stub otherwise. Empty `DT_INIT`/`DT_FINI` also resolve to
  the stub.

Two further observations shape how modules are consumed:

- The process loader does **not** load an application-owned PRX named in
  `DT_NEEDED`; only the platform-known `sce_module` entries such as
  `libc.prx` are picked up at start and unknown imports stay unbound (zero).
  Application modules are loaded at runtime.
- `sceKernelDlsym` returns `0x80020003` for every application-owned module,
  including the runtime shim, whatever the hash table contents, and the
  loader does not invoke `module_start` through symbol lookup either. The
  sample therefore publishes a static, relocated **export descriptor**
  (`hello_exports`, magic `PRXDESC1`) that a host finds by scanning the
  module segments reported by `sceKernelGetModuleInfo`. Function pointers in
  the descriptor are valid after the loader's RELATIVE relocations. This is
  the resolution path validated on hardware.

## Relocation policy

A module may only carry dynamic relocations that reference imported symbols
or are self-relative. If a module calls or takes the address of one of its own
exports through the GOT or PLT, the converter stops with
`link with -Bsymbolic`; `tools/build-module.sh` already passes that flag.
Initial-exec TLS (`R_X86_64_TPOFF64`) is rejected; general-dynamic TLS
(`DTPMOD64`/`DTPOFF64` with symbol 0) is accepted, matching the runtime shim.

## Inspecting a module

```sh
build/host/ps5-native-tool self --inspect --file .local/runtime/hello.prx
llvm-readelf-18 -h -l -d build/modules/hello/hello.elf
```

`tests/test_module_writer.py` is an independent Python reader that checks the
module type, parameter record, identities, NID encoding, hash reachability,
relocation policy, and FSELF round trip against a module linked with the host
Clang and lld. It skips when those tools are absent.

## Validation status

Host-validated: conversion, signing, inspection, and the application-side
import stub path. The module shape follows the runtime shim that the loader
accepts on firmware 6.02 and 12.70.

Validated on FW 12.02 (2026-09-06): `sceKernelLoadStartModule` of the packaged
sample from `/app0/sce_module/`, resolution of its exports through the
descriptor, calls into module code including a kernel import from inside the
module, and `sceKernelStopUnloadModule`. Not available on this firmware for
application modules: load-time `DT_NEEDED` binding, `sceKernelDlsym`, and
`module_start` invocation by the loader (`e_entry` is called instead).
