# osal/ — 操作系统抽象层

OSAL（OS Abstraction Layer）为 MPP 提供跨平台的系统接口封装，使上层代码不直接依赖特定 OS API。支持 Linux、Android、Windows 三个平台。

## 目录结构

```
osal/
├── inc/              对外头文件（其他模块 include 这里）
├── allocator/        内存分配器实现
│   ├── allocator_ion.c        ION 分配器（旧版 Android/Linux 内核）
│   ├── allocator_drm.c        DRM 分配器（通用 Linux）
│   ├── allocator_dma_heap.c   DMA-Heap 分配器（Linux 5.6+ 新接口）
│   ├── allocator_ext_dma.c    外部 DMA-BUF（用户传入已有的 fd）
│   └── allocator_std.c        标准 malloc 分配器（调试/非硬件场景）
├── driver/           硬件驱动交互
│   ├── mpp_service.c          MPP Service 驱动通信（新版内核驱动）
│   ├── vcodec_service.c       VCodec Service 驱动通信（旧版内核驱动）
│   ├── mpp_server.cpp         MPP Server 进程间服务
│   └── mpp_device.c           设备节点抽象（自动选择 mpp_service / vcodec_service）
├── linux/            Linux 平台特定实现（环境变量、日志、内存）
├── android/          Android 平台特定实现
├── windows/          Windows 平台特定实现
├── test/             OSAL 单元测试
│
│  ── 核心功能源文件 ──
├── mpp_mem.cpp       内存管理（malloc/calloc/realloc/free 带统计和泄漏检测）
├── mpp_allocator.cpp 分配器统一入口（根据平台和内核能力选择具体 allocator）
├── mpp_dmabuf.cpp    DMA-BUF 操作封装（import/export fd）
├── mpp_thread.cpp    线程封装（创建、同步、条件变量、信号）
├── mpp_time.cpp      时间工具（计时器、sleep、性能统计）
├── mpp_log.cpp       日志系统
├── mpp_env.cpp       环境变量读取（调试开关等）
├── mpp_platform.cpp  平台检测（芯片型号、编解码能力）
├── mpp_soc.cpp       SoC 信息（RK3588/RK3566 等芯片的硬件能力表）
├── mpp_runtime.cpp   运行时环境检查
├── mpp_list.cpp      链表实现
├── mpp_queue.cpp     队列实现
├── mpp_lock.cpp      锁封装
├── mpp_eventfd.cpp   eventfd 事件通知
├── mpp_mem_pool.cpp  内存池（预分配、减少碎片）
├── mpp_common.cpp    公共工具函数
├── mpp_compat.cpp    兼容性处理
├── mpp_trace.cpp     trace 跟踪
├── mpp_callback.cpp  回调机制
└── CMakeLists.txt    构建脚本
```

## 平台适配机制

OSAL 通过 `os_env.c`、`os_log.c`、`os_mem.c` 三个文件做平台分离，每个平台目录下各有一份实现：

| 接口 | Linux | Android | Windows |
|------|-------|---------|---------|
| 环境变量 | `/proc` 文件系统 | Android property | 注册表/环境变量 |
| 日志输出 | `printf` / syslog | `__android_log_print` | `OutputDebugString` |
| 内存调试 | 标准 libc | 标准 libc | Win32 API |

## 内存分配器选择

MPP 的硬件编解码需要物理连续内存，`mpp_allocator.cpp` 会按优先级自动选择：

1. **DMA-Heap** — Linux 5.6+ 推荐方式，通过 `/dev/dma_heap/` 分配
2. **DRM** — 通用 Linux，通过 `/dev/dri/` 分配 GEM buffer
3. **ION** — 旧版 Android/Linux 内核，通过 `/dev/ion` 分配
4. **External DMA** — 不分配，使用外部传入的 DMA-BUF fd
5. **Std** — 标准 malloc，仅用于调试，无法硬件加速

## 驱动通信

MPP 通过 `/dev/mpp_service`（新版）或 `/dev/vpu_service`（旧版）与内核驱动通信：

- **mpp_service**: 新版统一驱动，支持 RK3588 等新芯片，通过 ioctl 下发寄存器配置
- **vcodec_service**: 旧版驱动，兼容老芯片（RK3288 等）
- **mpp_device.c**: 自动探测并选择可用的驱动接口

## 测试程序

`test/` 目录下的测试程序可独立运行，用于验证各 OSAL 组件：

| 测试 | 验证内容 |
|------|---------|
| mpp_platform_test | 芯片平台检测、编解码能力查询 |
| mpp_runtime_test | 运行时环境检查 |
| mpp_mem_test | 内存分配/释放 |
| mpp_mem_pool_test | 内存池分配 |
| mpp_dmabuf_test | DMA-BUF 操作 |
| mpp_thread_test | 线程创建与同步 |
| mpp_time_test | 计时精度 |
| mpp_log_test | 日志输出 |
| mpp_env_test | 环境变量读写 |
| mpp_eventfd_test | eventfd 通知 |
| mpp_trace_test | trace 功能 |
