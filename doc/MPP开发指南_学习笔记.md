# MPP 开发指南 · 学习笔记（RK3588 / Linux 5.10）

> **原文**：`docs/cn/Common/MPP/Rockchip_Developer_Guide_MPP_CN.pdf`（V0.7，2023-10-17，共 46 页）
> **对照的源码**：SDK 里的 `external/mpp`（版本 1.0.6，2024-06-12），重点是 `test/mpi_enc_test.c`、`test/mpi_dec_test.c`、`inc/*.h`
> **Mac 上的路径**：`~/OrbStack/rk-dev/home/dev/rk3588/atk_dlrk3588_linux5.10/`
>
> 标记说明：📖 = 指南原文要点　🔍 = 对照源码后补充或更正　💡 = 和 Android MediaCodec 的类比

---

## 0. 一句话理解 MPP

**MPP（Media Process Platform）是瑞芯微的硬件编解码库，作用相当于 Android 的 MediaCodec。**
你把码流（H.264/H.265）交给它，它用 VPU 硬件解码成 YUV；你把 YUV 交给它，它编码成码流。整个过程 CPU 基本不参与运算。

---

## 1. RK3588 的编解码硬件能力（来自芯片 Datasheet V1.7）

| 方向 | 格式 | 最高规格 |
|---|---|---|
| **解码** | H.265 / HEVC Main10 | **8K@60fps** |
| | VP9 Profile0/2 | 8K@60fps |
| | AVS2 | 8K@60fps |
| | H.264 / AVC Main10 | **8K@30fps** |
| | AV1 Main 8/10bit | 4K@60fps |
| | MPEG-1/2、VC-1、VP8 | 1080p@60fps |
| **编码** | **H.264 / H.265** | **8K@30fps**，分辨率低时可以多路并行 |
| JPEG 编码 | Baseline | 96x96 到 8192x8192，每秒 9000 万像素 |
| JPEG 解码 | 支持 MJPEG | 1080p@280fps |

🔍 **注意**：MPP 本身支持的格式比 RK3588 硬件多，比如 H.263、VP8 编码。**具体某个格式能不能用，以芯片 Datasheet 为准**（指南 1.4 节也是这么说的）。

---

## 2. 系统架构（📖 第一章）

```
┌─────────────────────────────────────────┐
│ 应用层：你的程序 / GStreamer / FFmpeg / OpenMax │
├─────────────────────────────────────────┤
│ MPP 层（用户态库 librockchip_mpp.so）       │
│   MPI 接口 → 编解码器 → HAL → OSAL           │  ← 屏蔽不同芯片的差异
├─────────────────────────────────────────┤
│ 内核驱动：/dev/mpp_service（vcodec 驱动、MMU、时钟、电源） │
├─────────────────────────────────────────┤
│ 硬件：vdpu / rkvdec（解码）、vepu / rkvenc（编码）    │
└─────────────────────────────────────────┘
```

- MPP **依赖内核驱动**。内核里要启用 vcodec 驱动，设备树里要配置好对应节点。正点原子的 SDK 默认已经配好了。
- 💡 类比 Android：MPI 相当于 `MediaCodec` 的 API，MPP 内部相当于 Codec2/OMX 组件，`/dev/mpp_service` 相当于 HAL 下面的 VPU 驱动。

---

## 3. 核心数据结构（📖 第二章）⭐

**所有对象都是 `void*` 句柄**，只能通过 `mpp_xxx_set_yyy()` / `mpp_xxx_get_yyy()` 这类函数访问成员（为了保持二进制兼容）。

| 结构 | 是什么 | 💡 在 MediaCodec 里对应 |
|---|---|---|
| **MppCtx** | 一个编码器或解码器实例 | `MediaCodec` 对象 |
| **MppApi**（也叫 MPI） | 函数指针表：`decode_put_packet`、`encode_get_packet` 等 | MediaCodec 的各个方法 |
| **MppPacket** | **一维**数据，一般用来装**码流** | 装压缩数据的 `ByteBuffer` + `BufferInfo` |
| **MppFrame** | **二维**图像，一般用来装 **YUV/RGB** | 装原始画面的 `Image`、`ByteBuffer` |
| **MppBuffer** | 硬件能直接访问的内存（**dmabuf**，有 fd） | `HardwareBuffer` / gralloc 分配的 buffer |
| **MppBufferGroup** | MppBuffer 的缓存池 | codec 内部的 buffer 队列 |
| MppMem | 对 C 库 `malloc` 的封装 | — |
| MppTask / MppMeta | 高级的异步接口，按"关键字 → 值"扩展 | — |
| **MppEncCfg** | 编码参数，用"字符串 → 值"的方式设置 | `MediaFormat` |

### 3.1 MppBuffer（硬件内存）
- 重要成员：`ptr`（虚拟地址，CPU 读写用）、`size`、**`fd`（dmabuf 文件描述符，零拷贝的关键）**。
- 支持的分配器：ion、drm、dma_heap。
- 两种用法：
  - **internal（常规方式）**：由 MPP 创建 group，通过 `mpp_buffer_get` / `mpp_buffer_put` 申请和释放。
  - **external（外部导入）**：group 只负责管理，内存由外部分配后导入进来（比如显示模块、摄像头的 buffer），**方便做到零拷贝**。

### 3.2 MppPacket（码流）
| 成员 | 含义 |
|---|---|
| `data` / `size` | 整块缓冲区的起始地址和大小 |
| **`pos` / `length`** | **有效数据**的起始地址和长度（读码流时用这两个） |
| `pts` / `dts` | 显示时间戳 / 解码时间戳 |
| `eos` | 码流结束标志 |
| `flag` | EOS、EXTRA_DATA、INTERNAL、**INTRA（关键帧）** 等标志位 |

- 调用 `decode_put_packet` 以后，如果 `length` 变成 0，说明**这包数据已经被消耗完了**。
- 释放规则：外部 malloc 的地址 → MPP 不会 free；MPP 拷贝出来的 → 会 free；从 MppBuffer 生成的 → 用引用计数管理。

### 3.3 MppFrame（图像）
| 成员 | 含义 |
|---|---|
| `width` / `height` | 有效像素的宽和高 |
| **`hor_stride`** | 每一行实际占用的**字节数**（≥ width，有对齐） |
| **`ver_stride`** | 分量之间间隔的**行数**（≥ height，比如 1080 对齐到 1088） |
| `fmt` | 像素格式，比如 NV12 |
| `pts` / `dts` / `eos` | 从对应的输入 MppPacket 继承过来 |
| `errinfo` | 不等于 0 表示**这一帧解码有错误**，可以丢弃 |
| `discard` | 参考关系不满足，**这一帧应该丢弃，不显示** |
| `buf_size` | 解码器要求分配的缓存大小 |
| **`info_change`** | 这是一个"**分辨率或格式变了**"的通知帧，不是真正的图像 |

💡 `stride` 这个概念在 Android `Image.getPlanes()[0].getRowStride()` 里也有，是同一个意思。

---

## 4. MppApi 函数一览（📖 2.7 节）

| 函数 | 作用 | 备注 |
|---|---|---|
| `decode_put_packet(ctx, packet)` | 把码流送进解码器 | 返回非 0 表示队列满了，**等一会儿再送** |
| `decode_get_frame(ctx, &frame)` | 取出解码后的图像 | 返回 0 也要检查 frame 是不是 NULL |
| `decode(ctx, packet, &frame)` | 上面两个的组合 | — |
| `encode_put_frame(ctx, frame)` | 把图像送进编码器 | **阻塞调用**，要等硬件用完这帧才返回 |
| `encode_get_packet(ctx, &packet)` | 取出编码后的码流 | 默认非阻塞，可能取不到 |
| `encode(ctx, frame, &packet)` | — | **还没实现** |
| `poll` / `dequeue` / `enqueue` | MppTask 高级接口 | 零拷贝输出码流时才需要用 |
| `reset(ctx)` | 恢复到刚初始化的状态 | 同步阻塞；EOS 之后要 reset 才能继续用 |
| `control(ctx, cmd, param)` | 设置参数、查询状态 | 命令定义在 `rk_mpi_cmd.h` 里 |

生命周期：
```
mpp_create(&ctx, &mpi)   → 创建实例，同时拿到函数表
mpp_init(ctx, 类型, 格式) → 指定是编码(MPP_CTX_ENC)还是解码(MPP_CTX_DEC)，以及编码格式
mpi->control / put / get … → 干活
mpi->reset(ctx)           → （可选）
mpp_destroy(ctx)          → 销毁
```
💡 对应 MediaCodec 的 `createDecoderByType` → `configure` → `start` → `queue/dequeue` → `stop` → `release`。

---

## 5. 解码器（📖 3.1～3.3 节）

### 5.1 输入码流的两种方式（重要）
MPP 只接收**裸码流**，也就是没有 MP4/MKV 这类封装的 H.264/H.265 数据。

| 方式 | 说明 | 适用场景 |
|---|---|---|
| **外部分帧**（默认） | 每个 MppPacket **正好是完整的一帧** | 从 MP4 解封装出来的数据、RTP 组好帧的数据，效率高 |
| **内部分帧** | 按固定长度读文件就往里送，由 MPP 自己找帧边界 | 直接读 `.h264` 文件，使用简单，但效率低一些 |

⚠️ **两种方式不能混着用**，否则会解码出错。

🔍 **怎么打开内部分帧**：指南里说的是在 `mpp_init` **之前**调用 `MPP_DEC_SET_PARSER_SPLIT_MODE`。**SDK 自带的 `mpi_dec_test.c` 用的是新写法**：
```c
mpp_init(ctx, MPP_CTX_DEC, type);
mpp_dec_cfg_init(&cfg);
mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);   // 1 = 打开内部分帧
mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
```

### 5.2 解码主循环（🔍 根据 mpi_dec_test.c 整理）
```c
// ① 创建并初始化
mpp_create(&ctx, &mpi);
mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);      // H.264
/* 设置 split_parse，见上一节 */

// ② 循环：送码流 → 取图像
mpp_packet_init(&packet, buf, len);                     // 或者 mpp_packet_set_pos / set_length
if (读到文件末尾) mpp_packet_set_eos(packet);
mpi->decode_put_packet(ctx, packet);                    // 返回非 0 就 sleep 一下再送

mpi->decode_get_frame(ctx, &frame);
if (frame) {
    if (mpp_frame_get_info_change(frame)) {
        // ③ 第一次拿到的一般是 info_change，这时要分配缓存池
        w  = mpp_frame_get_width(frame);      h  = mpp_frame_get_height(frame);
        hs = mpp_frame_get_hor_stride(frame); vs = mpp_frame_get_ver_stride(frame);
        sz = mpp_frame_get_buf_size(frame);
        /* 按 sz 创建 MppBufferGroup（mpi_dec_test 默认分配 24 块） */
        mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
        mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);   // 通知解码器可以继续了
    } else {
        if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame)) {
            /* 有错误的帧，丢掉 */
        } else {
            /* 正常的一帧：通过 mpp_frame_get_buffer → mpp_buffer_get_ptr 拿到 YUV 数据 */
        }
        eos = mpp_frame_get_eos(frame);
    }
    mpp_frame_deinit(&frame);                // ⚠️ 每一帧用完都要释放，缓存才会还给解码器
}

// ④ 结束
mpi->reset(ctx);
mpp_destroy(ctx);
```

### 5.3 info change（分辨率变化通知）⭐
- **第一次调用 `decode_get_frame`，拿到的一般不是图像，而是一个 info_change 帧**，里面告诉你宽、高、stride 和需要的 `buf_size`。
- 码流中途改变分辨率，或者位深变了（8bit 变 10bit），也会再来一次 info change。
- 处理流程：按新参数（重新）分配缓存池 → 发送 `MPP_DEC_SET_EXT_BUF_GROUP` → 发送 `MPP_DEC_SET_INFO_CHANGE_READY`。
- 💡 相当于 MediaCodec 里的 `INFO_OUTPUT_FORMAT_CHANGED`。

### 5.4 解码输出的三种内存模式（📖 3.3.2 节）
| 模式 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| ① 纯内部分配 | 不设置 BUF_GROUP，info change 时直接发送 READY | 最简单，适合评估性能 | 内存用量不受控制；解码器销毁时如果还有帧没释放，可能崩溃；很难做零拷贝显示 |
| **② 半内部分配**（mpi_dec_test 默认用的） | 按 `buf_size` 创建 group，再设置给解码器；可以用 `mpp_buffer_group_limit_config` 限制内存用量 | 简单，能在一定程度上控制内存 | 内存用量会有波动；也不容易零拷贝显示 |
| ③ 纯外部分配 | 创建一个空的 external group，把外部 dmabuf 的 fd commit 进去 | **零拷贝显示**（Android 就是这么做的） | 比较难，要改应用代码的结构 |

纯外部分配时，缓存大小的估算方法：
- 像素数据：`hor_stride × ver_stride × 3 / 2`，另外再加上附加信息：`hor_stride × ver_stride / 2`
- **块数**：H.264/H.265 至少要 **20 块以上**，其他格式至少 10 块以上。分配太少，解码器会卡住。

### 5.5 解码器其他要点
- 输入队列默认最多缓存 **4 个 packet**，送得太快会返回错误，这时要等一下再送。
- MPI 接口是**线程安全的**：单线程写法参考 `mpi_dec_test`，多线程写法（一个线程送数据、一个线程取结果）参考 `mpi_dec_mt_test`。
- 送完带 EOS 标志的最后一包以后，解码器进入 EOS 状态，要调用 **`reset` 才能再用**。

### 5.6 常用的解码 control 命令
| 命令 | 作用 |
|---|---|
| `MPP_DEC_SET_EXT_BUF_GROUP` | 设置输出缓存池 |
| `MPP_DEC_SET_INFO_CHANGE_READY` | info change 处理完了，让解码器继续 |
| `MPP_DEC_SET_CFG` + `mpp_dec_cfg_*` | 🔍 新的配置接口（比如 split_parse） |
| `MPP_DEC_SET_PARSER_SPLIT_MODE` | 旧写法：打开内部分帧（在 init 之前调用） |
| `MPP_DEC_SET_PARSER_FAST_MODE` | 快速解析，提高并行度，但会影响错误标记 |
| `MPP_DEC_SET_IMMEDIATE_OUT` | H.264 立即输出（**降低延迟**，适合实时流） |
| `MPP_DEC_SET_DISABLE_ERROR` | 忽略错误，所有帧都输出 |
| `MPP_DEC_SET_OUTPUT_FORMAT` | 设置 JPEG 解码的输出格式 |

---

## 6. 编码器（📖 3.4～3.6 节）⭐

### 6.1 编码主循环（🔍 根据 mpi_enc_test.c 整理）
```c
// ① 创建 + 设置超时 + 初始化
mpp_create(&ctx, &mpi);
MppPollType timeout = MPP_POLL_BLOCK;                     // -1 表示阻塞等待
mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);     // 让 encode_get_packet 阻塞等待结果
mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);

// ② 设置编码参数（先 GET 默认值，改完再 SET）
mpp_enc_cfg_init(&cfg);
mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
mpp_enc_cfg_set_s32(cfg, "prep:width",      1920);
mpp_enc_cfg_set_s32(cfg, "prep:height",     1080);
mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", 1920);        // MPP_ALIGN(width, 16)
mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", 1088);        // MPP_ALIGN(height, 16)
mpp_enc_cfg_set_s32(cfg, "prep:format",     MPP_FMT_YUV420SP);   // NV12
mpp_enc_cfg_set_s32(cfg, "rc:mode",         MPP_ENC_RC_MODE_CBR);
mpp_enc_cfg_set_s32(cfg, "rc:bps_target",   4000000);     // 4 Mbps
mpp_enc_cfg_set_s32(cfg, "rc:bps_max",      4000000 * 17 / 16);
mpp_enc_cfg_set_s32(cfg, "rc:bps_min",      4000000 * 15 / 16);
mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num",   30);  /* fps_in_denorm / fps_out_* 也要设 */
mpp_enc_cfg_set_s32(cfg, "rc:gop",          60);          // 默认是 2 倍帧率，即 2 秒一个 I 帧
mpp_enc_cfg_set_s32(cfg, "codec:type",      MPP_VIDEO_CodingAVC);
mpp_enc_cfg_set_s32(cfg, "h264:profile",    100);         // 100 = High
mpp_enc_cfg_set_s32(cfg, "h264:level",      40);          // 4.0
mpp_enc_cfg_set_s32(cfg, "h264:cabac_en",   1);
mpi->control(ctx, MPP_ENC_SET_CFG, cfg);

// ③ （推荐）每个 IDR 帧前面都带上 SPS/PPS，这样中途接入的播放器也能解码
MppEncHeaderMode hm = MPP_ENC_HEADER_MODE_EACH_IDR;
mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &hm);
//    或者单独取一次头信息，写到文件最前面：
//    mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, hdr_packet);

// ④ 申请输入缓存（必须是硬件内存，DRM 类型）
mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE);
mpp_buffer_get(grp, &frm_buf, frame_size);       // NV12：hor_stride * ver_stride * 3/2（test 里按 64 对齐）

// ⑤ 循环：送一帧 → 取码流
void *ptr = mpp_buffer_get_ptr(frm_buf);  /* 把 YUV 数据写进去 */
mpp_frame_init(&frame);
mpp_frame_set_width(frame, 1920);      mpp_frame_set_height(frame, 1080);
mpp_frame_set_hor_stride(frame, 1920); mpp_frame_set_ver_stride(frame, 1088);
mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
mpp_frame_set_buffer(frame, frm_buf);
if (最后一帧) mpp_frame_set_eos(frame, 1);
mpi->encode_put_frame(ctx, frame);     // 阻塞，直到硬件读完这一帧
mpp_frame_deinit(&frame);

mpi->encode_get_packet(ctx, &packet);
if (packet) {
    fwrite(mpp_packet_get_pos(packet), 1, mpp_packet_get_length(packet), fp);
    eos = mpp_packet_get_eos(packet);
    mpp_packet_deinit(&packet);
}

// ⑥ 结束
mpi->reset(ctx);
mpp_destroy(ctx);
mpp_enc_cfg_deinit(cfg);
mpp_buffer_put(frm_buf);
mpp_buffer_group_put(grp);
```

### 6.2 编码器的几个关键点
| 要点 | 说明 |
|---|---|
| **输入必须是硬件内存** | 编码器**不能直接读 CPU `malloc` 出来的内存**，要先把数据拷进 MppBuffer（会影响效率）。**最好直接用 dmabuf 输入**，比如摄像头 V4L2 的 buffer，做到零拷贝 |
| `encode_put_frame` 是**阻塞**的 | 要等硬件读完输入图像才返回，这是为了避免拷贝大块图像数据 |
| `encode_get_packet` 默认**非阻塞** | 可能取不到数据。mpi_enc_test 里用 `MPP_SET_OUTPUT_TIMEOUT` 把它改成了阻塞 |
| **码流格式** | 输出固定是 **Annex-B 格式，也就是带 `00 00 00 01` 起始码**。需要 AVCC 格式（比如封装 MP4）时，要自己去掉起始码 |
| **SPS/PPS 头信息** | 默认和图像数据分开输出。取头信息用 `MPP_ENC_GET_HDR_SYNC`（推荐，线程安全，需要自己分配 packet，用完自己释放）；`MPP_ENC_GET_EXTRA_INFO` 是旧命令，**不要再用**。**要在参数配置完成以后再取**，否则拿到的是旧的头信息 |
| 输出码流会拷贝一次 | 简单接口没法指定输出缓存，所以一定有一次拷贝。码流比较小，可以接受。要零拷贝就得用 MppTask + enqueue/dequeue |
| 请求 I 帧 | `mpi->control(ctx, MPP_ENC_SET_IDR_FRAME, NULL)`，下一帧会编成 IDR 帧（比如有新客户端接入时用） |
| 两类控制信息 | 全局参数（码率、宽高）→ `control`；**每帧单独的**参数（OSD、用户数据）→ 挂在 MppFrame 的 **MppMeta** 上 |

### 6.3 宽高与 stride（📖 3.6.1 节）⭐ 最容易踩的坑
以 1920×1080 的 NV12 为例：
- **推荐做法**：`ver_stride = 1088`（对齐到 16），按 `1920 × 1088 × 3/2` 分配内存，Y 分量和 UV 分量之间空出 8 行。配置为 宽 1920、高 1080、hor_stride 1920、ver_stride 1088。
- **如果 Y 和 UV 紧挨着**（ver_stride = 1080）：编码器按 16 对齐读数据，读到 Y 的下边缘时会读到 UV 数据，读到 UV 的下边缘时会读出界。所以要**多分配 `1920 × 4` 字节**的填充空间，否则会访问越界。
- 🔍 mpi_enc_test 的默认做法：`hor_stride = MPP_ALIGN(width, 16)`，`ver_stride = MPP_ALIGN(height, 16)`，缓存大小再按 64 对齐：`MPP_ALIGN(hor,64) × MPP_ALIGN(ver,64) × 3/2`。
- ⚠️ **帧数据的 stride 必须和 `prep:hor_stride`、`prep:ver_stride` 一致**。摄像头出来的数据有它自己的 stride，要按实际值设置。

### 6.4 常用编码参数（MppEncCfg 的字符串 key）
**码率控制 `rc:`**
| key | 说明 |
|---|---|
| `rc:mode` | **VBR=0、CBR=1、FIXQP=2、AVBR=3**（🔍 `rk_venc_rc.h`） |
| `rc:bps_target` / `bps_max` / `bps_min` | 目标码率 / 最高码率 / 最低码率（单位 bps） |
| `rc:fps_in_num/_denorm/_flex`、`rc:fps_out_*` | 输入和输出帧率（用分数表示），flex=1 表示可变帧率 |
| **`rc:gop`** | I 帧间隔：0 = 只有第一帧是 I 帧，1 = 全部是 I 帧，N = 每 N 帧一个 I 帧。**默认是 2 倍帧率** |
| `rc:qp_init` / `qp_min` / `qp_max` / `qp_min_i` / `qp_max_i` | QP 的初始值和范围（QP 越小，画质越好，码率越高） |
| `rc:qp_ip` | I 帧和 P 帧的 QP 差值（0～8） |
| `rc:drop_mode` / `drop_thd` / `drop_gap` | 码率超标时丢帧的策略 |
| `rc:super_mode` / `super_i_thd` / `super_p_thd` | 超大帧的处理方式：丢弃或重新编码 |
| `rc:debreath_en` / `debreath_strength` | 去除"呼吸效应"（I 帧出现时画面周期性地一清一糊） |
| `base:low_delay` | 低延迟模式 |

码率控制模式怎么选：
| 模式 | 特点 | 适用场景 |
|---|---|---|
| **CBR** | 码率固定，由 `bps_target` 决定 | **直播、RTSP 推流**（带宽固定） |
| **VBR** | 码率在 min 和 max 之间浮动 | 录像（画质优先） |
| **AVBR** | 静止画面时靠近 min，运动画面时靠近 max，平均接近 target | **监控摄像头**（大部分时间是静止画面，很省带宽） |
| FIXQP | 固定 QP | 调试、评估性能 |

**预处理 `prep:`**：`width`、`height`、`hor_stride`、`ver_stride`、`format`、`rotation`（0/1/2/3 = 逆时针旋转 0/90/180/270 度）、`mirroring`（1 = 水平镜像，2 = 垂直镜像）、`colorspace`、`colorrange`

**编码格式 `codec:` / `h264:` / `h265:`**
| key | 说明 |
|---|---|
| `codec:type` | 必须和 `mpp_init` 时传的格式一致 |
| `h264:profile` | 66 = Baseline，77 = Main，**100 = High** |
| `h264:level` | 40/41 = 1080p@30，42 = 1080p@60，50/51 = 4K@30，52 = 4K@60。**一般用 4.1 就够了** |
| `h264:cabac_en` | 0 = CAVLC，1 = CABAC（压缩率更高，Baseline 不支持） |
| `h264:trans8x8` | 8x8 变换，只有 High profile 能开 |
| `h265:profile` | MPP 固定是 1（Main） |
| `h264:dblk_disable` / `h265:sao_*_disable` | 关闭去块滤波或 SAO（一般不用关） |
| `split:mode` / `split:arg` | slice 切分方式：按字节或按宏块数。**低延迟传输时有用** |
| `jpeg:q_factor` | JPEG 质量，1～99，默认 80 |

> `h264:qp_*`、`h265:qp_*` 这些是旧的写法，新代码统一用 `rc:qp_*`。

### 6.5 常用的编码 control 命令
| 命令 | 状态 |
|---|---|
| `MPP_ENC_SET_CFG` / `MPP_ENC_GET_CFG` | ✅ **主要的配置命令** |
| `MPP_ENC_GET_HDR_SYNC` | ✅ 获取 SPS/PPS/VPS |
| `MPP_ENC_SET_HEADER_MODE` | ✅ 🔍 mpi_enc_test 用它设置 `EACH_IDR`（每个 IDR 前都带头信息） |
| `MPP_ENC_SET_IDR_FRAME` | ✅ 请求下一帧编成 I 帧 |
| `MPP_ENC_SET_REF_CFG` | 高级功能：长期参考帧、时域分层（SVC） |
| `MPP_ENC_SET_OSD_PLT_CFG` / `MPP_ENC_SET_OSD_DATA_CFG` | OSD 调色板（新代码用 MppMeta 的 `KEY_OSD_DATA`） |
| `MPP_ENC_SET_PREP_CFG` / `RC_CFG` / `CODEC_CFG` | ❌ **已经废弃**，改用 `SET_CFG` |
| `MPP_ENC_GET_EXTRA_INFO`、`SET_SEI_CFG`、`SET_ROI_CFG`、`SET_QP_RANGE`、`SET_SPLIT` | ❌ 已经废弃，不要用 |

🔍 **指南里有一处写法过时了**：3.5.2 节说"rkvenc 系列只支持 H.264 编码，目前只配备于 RV1109/RV1126"。但 RK3588 的 Datasheet 写明它支持 **H.264 和 H.265 编码，最高 8K@30fps**，所以对 RK3588 来说这句话不成立。**以芯片 Datasheet 为准。**

---

## 7. 自带的测试程序（📖 第四章）

> 指南里的例子是按 Android 32 位平台写的。在 RK3588 的 Buildroot 系统上，这些程序在 `/usr/bin/` 下（工具链的 sysroot 里也有 `mpi_enc_test` 和 `mpi_dec_test`）。

### 7.1 解码测试 `mpi_dec_test`
```bash
mpi_dec_test -t 7 -i test.h264 -n 30 -o out.yuv      # H.264 解码前 30 帧，输出成 NV12
mpi_dec_test -t 16777220 -i test.h265 -v f            # H.265 解码，每秒打印一次帧率
```
| 参数 | 含义 |
|---|---|
| `-i` / `-o` | 输入码流文件 / 输出 YUV 文件 |
| **`-t`** | **编码格式（必填）** |
| `-w` / `-h` | 宽和高（可选） |
| `-f` | 输出格式，默认是 NV12 |
| `-n` | 最多解码多少帧 |
| `-s` | 同时开几个实例（测试多路性能） |
| `-v q` / `-v f` | 静默模式 / 每秒打印帧率 |

日志怎么看：
- `decode_get_frame get info changed found` → 收到了 info change
- `decoder require buffer w:h [1920:1080] stride [1920:1088] buf_size 4177920` → 解码器要求分配的缓存
- `decode 10 frames time 263ms delay 69ms fps 113.99` → 用时、首帧延迟、帧率
- `test success max memory 19.92 MB` → 成功完成，最大内存占用

### 7.2 编码测试 `mpi_enc_test`
```bash
mpi_enc_test -w 1920 -h 1080 -t 7 -i in.yuv -o out.h264 -n 300
mpi_enc_test -w 1920 -h 1080 -t 7 -o bar.h264 -n 300          # 不指定 -i 时会自己生成彩条图像来编码
mpi_enc_test -w 3840 -h 2160 -t 16777220 -rc 1 -bps 8000000:8000000:8000000 -o 4k.h265 -n 300
```
| 参数 | 含义 |
|---|---|
| **`-w` / `-h` / `-t`** | **宽、高、编码格式（必填）** |
| `-i` / `-o` | 输入 YUV 文件 / 输出码流文件 |
| `-hstride` / `-vstride` | stride |
| `-f` | 输入格式，默认是 NV12 |
| `-n` | 编码多少帧 |
| **`-rc`** | **0 = VBR，1 = CBR，2 = FIXQP，3 = AVBR** |
| **`-bps`** | `target:min:max`，单位 bps |
| `-fps` | `in_num:in_den:in_flex/out_num:out_den:out_flex` |
| `-qc` | `qp_init/min/max/min_i/max_i` |
| `-g` | GOP 参考模式（时域分层等） |
| `-s` | 实例数 |
| `-v f` | 每秒打印帧率 |

日志怎么看：
- `MPP_ENC_SET_RC_CFG bps 7776000 [486000 : 8262000] fps [30:30] gop 60` → 默认码率约 7.8Mbps，GOP 是 60
- `chn 0 encoded frame 0 size 218616 qp 11` → 每一帧的大小和 QP
- `encode 30 frames time 628 ms delay 4 ms fps 47.72 bps 10265048` → 总用时、首帧延迟、帧率、实际码率

**编码器参数也可以通过环境变量调**（Linux 上用 `export 变量名=值`）：`split_mode`、`split_arg`、`split_out`、`sei_mode`、`gop_mode`、`roi_enable`、`osd_enable`、`user_data_enable`、`constraint_set`。

### 7.3 其他测试工具
| 工具 | 用途 |
|---|---|
| `mpp_info_test` | 打印 MPP 版本（**反馈问题时要附上**） |
| `mpp_buffer_test` | 测试内核的内存分配器 |
| `mpp_mem_test` | 测试 C 库的内存分配 |
| `mpp_runtime_test` | 检查运行环境 |
| `mpp_platform_test` | 读取芯片平台信息 |

---

## 8. 速查：`-t` 编码格式和 `-f` 像素格式的数值

🔍 根据 `inc/rk_type.h`（`MppCodingType`）和 `inc/mpp_frame.h`（`MppFrameFormat`）计算：

| 格式 | 枚举 | **`-t` 的值** |
|---|---|---|
| MPEG-2 | `MPP_VIDEO_CodingMPEG2` | 2 |
| MPEG-4 | `MPP_VIDEO_CodingMPEG4` | 4 |
| **H.264** | `MPP_VIDEO_CodingAVC` | **7** |
| MJPEG | `MPP_VIDEO_CodingMJPEG` | 8 |
| VP8 | `MPP_VIDEO_CodingVP8` | 9 |
| VP9 | `MPP_VIDEO_CodingVP9` | 10 |
| **H.265** | `MPP_VIDEO_CodingHEVC` | **16777220**（0x01000004） |
| AV1 | `MPP_VIDEO_CodingAV1` | 16777224（0x01000008） |

| 像素格式 | 枚举 | **`-f` 的值** |
|---|---|---|
| **NV12**（YYYY… UVUV…） | `MPP_FMT_YUV420SP` | **0**（默认） |
| NV12 10bit | `MPP_FMT_YUV420SP_10BIT` | 1 |
| NV16 | `MPP_FMT_YUV422SP` | 2 |
| **I420**（YYYY… U… V…） | `MPP_FMT_YUV420P` | **4** |
| NV21 | `MPP_FMT_YUV420SP_VU` | 5 |
| YUY2 / YUYV | `MPP_FMT_YUV422_YUYV` | 8 |
| RGB888 | `MPP_FMT_RGB888` | 65542（0x10006） |
| BGR888 | `MPP_FMT_BGR888` | 65543 |
| ARGB8888 / ABGR8888 | | 65546 / 65547 |
| BGRA8888 / RGBA8888 | | 65548 / 65549 |

> 说明：H.265、AV1 这类格式的枚举值是从 `0x01000000` 开始编号的，RGB 类格式是从 `0x10000` 开始编号的，所以数字看起来很大。

---

## 9. 编译 MPP（📖 第五章）

- 源码：https://github.com/rockchip-linux/mpp （release 分支是稳定版，develop 分支是开发版）。**SDK 里已经带了源码**：`external/mpp`。
- 用 CMake 编译（3.x 版本）。Linux 下改 `build/linux/arm/arm.linux.cross.cmake` 里的工具链路径，然后执行 `make-Makefiles.bash` 和 `make -j`。
- 🔍 **我们一般不用自己编 MPP**：工具链 sysroot 里已经有 `librockchip_mpp.so` 和头文件，直接链接就行（参见 `rk_code/hello_mpp`）：
  ```cmake
  target_link_libraries(xxx rockchip_mpp)
  ```
  头文件的写法是 `#include <rockchip/rk_mpi.h>`。

---

## 10. 常见问题（📖 第六章）

| 现象 | 原因和处理 |
|---|---|
| 内核日志出现 `vpu_service_ioctl: unknow vpu service ioctl cmd 40086c01` | **正常现象**。MPP 会按不同版本的驱动接口逐个尝试，只打印一次，可以忽略 |
| MPP 运行异常 | 先看日志里有没有"打开内核设备失败"。如果有，检查内核和设备树里是不是启用了编解码器节点（`/dev/mpp_service`） |
| Android 64 位下 `undefined reference to __system_property_get` | NDK 的问题，和我们没关系 |

---

## 11. 我自己总结的要点和坑 ⭐

1. **拿到的第一帧往往不是图像**：解码时第一次 `decode_get_frame` 一般拿到的是 info_change，必须处理，否则后面拿不到图像。
2. **每个 frame 和 packet 用完都要 `deinit`**，否则缓存池会被占满，解码器就卡住不动了。
3. **stride 不等于 width**：YUV 数据每一行后面可能有填充。写文件、做显示、交给 RGA 处理时，都要按 stride 逐行处理。1080 要对齐到 1088。
4. **编码器只认硬件内存**：`malloc` 的内存不能直接送进去。要么拷进 MppBuffer，要么（更好的做法）直接用摄像头的 dmabuf。
5. **输出的码流是 Annex-B 格式**（带 `00 00 00 01` 起始码），可以直接存成 `.h264` 文件，也可以直接送给 RTSP/RTP；封装成 MP4 时要转成 AVCC 格式。
6. **推流时记得设置 `MPP_ENC_HEADER_MODE_EACH_IDR`**，否则中途接入的客户端没有 SPS/PPS，没法解码。
7. **看到 `SET_RC_CFG`、`SET_PREP_CFG` 这类旧示例代码，要改成 `MPP_ENC_SET_CFG` 加字符串 key 的写法**。网上很多老代码还在用旧写法。
8. **实时场景要降低延迟**：解码器打开 `MPP_DEC_SET_IMMEDIATE_OUT`，编码器打开 `base:low_delay` 或 slice 切分。
9. **反馈问题时要附上 `mpp_info_test` 的输出**。

---

## 12. 下一步

- [ ] 读 `external/mpp/test/mpi_enc_test.c`，对照本笔记第 6.1 节理解每一步
- [ ] 读 `external/mpp/test/mpi_dec_test.c`，对照第 5.2 节
- [ ] 在 `rk_code/` 下写 `yuv2h264`：读 YUV 文件 → MPP 编码 → 写出 `.h264` 文件（先在 Mac 上编译通过）
- [ ] 板子到了以后：跑 `mpi_dec_test -t 7 -i aaa.264`、`mpi_enc_test`，对比 CPU 占用
- [ ] 下一份文档：`docs/cn/Common/RGA/Rockchip_Developer_Guide_RGA_CN.pdf`
