"""Compile the firmware's actual filename policy and exercise hostile inputs."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class ReaderTransferPolicyTests(unittest.TestCase):
    def test_native_policy(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            self.skipTest("A host C++ compiler is required")
        source = root / "scripts/tests/reader_transfer_policy_test.cc"
        with tempfile.TemporaryDirectory() as directory:
            executable = pathlib.Path(directory) / "policy_test.exe"
            if pathlib.Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/EHsc", "/std:c++17", "/utf-8",
                           f"/I{root}", str(source), f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", f"-I{root}", str(source), "-o", str(executable)]
            subprocess.run(command, cwd=directory, check=True, capture_output=True, text=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
            self.assertIn("checks passed", result.stdout)
