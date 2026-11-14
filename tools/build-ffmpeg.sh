#!/bin/sh
# Cross-build the trimmed FFmpeg used by WiliMini's decoder for PPU/PSL1GHT.
set -e
REV="${FFMPEG_REV:-b1a4534186ca51b0457579fc05a5739eb2cc45cd}"
PREFIX="${FFMPEG_PREFIX:-/opt/ffmpeg-ps3}"
SRC="${FFMPEG_SRC:-/tmp/ffmpeg-src}"
BUILD="${FFMPEG_BUILD:-/tmp/ffmpeg-build}"

if [ ! -d "$SRC" ]; then
  mkdir -p "$SRC"
  curl -sL "https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/$REV" | tar xz -C "$SRC" --strip-components=1
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PATCHES="${FFMPEG_PATCHES:-$ROOT/patches}"
if [ -d "$PATCHES/ffmpeg" ] && [ ! -f "$SRC/.wili_patched" ]; then
  for p in "$PATCHES/ffmpeg/0001-psp-sampled-h264-profile.patch"            "$PATCHES/ffmpeg-psp-tx-tables.patch"            "$PATCHES/ffmpeg/0002-psp-cabac-movn.patch"            "$PATCHES/ffmpeg/0003-psp-approximate-reconstruction.patch"; do
    patch -d "$SRC" -p1 --forward < "$p" || true
  done
  touch "$SRC/.wili_patched"
fi

mkdir -p "$BUILD" && cd "$BUILD"

"$SRC/configure" \
  --prefix="$PREFIX" \
  --enable-cross-compile --target-os=none --arch=ppc64 --cpu=cell \
  --cross-prefix=ppu- \
  --disable-autodetect --disable-doc --disable-debug \
  --disable-programs --disable-network \
  --disable-avdevice --disable-avfilter --disable-postproc \
  --disable-swscale --disable-swresample --disable-pthreads \
  --disable-everything \
  --enable-decoder=h264,aac --enable-parser=h264 \
  --enable-demuxer=mov,flv --enable-protocol=file \
  --enable-bsf=h264_mp4toannexb \
  --extra-cflags="-O2 -mcpu=cell -fno-pic -DPSP_CABAC_MOVN=0" \
  --extra-ldflags="-mcpu=cell" \
  --pkg-config=/bin/false

make -j"$(nproc)"
make install
