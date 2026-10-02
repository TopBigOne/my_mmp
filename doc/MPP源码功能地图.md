# MPP 源码功能地图（RK3588 视角）

> 源码位置：`/Users/dev/Documents/AV/rk_work/rk_code/external/mpp`（版本 1.0.6，见 `CHANGELOG.md`）
> 配套笔记：同目录下的 `MPP开发指南_学习笔记.md`（讲 API 怎么用）；本文讲**源码里有什么、在哪、对你的目标有什么用**
> 文中路径都相对于 mpp 根目录，行号按当前版本

---

## 0. 先说结论：MPP 是什么，不是什么

**MPP（Media Process Platform）= 瑞芯微硬件视频编解码器的用户态驱动库**。它只做一件事：把"压缩码流 ⇄ YUV 图像"的转换交给芯片里的硬件编解码单元（VPU）去做。

| MPP 管 | MPP **不管**（要靠别的库） |
|---|---|
| H.264 / H.265 / VP9 / AV1 / JPEG ... **解码** | 读写 MP4 / MKV / TS / FLV 等**封装格式** → FFmpeg (libavformat) |
| H.264 / H.265 / JPEG / VP8 **编码** | RTSP / RTMP / WebRTC / HLS 等**网络传输** → live555、ZLMediaKit、SRS、FFmpeg |
| 码率控制、GOP、ROI、OSD 叠加、SEI | 摄像头采集 → V4L2（MPP 只附带了一个简单示例） |
| 硬件可访问的内存（DMA-BUF）管理 | 缩放、裁剪、颜色转换、多画面拼接 → **RGA**（librga，另一个仓库） |
| | 音频编解码 → ALSA + FFmpeg / fdk-aac 等，MPP **完全不涉及音频** |
| | 显示 → DRM/KMS、GStreamer、Qt |

所以对你的三个目标：
- **音视频编解码**：MPP 就是核心，必须吃透
- **NVR**（网络录像机）：MPP 负责"多路解码 + 再编码"，前后还需要 RTSP 拉流、RGA 拼画面、MP4 封装录像
- **流媒体传输**：MPP 只产出 H.264/H.265 裸码流（Annex-B），推流/拉流靠 live555 / ZLMediaKit / FFmpeg

---

## 1. RK3588 上到底有哪些硬件（源码证据）

在 [osal/mpp_soc.cpp:907-919](../../../rk_work/rk_code/external/mpp/osal/mpp_soc.cpp) 里，RK3588 是这样登记的：

```c
/*
 * rk3588 has codec:
 * 1 - vpu2 for jpeg/vp8 encoder and decoder
 * 2 - RK H.264/H.265/VP9 8K decoder
 * 3 - RK H.264/H.265 8K encoder
 * 4 - RK jpeg decoder
 */
"rk3588",
{ &vdpu38x, &rkjpegd, &vdpu2, &vdpu2_jpeg_pp_fix, &av1d, &avspd },   // 解码器
{ &vepu58x, &vepu2, &vepu2_jpeg_enhanced, NULL },                    // 编码器
```

翻译成表格：

| 硬件单元 | 方向 | 支持的格式 | 对应的 HAL 源码（真正写寄存器的地方） |
|---|---|---|---|
| **RKVDEC（vdpu381）** | 解码 | H.264、H.265、VP9、AVS2，最高 8K | `mpp/hal/rkdec/*/hal_*_vdpu34x.c`（381 和 34x 共用一套代码，见 `hal_h264d_api.c:304`） |
| **AV1DEC** | 解码 | AV1 | `mpp/hal/vpu/av1d/` |
| **RKJPEGD** | 解码 | JPEG / MJPEG | `mpp/hal/vpu/jpegd/hal_jpegd_rkv.c` |
| **VDPU2** | 解码 | MPEG-2、MPEG-4、H.263、VP8、JPEG（老格式） | `mpp/hal/vpu/{m2vd,mpg4d,h263d,vp8d}` |
| AVSPD | 解码 | AVS+（国标，了解即可） | `mpp/hal/rkdec/avsd` |
| **RKVENC（vepu580）** | 编码 | **H.264、H.265**，最高 8K | `mpp/hal/rkenc/h264e/hal_h264e_vepu580.c`、`h265e/hal_h265e_vepu580.c` |
| VEPU2 | 编码 | JPEG、VP8 | `mpp/hal/vpu/{jpege,vp8e}` |

> 💡 看到 `vepu510`、`vepu540c`、`vdpu383` 之类的文件不用管，那是其他芯片（RK3576、RK3528 等）的。**RK3588 只看 `vepu580` 和 `vdpu34x`**。

---

## 2. 分层架构：一次 `encode_put_frame` 从上到下经过了哪些代码

```
你的程序
  │  mpi->encode_put_frame(ctx, frame)
  ▼
┌──────────────────────────────────────────────────────────────┐
│ ① MPI 接口层     inc/rk_mpi.h + mpp/mpi.cpp                    │  对外 API：mpp_create / mpp_init / MppApi 函数表
├──────────────────────────────────────────────────────────────┤
│ ② Mpp 调度层     mpp/mpp.cpp（class Mpp）                      │  输入/输出队列、线程、control 命令分发
├──────────────────────────────────────────────────────────────┤
│ ③ Codec 层       mpp/codec/                                    │  软件部分：
│                  ├ mpp_enc_impl.cpp  编码线程、码率控制调度     │   解码：解析码流（SPS/PPS/slice）、管理参考帧 DPB
│                  ├ mpp_dec*.cpp      解码线程                  │   编码：生成 SPS/PPS/slice 头、参考帧管理、码率控制
│                  ├ dec/h264 ... 各格式解析器                   │
│                  ├ enc/h264 ... 各格式编码器"软件壳"           │
│                  └ rc/        码率控制算法                     │
├──────────────────────────────────────────────────────────────┤
│ ④ HAL 层         mpp/hal/                                      │  把参数翻译成**硬件寄存器值**
│                  ├ rkenc/h264e/hal_h264e_vepu580.c             │   （每款硬件 IP 一份代码）
│                  └ rkdec/h264d/hal_h264d_vdpu34x.c ...         │
├──────────────────────────────────────────────────────────────┤
│ ⑤ OSAL 层        osal/                                         │  操作系统抽象：线程、锁、日志、内存、
│                  ├ allocator/  DRM / DMA-HEAP / ION 内存分配    │   识别芯片型号（mpp_soc.cpp）
│                  └ driver/mpp_service.c  ioctl 下发寄存器       │
└──────────────────────────────────────────────────────────────┘
  │  ioctl(/dev/mpp_service, ...)
  ▼
Linux 内核 mpp_service 驱动（rk_vcodec）→ 硬件 VPU 开始工作 → 中断 → 结果返回
```

这个分层在 `doc/design/1.mpp_design.txt` 里有官方描述，`doc/design/2.kernel_driver.txt` 讲内核驱动交互，建议对照着看。

**类比 Android**：① 像 `MediaCodec` 的 Java API；② 像 `MediaCodec` 的 native 状态机；③④ 像 Codec2 / OMX 组件；⑤ 像 HAL + ion/gralloc。

---

## 3. 目录逐个看：都有什么功能

### 3.1 `inc/` —— 对外头文件（**你的程序只 include 这里**）⭐⭐⭐

| 文件 | 内容 | 学习优先级 |
|---|---|:---:|
| `rk_mpi.h` | `MppApi` 函数表、`mpp_create/init/destroy` | ⭐⭐⭐ |
| `rk_mpi_cmd.h` | 所有 `control` 命令（见第 4 节功能清单） | ⭐⭐⭐ |
| `mpp_frame.h` | `MppFrame`：一帧 YUV 图像 + 宽高/stride/格式/时间戳 | ⭐⭐⭐ |
| `mpp_packet.h` | `MppPacket`：一段压缩码流 | ⭐⭐⭐ |
| `mpp_buffer.h` | `MppBuffer` / `MppBufferGroup`：硬件可访问内存，**可导入导出 DMA-BUF fd**（零拷贝的关键） | ⭐⭐⭐ |
| `rk_venc_cfg.h` / `rk_venc_cmd.h` | 编码配置（`MppEncCfg` 字符串键：`rc:mode`、`prep:width` ...）| ⭐⭐⭐ |
| `rk_venc_rc.h` | 码率控制模式 CBR / VBR / AVBR / FIXQP | ⭐⭐ |
| `rk_type.h` | `MppCodingType`（H.264=7、H.265=16777220 ...）| ⭐⭐ |
| `rk_vdec_cfg.h` / `rk_vdec_cmd.h` | 解码配置 | ⭐⭐ |
| `mpp_meta.h` | 附在 frame/packet 上的元数据（OSD、ROI、是否关键帧 ...）| ⭐⭐ |
| `mpp_task.h` | 高级 poll/dequeue/enqueue 接口用的任务对象 | ⭐ |
| `rk_venc_ref.h` | 参考帧结构（长期参考帧、SVC 时域分层）| ⭐（进阶） |
| `rk_hdr_meta_com.h` | HDR 元数据 | ⭐（进阶） |
| `vpu.h` / `vpu_api.h` | **旧版**接口，新代码不要用 | — |

### 3.2 `mpp/` —— 核心库（生成 `librockchip_mpp.so`）

| 路径 | 功能 |
|---|---|
| `mpp/mpi.cpp` | API 入口。`mpp_create`（第 412 行）、`mpp_init`（第 457 行）、`mpi_encode_put_frame`（第 199 行）... 每个函数基本只是检查参数后转给 `Mpp` 类 |
| `mpp/mpp.cpp` | **`class Mpp` 调度中心**。`put_frame`（637）、`get_packet`（755）、`put_packet`（369）、`get_frame`（485）、`control`（1003）把命令分到 dec/enc/osal |
| `mpp/base/` | 基础数据结构的实现：`mpp_frame.cpp`、`mpp_packet.cpp`、`mpp_buffer_impl.cpp`（内存池）、`mpp_buf_slot.cpp`（**解码输出帧槽管理**）、`mpp_enc_cfg.cpp`（字符串键配置表）、`mpp_enc_refs.cpp`（参考帧关系）、`mpp_bitread/bitwrite.c`（读写比特流） |
| `mpp/codec/mpp_enc_impl.cpp` | **编码主线程**（99KB，最大的文件）：取任务 → 码率控制 → 调 HAL → 输出 packet；支持重编码（`reenc`，码率超标时重编）、低延迟模式（`try_proc_low_deley_task`）、两遍编码 |
| `mpp/codec/mpp_dec*.cpp` | 解码主线程：`mpp_dec_normal.cpp`（正常模式）、`mpp_dec_no_thread.cpp`（无线程模式，调用方自己驱动）|
| `mpp/codec/dec/<格式>/` | 码流**解析器**（纯软件）：拆 NAL、解 SPS/PPS/SEI/slice 头、DPB 参考帧管理。例：`dec/h264/h264d_sps.c`、`h264d_dpb.c`、`h264d_sei.c` |
| `mpp/codec/enc/<格式>/` | 编码器软件部分：生成 SPS/PPS/VPS、slice 头、管理参考帧。例：`enc/h264/h264e_sps.c` |
| `mpp/codec/rc/` | **码率控制算法**：`rc_model_v2.c`（主算法，CBR/VBR/AVBR）、`rc_model_v2_smt.c`（智能码率，适合监控）|
| `mpp/hal/` | 硬件抽象层，见第 1 节表格 |
| `mpp/vproc/` | 视频后处理：`iep2/`（**去隔行**，处理 1080i 等隔行源）、`rga/`（MPP 内部调用 RGA）、`iep/`（老版去隔行）、`vdpp/`（画质增强，主要是其他芯片） |
| `mpp/legacy/` | 旧 `vpu_api` 兼容层（老代码 / 老版 Android 用），**不用看** |

### 3.3 `osal/` —— 操作系统抽象层

| 文件 | 功能 | 值得看吗 |
|---|---|:---:|
| `mpp_soc.cpp` | 根据 `/proc/device-tree/compatible` 识别芯片，登记每种芯片有哪些硬件（第 1 节就出自这里） | ⭐⭐ 看 RK3588 那段 |
| `allocator/allocator_drm.c` | 从 `/dev/dri/card0` 分配 DRM 内存（`MPP_BUFFER_TYPE_DRM`，你 Day 1 用的就是它）| ⭐⭐ |
| `allocator/allocator_dma_heap.c` | 从 `/dev/dma_heap/*` 分配（新内核推荐）| ⭐ |
| `allocator/allocator_ext_dma.c` | **导入外部 DMA-BUF**（摄像头 / RGA 给的 fd）→ Day 3 零拷贝要用 | ⭐⭐ |
| `allocator/allocator_ion.c`、`allocator_std.c` | ION（老 Android）、普通 malloc（仅调试）| — |
| `driver/mpp_service.c` | 通过 ioctl 向内核 `/dev/mpp_service` 提交寄存器、等待完成 | ⭐ 想知道"最底层怎么和硬件说话"时看 |
| `mpp_log.cpp`、`mpp_env.cpp` | 日志和环境变量调试开关（`mpp_debug`、`mpp_enc_debug` 等）| ⭐ |
| `mpp_thread.cpp`、`mpp_list.cpp`、`mpp_mem.cpp` | 线程、链表、内存（带泄漏检查）| — |

### 3.4 `test/` —— 官方示例程序（**最好的学习材料**）⭐⭐⭐

编出来就是板子上的 `mpi_enc_test`、`mpi_dec_test` 等命令。

| 程序 | 演示什么 | 对应你的哪个阶段 |
|---|---|---|
| `mpp_info_test.c`（41 行） | 打印 MPP 版本信息，最小程序 | 验证环境 |
| ⭐ `mpi_enc_test.c` | **编码**：YUV 文件 → H.264/H.265/JPEG；`-i /dev/videoX` 时**直接从摄像头采集编码**（第 188 行）；支持多实例 `-s` | Day 1、Day 3 |
| ⭐ `mpi_dec_test.c` | **解码**：码流文件 → YUV；演示同步/异步接口、内部/外部 buffer 模式（`-bufmode`）、分辨率变化（info change）处理 | Day 2 |
| `mpi_enc_mt_test.cpp` | 编码的**输入、输出分在两个线程**（第 1042、1048 行），吞吐更高 | 做实时编码时参考 |
| `mpi_dec_mt_test.c` | 解码的输入、输出分在两个线程 | 实时拉流解码 |
| ⭐ `mpi_dec_multi_test.c` | **多路同时解码**（每路一个线程，第 603 行） | **NVR 多路解码的原型** |
| `mpi_dec_nt_test.c` | 无线程模式（`MPP_SET_DISABLE_THREAD`，第 383 行），由调用方自己驱动 | 想自己控制调度时 |
| `mpi_rc2_test.c` + `mpi_rc.cfg` | 详细的码率控制参数，从配置文件读入 | Day 1 晚上码率实验 |
| `vpu_api_test.c` | 旧接口示例 | 不用看 |

其他小测试：`mpp/vproc/{rga,iep2}/test/`（后处理）、`mpp/base/test/`、`osal/test/`、`mpp/codec/rc/test/`（单元测试）。

### 3.5 `utils/` —— 示例程序共用的工具代码

| 文件 | 功能 | 用处 |
|---|---|---|
| `utils.c` | `read_image`（按 stride 读 YUV）、`dump_mpp_frame_to_file`（把解码结果写成文件） | Day 1/2 直接参考 |
| `mpi_enc_utils.c` | 编码命令行参数解析（`-rc`、`-bps`、`-g`、`-qc` ... 第 499-520 行）、默认码率计算 | 查参数含义 |
| `mpi_dec_utils.c` | 解码参数解析、**码流文件读取器**（把 .h264 文件切成一个个 packet） | Day 2 |
| ⭐ `camera_source.c` | **最简 V4L2 摄像头采集**（MMAP 方式，支持 NV12/NV16/YUYV ...） | Day 3 入门 |
| `mpp_enc_roi_utils.c` | ROI 区域编码辅助 | 进阶 |
| `iniparser.c`、`dictionary.c`、`mpp_opt.c` | ini 配置解析、命令行解析 | — |

### 3.6 其他目录

| 目录 | 内容 |
|---|---|
| `doc/Rockchip_Developer_Guide_MPP_CN.md` | 官方开发指南（中文，90KB），你的学习笔记就是基于它 |
| `doc/design/` | 4 篇设计文档：总体设计、内核驱动、buffer 设计、task 设计 —— **读源码前先读这 4 篇** |
| `build/linux/aarch64/` | 官方的交叉编译脚本（make-Makefiles.bash + arm.linux.cross.cmake） |
| `tools/yuvplay` | YUV 查看工具 |
| `debian/`、`pkgconfig/`、`LICENSES/` | 打包相关，不用看 |

---

## 4. 功能清单：MPP 能做的事（从 `inc/rk_mpi_cmd.h` 整理）

### 4.1 编码功能

| 功能 | 怎么用 | 应用场景 |
|---|---|---|
| H.264 / H.265 / JPEG / VP8 编码 | `mpp_init(ctx, MPP_CTX_ENC, type)` | 基础 |
| 码率控制 CBR / VBR / AVBR / FIXQP | `rc:mode` | 直播用 CBR，录像用 VBR/AVBR |
| 码率、帧率、QP 范围 | `rc:bps_*`、`rc:fps_*`、`rc:qp_*`，`MPP_ENC_SET_QP_RANGE` | 画质/带宽权衡 |
| GOP 长度 | `rc:gop` | 拖动/秒开 vs 文件大小 |
| **强制 IDR** | `MPP_ENC_SET_IDR_FRAME` | **流媒体新客户端接入时立刻给关键帧**、丢包恢复 |
| 取 SPS/PPS/VPS | `MPP_ENC_GET_HDR_SYNC` | 写文件头、RTSP 的 SDP 里填 sprop-parameter-sets |
| 每个 IDR 带头 | `MPP_ENC_SET_HEADER_MODE` | 流媒体中途加入也能解 |
| **ROI 感兴趣区域** | `MPP_ENC_SET_ROI_CFG` | 监控里人脸/车牌区域给更多码率 |
| **OSD 叠加** | `MPP_ENC_SET_OSD_PLT_CFG` / `OSD_DATA_CFG`（最多 8 个区域） | 监控画面叠加时间、通道名 |
| SEI 自定义数据 | `MPP_ENC_SET_SEI_CFG` | 在码流里塞时间戳、AI 检测结果 |
| Slice 切分 | `MPP_ENC_SET_SPLIT` | 低延迟传输、抗丢包 |
| 参考帧结构（长期参考、SVC 时域分层） | `MPP_ENC_SET_REF_CFG` | 弱网传输、可丢帧 |
| 旋转 / 镜像 / 裁剪 | `prep:rotation`、`prep:mirroring` ... | 你的摄像头画面转了 90°，可以在编码时转回来 |
| 智能码率（监控场景） | `-sm 1`（scene mode ipc）→ `rc_model_v2_smt.c` | 静止画面码率很低 |
| 多实例 | 多个 `MppCtx` | 主码流 + 子码流（NVR / IPC 必备） |

### 4.2 解码功能

| 功能 | 怎么用 | 应用场景 |
|---|---|---|
| H.264 / H.265 / VP9 / AV1 / JPEG / MPEG-2/4 ... 解码 | `mpp_init(ctx, MPP_CTX_DEC, type)` | 基础 |
| 码流自动切帧 | `MPP_DEC_SET_PARSER_SPLIT_MODE` | 从文件/网络读来的是任意长度数据时 |
| **分辨率变化处理** | `MPP_DEC_SET_INFO_CHANGE_READY` | 码流中途改分辨率（**必须处理**，否则卡住） |
| **外部 buffer 组** | `MPP_DEC_SET_EXT_BUF_GROUP` | 解码结果直接给 RGA / 显示，零拷贝 |
| 立即输出 / 快速播放 | `MPP_DEC_SET_IMMEDIATE_OUT`、`ENABLE_FAST_PLAY` | **降低直播延迟、秒开** |
| 容错 | `MPP_DEC_SET_DISABLE_ERROR` | 网络丢包时尽量继续出图 |
| 输出格式 | `MPP_DEC_SET_OUTPUT_FORMAT` | 选 NV12 / FBC 压缩格式等 |
| 去隔行 | `MPP_DEC_SET_ENABLE_DEINTERLACE`（内部用 IEP2） | 老摄像头的隔行源 |
| 无线程模式 | `MPP_SET_DISABLE_THREAD` | 自己管理大量通道时 |
| 缩略图 | `MPP_DEC_GET_THUMBNAIL_FRAME_INFO` | NVR 回放的缩略图 |

### 4.3 内存功能

| 功能 | 说明 |
|---|---|
| 内部分配 | `mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM)` |
| **导入外部 fd** | `mpp_buffer_import()` + `MppBufferInfo.fd`：把摄像头 / RGA 的 DMA-BUF 直接交给编码器，**不拷贝** |
| 导出 fd | `mpp_buffer_get_fd()`：把解码结果的 fd 交给 RGA / DRM 显示 |
| 缓存同步 | `mpp_buffer_sync_begin/end`：CPU 和硬件之间的 cache 一致性 |

---

## 5. 按你的目标，MPP 源码该怎么用

### 5.1 音视频编解码（国庆 5 天计划）

| 天 | 看哪个示例 | 重点源码 |
|---|---|---|
| D1 编码 | `test/mpi_enc_test.c` | `utils/utils.c` 的 `read_image`、`mpp/base/mpp_enc_cfg.cpp`（看有哪些配置键） |
| D2 解码 | `test/mpi_dec_test.c` | `utils/mpi_dec_utils.c`（码流读取）、info change 处理、`mpp/base/mpp_buf_slot.cpp` |
| D3 摄像头 | `utils/camera_source.c` + `mpi_enc_test -i /dev/video31` | `osal/allocator/allocator_ext_dma.c`（DMA-BUF 导入） |
| D4 RGA + 双码流 | 多实例编码（`-s 2`） | MPP 只管编码，缩放看 librga |
| D5 转码 + RTSP | `mpi_dec_mt_test.c` + `mpi_enc_mt_test.cpp` | 解码 fd → 编码，零拷贝 |

### 5.2 NVR

```
N 路 IPC ──RTSP 拉流──► [live555/FFmpeg 解封装] ──H.264/H.265──► MPP 解码 × N
                                    │                              │ (NV12, DMA-BUF fd)
                                    └──► 直接写 MP4 录像（不解码）    ▼
                                                              RGA 缩放 + 拼成 4/9/16 宫格
                                                                   │
                                                    ┌──────────────┴────────────┐
                                                    ▼                           ▼
                                               HDMI 显示 (DRM)          MPP 编码 → 推流 / 预览
```
- MPP 里对应的参考：`mpi_dec_multi_test.c`（多路解码）、`MPP_DEC_SET_EXT_BUF_GROUP`（零拷贝输出）、`MPP_DEC_SET_DISABLE_ERROR`（网络丢包容错）
- 性能上限看 RKVDEC：官方标称 8K@60 级别的 H.265 解码能力，大约能换算成十几到三十多路 1080p@30（实际要在板子上测）

### 5.3 流媒体传输

| 需求 | MPP 里的对应功能 |
|---|---|
| RTSP 服务器发 SDP | `MPP_ENC_GET_HDR_SYNC` 拿 SPS/PPS |
| 新客户端接入秒开 | `MPP_ENC_SET_IDR_FRAME` 立即出 I 帧 + `HEADER_MODE_EACH_IDR` |
| 网络带宽变化 | 运行中改 `rc:bps_target` 后再 `MPP_ENC_SET_CFG`（动态码率） |
| 低延迟 | slice 切分 `MPP_ENC_SET_SPLIT`、解码端 `IMMEDIATE_OUT` |
| 打包成 RTP | MPP 不管，输出是 Annex-B（`00 00 00 01` 起始码），交给 live555 的 `H264VideoStreamFramer` 等 |

---

## 6. 推荐的源码阅读顺序

1. `doc/design/1~4.*.txt`（1 小时）：先建立整体概念
2. `inc/rk_mpi.h`、`mpp_frame.h`、`mpp_packet.h`、`mpp_buffer.h`（1 小时）：知道有哪些 API
3. `test/mpi_enc_test.c`、`test/mpi_dec_test.c`（Day 1、Day 2）：会用
4. 想知道"put_frame 之后发生了什么"：`mpp/mpi.cpp` → `mpp/mpp.cpp: Mpp::put_frame` → `mpp/codec/mpp_enc_impl.cpp: try_proc_normal_task`（2254 行）→ `mpp/hal/rkenc/h264e/hal_h264e_vepu580.c` → `osal/driver/mpp_service.c`
5. 想懂码流格式：`mpp/codec/dec/h264/h264d_sps.c`、`h264d_slice.c`（对照 H.264 标准读，是学码流结构最好的代码）
6. 想调码率：`mpp/codec/rc/rc_model_v2.c`

> 不建议从头读 HAL（`hal_*_vepu580.c` 都是几千行寄存器赋值，没有芯片手册看不懂），知道它在哪一层就够了。

---

## 7. demo：在板子上用官方工具快速体验各个功能

板子出厂系统里已经带了编好的 `mpi_enc_test` / `mpi_dec_test`（在 `/usr/bin`）。先 `ssh root@$IP`，然后：

```bash
cd /userdata/av

# 0. 版本信息
mpp_info_test

# 1. 编码：NV12 → H.264（-t 7 = H.264，-f 0 = NV12）
mpi_enc_test -w 1920 -h 1080 -t 7 -f 0 -i in_1080p_60f.nv12 -o enc.h264 -n 60

# 2. 编码 H.265（-t 16777220），对比文件大小
mpi_enc_test -w 1920 -h 1080 -t 16777220 -f 0 -i in_1080p_60f.nv12 -o enc.h265 -n 60
ls -l enc.h264 enc.h265

# 3. 码率控制：CBR 2Mbps、gop=30（-rc 1 = CBR）
mpi_enc_test -w 1920 -h 1080 -t 7 -i in_1080p_60f.nv12 -o cbr2m.h264 -rc 1 -bps 2000000 -g 0:30:0 -n 60

# 4. 解码：H.264 → NV12（-t 7），并打印 fps
mpi_dec_test -t 7 -i enc.h264 -o dec.nv12 -v f

# 5. 多路解码压力测试：同时 4 路，看总 fps（NVR 能带几路的粗略估计；不写输出文件）
mpi_dec_test -t 7 -i enc.h264 -s 4 -v f

# 6. 直接从摄像头编码（你的 IMX415 mainpath 是 /dev/video31）
mpi_enc_test -w 1920 -h 1080 -t 7 -f 0 -i /dev/video31 -o cam.h264 -n 150
```

把结果拉回 Mac 播放：
```bash
scp root@$IP:/userdata/av/{enc.h264,enc.h265,cam.h264} /Users/dev/Documents/AV/rk_test_data/
```

> 参数的完整说明：运行 `mpi_enc_test` / `mpi_dec_test` 不带参数会打印帮助；源码里在 `utils/mpi_enc_utils.c:499` 和 `utils/mpi_dec_utils.c:627`。
> 想看 MPP 内部日志：运行前 `export mpp_debug=1`（或 `mpp_enc_debug`、`h264e_debug` 等，具体开关在各模块的 `*_debug.h` 里）。
