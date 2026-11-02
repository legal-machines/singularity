#!/usr/bin/env python3
"""Compatibility wrapper for the unified Singularity developer entry point."""

import sys

from singularity import cli


if __name__ == "__main__":
    sys.argv.insert(1, "dev")
    cli()
