# pilis3

PlayStation 3 上的哔哩哔哩客户端。UI 与构建沿用 pkgi-ps3 的 PSL1GHT/ya2d 路线，
网络与 B 站 API 复用 WiliMini 的 curl / wbi / 二维码登录思路，视频用
VDEC 硬解（480/720/1080，设置里可切回 PPU 软解）；480p 和 720p 已在真机上
连续播放通过，1080p 解码通路可用、待复验，评估与复验过程见
[docs/decoding.md](docs/decoding.md)。

## 当前进度

| 模块 | 状态 |
| --- | --- |
| PSL1GHT + ya2d + tiny3d 工程 | 可编译，out/pilis3.pkg 可打包 |
| 推荐 / 热门列表 | 已接真实 API，含封面、UP、播放/弹幕数、时长 |
| 分区 | 20 个父分区网格，接 newlist 接口 |
| 动态 | 关注动态（需登录） |
| 我的 | 二维码登录、用户资料、历史/收藏/稍后再看入口 |
| 详情页 | 封面、标题、UP、统计、简介、取流 |
| 播放器 | DASH 双流 + FFmpeg 解封装 + VDEC 硬解（480/720/1080，设置可切软解）+ AAC + OSD |
| 播放控制 | 快进/快退 10 秒（DASH sidx 定位，原地 flush 不重开播放器）、L1/R1 切 720P/1080P、暂停/继续 |
| 弹幕 | 滚动/顶部/底部，颜色与开关；XML 拉取与 deflate 解压 |
| 收藏 / 稍后再看 / 历史 | 列表 + 详情页加入默认收藏夹和稍后再看 |
| 搜索 | 系统 OSK 输入 + wbi 搜索接口 + 结果列表 |

> VDEC 硬解已接入播放器：480p（852x480）和 720p 真机连续播放通过，1080p 通路
> 可用、待复验。设置里可切回 PPU 软解；复验判据与踩坑记录见
> [docs/decoding.md](docs/decoding.md)。

## 构建

本仓库的构建镜像由 docker/Dockerfile.ps3dev（本地）或 .ide/Dockerfile（CNB）
提供：PS3 SDK（bucanero/ps3toolchain 2020-08-31）、tiny3D、ya2d、裁剪版 FFmpeg。

本地：

    docker build --platform linux/amd64 -f docker/Dockerfile.ps3dev -t ps3dev-pilis3 .
    docker run --rm -v "$PWD:/ws" -w /ws ps3dev-pilis3 make -j8 pkg

CNB：直接用仓库里的 .cnb.yml 打开云原生开发环境，然后 make -j16 pkg。

产物：

- out/pilis3.pkg / out/pilis3.gnpdrm.pkg
- probe/out/vdec_probe.pkg

## 安装与测试

1. 先装 probe/out/vdec_probe.pkg（APPID NP00VDEC0，不会覆盖主程序）。
2. 启动「pilis3 vdec probe」，等待 10–30 秒。屏幕应出现彩条测试图；
   失败时是红色背景。日志写到 /dev_hdd0/tmp/pilis3-vdec.log。
3. 再装 out/pilis3.pkg（APPID NP00WILI1，标题 pilis3，复用 WiliMini PS3
   的安装目录和 pkgfiles）。
4. 进入「我的」按 × 显示二维码，用手机 B 站扫码登录；登录后才能看 1080P、
   动态、收藏和历史。

日志：

- 主程序：/dev_hdd0/tmp/pilis3.log
- VDEC 探针：/dev_hdd0/tmp/pilis3-vdec.log

## 目录

| 路径 | 内容 |
| --- | --- |
| source/platform/ | ya2d 画布、输入、系统、日志、FreeType 字形缓存 |
| source/net/ | curl 封装、cookie、wbi 签名、B 站 API、后台任务线程 |
| source/ui/ | 封面缓存、二维码、主题 |
| source/player/ | 环形流缓冲、FFmpeg 解封装、PPU 软解、PS3 音频、播放器 OSD |
| probe/ | 独立的 VDEC 真机验证包（见 [probe/README.md](probe/README.md)） |
| pkgfiles/ | 复用 WiliMini PS3 的图标、证书、字体资源 |
| tools/、patches/ | FFmpeg 交叉编译与 tiny3D/FFmpeg 补丁 |
| docs/ | 设计与取舍记录（如 [解码路径](docs/decoding.md)） |
| status.md | 当前进展：真机已验证项、已修的坑、待验证清单 |

## 已知风险

- 软解只吃 PPU 一个核（deblock/IDCT 没有 ppc 汇编版），720P 约 25–40 fps，
  1080P 不够流畅。
- VDEC 硬解 480p/720p 已真机连续播放通过；1080p 通路可用但未复验。1080p 下
  图片环 16 × 3.1 MB 加 52 MB 工作区，内存较紧，打开失败会回退软解。
  probe/out/vdec_probe.pkg 保留作为复验工具。
- 快进/快退依赖 DASH SegmentBase 的 sidx；没有索引的视频会提示不支持跳转。
  sidx 在播放开始时后台预取，第一次 seek 不用再等网络。
- 画质切换通过重新取流 + 重新打开播放器实现，切换瞬间会短暂缓冲。
- 弹幕发送还未接入。
- 二维码登录依赖 passport 接口；接口变动时会退回「二维码不可用」。
