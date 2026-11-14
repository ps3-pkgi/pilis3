# 为什么用软解：PS3 VDEC 评估记录

> 结论（2026-11-14 更新）：**VDEC 硬解可用**。旧 probe 一帧不出是初始化/时序
> 问题，不是固件限制；probe 已在真机上解出 H.264 1080p/720p 并正常 close。
> 播放器当前仍走 libavcodec 软解，接入 VDEC 是下一步。
>
> 2026-11-13 的原始评估保留在下面作为排错记录；复验结论见第 6 节。

## 1. PSL1GHT 到底提供了什么

| 项 | 实际情况 |
| --- | --- |
| 头文件 | 只有 `codec/vdec.h`，9 个函数：`vdecQueryAttr` / `vdecOpen` / `vdecStartSequence` / `vdecDecodeAu` / `vdecGetPicItem` / `vdecGetPicture` / `vdecEndSequence` / `vdecClose` |
| 库 | `libvdec.a` 是 **scefstub**，不是实现（`ppu-nm` 可见 `header` / `scefstub` / `name="libvdec"` / `vdecOpen_fnid` / `__vdecOpen`） |
| 官方 `cellVdec` | **整个 SDK 搜不到**，连 `cell/codec/vdec.h` 都没有 |
| sysmodule | `0x05` VDEC 主库、`0x24` MPEG2 核、`0x25` AVC 核（`libavcdec.sprx`）、`0x3c` DivX |

也就是说，PSL1GHT 给的是**系统播放器内部使用的那套 vdec 接口**，运行时按 fnid
从固件的 `libvdec.sprx` 里解析符号。它没有把解码器编进 app，也没有暴露索尼
SDK 里那套 `cellVdec*`。

## 2. 真机验证

### 2.1 先排除自己的问题

probe 每次只改一个变量，逐条排除：

| 版本 | 改动 | 结果 |
| --- | --- | --- |
| v1 | 原始 | `aus=1 pics=0`，但 AU 切分有 bug：每个 AU 吞掉下一帧的第一个 slice（1162/149，正确是 1092/70） |
| v2 | 修 AU 边界（`next_au` 截在起始 NAL 上） | `aus=5`，1092/70/74/75/75，仍然是 `pics=0` |
| v3 | AU 拷进 128 字节对齐缓冲（原来直接喂 `.rodata`，AU1 只有 2 字节对齐） | 无变化 |
| v4 | `num_spus` 改成 2、3 优先 | 无变化 |
| v5 | `vdecStartSequence` 之后先等 100 ms | 无变化 |
| v6 | 收尾分段打点（endSequence rc / close entered / close returned） | 定位到 `vdecClose` 永久阻塞 |

### 2.2 最终日志（spus=2）

```
[vdec] queryAttr(level=42) rc=00000000 mem_size=54157309 cmd_depth=4 ver=...
[vdec] open(spus=2) rc=00000000 handle=1076887680
[red1080] au 0 feed size=1092 decode rc=00000000
[red1080] au 1 feed size=70   decode rc=00000000
[red1080] au 2 feed size=74   decode rc=00000000
[red1080] au 3 feed size=75   decode rc=00000000
[red1080] au 4 feed size=75   decode rc=00000000
[red1080] done: aus=5 pics=0 decode_ms=2522 cb(au=0 pic=0 seq=0 err=0)
[vdec] endSequence rc=00000000
[vdec] close handle=1076887680 seqdone=0
（之后没有 "close returned"，也没有 "memory released"）
```

### 2.3 怎么解读

- **所有调用返回 0**，`queryAttr` 还能按 level 报出 54 MB 工作区和版本号，
  说明结构体/ABI 大致是对的，库确实在读我们的参数。
- **AUDONE 一次都没有**（`cb(au=0 ...)`）。正常解码器即使解不出来，消费一个
  AU 也应该发 AUDONE。没有它说明工作线程根本没动。
- **`vdecEndSequence` 返回 0 但 SEQDONE 永远不来**，随后 `vdecClose` 阻塞。
  这和"线程没跑起来、在等一个永远不会发生的握手"是同一个现象。
- 1 个 SPE、2 个 SPE，对齐不对齐，结果**逐字节一致** —— 不是参数问题。

代价也要记一笔：1080p 的 `mem_size` 是 54,157,309 字节（`0x033A5FFD`），
每次 `open` 都要独占这么多连续内存，在 256 MB 的机器上做参数扫射很贵。

## 3. 为什么当时不再投入

> 2026-11-14 更新：以下判断已被推翻，VDEC 已跑通，保留作为排错记录。

剩下能做的只有两件事，都属于固件/逆向层面，收益不确定：

1. dump `libvdec.sprx` 的导出表，确认它是否同时导出 `cellVdec*`。如果有，
   可以按 fnid 自己写 import stub 走官方 ABI（PS3 上 `cellVdec` 的实现就在
   libvdec 里）。没有就没戏。
2. 查 HEN/CFW 是否对 homebrew 限制了 vdec/SPU 权限，对比 webMAN 这类能放视频
   的 app 的加载方式。

在此之前，继续调 probe 的参数（level、SPE 数、对齐）已经没有信息量。

## 4. 现在的实现

```
avformat 解封装 (DASH fMP4)
  → avcodec H.264 软解（PPU，Altivec）
  → frame_to_slot() 打包成紧凑 YUV420P
  → RSX YUV 纹理 + 全屏 quad
  → tiny3d_Flip()
```

- 不再需要 `h264_mp4toannexb`：AVCC 包直接喂 `avcodec_send_packet`。
- 默认 `qn = 64`（720P）；L1/R1 仍可切 1080P，但纯 C 之外的优化有限，
  1080P 只适合验证，不适合观看。
- FFmpeg 用 Altivec 编（`--disable-asm` 已从 `tools/build-ffmpeg.sh` 去掉，
  配置结果 `HAVE_ALTIVEC=1 / HAVE_VSX=0`，Cell 的 PPU 只有 VMX 没有 VSX）。
- `copy_plane()` 在 `linesize == width` 时整平面一次 memcpy，720P 每帧只有
  3 次拷贝。

预期性能：720P 大约 25–40 fps（qpel/chroma 有 Altivec 版，deblock/IDCT 仍是
纯 C）。如果不够，下一步是降到 480P 或给 deblock 写 VMX。

## 5. 什么时候可以回头

> 2026-11-14：已经回头并跑通，判据达成；见第 6 节。

保留判断依据，别凭印象：

- `probe/out/vdec_probe.pkg` 还在，标准很明确：**`pics > 0` 且日志里出现
  `[vdec] close returned`**。两者缺一都不算成功。
- 如果确认 `libvdec.sprx` 导出 `cellVdec*`，值得再试一轮官方 ABI。
- `source/player/vdec.c` 仍然留在工程里（当前无人调用），重试时可以直接接回
  播放器的 `video_thread`。

## 6. 2026-11-14 复验：VDEC 可以硬解

旧 probe 所有调用都返回 0 但不出帧，原因不是固件，而是初始化：

| 项 | 旧 probe | 正确做法 |
| --- | --- | --- |
| 回调 closure | `fn = (u32)&vdec_callback`，截断了 64 位 OPD | 32 位 OPD：`{u32 func, u32 rtoc}`，从 64 位函数描述符取 `opd[0]/opd[1]` |
| 工作区内存 | `memalign(128)` | `sysMemoryAllocate(round_up(mem_size, 1MB), SYS_MEMORY_PAGE_SIZE_1M)`，`sysMemoryFree` 释放 |
| H.264 SPU | 从 2 开始，open 成功就停 | 先 4 个（cellVdec 的流水线配置），失败再 3/2/1 |
| AU 生命周期 | `vdecDecodeAu` 返回后立刻复用缓冲 | 等 `AUDONE`（5s 超时）再复用 |
| 收尾 | `vdecEndSequence` 等 1s 就 close | 等到 `SEQDONE` 再 `vdecClose` |
| H.264 信息 | PSL1GHT 的 `s8 pic_order_count[2]` 偏移错误 | 按固件布局修正，POC/宽高才对 |
| YUV 平面 | 按 1088 读 | 按 **1080** 读；`picture_size` 多出来的是尾部 padding |
| 显示 | tiny3d YUV8 + 单纹理 | 显式 BT.601 YUV→RGB + 双缓冲 RGB 纹理；否则切换会撕裂 |

真机结果（build 2026-11-14 14:11，probe）：

- 红 1080：`aus=5 pics=4`
- testsrc 1080：`aus=15 pics=14`
- testsrc 720：`aus=15 pics=14`
- 三个 session 都出现 `[vdec] close returned`

判据仍是：`pics > 0` 且出现 `[vdec] close returned`。

播放器已经接入这套解码（480p/720p/1080p），设置里可以切回软解；硬件初始化
失败会自动回退软解。2026-11-14 真机验证：480p（852x480）连续播放通过。关键修复
是**等 AUDONE 时继续取 PICOUT**，否则图片队列满会把解码器反压死锁；卡住时保留
`VdecCtx`（固件仍持有回调上下文）并在本次运行禁用 VDEC。720p/1080p 待验证。

## 7. 相关文件

| 文件 | 作用 |
| --- | --- |
| `probe/source/main.c` | VDEC 验证包：AU 切分、平面布局探测、阶段日志 |
| `source/player/player.c` | 当前解码路径（avcodec + frame_to_slot） |
| `source/player/vdec.c` | 旧的 VDEC 封装，保留未用 |
| `tools/build-ffmpeg.sh` | FFmpeg 裁剪 + Altivec 构建 |
