#!/usr/bin/env bash
# Builds the Docker image `ps3emu-psl1ght`: the PS3 homebrew toolchain (GCC, binutils, newlib and
# the PSL1GHT SDK), built from the ps3dev project's public sources. CMake uses it to compile the
# programs in examples/psl1ght. Expect it to take a while (roughly 20-60 minutes).
set -euo pipefail

# ps3toolchain pins its archives by SHA-256; PSL1GHT itself is fetched from its master branch.
TOOLCHAIN_COMMIT=de85d7d5d11dc0a99d823ece3d440eea46c34d0f

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

git clone --quiet https://github.com/ps3dev/ps3toolchain.git "$work/ps3toolchain"
git -C "$work/ps3toolchain" checkout --quiet "$TOOLCHAIN_COMMIT"
docker build --tag ps3emu-psl1ght-base "$work/ps3toolchain"

# Steps: 1-2 binutils, GCC and newlib for the PPU; 4 tool symlinks; 5-6 the same for the SPU;
# 8 PSL1GHT. Skipped: 3 and 7 (GDB) and 9 (extra libraries).
docker rm --force ps3emu-psl1ght-build >/dev/null 2>&1 || true
docker run --name ps3emu-psl1ght-build ps3emu-psl1ght-base ./toolchain.sh 1 2 4 5 6 8
docker commit ps3emu-psl1ght-build ps3emu-psl1ght
docker rm ps3emu-psl1ght-build
echo "Built image ps3emu-psl1ght. Re-run CMake to enable the PSL1GHT examples."
