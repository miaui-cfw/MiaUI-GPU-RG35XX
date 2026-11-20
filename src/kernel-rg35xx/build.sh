#!/bin/bash
# Builds the RG35XX kernel from source: rg35xx-cfw/s500_kernel @81cc77b592 + patches/ + config/rg35xx-test.config,
# with the Linaro GCC 4.9.4 toolchain (the compiler of the console's kernel), in the koriki-soh Docker image.
#
#   build.sh            -> build/out/arch/arm/boot/zImage, build/out/uImage, build/out modules
#
# Env: BUILD_DIR (default: <this dir>/build), SRC_TARBALL (a local copy of the source archive, skips the download).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
COMMIT=81cc77b59250341b9abe90ed4188cde7b7aa1eeb
BUILD_DIR="${BUILD_DIR:-$HERE/build}"
SRC="$BUILD_DIR/s500_kernel-$COMMIT"
TC=gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf

mkdir -p "$BUILD_DIR"

# Pristine source, as a git tree so the patches apply with git am
if [ ! -d "$SRC/.git" ]; then
    TARBALL="${SRC_TARBALL:-$BUILD_DIR/s500_kernel-$COMMIT.tar.gz}"
    [ -f "$TARBALL" ] || curl -L -o "$TARBALL" "https://github.com/rg35xx-cfw/s500_kernel/archive/$COMMIT.tar.gz"
    tar xzf "$TARBALL" -C "$BUILD_DIR"
    git -C "$SRC" init -q
    git -C "$SRC" add -A
    git -C "$SRC" -c user.name=kernel-rg35xx -c user.email=build@localhost commit -q -m "s500_kernel @${COMMIT:0:10} (pristine)"
    # The vendor files mix CRLF and LF line endings: --keep-cr keeps them byte-exact
    git -C "$SRC" -c user.name=kernel-rg35xx -c user.email=build@localhost am -q --keep-cr "$HERE"/patches/*.patch
fi

# Toolchain from the repo root tarball
[ -d "$BUILD_DIR/$TC" ] || tar xJf "$REPO/$TC.tar.xz" -C "$BUILD_DIR"

mkdir -p "$BUILD_DIR/out"
cp "$HERE/config/rg35xx-test.config" "$BUILD_DIR/out/.config"

docker run --rm -v "$BUILD_DIR:/kb" -w "/kb/s500_kernel-$COMMIT" \
    -e KBUILD_BUILD_USER=koriki -e KBUILD_BUILD_HOST=rg35xx-test koriki-soh bash -c "
    X='ARCH=arm CROSS_COMPILE=/kb/$TC/bin/arm-linux-gnueabihf- O=/kb/out HOSTCFLAGS=-fcommon'
    make \$X olddefconfig && make \$X -j\$(nproc) zImage modules && chmod -R a+rwX /kb/out"

python3 "$HERE/tools/mkuimage.py" "$BUILD_DIR/out/arch/arm/boot/zImage" "$BUILD_DIR/out/uImage"
echo "uImage: $BUILD_DIR/out/uImage"
