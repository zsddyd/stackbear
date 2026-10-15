"""The OTA endpoints must serve the manifest and images and nothing else."""

from __future__ import annotations

import json
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import server  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


def test_firmware_endpoints() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        server.FIRMWARE_DIR = pathlib.Path(tmp)
        # Nothing published yet: the device must get a clear 404.
        _, status, _ = server._serve_firmware("/firmware/latest.json")
        check(status.startswith("404"), f"unpublished manifest -> 404, got {status}")

        manifest = {"version": "v0.5.0+abc1234", "sha256": "ab" * 32,
                    "url": "http://host/firmware/app-abc1234.bin"}
        (server.FIRMWARE_DIR / "latest.json").write_text(json.dumps(manifest), encoding="utf-8")
        body, status, ctype = server._serve_firmware("/firmware/latest.json")
        check(status.startswith("200") and json.loads(body)["version"] == manifest["version"],
              "manifest served")
        check(ctype == "application/json", f"manifest content type {ctype}")

        (server.FIRMWARE_DIR / "app-abc1234.bin").write_bytes(b"OTAIMAGE")
        body, status, ctype = server._serve_firmware("/firmware/app-abc1234.bin")
        check(status.startswith("200") and body == b"OTAIMAGE", "image served")
        check(ctype == "application/octet-stream", f"image content type {ctype}")

        # Path traversal and odd names must not escape or match.
        for bad in ("/firmware/../secrets", "/firmware/other.bin",
                    "/firmware/app-x.bin/../latest.json", "/firmware/"):
            _, status, _ = server._serve_firmware(bad)
            check(status.startswith("404"), f"{bad} -> 404, got {status}")


def main() -> int:
    test_firmware_endpoints()
    if FAILURES:
        print(f"test_firmware: {FAILURES} failure(s)")
        return 1
    print("test_firmware: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
