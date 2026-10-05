# mpp/ — MPP 核心实现

MPP（Media Process Platform）的核心库，最终编译为 `librockchip_mpp.so`（动态库）和 `librockchip_mpp.a`（静态库）。

## 目录结构

```
mpp/
├── inc/            内部头文件（MppCtx、MppImpl、编解码 cfg 等）
├── base/           基础组件（Buffer、Packet、Frame、Meta、Task、码率配置等）
│   ├── inc/        base 对外头文件
│   └── test/       base 单元测试
├── codec/          编解码器（parser / 语法解析）
│   ├── dec/        解码器：av1 avs avs2 h263 h264 h265 jpeg m2v mpg4 vp8 vp9
│   ├── enc/        编码器：h264 h265 jpeg vp8
│   └── rc/         码率控制（RC）：CBR/VBR/AVBR 等策略实现
├── hal/            硬件抽象层（HAL）—— 将语法结构转为硬件寄存器
│   ├── rkdec/      解码 HAL：av1d avs2d avsd h264d h265d vp9d + vdpu 通用
│   ├── rkenc/      编码 HAL：h264e h265e jpege
│   ├── vpu/        旧版 VPU HAL（兼容）
│   ├── dummy/      空实现（占位/测试）
│   └── common/     HAL 公共工具
├── vproc/          视频后处理
│   ├── iep/        IEP 去隔行
│   ├── iep2/       IEP2 去隔行
│   ├── rga/        RGA 缩放/旋转/格式转换
│   └── vdpp/       VDPP 视频显示后处理
├── common/         编解码公共语法定义头文件（*_syntax.h）
├── legacy/         旧版 VPU API 兼容层（vpu_api_legacy）
├── mpi.cpp         MPI 接口实现（用户调用的 mpp_create/mpp_init/encode/decode 等）
├── mpp.cpp         MPP 上下文管理（创建/销毁/put/get 流程调度）
├── mpp_impl.cpp    MPP 内部实现细节
├── mpp_info.cpp    版本信息
└── CMakeLists.txt  构建脚本
```

## 数据流概览

```
应用层 (mpi.cpp — MPI 接口)
    │
    ▼
MPP 上下文 (mpp.cpp — 流程调度)
    │
    ├──► codec/dec/ (parser 解析码流 → 语法结构)
    │        │
    │        ▼
    │    hal/rkdec/ (语法结构 → 硬件寄存器 → 硬件解码)
    │        │
    │        ▼
    │    vproc/ (可选：去隔行、后处理)
    │
    └──► codec/enc/ (原始帧 → 语法结构)
             │
             ├── codec/rc/ (码率控制决策)
             │
             ▼
         hal/rkenc/ (语法结构 → 硬件寄存器 → 硬件编码)
```

## 关键源文件说明

| 文件 | 作用 |
|---|---|
| `mpi.cpp` | MPI（Media Process Interface）对外接口，封装 `mpp_create`、`decode_put/get_packet/frame`、`encode` 等 |
| `mpp.cpp` | MPP 核心上下文，管理编解码线程、输入输出队列、put/get 流程 |
| `mpp_impl.cpp` | MPP 内部实现辅助（调试、日志、状态管理） |
| `base/mpp_buffer.cpp` | Buffer 管理（DMA-BUF / ION 内存分配） |
| `base/mpp_packet.cpp` | Packet（码流数据包）管理 |
| `base/mpp_frame.cpp` | Frame（图像帧）管理 |
| `base/mpp_enc_cfg.cpp` | 编码配置参数（分辨率、码率、GOP 等） |
| `codec/rc/rc_model_v2.c` | v2 码率控制模型（核心 RC 算法） |

## 编译产物

- **动态库**: `librockchip_mpp.so`（链接 mpp_codec + mpp_hal + mpp_vproc + mpp_base）
- **静态库**: `librockchip_mpp.a`（合并了 osal、mpp_base 等所有 .o）

## 支持的编解码格式

| 类型 | 格式 |
|---|---|
| 解码 | H.264, H.265, VP9, VP8, AV1, AVS, AVS2, MPEG-2, MPEG-4, H.263, JPEG |
| 编码 | H.264, H.265, VP8, JPEG |
