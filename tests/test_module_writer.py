# ps5-native-app-boilerplate - Host integration tests for the module writer.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Links a tiny shared object with the host Clang/lld, converts it with
# `ps5-native-tool link --module`, and checks the loader-visible result with
# an independent Python ELF reader: module type, module-parameter header,
# export/import identities, NID hashing, SysV hash lookup, relocation policy,
# and FSELF wrapping. Skips when the host toolchain is unavailable.

import base64
import hashlib
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "tooling" / "native"

NID_SUFFIX = bytes.fromhex("518d64a635ded8c1e6b039b1c3e55230")
ET_SCE_DYNAMIC = 0xFE18
PT_LOAD = 1
PT_DYNAMIC = 2
PT_TLS = 7
PT_SCE_MODULE_PARAM = 0x61000002
PT_SCE_PROCPARAM = 0x61000001
DT_NEEDED = 1
DT_HASH = 4
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_SONAME = 14
DT_JMPREL = 23
DT_PLTRELSZ = 2
DT_RELACOUNT = 0x6FFFFFF9
DT_SCE_MODULE_ATTR = 0x61000011
DT_SCE_EXPORT_LIB_ATTR = 0x61000017
DT_SCE_IMPORT_LIB_ATTR = 0x61000019
DT_SCE_HASHSZ = 0x6100003D
DT_SCE_SYMTABSZ = 0x6100003F
DT_SCE_ORIGINAL_FILENAME = 0x61000041
DT_SCE_MODULE_INFO = 0x61000043
DT_SCE_NEEDED_MODULE = 0x61000045
DT_SCE_EXPORT_LIB = 0x61000047
DT_SCE_IMPORT_LIB = 0x61000049
MODULE_PARAM_MAGIC = 0x3C13F4BF

MODULE_SOURCE = """
int sceKernelUsleep(unsigned int microseconds);
static unsigned int calls;
int hello_add(int left, int right) { ++calls; return left + right; }
unsigned int hello_sleep(unsigned int us) { (void)sceKernelUsleep(us); return ++calls; }
const unsigned int hello_version = 0x10000;
"""
STUB_SOURCE = "int sceKernelUsleep(unsigned int microseconds) { (void)microseconds; return 0; }\n"
EXPORTS = "{ global: hello_add; hello_sleep; hello_version; local: *; };\n"


def nid(name):
    digest = hashlib.sha1(name.encode("ascii") + NID_SUFFIX).digest()[:8][::-1]
    return base64.b64encode(digest).decode("ascii")[:11].replace("/", "-")


def elf_hash(name):
    value = 0
    for byte in name.encode("ascii"):
        value = ((value << 4) + byte) & 0xFFFFFFFF
        carry = value & 0xF0000000
        if carry:
            value ^= carry >> 24
        value &= ~carry & 0xFFFFFFFF
    return value


def find_tool(*names):
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


class ElfModule:
    """Minimal ELF64 reader that works without section headers."""

    def __init__(self, data):
        self.data = data
        assert data[:4] == b"\x7fELF"
        self.e_type = struct.unpack_from("<H", data, 0x10)[0]
        self.entry = struct.unpack_from("<Q", data, 0x18)[0]
        phoff = struct.unpack_from("<Q", data, 0x20)[0]
        phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
        self.phdrs = []
        for index in range(phnum):
            at = phoff + index * phentsize
            p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, p_align = struct.unpack_from(
                "<IIQQQQQQ", data, at
            )
            self.phdrs.append(
                dict(type=p_type, flags=p_flags, offset=p_offset, vaddr=p_vaddr,
                     filesz=p_filesz, memsz=p_memsz, align=p_align)
            )
        self.dynamic = self._read_dynamic()

    def file_offset(self, vaddr):
        for phdr in self.phdrs:
            if phdr["type"] == PT_LOAD and phdr["vaddr"] <= vaddr < phdr["vaddr"] + phdr["filesz"]:
                return phdr["offset"] + (vaddr - phdr["vaddr"])
        raise AssertionError(f"virtual address {vaddr:#x} is not file-backed")

    def _read_dynamic(self):
        entries = []
        for phdr in self.phdrs:
            if phdr["type"] != PT_DYNAMIC:
                continue
            for at in range(phdr["offset"], phdr["offset"] + phdr["filesz"], 16):
                tag, value = struct.unpack_from("<qQ", self.data, at)
                if tag == 0:
                    break
                entries.append((tag, value))
        return entries

    def tag(self, wanted):
        return [value for tag, value in self.dynamic if tag == wanted]

    def string(self, offset):
        strtab = self.file_offset(self.tag(DT_STRTAB)[0])
        end = self.data.index(b"\0", strtab + offset)
        return self.data[strtab + offset:end].decode("ascii")

    def symbols(self):
        symtab = self.file_offset(self.tag(DT_SYMTAB)[0])
        count = self.tag(DT_SCE_SYMTABSZ)[0] // 24
        result = []
        for index in range(count):
            name, info, other, shndx, value, size = struct.unpack_from(
                "<IBBHQQ", self.data, symtab + index * 24
            )
            result.append(dict(name=self.string(name), info=info, other=other, shndx=shndx,
                               value=value, size=size))
        return result

    def hash_lookup(self, name):
        table = self.file_offset(self.tag(DT_HASH)[0])
        nbucket, nchain = struct.unpack_from("<II", self.data, table)
        bucket = struct.unpack_from("<I", self.data, table + 8 + (elf_hash(name) % nbucket) * 4)[0]
        symbols = self.symbols()
        visited = set()
        while bucket != 0 and bucket not in visited:
            visited.add(bucket)
            if symbols[bucket]["name"] == name:
                return bucket
            bucket = struct.unpack_from("<I", self.data, table + 8 + nbucket * 4 + bucket * 4)[0]
        return None

    def relocations(self):
        result = []
        for table, size in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
            address = self.tag(table)
            length = self.tag(size)
            if not address or not length or length[0] == 0:
                continue
            base = self.file_offset(address[0])
            for at in range(base, base + length[0], 24):
                offset, info, addend = struct.unpack_from("<QQq", self.data, at)
                result.append((offset, info >> 32, info & 0xFFFFFFFF, addend))
        return result


class ElfSharedObject:
    """Reads dynamic symbols of the lld input through its section headers."""

    def __init__(self, data):
        shoff = struct.unpack_from("<Q", data, 0x28)[0]
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        sections = []
        for index in range(shnum):
            at = shoff + index * shentsize
            name, s_type, _, addr, offset, size, link, _, _, entsize = struct.unpack_from(
                "<IIQQQQIIQQ", data, at
            )
            sections.append(dict(name=name, type=s_type, addr=addr, offset=offset, size=size,
                                 link=link, entsize=entsize))
        names = sections[shstrndx]
        self.sections = {}
        for section in sections:
            start = names["offset"] + section["name"]
            end = data.index(b"\0", start)
            self.sections[data[start:end].decode("ascii")] = section
        dynsym = self.sections[".dynsym"]
        dynstr = sections[dynsym["link"]]
        self.dynamic_symbols = {}
        for at in range(dynsym["offset"], dynsym["offset"] + dynsym["size"], 24):
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, at)
            start = dynstr["offset"] + name
            symbol = data[start:data.index(b"\0", start)].decode("ascii")
            self.dynamic_symbols[symbol] = dict(info=info, shndx=shndx, value=value, size=size)


class ModuleWriterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clang = find_tool("clang-18", "clang")
        cls.lld = find_tool("ld.lld-18", "ld.lld")
        cls.cxx = find_tool("clang++-18", "clang++", "c++")
        if not (cls.clang and cls.lld and cls.cxx):
            raise unittest.SkipTest("host clang, lld and a C++20 compiler are required")
        cls.work = Path(tempfile.mkdtemp(prefix="ps5-module-writer-"))
        cls.tool = cls.work / "ps5-native-tool"
        cls._build_tool()
        cls.stub = cls._link_shared("libkernel", STUB_SOURCE, "libkernel.prx",
                                    exports="{ global: sceKernelUsleep; local: *; };\n")
        cls.shared = cls._link_shared("libhello", MODULE_SOURCE, "libhello.prx", exports=EXPORTS,
                                      inputs=[cls.stub])

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "work"):
            shutil.rmtree(cls.work, ignore_errors=True)

    @classmethod
    def _build_tool(cls):
        sources = [
            NATIVE / "native_app_builder.cpp", NATIVE / "self_container.cpp",
            NATIVE / "elf_object.cpp", NATIVE / "sce_module_writer.cpp",
        ]
        command = [cls.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                   *map(str, sources), "-o", str(cls.tool)]
        zlib_root = ROOT / ".deps" / "native" / "zlib" / "root"
        archives = list(zlib_root.rglob("libz.a")) if zlib_root.exists() else []
        if archives:
            command[-2:-2] = ["-I", str(zlib_root / "usr" / "include"), str(archives[0])]
        else:
            command.insert(-2, "-lz")
        subprocess.run(command, check=True, capture_output=True, text=True)

    @classmethod
    def _link_shared(cls, name, source, soname, exports, inputs=(), symbolic=True):
        directory = cls.work / name
        directory.mkdir(exist_ok=True)
        (directory / "module.c").write_text(source, encoding="utf-8")
        (directory / "exports.map").write_text(exports, encoding="utf-8")
        subprocess.run(
            # Same target and visibility model as tooling/prospero-clang18: the
            # PS5 target hides symbols unless nodllstorageclass visibility is
            # made default, so without the flag a module would export nothing.
            [cls.clang, "-target", "x86_64-sie-ps5", "-fvisibility-nodllstorageclass=default",
             "-std=c11", "-O2", "-fPIC", "-fno-plt", "-fno-stack-protector",
             "-ffunction-sections", "-fdata-sections", "-nostdlib",
             "-c", str(directory / "module.c"), "-o", str(directory / "module.o")],
            check=True, capture_output=True, text=True,
        )
        command = [cls.lld, "-m", "elf_x86_64", "--shared", "-z", "max-page-size=0x4000",
                   "--hash-style=gnu", "--eh-frame-hdr", "-T", str(NATIVE / "ps5-pie.ld"),
                   "--version-script", str(directory / "exports.map"), "-soname", soname,
                   "-o", str(directory / f"{name}.so"), str(directory / "module.o"),
                   "--as-needed", *map(str, inputs)]
        if symbolic:
            command.insert(3, "-Bsymbolic")
        subprocess.run(command, check=True, capture_output=True, text=True)
        return directory / f"{name}.so"

    def _convert(self, shared, *extra, expect_failure=False):
        output = self.work / (shared.stem + ".elf")
        result = subprocess.run(
            [str(self.tool), "link", "--module", "--in", str(shared), "--out", str(output),
             "--stub", str(self.stub), "--file-name", "libhello.prx", *extra],
            capture_output=True, text=True,
        )
        if expect_failure:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            return result.stderr
        self.assertEqual(result.returncode, 0, result.stderr)
        return output.read_bytes()

    def test_module_has_loader_visible_identity(self):
        module = ElfModule(self._convert(self.shared))
        self.assertEqual(module.e_type, ET_SCE_DYNAMIC)
        self.assertEqual(module.entry, 0)
        types = [phdr["type"] for phdr in module.phdrs]
        self.assertIn(PT_SCE_MODULE_PARAM, types)
        self.assertNotIn(PT_SCE_PROCPARAM, types)
        self.assertEqual(len(module.phdrs), 14)
        param = next(phdr for phdr in module.phdrs if phdr["type"] == PT_SCE_MODULE_PARAM)
        size, magic, version, companion, sdk, flag = struct.unpack_from(
            "<QIIIII", module.data, param["offset"]
        )
        self.assertEqual((size, magic, version, companion, sdk, flag),
                         (0x20, MODULE_PARAM_MAGIC, 3, 0x08050001, 0x02000009, 1))
        relro = next(phdr for phdr in module.phdrs if phdr["type"] == PT_LOAD and
                     phdr["vaddr"] <= param["vaddr"] < phdr["vaddr"] + phdr["memsz"])
        self.assertEqual(relro["flags"], 6, "module parameters live in the RELRO load")
        for phdr in module.phdrs:
            if phdr["type"] == PT_LOAD and phdr["flags"] != 0:
                self.assertEqual(phdr["offset"] % 0x4000, phdr["vaddr"] % 0x4000)
        self.assertEqual(module.string(module.tag(DT_SONAME)[0]), "libhello.prx")
        self.assertEqual(module.string(module.tag(DT_SCE_ORIGINAL_FILENAME)[0]), "libhello.prx")
        info = module.tag(DT_SCE_MODULE_INFO)[0]
        self.assertEqual(module.string(info & 0xFFFFFFFF), "libhello")
        self.assertEqual((info >> 32) & 0xFFFF, 0x0101)
        self.assertEqual(info >> 48, 0)
        self.assertEqual(module.tag(DT_SCE_MODULE_ATTR), [0])

    def test_exports_and_imports_use_encoded_nids(self):
        module = ElfModule(self._convert(self.shared))
        source = ElfSharedObject(self.shared.read_bytes())
        # One import library (libkernel, id 0) and one export library (id 1).
        needed = [module.string(value) for value in module.tag(DT_NEEDED)]
        self.assertEqual(needed, ["libkernel.prx"])
        imported = module.tag(DT_SCE_IMPORT_LIB)[0]
        self.assertEqual(module.string(imported & 0xFFFFFFFF), "libkernel")
        self.assertEqual(imported >> 48, 0)
        self.assertEqual(module.tag(DT_SCE_IMPORT_LIB_ATTR), [9])
        exported = module.tag(DT_SCE_EXPORT_LIB)[0]
        self.assertEqual(module.string(exported & 0xFFFFFFFF), "libhello")
        self.assertEqual((exported >> 32) & 0xFFFF, 1)
        self.assertEqual(exported >> 48, 1)
        self.assertEqual(module.tag(DT_SCE_EXPORT_LIB_ATTR), [(1 << 48) | 1])

        symbols = {symbol["name"]: symbol for symbol in module.symbols()}
        for name in ("hello_add", "hello_sleep", "hello_version"):
            mangled = f"{nid(name)}#B#A"
            self.assertIn(mangled, symbols, f"export {name} missing")
            expected = source.dynamic_symbols[name]
            self.assertEqual(symbols[mangled]["value"], expected["value"])
            self.assertEqual(symbols[mangled]["size"], expected["size"])
            self.assertEqual(symbols[mangled]["info"], expected["info"])
            self.assertNotEqual(symbols[mangled]["shndx"], 0)
            self.assertIsNotNone(module.hash_lookup(mangled), f"{name} not reachable by hash")
        import_name = f"{nid('sceKernelUsleep')}#A#B"
        self.assertIn(import_name, symbols)
        self.assertEqual(symbols[import_name]["shndx"], 0)
        self.assertEqual(symbols[import_name]["value"], 0)
        self.assertEqual(len(symbols), 1 + 3 + 1)
        # Exported code sits in the executable load; exported data in a data load.
        code = next(phdr for phdr in module.phdrs if phdr["type"] == PT_LOAD and phdr["flags"] == 1)
        add = symbols[f"{nid('hello_add')}#B#A"]["value"]
        self.assertTrue(code["vaddr"] <= add < code["vaddr"] + code["memsz"])
        self.assertEqual(module.tag(DT_SCE_SYMTABSZ), [5 * 24])
        hash_size = module.tag(DT_SCE_HASHSZ)[0]
        self.assertEqual(hash_size, 8 + 5 * 8)

    def test_dynamic_relocations_only_reference_imports(self):
        module = ElfModule(self._convert(self.shared))
        symbols = module.symbols()
        relative = 0
        for _, symbol, rel_type, _ in module.relocations():
            if rel_type == 8:
                relative += 1
                self.assertEqual(symbol, 0)
            else:
                self.assertIn(rel_type, (1, 6, 7))
                self.assertEqual(symbols[symbol]["shndx"], 0, "symbolic relocation must import")
        self.assertEqual(module.tag(DT_RELACOUNT), [relative])

    def test_rejects_module_that_binds_its_own_exports_dynamically(self):
        # Without -Bsymbolic a default-visibility export is preemptible, so
        # taking its address needs a GOT entry relocated against the symbol.
        source = MODULE_SOURCE + "void *hello_self(void) { return (void *)hello_add; }\n"
        exports = "{ global: hello_add; hello_sleep; hello_version; hello_self; local: *; };\n"
        shared = self._link_shared("libpreemptible", source, "libhello.prx", exports=exports,
                                   inputs=[self.stub], symbolic=False)
        error = self._convert(shared, expect_failure=True)
        self.assertIn("link with -Bsymbolic", error)

    def test_rejects_module_without_exports(self):
        source = "int sceKernelUsleep(unsigned int); int quiet(void) { return sceKernelUsleep(1); }\n"
        shared = self._link_shared("libsilent", source, "libhello.prx", exports="{ local: *; };\n",
                                   inputs=[self.stub])
        error = self._convert(shared, expect_failure=True)
        self.assertIn("publishes no global symbols", error)

    def test_module_name_and_export_library_options(self):
        module = ElfModule(self._convert(self.shared, "--module-name", "libSceHello",
                                         "--export-library", "libSceHelloCore"))
        info = module.tag(DT_SCE_MODULE_INFO)[0]
        self.assertEqual(module.string(info & 0xFFFFFFFF), "libSceHello")
        exported = module.tag(DT_SCE_EXPORT_LIB)[0]
        self.assertEqual(module.string(exported & 0xFFFFFFFF), "libSceHelloCore")

    def test_signed_container_round_trips(self):
        raw = self.work / "libhello.elf"
        raw.write_bytes(self._convert(self.shared))
        signed = self.work / "libhello.prx"
        subprocess.run([str(self.tool), "self", "--sign", "--in", str(raw), "--out", str(signed)],
                       check=True, capture_output=True, text=True)
        inspect = subprocess.run([str(self.tool), "self", "--inspect", "--file", str(signed)],
                                 check=True, capture_output=True, text=True).stdout
        self.assertIn("integrity: valid", inspect)
        self.assertIn("program type: 0x0000000000000001", inspect)
        extracted = self.work / "libhello.extracted.elf"
        subprocess.run([str(self.tool), "self", "--extract", "--file", str(signed), "--out",
                        str(extracted)], check=True, capture_output=True, text=True)
        self.assertEqual(ElfModule(extracted.read_bytes()).e_type, ET_SCE_DYNAMIC)

    def test_shared_object_doubles_as_import_stub(self):
        # The application-side converter must accept the module's lld output
        # as the stub that describes libhello.prx exports.
        app_source = ("int hello_add(int, int); void _start(void); "
                      "void _start(void) { (void)hello_add(1, 2); }\n")
        app = self._link_shared("app", app_source, "eboot.prx",
                                exports="{ global: _start; local: *; };\n", inputs=[self.shared])
        output = self.work / "app.elf"
        result = subprocess.run(
            [str(self.tool), "link", "--module", "--in", str(app), "--out", str(output),
             "--stub", str(self.shared), "--file-name", "app.prx"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        module = ElfModule(output.read_bytes())
        self.assertEqual([module.string(v) for v in module.tag(DT_NEEDED)], ["libhello.prx"])
        symbols = {symbol["name"] for symbol in module.symbols()}
        self.assertIn(f"{nid('hello_add')}#A#B", symbols)


if __name__ == "__main__":
    unittest.main()
