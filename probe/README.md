# pilis3 VDEC probe

独立的验证包（APPID `NP00VDEC0`，不会覆盖 pilis3 本体）。目的只有一个：在真机上把
PS3 固件的 VDEC 摸清楚，给 `source/player/vdec.c` 提供依据。

## 验证内容

- `sysModuleLoad(SYSMODULE_VDEC / VDEC_H264)` 的返回值
- `vdecQueryAttr` 报告的工作区大小与版本
- `vdecOpen` 能用的 `profile_level` 和 SPU 数量（当前先试 4.2 + 4 SPU）
- `vdecDecodeAu` 的 BUSY 重试，`AUDONE` / `PICOUT` / `SEQDONE` 回调时序
- `picture_size`、`codec_specific_addr` 里的宽高、PTS 与 POC
- **YUV420P 平面布局**：红帧底部有一条蓝条，`probe_layout()` 会按可见高度、
  16 对齐、128 对齐三个候选分别算色度平面方差，选最平坦的那个
- 解码耗时（每个 AU 的 `t_decode` 与总耗时）

## 目录

| 路径 | 说明 |
| --- | --- |
| `sample_1080p.h264` | 1080p testsrc 样流（x264，ref=3） |
| `sample_720p.h264` | 720p testsrc 样流 |
| `sample_1080p_red.h264` | 1080p 红帧 + 底部蓝条，专门判平面布局 |
| `source/sample_*.h` | 由上面的码流生成，已入库；正常构建不会重写 |
| `source/main.c` | probe 本体：加载模块 → 解码 → 取图 → 上屏/写日志 |
| `build/`、`out/` | 构建产物，`out/vdec_probe.pkg` 已入库 |

## 构建

```sh
export PSL1GHT=/usr/local/ps3dev
make            # out/vdec_probe.pkg + build/pkg/USRDIR/EBOOT.BIN
```

`pkg` 目标会先跑 `samples`：只有当某个 `.h264` 比对应的 `source/*.h` 新时才重新生成。

换样本时把新的 `.h264` 放进本目录（文件名要对应 `Makefile` 里的 `SAMPLES`），然后：

```sh
make samples
```

生成规则在 `Makefile` 里：`od -An -v -tx1` 每行 16 字节，输出
`static const unsigned char sample_xxx[]` 和 `sample_xxx_size`。

## 运行与判读

装 `out/vdec_probe.pkg` 启动。日志写到 `/dev_hdd0/tmp/pilis3-vdec.log`（同时打到
stdout）；屏幕会依次显示解出来的红帧和 testsrc。

通过判据（与 [docs/decoding.md](../docs/decoding.md) 一致）：

- `pics > 0`
- 日志里出现 `[vdec] close returned`

已记录的真机结果（2026-11-14）：

| 样本 | 结果 |
| --- | --- |
| 红 1080 | `aus=5 pics=4` |
| testsrc 1080 | `aus=15 pics=14` |
| testsrc 720 | `aus=15 pics=14` |

平面布局看这几行，`chosen` 就是固件实际的平面高度（结论是可见高度 1080）：

```
[layout] cand h=1088  ... score=...
[layout] cand h=1080  ... score=...
[layout] chosen height=1080
```

## 和播放器的关系

probe 的结论已经搬到播放器：4 SPU + level 4.2、`sysMemoryAllocate` 1 MB 页工作区、
32 位 OPD 回调、等 AUDONE 时继续取 PICOUT、1080p 色度按可见高度、
`vdecGetPicture(NULL)` 归还坏帧。probe 保留自己的一份解码循环，不链接
`source/player`，所以改播放器时不用同步改它。
