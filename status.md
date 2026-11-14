# pilis3 进展状态

> 更新于 2026-11-14。最近一次**上机验证**的是 2026-11-14 的包（curl 阶段日志）；
> 首页列表、PPU 软解播放和 **VDEC 硬解 probe** 都已跑通。
>
> 一句话：RSX 通路、HTTPS、首页列表、软解播放和 VDEC 硬解都已在真机上跑通；
> 播放器接入 VDEC 是下一步。

## 1. 已在真机验证

| 环节 | 状态 | 证据 |
| --- | --- | --- |
| 工具链 / SDK | 正常 | bucanero ps3toolchain 2020-08-31，`gcmInitBody ret=0 ctx=102be4cc` |
| RSX 初始化 | 正常 | `t3d 06c label=5a5a5a5a want=5a5a5a5a after 1 ms` |
| 翻页 / 双缓冲 | 正常 | `t3d 09a flipstatus=0 after 0 ms`、`t3d 10d put=1454 get=1454` |
| 闪屏 | **已修** | 恢复 `gcmSetWaitFlip()` 后上机确认不再闪 |
| 网络初始化 | 正常 | `[http] net init netInitialize=0 sysModuleLoad=0 netCtlInit=0`、`net state=3`（IPObtained） |
| 明文 HTTP | 正常 | probe 拿到 307 并跟随重定向，`Connection #0 left intact` |
| HTTPS / TLS | **已通** | `Handshake complete, cipher is TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256` → `[http] #N -> 200 (259 bytes)` |
| VDEC 硬解 | **可用（probe）** | 2026-11-14 真机：H.264 1080p/720p 均出帧，`pics=4/14/14`、`[vdec] close returned`；配置见 [docs/decoding.md](docs/decoding.md) |
| 首页列表 | **正常** | 2026-11-14 上机：推荐列表正常返回并显示 |
| PPU 软解 | **可播放** | 2026-11-14 上机：libavcodec 软解出画面；分辨率、帧率、音频/弹幕待补 |

## 2. 这一轮修掉的坑

### 2.1 闪屏：tiny3D 补丁删掉了 `gcmSetWaitFlip()`

补丁为了绕开"flip 不完成导致 RSX 停住"，把 `flip()` 里的 `gcmSetWaitFlip()` 删了、
`waitFlip()` 从 2 s 缩到 33 ms。FIFO 的根因（`MakeCmdSpace(0x4000)` 超过 28 KB 的
FIFO）修好之后，这个绕法就只剩副作用：下一帧会画进正在扫描输出的缓冲 → 闪。
恢复上游语义后闪屏消失，`flipstatus` 仍在 10 ms 内归零，说明 RSX 没被停死。

### 2.2 `sysSemCreate(NULL)` 直接失败

`[api] sem create failed 8001000d`（EACCES）。LV2 不接受 NULL 属性，必须填
`attr_protocol` / `attr_pshared` / `name`。`source/platform/sys.c` 的
`sys_sem_create()` 统一处理，api / player（两个）/ stream / cover 五处全部换掉。

### 2.3 PolarSSL 没有可用熵源

| 尝试 | 结果 |
| --- | --- |
| 默认（`/dev/urandom`） | `ctr_drbg_init returned (-0x0034) CTR_DRBG - The entropy source failed`，握手起不来 |
| `sysGetRandomNumber` | 不再报错，但调用**永不返回**，表现为"握手卡死、无超时" |
| **时钟 + 计数器**（同 wilips3） | 握手成功 |

`platform_entropy_poll()` / `hardclock_poll()` 定义在 `source/net/http.c`，
靠"对象先于静态库解析"把 `libpolarssl.a` 的 `entropy_poll.o` 挡在链接之外。

### 2.4 不要给 PS3 的 socket 设 `SO_NBIO`

设了之后 `recv` 返回的错误 PolarSSL 1.3.9 不认作 would-block，握手刚发出
ClientHello 就失败：`ssl_handshake returned (-0x004C) NET - Reading information from
the socket failed`。libcurl 自己用 `fcntl` 切非阻塞，PS3 忽略它 —— 保持阻塞 socket
即可，只留 15 s 收发超时做兜底。

### 2.5 CA 路径

不设 `CURLOPT_CAINFO` 时 curl 会去读编译期默认的
`/usr/ssl/certs/ca-bundle.crt`（PS3 上不存在）。现在无条件指向
`/dev_hdd0/game/NP00WILI1/USRDIR/cacert.pem`。

### 2.6 未登录时 `wbi_init` 失败

`/x/web-interface/nav` 在未登录时返回 `{"code":-101,"message":"账号未登录"}`，
但 `data.wbi_img` 仍可用，推荐/搜索本来不需要登录。原来用 `json_error()` 要求
`code == 0`，导致首页永远停在加载中。现在只检查 `wbi_img` 是否取到。
UA 同时换成 `Mozilla/5.0 (PLAYSTATION 3; 1.00)`（`wiliwili` 会被 WBI 接口拒）。

### 2.7 API worker 栈溢出

`thread_main()` 里 `VideoItem items[40]`（每个约 2 KB）就占 80 KB，加上
`PlayUrl`/`view`/`folders[20]`、`bili_recommend()` 的 `url[4600]` 和
`signed_query[4096]`、再叠 curl 与 TLS 握手，128 KB 的线程栈直接踩穿，
worker 卡死在第一个 https 请求里 —— 而同一个 URL 在主线程（probe）里是 200。
栈已改为 512 KB（`api.c`），**已上机确认**：2026-11-14 的包取列表和播放都正常。

### 2.8 分区网格按 ○ 直接退出，看起来像闪退

分区网格的 `handle_region_grid()` 原来把 `○` 当成退出程序（`g_running = 0`），
但底部提示写的是"○ 返回"。从分区列表退回网格后再按一次 `○`，程序会正常走到
`[app] exit` / `[sys] shutdown`，日志里没有任何异常，因此很容易误判成崩溃。
现在网格里的 `○` 改为回到上一个 tab（`g_prev_tab`），并给退出路径加了
`[app] exit: cancel on tab …` / `[app] exit: system request` 日志，下次再看
日志就能直接分清是按键退出还是真崩溃。摇杆向右也会被当成 `○`（见 2.9），
所以这个"随机退出"的根因其实是输入位冲突，2.9 一并修掉。

### 2.9 方向键无效、摇杆上下会误触 X/△

`read_buttons()` 把 `padData.button[2]`（digital1：方向键、START）和
`button[3]`（digital2：□×○△、L1/R1）拼成一个 u32，digital1 落在高 16 位；
但方向键和 START 的检查还用着未移位的 `PAD_CTRL_*`，只能命中低 16 位的
digital2，所以方向键完全没反应。摇杆方向又用未移位常量 OR 进去，正好和面键
撞位：向下 = `×`（打开详情）、向上 = `△`（搜索）、向右 = `○`（取消/退出）、
向左 = `□`（收藏 toast）。

现在 `input.h` 定义 `PAD_NAV_*`（左移 16 位），`input.c` 明确按
`digital1 << 16 | digital2` 拼掩码，app/player 的方向和 START 检查全部改为
`PAD_NAV_*`；面键、L1/R1 仍用原来的 `PAD_CTRL_*`。

### 2.10 VDEC 硬解：从一帧不出到 probe 通过

旧 probe 不是固件限制，是初始化/时序问题：`vdecClosure.fn` 必须是 **32 位 OPD**
（`func` + `rtoc`），工作区要用 `sysMemoryAllocate` 按 1 MB 页分配，H.264 要
**4 个 SPU**，每帧送完要等 `AUDONE` 再复用 AU 缓冲，`vdecClose` 前要等
`SEQDONE`。显示侧色度平面按 **1080** 排（`picture_size` 多出来的是尾部
padding），RGB 纹理还要双缓冲，否则红帧和下一张测试图会在同一帧里撕裂。

现在 probe 在真机上：红 1080 出 4 帧、testsrc 1080/720 各 14 帧，
`[vdec] close returned` 正常。细节和判据见 [docs/decoding.md](docs/decoding.md)。

### 2.11 VDEC 播放器 480p 卡死：等 AUDONE 时没有继续取帧

`vdec_feed` 原来在 `vdecDecodeAu` 后阻塞等 AUDONE；图片队列一满，解码器就无法
完成当前 AU，AUDONE 永远不来，随后 endSequence 等不到 SEQDONE，close 被跳过，
下一部视频复用卡死的 VDEC，整个应用死住。现在 feed 只负责提交，播放线程在等
AUDONE 时持续调用 `vdec_pump` 取 PICOUT；presenter 3 秒不动就报错；
`VdecCtx` 改堆分配，卡住时保留 context 不释放并在本次运行禁用 VDEC。480p
（852x480）已上机连续播放通过。

## 3. 待验证 / 未完成

| 项 | 说明 |
| --- | --- |
| 性能 | 720P 预期 25–40 fps（qpel/chroma 有 Altivec，deblock/IDCT 仍是纯 C）；1080P 不建议 |
| 字体 | 日志里 `[text] fonts latin=0 cgb=0 jpn=0 kor=0`，需要确认 libfont 的 `TTFLoadFont` 返回 0 是成功还是失败；若失败则文字不显示 |
| probe 目录 | **已上机通过**（2026-11-14）：`pics=4/14/14`、`close returned`；产物 `probe/out/vdec_probe.pkg` |
| 播放器接 VDEC | **480p 已上机通过**（2026-11-14）：852x480 连续播放正常；设置可切软解；720p/1080p 待验证 |
| 其它功能 | 二维码登录、弹幕、收藏/稍后再看/历史 均未在真机验证 |

## 4. 日志判读

| 行 | 含义 |
| --- | --- |
| `build <日期 时间>` | **先看这行**，确认跑的是哪一次构建 |
| `t3d 09a flipstatus=0 after N ms` | 翻页完成；`after` 很小（0～10 ms）才算正常 |
| `[http] net init …` | 三个初始化结果，全 0 才是好 |
| `[http] net state=3` | 3 = IPObtained |
| `[http] #N request <方法> <url> (timeout … verify …)` | 请求**发出前**打印；N 是请求编号，卡住时从这一行往下找 |
| `[http] #N socket fd=…` | DNS 已解析、socket 已建（下一步才是 TCP connect） |
| `[curl #N +X ms] …` | curl 的阶段行：`Trying`（TCP）→ `Connected`（TCP 完成）→ `PolarSSL: Connecting` / `Handshake complete`（TLS）→ `Request completely sent off`（等响应） |
| `[http] #N -> <状态码> (N bytes) in X ms (dns / connect / tls / ttfb)` | 请求成功；括号里是从请求开始累计的时间点，相邻相减即各阶段耗时 |
| `[http] #N request failed rc=<curl> http=<状态码> after X ms …` | rc 是 CURLcode（35=TLS、28=超时、6=解析），后面同样带四个时间点 |
| `[api] task N start` / `done rc=` | 任务开始/结束；只有 start 没有 done 就是卡在请求或栈 |
| `[app] exit: cancel on tab …` / `system request` | 主循环收到的退出原因；没有这类行就中断才是真崩溃 |
| `[bili] wbi ready` / `recommend page N: M items` | 接口层结果 |
| `[player] software decode WxH` | 软解初始化成功 |
| `[vdec] …` / `[t3d] …` | 只在 probe 包和 t3D 初始化阶段出现 |

卡住时只看最后几行：

- 只有 `[http] #N request …`、没有 socket 行 → 卡在域名解析（阻塞解析器可以
  吞掉 `CURLOPT_TIMEOUT`，所以看不到 `rc=28`）；
- 停在 `socket fd=…` 或 `[curl] Trying …` → TCP connect；
- 停在 `PolarSSL: Connecting to …` → TLS 握手；
- 停在 `Handshake complete` 或 `Request completely sent off` → 服务器还没回包。

`[curl]` 行只转发 curl 的阶段文本，不记录请求头/响应头，Cookie 和登录参数不会
写进日志。

## 5. 构建与产物

```sh
export PSL1GHT=/usr/local/ps3dev
make -j8 pkg          # 产物：out/pilis3.pkg + build/pkg/USRDIR/EBOOT.BIN
```

- **提交只带 `build/pkg/USRDIR/EBOOT.BIN`**，`.pkg` 不入库（`.gitignore` 已说明），
  本地重打即可；
- tiny3D 是打过 `patches/tiny3d/0001-rsx-init-trace.patch` 的版本（保留初始化打点，
  去掉每帧的 MakeCmdSpace 日志，恢复 `gcmSetWaitFlip()`）；
- FFmpeg 用 `tools/build-ffmpeg.sh` 构建（已开 Altivec，`--disable-asm` 已移除）。

## 6. 本轮提交索引

| 提交 | 内容 |
| --- | --- |
| `d860b72` | API worker 栈 128 KB → 512 KB |
| `8e3da73` | `wbi_init` 不再要求 nav 的 `code==0`；加任务结束日志 |
| `6f2c4be` | HTTPS 打通后清掉调试脚手架（probe / verbose / fd 日志） |
| `ef35443` | 去掉 `SO_NBIO`；CAINFO 无条件指向 app 的 cacert.pem |
| `a62624d` | 熵源改时钟+计数器；UA 换主机型 UA |
| `d3b1c0d` | 去掉 tiny3D 每帧 MakeCmdSpace 日志 |
| `9288977` | 给 PolarSSL 补熵源（第一版，后换成时钟方案） |
| `3794562` | `docs/decoding.md`：为什么用软解 |
| `999a2cb` | FFmpeg 开 Altivec；`copy_plane` 单次拷贝 |
| `ce77038` | 播放器改 libavcodec 软解，默认 720P |
| `5a607b7` | tiny3D 恢复 `gcmSetWaitFlip()`（修闪屏） |
| `96a17c6` | 网络 bring-up 移植自 wilimini/wilips3 |
| `03d2dea` | `sys_sem_create()` 修 0x8001000d |
