# ps5-native-app-boilerplate - Runs the C host test for modules/prx_loader.h.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds tests/test_prx_loader.c with the host C compiler, once normally and
# once with NDEBUG, and also checks that the header compiles in its native
# configuration (PRX_LOADER_NATIVE) as a C11 translation unit. Skips when no
# C compiler is available.

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PrxLoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which("clang-18") or shutil.which("clang") or shutil.which("cc")
        if not cls.cc:
            raise unittest.SkipTest("a host C compiler is required")
        cls.work = Path(tempfile.mkdtemp(prefix="ps5-prx-loader-"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def _build_and_run(self, *extra):
        binary = self.work / ("test" + "".join(flag.strip("-") for flag in extra))
        subprocess.run(
            [self.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *extra,
             str(ROOT / "tests" / "test_prx_loader.c"), "-o", str(binary)],
            check=True, capture_output=True, text=True,
        )
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("prx_loader tests passed", result.stdout)

    def test_host_suite_passes(self):
        self._build_and_run()

    def test_host_suite_passes_with_ndebug(self):
        self._build_and_run("-DNDEBUG")

    def test_native_configuration_compiles(self):
        subprocess.run(
            [self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
             "-DPRX_LOADER_IMPLEMENTATION", "-DPRX_LOADER_NATIVE", "-x", "c",
             str(ROOT / "modules" / "prx_loader.h")],
            check=True, capture_output=True, text=True,
        )


if __name__ == "__main__":
    unittest.main()
