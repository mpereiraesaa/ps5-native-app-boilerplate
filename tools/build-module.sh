#!/usr/bin/env bash
# ps5-native-app-boilerplate - Native PS5 dynamic-module (PRX) build.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compiles modules/<name>/*.c into a signed PS5 module under .local/runtime/
# and keeps its lld shared object under .local/stubs/ as the import stub the
# application links against. Select both with APP_RUNTIME_MODULES and
# APP_IMPORT_STUBS when building the application.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
name=${1:-}
[[ $name =~ ^[A-Za-z][A-Za-z0-9_-]*$ && -d $root/modules/$name ]] || {
    echo "usage: tools/build-module.sh <module-name>   (source in modules/<module-name>/)" >&2
    exit 2
}
module_dir="$root/modules/$name"
exports_map="$module_dir/exports.map"
[[ -f $exports_map ]] || {
    echo "missing version script: modules/$name/exports.map" >&2
    exit 2
}

bash "$root/tools/setup-native-dependencies.sh" >/dev/null
sdk_root="$root/.deps/native/ps5-payload-sdk"
zlib_root="$root/.deps/native/zlib/root"
zlib_archive=$(find "$zlib_root" -type f -name libz.a -print -quit)
native="$root/tooling/native"
build="$root/build/modules/$name"
tool="$root/build/host/ps5-native-tool"
mkdir -p "$build/obj" "$root/.local/runtime" "$root/.local/stubs"

if [[ ! -x $tool ]]; then
    cxx=${CXX:-}
    [[ -n $cxx ]] || cxx=$(command -v clang++-18 || command -v clang++)
    [[ -n $cxx ]] || { echo "Clang++ was not found" >&2; exit 2; }
    mkdir -p "$(dirname -- "$tool")"
    "$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
        -I "$zlib_root/usr/include" \
        "$native/native_app_builder.cpp" "$native/self_container.cpp" \
        "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
        "$zlib_archive" -o "$tool"
fi

# Loader/container constants shared with tools/build.sh.
module_sdk=0x02000009
companion_sdk=0x08050001

mapfile -d '' -t sources < <(find "$module_dir" -type f -name '*.c' -print0 | sort -z)
(( ${#sources[@]} > 0 )) || { echo "modules/$name has no C sources" >&2; exit 2; }
objects=()
for source in "${sources[@]}"; do
    object="$build/obj/$(basename -- "$source").o"
    # -fPIC rather than the executable's PIE model: a module may not use copy
    # relocations for imported data, and -Bsymbolic below binds its own
    # symbols locally so no dynamic relocation refers to an export.
    PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" \
        -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
        -ffunction-sections -fdata-sections \
        -c "$source" -o "$object"
    objects+=("$object")
done

shared="$build/llvm-shared.elf"
"$sdk_root/bin/prospero-lld" --shared -Bsymbolic -T "$native/ps5-pie.ld" --eh-frame-hdr \
    --version-script "$exports_map" -soname "$name.prx" \
    -o "$shared" "${objects[@]}" --as-needed "$sdk_root"/target/lib/*.so
"$tool" link --module --in "$shared" --out "$build/$name.elf" \
    --stub-dir "$sdk_root/target/lib" --module-sdk "$module_sdk" \
    --companion-sdk "$companion_sdk" --file-name "$name.prx"
"$tool" self --sign --in "$build/$name.elf" --out "$root/.local/runtime/$name.prx"
"$tool" self --inspect --file "$root/.local/runtime/$name.prx"
cp "$shared" "$root/.local/stubs/$name.so"
sha256sum "$build/$name.elf" "$root/.local/runtime/$name.prx"

printf 'Module complete.\nRuntime module: .local/runtime/%s.prx\nImport stub:    .local/stubs/%s.so\n' \
    "$name" "$name"
printf 'Build the application with:\n  make APP_RUNTIME_MODULES=.local/runtime/%s.prx APP_IMPORT_STUBS=.local/stubs/%s.so\n' \
    "$name" "$name"
