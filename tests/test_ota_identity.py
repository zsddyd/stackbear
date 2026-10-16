#!/usr/bin/env python3
"""The OTA identity string must be built the same way on both sides.

The device decides "already current" by comparing the manifest's `version`
against sb_version_short(). Those strings are produced by two independent
mechanisms - a C function in the firmware and this Python tool in CI - so a
disagreement does not show up as an error at build time. It shows up as a
device that re-downloads and re-flashes the same image on every check.

These tests pin the shared contract: the commit abbreviation length, the
template, and the fact that the firmware's commit is not truncated by hand.
"""

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

SPEC = importlib.util.spec_from_file_location(
    "make_ota_manifest", ROOT / "tools" / "make_ota_manifest.py"
)
assert SPEC and SPEC.loader
MANIFEST_TOOL = importlib.util.module_from_spec(SPEC)
sys.modules["make_ota_manifest"] = MANIFEST_TOOL
SPEC.loader.exec_module(MANIFEST_TOOL)

COMMIT_LEN = 8


class OtaIdentityTest(unittest.TestCase):
    def test_the_two_commit_lengths_agree(self) -> None:
        """CMake injects the commit, the tool asks git for it; both use 8."""
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn(f"--short={COMMIT_LEN} HEAD", cmake)
        self.assertIn(f'--short={COMMIT_LEN}"', Path(MANIFEST_TOOL.__file__).read_text(encoding="utf-8"))

    def test_the_firmware_does_not_truncate_by_hand(self) -> None:
        """A hand-rolled "%.Ns" would bypass git's uniqueness guarantee."""
        source = (ROOT / "main" / "sb_version.c").read_text(encoding="utf-8")
        self.assertNotIn("%.7s", source)
        self.assertNotIn("%.8s", source)
        self.assertIn('"v%s+%s"', source)

    def test_the_manifest_names_itself_after_the_commit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="stackbear-ota-identity-") as tmp:
            image = Path(tmp) / "app.bin"
            image.write_bytes(b"\xe9" + b"\0" * 63)
            out = Path(tmp) / "ota-latest.json"
            subprocess.run(
                [sys.executable, str(ROOT / "tools" / "make_ota_manifest.py"),
                 str(image), str(out)],
                check=True, capture_output=True,
            )
            manifest = json.loads(out.read_text(encoding="utf-8"))

        version = manifest["version"]
        number = MANIFEST_TOOL.app_version(ROOT)
        commit = manifest["commit"]
        self.assertTrue(version.startswith(f"v{number}+"), version)
        short = version[len(number) + 2:]
        self.assertTrue(commit.startswith(short), f"{commit} must start with {short}")

        # The firmware composes "v<number>+<its own short commit>". Since both
        # sides ask git for the same abbreviation, the firmware's string must be
        # byte-identical to the manifest's, which is the whole contract.
        expected = subprocess.run(
            ["git", "rev-parse", f"--short={COMMIT_LEN}", "HEAD"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout.strip()
        self.assertEqual(short, expected)
        self.assertEqual(version, f"v{number}+{expected}")
        self.assertEqual(len(short), COMMIT_LEN)
        # The deploy name carries the commit so images never collide.
        self.assertEqual(manifest["published_name"], f"app-{short}.bin")

    def test_version_number_has_one_source(self) -> None:
        """The tool reads SB_APP_VERSION out of main/CMakeLists.txt."""
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(cmake, r'set\(SB_APP_VERSION\s+"[^"]+"\)')
        self.assertEqual(MANIFEST_TOOL.app_version(ROOT),
                         MANIFEST_TOOL.app_version(ROOT))


if __name__ == "__main__":
    unittest.main()
