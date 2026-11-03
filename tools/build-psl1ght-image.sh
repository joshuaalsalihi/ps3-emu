#!/usr/bin/env bash
# Builds the Docker images CMake uses to compile the programs in examples/psl1ght:
#  - `ps3emu-psl1ght`: the PS3 homebrew toolchain (GCC, binutils, newlib and the PSL1GHT SDK),
#    built from the ps3dev project's public sources. Expect it to take a while (roughly 20-60
#    minutes).
#  - `ps3emu-cgcomp`: PSL1GHT's shader compiler with NVIDIA's Cg compiler library, for shaders
#    written in Cg (the toolchain image only assembles ARB-style shader assembly). NVIDIA only
#    ships the library for x86-64 Linux, so this image is x86-64 and runs emulated on other hosts.
#    It also keeps the PSL1GHT sources, whose samples the examples can build.
# Pass --cgcomp-only to build just the second, which takes a few minutes.
set -euo pipefail

# ps3toolchain pins its archives by SHA-256; we pin PSL1GHT ourselves.
TOOLCHAIN_COMMIT=de85d7d5d11dc0a99d823ece3d440eea46c34d0f
PSL1GHT_COMMIT=574b219ccd34be6d016aea11ecfa43da37a16edf
CG_URL=https://developer.download.nvidia.com/cg/Cg_3.1/Cg-3.1_April2012_x86_64.tgz
CG_SHA256=e8ff01e6cc38d1b3fd56a083f5860737dbd2f319a39037528fb1a74a89ae9878

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Built by running the setup in a container and committing it, as for the toolchain, since not
# every Docker builder honors --platform.
build_cgcomp() {
    docker rm --force ps3emu-cgcomp-build >/dev/null 2>&1 || true
    docker run --platform linux/amd64 --name ps3emu-cgcomp-build ubuntu:24.04 sh -euc "
        apt-get update -qq && apt-get install -y -qq g++ make git curl ca-certificates >/dev/null
        curl -sSfL -o /tmp/cg.tgz $CG_URL
        echo '$CG_SHA256  /tmp/cg.tgz' | sha256sum -c -
        tar xzf /tmp/cg.tgz -C / ./usr/lib64/libCg.so
        mv /usr/lib64/libCg.so /usr/local/lib/ && ldconfig && rm /tmp/cg.tgz
        git clone --quiet https://github.com/ps3dev/PSL1GHT.git /psl1ght
        git -C /psl1ght checkout --quiet $PSL1GHT_COMMIT
        make -C /psl1ght/tools/cgcomp >/dev/null
        install /psl1ght/tools/cgcomp/cgcomp /usr/local/bin/"
    docker commit ps3emu-cgcomp-build ps3emu-cgcomp
    docker rm ps3emu-cgcomp-build
}

if [[ "${1:-}" == "--cgcomp-only" ]]; then
    build_cgcomp
    echo "Built image ps3emu-cgcomp. Re-run CMake to use it."
    exit 0
fi

git clone --quiet https://github.com/ps3dev/ps3toolchain.git "$work/ps3toolchain"
git -C "$work/ps3toolchain" checkout --quiet "$TOOLCHAIN_COMMIT"
docker build --tag ps3emu-psl1ght-base "$work/ps3toolchain"

# Steps: 1-2 binutils, GCC and newlib for the PPU; 4 tool symlinks; 5-6 the same for the SPU.
# Skipped: 3 and 7 (GDB), 8 (PSL1GHT, built below instead) and 9 (extra libraries).
docker rm --force ps3emu-psl1ght-build >/dev/null 2>&1 || true
docker run --name ps3emu-psl1ght-build ps3emu-psl1ght-base ./toolchain.sh 1 2 4 5 6
docker commit ps3emu-psl1ght-build ps3emu-psl1ght-toolchain
docker rm ps3emu-psl1ght-build

# PSL1GHT, as step 8 builds it but at a pinned commit, and with -fno-strict-aliasing: with GCC
# 13's strict aliasing, its float-swapping code compiles wrongly, and rsxSetFragmentProgramParameter
# sends zeros for three-component constants.
docker run --name ps3emu-psl1ght-build ps3emu-psl1ght-toolchain sh -euc "
    git clone --quiet https://github.com/ps3dev/PSL1GHT.git /tmp/psl1ght
    git -C /tmp/psl1ght checkout --quiet $PSL1GHT_COMMIT
    cd /tmp/psl1ght
    find . -name Makefile -exec sed -i 's/^\(CFLAGS[[:space:]]*:=[[:space:]]*-O2\)/\1 -fno-strict-aliasing/' {} +
    make install-ctrl && make && make install
    cd / && rm -rf /tmp/psl1ght"
docker commit ps3emu-psl1ght-build ps3emu-psl1ght
docker rm ps3emu-psl1ght-build
build_cgcomp
echo "Built images ps3emu-psl1ght and ps3emu-cgcomp. Re-run CMake to enable the PSL1GHT examples."
