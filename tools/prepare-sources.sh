#!/bin/sh
# Copy WiliMini sources into build/src and apply PS3 port edits, leaving the
# vendored tree untouched.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="${WILIMINI_SRC:-$ROOT/../WiliMini}"
OUT="$ROOT/work/src"
GEN="$ROOT/work/generated"

rm -rf "$OUT"
mkdir -p "$OUT" "$GEN"
cp -R "$SRC/client" "$OUT/client"
cp -R "$SRC/decoder" "$OUT/decoder"
cp -R "$SRC/platform" "$OUT/platform"
cp -R "$SRC/resources" "$OUT/resources"

# PSP-only entry points and modules
rm -f "$OUT/client/main.cpp" "$OUT/client/psp_select.c" "$OUT/client/bootstrap.c"
rm -f "$OUT/client/memory_profile.c"
rm -rf "$OUT/decoder/me"
rm -f "$OUT/decoder/src/vfpu_dsp.c" "$OUT/decoder/src/audio_vfpu.c" \
      "$OUT/decoder/src/me_decode.c" "$OUT/decoder/src/me_pipeline.c"

# The update swap relies on the PSP loader; PS3 uses its own updater later.

cat > "$OUT/client/storage_paths.h" <<'EOF'
#ifndef WILIMINI_STORAGE_PATHS_H
#define WILIMINI_STORAGE_PATHS_H
#include <stdio.h>
#include <string.h>

#define WILI_STORAGE_PS3 "/dev_hdd0/game/NP00WILI1/USRDIR"
#define WILI_STORAGE_MS0 WILI_STORAGE_PS3
#define WILI_STORAGE_EF0 WILI_STORAGE_PS3
#define WILI_STORAGE_HOST0 WILI_STORAGE_PS3

static const char *const wili_storage_roots[] = {
    WILI_STORAGE_PS3, WILI_STORAGE_PS3, WILI_STORAGE_PS3
};

static inline const char *wili_select_storage(int (*file_exists)(const char *), int require_me) {
    (void)file_exists; (void)require_me;
    return WILI_STORAGE_PS3;
}

static inline const char *wili_storage_from_client(const char *path) {
    (void)path;
    return WILI_STORAGE_PS3;
}
#endif
EOF

# The PS3 portlibs curl predates the *_STR protocol options.
sed -i 's/CURLOPT_PROTOCOLS_STR/CURLOPT_PROTOCOLS/g; s/CURLOPT_REDIR_PROTOCOLS_STR/CURLOPT_REDIR_PROTOCOLS/g'     "$OUT/client/services/update_curl.cpp" "$OUT/client/bilibili.c" "$OUT/client/live_stream.c"
sed -i 's/CURLPROTO_HTTPS/CURLPROTO_ALL/g; s/"http,https"/CURLPROTO_ALL/g; s/"https"/CURLPROTO_HTTPS/g'     "$OUT/client/services/update_curl.cpp" "$OUT/client/bilibili.c" "$OUT/client/live_stream.c"
sed -i 's/CURLOPT_REDIR_PROTOCOLS_STR, "https"/CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS/'     "$OUT/client/services/update_curl.cpp" "$OUT/client/bilibili.c" "$OUT/client/live_stream.c"
# Older portlibs curl (pre-7.32) has no xferinfo callbacks and no *_T info
# codes; the deprecated progress pair takes the same variadic arguments.
sed -i 's/CURLOPT_XFERINFOFUNCTION/CURLOPT_PROGRESSFUNCTION/g; s/CURLOPT_XFERINFODATA/CURLOPT_PROGRESSDATA/g; s/CURLINFO_CONTENT_LENGTH_DOWNLOAD_T/CURLINFO_CONTENT_LENGTH_DOWNLOAD/g'     "$OUT/client/services/update_curl.cpp" "$OUT/client/bilibili.c" "$OUT/client/live_stream.c"


python3 "$(dirname "$0")/patch_sources.py" "$OUT"
python3 "$SRC/scripts/generate-localization.py" \
    --zh-hans "$SRC/resources/i18n/zh-Hans.json" \
    --en-us "$SRC/resources/i18n/en-US.json" \
    --output-dir "$GEN"
cat > "$GEN/build_info.hpp" <<'EOF'
#pragma once
namespace pspapp {
inline constexpr char buildVersion[] = "ps3-0.1";
}
EOF
cat > "$GEN/build_diagnostics.h" <<'EOF'
#ifndef WILI_BUILD_DIAGNOSTICS_H
#define WILI_BUILD_DIAGNOSTICS_H
#define WILI_DIAGNOSTIC_BUILD "ps3-0.1"
#define WILI_DIAGNOSTIC_COMPILED_UTC __DATE__ " " __TIME__
#define WILI_DIAGNOSTIC_BUILD_TYPE "Release"
#define WILI_DIAGNOSTIC_ME_BACKENDS "custom-core"
#endif
EOF
echo "prepared $OUT"

# No kernel bridge on PS3; report the extended-memory profile.
cat > "$OUT/platform/src/psp_device.c" <<'EOF'
#include <pspkernel.h>
#include "psp_device.h"

PspDeviceInfo psp_device_query(void)
{
    PspDeviceInfo device = { 2, 192u * 1024u * 1024u };
    return device;
}
EOF
