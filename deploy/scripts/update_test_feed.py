#!/usr/bin/env python3
"""A test feed for Singularity's self-installing update, never the site's.

It writes, in one folder, latest.json - a release exactly as the site answers
/singularity/api/releases/latest, signed with publish.py's own SiteEntry text -
and the installer it names, so a development (Debug) build can find, download,
check and install an update before one is offered:

    python3 deploy/scripts/update_test_feed.py <installer> --version 0.1.2 --out <folder>
    SINGULARITY_UPDATE_FEED=<folder> SINGULARITY_UPDATE_KEY=<printed key> <run Singularity>

It signs with a throwaway Ed25519 key it makes in <folder>, and refuses
Legal Machines' release key: a test feed must never be one a shipping build
would take. A shipping build reads neither variable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import publish  # noqa: E402  (SiteEntry, sign_text, verify_text, find_openssl, public_key_of)

SUFFIX_OS = {".dmg": "macos", ".exe": "windows", ".deb": "linux"}
OS_NAME = {"macos": "macOS", "windows": "Windows", "linux": "Linux"}


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Write a signed test feed for Singularity's self-installing update.")
    parser.add_argument("installer", type=Path, help="The installer to offer: a .dmg, .exe or .deb")
    parser.add_argument("--version", required=True, help="The version to offer it as, such as 0.1.2")
    parser.add_argument("--out", required=True, type=Path, help="The feed's folder")
    parser.add_argument("--arch", choices=["arm64", "x64"], help="Its processor, when its name does not say it")
    parser.add_argument("--notes", default="A test of Singularity's self-installing update.", help="Its notes")
    arguments = parser.parse_args()

    installer = arguments.installer.resolve()
    if not installer.is_file():
        raise SystemExit(f"No installer at {installer}")
    system = SUFFIX_OS.get(installer.suffix.lower())
    if system is None:
        raise SystemExit("The installer must be a .dmg, .exe or .deb")
    if not re.fullmatch(r"\d{1,9}\.\d{1,9}\.\d{1,9}", arguments.version):
        raise SystemExit("A version is three numbers, such as 0.1.2")
    arch = arguments.arch or ("arm64" if "arm64" in installer.name.lower() else "x64" if "x64" in installer.name.lower() else None)
    if arch is None:
        raise SystemExit("Name its processor with --arch arm64 or --arch x64")

    out = arguments.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    openssl = publish.find_openssl()
    key = out / "throwaway-ed25519.pem"
    if not key.is_file():
        subprocess.run([openssl, "genpkey", "-algorithm", "ed25519", "-out", str(key)], check=True, capture_output=True)
        key.chmod(0o600)
    public_key = publish.public_key_of(key, openssl)
    if public_key in publish.RELEASE_PUBLIC_KEYS:
        raise SystemExit("This is Legal Machines' release key: a test feed is signed with a throwaway key only.")

    # Named after the version it is offered as, as the release builds name theirs.
    file_name = f"singularity-{arguments.version}-{OS_NAME[system]}-{arch}{installer.suffix.lower()}"
    served = out / file_name
    if served.resolve() != installer:
        shutil.copyfile(installer, served)
    entry = publish.SiteEntry(
        version=arguments.version, os=system, arch=arch, file_name=file_name, bytes=served.stat().st_size,
        sha256=sha256_of(served), published_at=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        notes=arguments.notes)
    text = entry.signed_text()
    signature = publish.sign_text(text, key, openssl)
    if not publish.verify_text(text, signature, public_key, openssl):
        raise SystemExit("The throwaway key's signature did not verify.")
    (out / "latest.json").write_text(json.dumps(entry.as_json(signature), indent=2) + "\n", encoding="utf-8")

    print(f"Test feed: {out}")
    print(f"  offers Singularity {arguments.version} for {system}/{arch}: {file_name}, {entry.bytes} bytes")
    print(f"SINGULARITY_UPDATE_FEED={out}")
    print(f"SINGULARITY_UPDATE_KEY={public_key}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
