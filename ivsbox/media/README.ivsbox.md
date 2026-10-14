# bird_media 收编说明（M6-S6.0，2026-10-14）

本目录收编自姊妹工程 `F:/debug/T113_TCP/bird_media`（用户 2026-10-14 指令：
**M6 视频相关代码调用 bird_media，不需要自己写**）。

## 收编范围与原样纪律

- 收编时 bird_media 无版本管理（工程根下未跟踪目录），以**收编当日工作副本**
  为准；`main.c` 版本串 `V0.0.1-20260417`。
- **一行未改**：`main.c`、`include/`、`src/`、`third_party/ffmpeg/include/` 与
  两套预编译库全部原样。构建不走它的 CMake（本工程唯一入口是 `ivsbox/Makefile`），
  由顶层 Makefile 的 `media/%.o` 专属规则以 `CFLAGS_THIRD_PARTY`（去 `-Werror`）
  编译——同 `third_party/minmea` 的"上游原样"纪律。
- `CMakeLists.txt` 保留（上游原样的证据，也便于对照其依赖与编译选项：
  链接序 avformat→avcodec→avutil→avdevice ＋ m z dl pthread）。
- `build/`（其 CMake 产物）未收编。

## 构建要点

- ffmpeg 头在 `third_party/ffmpeg/include`，mongoose 头在 `src/http/`。
- 预编译库：`lib_arm/`＝**ARM EABI5 soft-float**（与 VM `arm-linux-gnueabi-`
  工具链匹配，ELF 成员 e_flags=0x05000000 已核）、`lib_x86/`＝x86-64（与
  VM 宿主 gcc 匹配）。**`lib_x86/*.a`（33 MB）不入库**（`.gitignore` 忽略），
  宿主构建（`make host`/`analyze`）需要时从姊妹工程 bird_media 同路径拷贝；
  `make test/asan/tsan` 只建单测，不受影响。`lib_arm/*.a` 随库入库
  （板端交付必需，约 3 MB）。

## 运行形态（契约）

独立进程 `ivsbox-media`：mongoose HTTP 监听 `0.0.0.0:18000`（无鉴权），四接口：

| 接口 | 语义 | bird_media 实现点 |
|---|---|---|
| `GET /capture?path=<jpg 路径>` | V4L2 MJPEG 抓图（1280×720，丢 5 帧等曝光稳定），**同步阻塞**到出帧才回 200 | `src/media/capture.c` |
| `GET /record/start?id=<0..7>&url=<rtsp>` | 置录像标志（worker 线程异步接手） | `src/media/media.c` |
| `GET /record/stop?id=<0..7>` | 清标志（worker 在写帧循环里轮询退出） | 同上 |
| `GET /record/status?id=<0..7>` | body `1`/`0`；**查询即给该路续命**（15 min 无人查询自动停录） | 同上 |

录像＝FFmpeg remux **直封装不转码**（RTSP over TCP，2 MB 缓冲；MP4
`frag_keyframe+empty_moov`，15 min 分片＋关键帧对齐，无关键帧时 30 s 强切；
音频流初始化失败自动降级为纯视频重试；ENOSPC 致命停录）。落盘
`PATH_RECORD_ROOT=/mnt/sdcard/video/<yyyymmdd>/channel<id>_<hhmmss>.mp4`，
日志 `/opt/log/media.log`（5 MB 滚动）。两个路径与监听地址都是其 `defs.h`
编译期常量——**板端 SD 挂载点与 `/opt` 可写性确认前不改动**（登记见
修改记录）；ivsboxd 侧桥接见 `src/modules/media/iv_media.c`。

## 已知边界（登记，不掩盖）

- HTTP 无鉴权且监听 `0.0.0.0`：网内任意主机可开关录像。生产部署在 4G/以太
  私网，风险可接受；远程访问边界属 M8 收口项。
- `/capture` 在无 `/dev/video0` 的板（或设备被占）时 init 失败：接口仍回 200，
  **真实成败由调用方 stat 落盘文件判定**（iv_media 桥接已实现）。
- bird_media 上游更新时：同步本目录后核对"一行未改"差异，构建规则无需动。
