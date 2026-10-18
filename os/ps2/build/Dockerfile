# ---------------------------------------------------------------------------
# Derived build image for the Fallout 2 PS2 port.
#
# The upstream ps2dev/ps2dev image intentionally ships ONLY the cross
# toolchain (gcc/binutils, ps2sdk, gsKit, ps2-packer) on Alpine, without any
# host-side build tools. This image adds cmake/make/ninja once, so that every
# build command is a plain `docker run` with no package installation.
#
# Build (once):
#   docker build -t fallout-ce-ps2-build os/ps2/build
#
# Use (from the repository root):
#   docker run --rm -v "$(pwd):/src" -w /src fallout-ce-ps2-build \
#     sh -c "cmake -B build_ps2 -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-ps2.cmake \
#            -DCMAKE_BUILD_TYPE=Release && cmake --build build_ps2 -j\$(nproc)"
# ---------------------------------------------------------------------------
FROM ps2dev/ps2dev:latest

# git + ca-certificates are needed by CMake FetchContent (fpattern). zlib comes
# from the ps2sdk ports collection, which ships with the toolchain image.
RUN apk add --no-cache cmake make ninja git ca-certificates
