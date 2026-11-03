#!/bin/sh
# Runs in the ps3emu-cgcomp image: copies PSL1GHT's graphics samples to /out/graphics, and
# compiles every Cg shader in them into /out/shaders, for cgcomp-prebuilt to hand to their
# makefiles.
set -eu
rm -rf /out/graphics /out/shaders
mkdir -p /out/shaders
cp -r /psl1ght/samples/graphics /out/graphics
for shader in $(find /out/graphics -name '*.vcg'); do
    cgcomp -v "$shader" "/out/shaders/$(basename "${shader%.vcg}").vpo"
done
for shader in $(find /out/graphics -name '*.fcg'); do
    cgcomp -f "$shader" "/out/shaders/$(basename "${shader%.fcg}").fpo"
done
