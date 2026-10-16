#!/usr/bin/env python3
"""Build the OTA manifest the device polls at /firmware/latest.json.

The manifest carries the version string the firmware compares against, the
sha256 the device verifies after download, and the URL it fetches. The version
string must match what sb_version_short() produces on the device
("v<APP_VERSION>+<git short commit>"), so it recognises "already current".

The commit abbreviation length is shared with the firmware: both this tool and
main/CMakeLists.txt ask git for --short=8, so both sides get the same value and
neither truncates by hand (a hand-rolled length would bypass git's uniqueness
guarantee and can make the device re-flash the same image forever).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess


def git_value(args: list[str], default: str) -> str:
    try:
        out = subprocess.run(["git", *args], capture_output=True, text=True,
                             check=True).stdout.strip()
        return out or default
    except (OSError, subprocess.SubprocessError):
        return default


def app_version(repo: pathlib.Path) -> str:
    """Read SB_APP_VERSION out of main/CMakeLists.txt so there is one source."""
    text = (repo / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r'set\(SB_APP_VERSION\s+"([^"]+)"\)', text)
    return match.group(1) if match else "0.0.0"


def main() -> int:
    parser = argparse.ArgumentParser(description="Create the OTA manifest")
    parser.add_argument("image", help="OTA image (app-only .bin)")
    parser.add_argument("output", help="manifest path to write")
    parser.add_argument("--base-url", default="http://8.137.56.181:18080",
                        help="base URL the device uses to fetch the image")
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parent.parent
    image = pathlib.Path(args.image)
    blob = image.read_bytes()
    sha = hashlib.sha256(blob).hexdigest()
    # Must stay in step with the firmware: main/CMakeLists.txt injects the same
    # 8-character abbreviation, and sb_version_short() composes the identical
    # string. `--short=8` means "at least 8, unique", so git extends it when
    # necessary - both sides use git's answer rather than truncating by hand.
    commit = git_value(["rev-parse", "--short=8", "HEAD"], "unknown")
    version = f"v{app_version(repo)}+{commit}"
    # Deploy name carries the commit so images never collide in the firmware dir.
    published = f"app-{commit}.bin"

    manifest = {
        "version": version,
        "sha256": sha,
        "size": len(blob),
        "commit": git_value(["rev-parse", "HEAD"], "unknown"),
        "branch": git_value(["rev-parse", "--abbrev-ref", "HEAD"], "unknown"),
        "url": f"{args.base_url.rstrip('/')}/firmware/{published}",
        "published_name": published,
    }
    pathlib.Path(args.output).write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"{args.output}: {version} ({len(blob)} bytes) sha256={sha[:12]}...")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
