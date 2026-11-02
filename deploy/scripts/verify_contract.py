#!/usr/bin/env python3
"""Compatibility entry point for the canonical Native API generator check."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def main() -> int:
    generator = Path(__file__).with_name("generate_native_contract.py")
    return subprocess.call([sys.executable, str(generator), "--check"])


if __name__ == "__main__":
    raise SystemExit(main())
