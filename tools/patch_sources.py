#!/usr/bin/env python3
"""Port edits applied to the copied WiliMini sources (build/src)."""
import os
import sys

out = sys.argv[1]


def edit(path, old, new):
    with open(path) as f:
        s = f.read()
    if new in s:
        return
    if old not in s:
        raise SystemExit("patch anchor missing in " + path)
    with open(path, "w") as f:
        f.write(s.replace(old, new, 1))


player = os.path.join(out, "decoder/src/player.c")
edit(player,
     "static int draw_frame(const AVFrame *frame)\n{",
     """#ifdef WILI_PS3_RENDER
int wili_ps3_draw_frame(const AVFrame *frame, int (*present)(const void *, PspVideoPixelFormat));
#endif
static int draw_frame(const AVFrame *frame)
{
#ifdef WILI_PS3_RENDER
    return wili_ps3_draw_frame(frame, present_video);
#endif""")
edit(player,
     "static int seek_in_session(AVFormatContext *format",
     """#ifdef WILI_PS3_RENDER
int wili_ps3_present_video(const void *pixels, PspVideoPixelFormat fmt) { return present_video(pixels, fmt); }
#endif
static int seek_in_session(AVFormatContext *format""")

with open(os.path.join(out, "platform/src/psp_device.c"), "w") as f:
    f.write("""#include <pspkernel.h>
#include "psp_device.h"

PspDeviceInfo psp_device_query(void)
{
    PspDeviceInfo device = { 2, 192u * 1024u * 1024u };
    return device;
}
""")
import os
import sys

out = sys.argv[1]

def edit(path, old, new):
    s = open(path).read()
    if new in s:
        return
    if old not in s:
        raise SystemExit('patch anchor missing in ' + path)
    s = s.replace(old, new, 1)
    open(path, 'w').write(s)

ui = os.path.join(out, 'client/ui.cpp')
edit(ui, '#include <pspgu.h>', '#include <pspgu.h>\n#include <wili_ps3_kb.h>')

keyboard_block = '''        previous = pad.Buttons;
#ifdef WILI_PS3_KEYBOARD
        /* USB keyboard: typed characters and editing keys go straight into the IME. */
        for (int key = ps3_kb_take_event(); key != 0; key = ps3_kb_take_event()) {
            if (key > 0) {
                const char typed = static_cast<char>(key);
                const unsigned char utyped = static_cast<unsigned char>(typed);
                if (ime.composing() && typed >= '1' && typed <= '9') {
                    const size_t index = pageStarts.back() + static_cast<size_t>(typed - '1');
                    if (index < pageEnd(pageStarts.back())) pick(index);
                } else if (std::isalpha(utyped) && chinese && !symbols) {
                    if (ime.addLetter(static_cast<char>(std::tolower(utyped)))) resetPaging();
                } else if (std::isalpha(utyped)) {
                    flushRaw();
                    append(std::string(1, typed));
                } else {
                    flushBest();
                    append(std::string(1, typed));
                }
            } else {
                switch (-key) {
                case PS3_KB_BACKSPACE:
                case PS3_KB_DELETE:
                    if (inCandidates) inCandidates = false; else backspace(false);
                    break;
                case PS3_KB_ENTER:
                    if (inCandidates) pick(candidateCursor);
                    else if (ime.composing()) {
                        if (!ime.candidates().empty()) pick(pageStarts.back()); else flushRaw();
                    } else { flushRaw(); done = accepted = true; }
                    break;
                case PS3_KB_ESC:
                    done = true;
                    accepted = false;
                    break;
                case PS3_KB_LEFT: move(-1, 0); break;
                case PS3_KB_RIGHT: move(1, 0); break;
                case PS3_KB_UP: move(0, -1); break;
                case PS3_KB_DOWN: move(0, 1); break;
                case PS3_KB_SPACE:
                case PS3_KB_TAB: pressKey({ImeKeySpec::Space, "", 0}); break;
                default: break;
                }
            }
        }
        if (done) break;
#endif
'''
edit(ui, '        previous = pad.Buttons;\n', keyboard_block)
print('patched ui.cpp keyboard')

import os, sys
out = sys.argv[1]
p = os.path.join(out, 'client/app.hpp')
s = open(p).read()
if 'playbackQuality_ = 64' not in s:
    if 'playbackQuality_ = 16' not in s:
        raise SystemExit('quality anchor missing')
    s = s.replace('int playbackQuality_ = 16;', 'int playbackQuality_ = 64; /* PS3: prefer 720p AVC when available */')
    open(p, 'w').write(s)
print('quality patched')

# ---------------------------------------------------------------------------
# PS3 curl defaults + first-request diagnostics.
# Mirrors ps3-pkgi/pkgi's pkgi_curl_init() (TLS 1.2, console UA, netCtl proxy).
# Call ps3_curl_setup() AFTER the file's own setopt block: bilibili.c sets
# CURLOPT_USERAGENT/CONNECTTIMEOUT late and would otherwise overwrite them.
# These edits live here (not as sed in prepare-sources.sh) so the committed
# work/src tree and a fresh regeneration stay identical.
# ---------------------------------------------------------------------------
import os, sys
out = sys.argv[1]

def ps3_edit(rel, old, new):
    p = os.path.join(out, rel)
    s = open(p).read()
    if new in s:
        return
    if old not in s:
        raise SystemExit('ps3 curl anchor missing in ' + rel)
    open(p, 'w').write(s.replace(old, new, 1))

bili = 'client/bilibili.c'
ps3_edit(bili, '#include "bilibili.h"', '#include "bilibili.h"\n#include <wili_ps3_curl.h>')
ps3_edit(bili,
    '    curl_easy_setopt(c,CURLOPT_TIMEOUT,(sink->file || sink->stream)?0L:playback_address_request(url)?45L:25L);\n'
    '    sink->handle=c;',
    '    curl_easy_setopt(c,CURLOPT_TIMEOUT,(sink->file || sink->stream)?0L:playback_address_request(url)?45L:25L);\n'
    '    ps3_curl_setup(c);\n'
    '    sink->handle=c;')
ps3_edit(bili,
    '    long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);',
    '    long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);\n'
    '    ps3_curl_trace("api", (int)r, status, detail);')
ps3_edit(bili,
    '    *headers = range_headers();\n'
    '    if (*headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);\n}',
    '    *headers = range_headers();\n'
    '    if (*headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);\n'
    '    ps3_curl_setup(curl);\n}')

live = 'client/live_stream.c'
ps3_edit(live, '#include "bilibili.h"', '#include "bilibili.h"\n#include <wili_ps3_curl.h>')
ps3_edit(live,
    '  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);\n'
    '  CURLcode result = curl_easy_perform(curl);',
    '  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);\n'
    '  ps3_curl_setup(curl);\n'
    '  CURLcode result = curl_easy_perform(curl);')
ps3_edit(live,
    '  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);',
    '  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);\n'
    '  ps3_curl_trace("live", (int)result, status, curl_easy_strerror(result));')

upd = 'client/services/update_curl.cpp'
ps3_edit(upd, '#include <curl/curl.h>', '#include <curl/curl.h>\n#include <wili_ps3_curl.h>')
ps3_edit(upd,
    '    error_.clear();\n    CURLcode rc = curl_easy_perform(h);',
    '    ps3_curl_setup(h);\n'
    '    error_.clear();\n    CURLcode rc = curl_easy_perform(h);')
ps3_edit(upd,
    '    CURLcode rc = curl_easy_perform(h);',
    '    CURLcode rc = curl_easy_perform(h);\n'
    '    ps3_curl_trace("update", (int)rc, (long)c.status, err);')
print('ps3 curl options patched')

# ---------------------------------------------------------------------------
# Native-resolution TTF text (WILI_PS3_TTF). Text draws are recorded instead of
# rasterised into the 480x272 software framebuffer; ps3_font_flush() replays
# them as tiny3D quads in screen pixels. Guarded so WILI_PS3_TTF=0 leaves the
# original bitmap-font path byte-for-byte unchanged.
# ---------------------------------------------------------------------------
ui_rel = 'client/ui.cpp'
ps3_edit(ui_rel, '#include "bitmap_font.hpp"', '#include "bitmap_font.hpp"\n#include <wili_ps3_font.h>')

ps3_edit(ui_rel,
    'void drawText(int x, int y, float scale, uint32_t shade, std::string_view text, int maxWidth,\n'
    '              bool centered = false, int clipTop = 0, int clipBottom = kHeight) {\n'
    '    const int size = fontSize(scale);',
    'void drawText(int x, int y, float scale, uint32_t shade, std::string_view text, int maxWidth,\n'
    '              bool centered = false, int clipTop = 0, int clipBottom = kHeight) {\n'
    '#if WILI_PS3_TTF\n'
    '    const int tsize = fontSize(scale);\n'
    '    int tx = x;\n'
    '    if (centered) tx += std::max(0, (maxWidth - bitmapMeasure(text, tsize)) / 2);\n'
    '    const std::string owned(text);\n'
    '    ps3_font_text(tx, y, tsize, shade | 0xff000000u, owned.c_str(), maxWidth,\n'
    '                  std::max(0, clipTop), std::min(kHeight, clipBottom));\n'
    '    return;\n'
    '#endif\n'
    '    const int size = fontSize(scale);')

ps3_edit(ui_rel,
    'void drawTitleText(int x, int y, uint32_t shade, std::string_view text, int maxWidth) {\n'
    '    const int width = titleFontReady() ? titleMeasure(text) : -1;',
    'void drawTitleText(int x, int y, uint32_t shade, std::string_view text, int maxWidth) {\n'
    '#if WILI_PS3_TTF\n'
    '    const std::string owned(text);\n'
    '    ps3_font_text(x, y, 32, shade | 0xff000000u, owned.c_str(), maxWidth, 0, kHeight);\n'
    '    return;\n'
    '#endif\n'
    '    const int width = titleFontReady() ? titleMeasure(text) : -1;')

ps3_edit(ui_rel,
    'void drawBigText(int x, int y, uint32_t shade, std::string_view text, int maxWidth) {\n'
    '    const int right = std::min(kWidth, x + maxWidth);',
    'void drawBigText(int x, int y, uint32_t shade, std::string_view text, int maxWidth) {\n'
    '#if WILI_PS3_TTF\n'
    '    const std::string owned(text);\n'
    '    ps3_font_text(x, y, 32, shade | 0xff000000u, owned.c_str(), maxWidth, 0, kHeight);\n'
    '    return;\n'
    '#endif\n'
    '    const int right = std::min(kWidth, x + maxWidth);')
print('ps3 ttf text hooks patched')