#!/bin/bash
# The Linux package's last check before it is called made
# (deploy/scripts/singularity.py, release --platform linux): the .deb is
# installed into a clean Ubuntu 24.04, as a person would install it, and every
# program and library it ships must find each library it needs, in the package
# or in what the package depends on. Otherwise it would not start there.
#
#   check-linux-package.sh <package.deb>
#
# Runs as root in a throwaway ubuntu:24.04 container; it needs the network for
# the package's dependencies.
set -u
package="$1"
export DEBIAN_FRONTEND=noninteractive

if ! apt-get update -qq >/dev/null || ! apt-get install -y -qq --no-install-recommends "$package" file >/tmp/install.log 2>&1; then
  echo "The Linux package does not install on a clean Ubuntu 24.04:"
  tail -20 /tmp/install.log
  exit 1
fi

failed=0
for expected in /usr/bin/singularity /opt/singularity/singularity /opt/singularity/singularity-mcp \
    /opt/singularity/english_common_words.txt /opt/singularity/llama.cpp/llama-server \
    /opt/singularity/frontend-dist/index.html; do
  if [ ! -e "$expected" ]; then
    echo "The package lacks $expected"
    failed=1
  fi
done

while IFS= read -r file; do
  if file -b "$file" | grep -q '^ELF'; then
    missing=$(ldd "$file" 2>&1 | grep "not found")
    if [ -n "$missing" ]; then
      echo "$file needs what neither the package nor its dependencies have:"
      echo "$missing"
      failed=1
    fi
  fi
done < <(find /opt/singularity -type f)

if [ "$failed" -eq 0 ]; then
  echo "Linux package: installs on a clean Ubuntu 24.04, and every program in it finds what it needs."
fi
exit "$failed"
