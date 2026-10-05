# inc/ — MPP 对外公共头文件

这是 MPP 库暴露给应用层的公共 API 头文件目录。应用程序通过 `#include` 这里的头文件来使用 MPP 的编解码功能。

## 头文件分类

### 核心 API（应用必用）

| 头文件 | 作用 |
|--------|------|
| `rk_mpi.h` | **MPI 主接口** — `mpp_create`、`mpp_init`、`decode`/`encode`、`put_packet`/`get_frame` 等 |
| `rk_mpi_cmd.h` | **控制命令** — `MpiCmd` 枚举，用于 `control()` 接口设置/获取编解码参数 |
| `rk_type.h` | **基础类型** — `RK_U32`、`RK_S32`、`MppCtx`、`MppApi`、`MppCodingType` 等 |
| `mpp_err.h` | **错误码** — `MPP_OK`、`MPP_NOK`、`MPP_ERR_xxx` 等 |

### 数据结构

| 头文件 | 作用 |
|--------|------|
| `mpp_frame.h` | **帧（Frame）** — 图像帧属性：宽高、stride、格式（`MppFrameFormat`）、色彩空间、HDR 信息等 |
| `mpp_packet.h` | **包（Packet）** — 码流数据包：创建、设置/获取 data/size/pts/dts/eos 等 |
| `mpp_buffer.h` | **缓冲区（Buffer）** — 内存管理：Buffer/BufferGroup 的创建、获取 fd/ptr/size |
| `mpp_task.h` | **任务（Task）** — 高级 Task 模式接口（`poll`/`dequeue`/`enqueue`） |
| `mpp_meta.h` | **元数据（Meta）** — 键值对形式的附加信息（QP、码率统计、ROI 等） |

### 编码配置

| 头文件 | 作用 |
|--------|------|
| `rk_venc_cmd.h` | **编码控制命令**（最大的头文件）— 编码参数结构体：`prep`/`rc`/`codec`/`split`/`roi` 等全部编码配置 |
| `rk_venc_cfg.h` | **编码配置类型** — `MppEncCfgSet` 顶层配置结构 |
| `rk_venc_rc.h` | **码率控制** — RC 模式定义：CBR/VBR/AVBR/CQP 等 |
| `rk_venc_ref.h` | **参考帧** — 编码参考帧模式配置（时域分层等） |

### 解码配置

| 头文件 | 作用 |
|--------|------|
| `rk_vdec_cmd.h` | **解码控制命令** — 解码参数配置 |
| `rk_vdec_cfg.h` | **解码配置类型** — 解码配置结构体 |

### 其他

| 头文件 | 作用 |
|--------|------|
| `mpp_log.h` / `mpp_log_def.h` | 日志接口 |
| `mpp_rc_api.h` / `mpp_rc_defs.h` | 码率控制 API 和定义（内部用为主） |
| `mpp_compat.h` | 兼容性配置 |
| `rk_hdr_meta_com.h` | HDR 元数据定义 |
| `vpu.h` / `vpu_api.h` | 旧版 VPU API（已过时，保留兼容） |

## 典型使用方式

编码应用一般需要 include：

```c
#include "rk_mpi.h"        // MPI 接口
#include "mpp_frame.h"     // MppFrame
#include "mpp_packet.h"    // MppPacket
#include "mpp_buffer.h"    // MppBuffer
#include "rk_venc_cmd.h"   // 编码参数
```

解码应用一般需要 include：

```c
#include "rk_mpi.h"        // MPI 接口
#include "mpp_frame.h"     // MppFrame
#include "mpp_packet.h"    // MppPacket
```

## 与其他目录的关系

- `inc/` — 对外公共头文件（本目录，应用层使用）
- `mpp/inc/` — MPP 内部头文件（库内部使用，不对外暴露）
- `mpp/base/inc/` — base 组件内部头文件
- `mpp/codec/inc/` — codec 组件内部头文件
- `osal/inc/` — OSAL 内部头文件
