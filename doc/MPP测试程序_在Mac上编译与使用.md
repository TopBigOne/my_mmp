# MPP 测试程序：在 Mac 上用 CLion 编译与使用

> **适用环境**：Mac（Apple M 系列）+ CLion 2026（自带 CMake 4.3）+ 正点原子 RK3588 5.10 交叉编译工具链
> **MPP 源码**：`/Users/dev/Documents/AV/rk_work/rk_code/external/mpp`（版本 1.0.6，2024-06-12）
> **验证日期**：2026-10-02 ✅ `mpi_enc_test`、`mpi_dec_test`、`mpp_info_test` 编译通过，用时约 20 秒，输出是 `ELF 64-bit ARM aarch64`

---

## 一、在 CLion 里打开 MPP 工程

### 1.1 CMake 配置
**Settings（`Cmd + ,`）→ Build, Execution, Deployment → CMake**

| 字段 | 填什么 |
|---|---|
| Name | `rk_3588_debug`（名字随意） |
| Build type | `Debug` |
| Toolchain | `Default` |
| Generator | `Ninja` |
| **CMake options** | `-DCMAKE_TOOLCHAIN_FILE=/Users/dev/rk-toolchain/rk3588-toolchain.cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5` |

> ⚠️ **删掉 Mac 本机的 `Debug` 配置**（它生成的是 `cmake-build-debug` 目录），留着会干扰代码分析，导致跳转出问题。

### 1.2 为什么要加 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`
不加的话，CMake 配置会失败：
```
CMake Error at CMakeLists.txt:32 (cmake_minimum_required):
  Compatibility with CMake < 3.5 has been removed from CMake.
```
- MPP 源码比较老，`CMakeLists.txt` 第 32 行写的是 `cmake_minimum_required (VERSION 2.8.8)`
- CLion 自带的是 **CMake 4.3**，不再支持声明版本低于 3.5 的工程
- 加上这个参数，CMake 会按 3.5 的规则处理，**不用修改 MPP 源码**
- **配置失败的话，CLion 分析不出工程结构，代码就不能跳转**

配置成功后，日志里会有 `Configuring done`。中间出现 `Could NOT find Threads` 不用管，不影响编译和跳转。

### 1.3 确认能跳转
打开 `test/mpi_enc_test.c`，按住 **Cmd** 点击 `mpp_create`、`encode_put_frame` 等函数，能跳到 `mpp/mpi.cpp` 等源码里，就说明配置好了。

---

## 二、有哪些测试程序（共 38 个）

### 2.1 ⭐ 编解码 demo（重点）
| 程序 | 源码 | 作用 | 学习计划 |
|---|---|---|---|
| ⭐ **`mpi_enc_test`** | `test/mpi_enc_test.c` | **编码**：YUV 文件或摄像头 → H.264 / H.265 / JPEG / VP8 | Day 1、Day 3 |
| ⭐ **`mpi_dec_test`** | `test/mpi_dec_test.c` | **解码**：H.264 / H.265 / VP9 / AV1 / JPEG 等 → YUV | Day 2 |
| `mpi_dec_mt_test` | `test/mpi_dec_mt_test.c` | 多线程解码：一个线程送码流，一个线程取图像 | Day 5 |
| `mpi_dec_multi_test` | `test/mpi_dec_multi_test.c` | 多个解码实例同时运行 | Day 5、以后做 NVR |
| `mpi_dec_nt_test` | `test/mpi_dec_nt_test.c` | 非阻塞（no-thread）方式解码 | 了解即可 |
| `mpi_enc_mt_test` | `test/mpi_enc_mt_test.cpp` | 多线程编码（C++） | Day 5 |
| `mpi_rc2_test` | `test/mpi_rc2_test.c` | 码率控制测试：编码后再解码，比较画质 | Day 1 晚上的实验 |
| `vpu_api_test` | `test/vpu_api_test.c` | 旧版 VPU 接口 | 已过时，不用看 |

### 2.2 工具和信息类
| 程序 | 源码 | 作用 |
|---|---|---|
| `mpp_info_test` | `test/mpp_info_test.c` | 打印 MPP 版本（**反馈问题时附上**） |
| `mpp_platform_test` | `osal/test/mpp_platform_test.c` | 读取芯片平台信息 |
| `mpp_runtime_test` | `osal/test/mpp_runtime_test.c` | 检查运行环境 |
| `mpp_buffer_test` | `mpp/base/test/mpp_buffer_test.c` | 测试内核的内存分配器 |
| `mpp_dmabuf_test` | `osal/test/mpp_dmabuf_test.c` | 测试 DMA-BUF |
| `mpp_mem_test` | `osal/test/mpp_mem_test.c` | 测试 C 库的内存分配 |

### 2.3 其他硬件模块
| 程序 | 作用 |
|---|---|
| `rga_test` | RGA 的简单测试（RGA 主要还是看 `linux-rga/samples`） |
| `iep_test` / `iep2_test` | 图像增强（去隔行扫描） |
| `vdpp_test` | 视频显示后处理 |

### 2.4 单元测试（MPP 内部模块，可以不看）
`mpp_bit_test`、`mpp_bit_read_test`、`mpp_cluster_test`、`mpp_dec_cfg_test`、`mpp_enc_cfg_test`、`mpp_enc_ref_test`、`mpp_meta_test`、`mpp_packet_test`、`mpp_task_test`、`mpp_trie_test`、`mpp_env_test`、`mpp_eventfd_test`、`mpp_log_test`、`mpp_mem_pool_test`、`mpp_thread_test`、`mpp_time_test`、`mpp_trace_test`、`rc_api_test`、`rc_base_test`、`hwpq_test`

> 测试程序是在 `test/CMakeLists.txt` 里通过 `add_mpp_test(mpi_enc c)` 这样的宏添加的，宏会生成名为 `mpi_enc_test` 的目标。

---

## 三、编译

### 3.1 在 CLion 里编译
右上角的目标下拉框选 `mpi_enc_test`（或其他测试程序）→ **Cmd + F9**。
- 第一次编译会顺带把 MPP 库本身（`librockchip_mpp.so`）也编出来，大约二三十秒
- 编出来的程序在 `cmake-build-<配置名>/test/` 目录下

### 3.2 在命令行里编译
```bash
cd /Users/dev/Documents/AV/rk_work/rk_code/external/mpp
CMAKE=/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake
NINJA=/Applications/CLion.app/Contents/bin/ninja/mac/aarch64/ninja

$CMAKE -S . -B build_rk3588 -G Ninja -DCMAKE_MAKE_PROGRAM=$NINJA -DCMAKE_BUILD_TYPE=Debug \
       -DCMAKE_TOOLCHAIN_FILE=/Users/dev/rk-toolchain/rk3588-toolchain.cmake \
       -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev
$CMAKE --build build_rk3588 -j8 --target mpi_enc_test mpi_dec_test mpp_info_test

file build_rk3588/test/mpi_enc_test     # 应该显示 ELF 64-bit ... ARM aarch64
$CMAKE --build build_rk3588 --target help | grep _test     # 列出所有测试程序
```

---

## 四、在板子上运行

### 4.1 上传
```bash
IP=<板子IP>        # 用 day1_yuv2h264/run_get_board_ip.sh 查看
scp cmake-build-rk_3588_debug/test/mpi_enc_test cmake-build-rk_3588_debug/test/mpi_dec_test root@$IP:/userdata/av/
```
> 板子出厂系统的 `/usr/bin/` 下也自带了 `mpi_enc_test` 和 `mpi_dec_test`，直接在板子上执行也可以。自己编的版本主要是为了**能断点调试**。

### 4.2 `mpi_enc_test`：编码
```bash
# 把 NV12 文件编码成 H.264（-t 7 = H.264）
mpi_enc_test -w 1920 -h 1080 -t 7 -i /userdata/av/in_1080p_60f.nv12 -o /userdata/av/ref.h264 -n 60

# 编码成 H.265（-t 16777220 = H.265）
mpi_enc_test -w 1920 -h 1080 -t 16777220 -i /userdata/av/in_1080p_60f.nv12 -o /userdata/av/ref.h265 -n 60

# 不指定 -i 时，会自己生成彩条图像来编码
mpi_enc_test -w 1920 -h 1080 -t 7 -o /userdata/av/bar.h264 -n 60

# CBR 4Mbps，每秒打印一次帧率
mpi_enc_test -w 1920 -h 1080 -t 7 -i /userdata/av/in_1080p_60f.nv12 -o /userdata/av/cbr.h264 \
             -rc 1 -bps 4000000:4000000:4000000 -v f
```
| 参数 | 含义 |
|---|---|
| **`-w` / `-h` / `-t`** | **宽、高、编码格式（必填）** |
| `-i` / `-o` | 输入 YUV 文件 / 输出码流文件 |
| `-f` | 输入格式，默认 0 = NV12 |
| `-n` | 编码多少帧 |
| `-rc` | 0 = VBR，1 = CBR，2 = FIXQP，3 = AVBR |
| `-bps` | `target:min:max`，单位 bps |
| `-fps` | `in_num:in_den:in_flex/out_num:out_den:out_flex` |
| `-g` | GOP 参考模式 |
| `-s` | 同时开几个实例 |
| `-v f` | 每秒打印帧率；`-v q` 是静默模式 |

### 4.3 `mpi_dec_test`：解码
```bash
# 解码 H.264，输出 NV12
mpi_dec_test -t 7 -i /userdata/av/aaa.264 -o /userdata/av/out.nv12 -n 60

# 只测解码速度，不写文件
mpi_dec_test -t 7 -i /userdata/av/aaa.264 -n 300 -v f
```
| 参数 | 含义 |
|---|---|
| **`-i` / `-t`** | **输入码流文件、编码格式（必填）** |
| `-o` | 输出 YUV 文件 |
| `-n` | 最多解码多少帧 |
| `-f` | 输出格式，默认 NV12 |
| `-s` | 同时开几个实例 |
| `-v f` | 每秒打印帧率 |

### 4.4 `-t` 编码格式速查
| 格式 | `-t` 的值 |
|---|---|
| **H.264** | **7** |
| **H.265** | **16777220** |
| MJPEG | 8 |
| VP8 | 9 |
| VP9 | 10 |
| AV1 | 16777224 |
| MPEG-2 / MPEG-4 | 2 / 4 |

### 4.5 看结果
```bash
scp root@$IP:/userdata/av/ref.h264 /Users/dev/Documents/AV/rk_test_data/
open -a VLC /Users/dev/Documents/AV/rk_test_data/ref.h264

# YUV 文件用 ffplay 看
/usr/local/ffmpeg/4.4/bin/ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 out.nv12
```

---

## 五、断点调试 MPP 测试程序

### 5.1 只调试测试程序本身
和 `day_01/rk_test_env` 一样，建一个 **Remote GDB Server** 配置：
| 字段 | 填什么 |
|---|---|
| Target / Executable | `mpi_enc_test` |
| GDB | Bundled GDB multiarch |
| Credentials | `root@<板子IP>:22` |
| Upload path | `/tmp/CLion/debug` |
| 'target remote' args | `<板子IP>:1234` |
| GDB Server | `/usr/bin/gdbserver` |
| GDB Server args | `:1234 /tmp/CLion/debug/mpi_enc_test -w 1920 -h 1080 -t 7 -i /userdata/av/in_1080p_60f.nv12 -o /userdata/av/dbg.h264 -n 10` |

建议的断点（对照 Day 1 计划）：
| 位置 | 看什么 |
|---|---|
| `test_ctx_init`，第 155 行附近 | `hor_stride`、`ver_stride`、`frame_size` 的值 |
| `test_mpp_enc_cfg_setup`，第 501 行 `MPP_ENC_SET_CFG` | 所有编码参数 |
| `test_mpp_run`，第 758 行 `encode_put_frame` | `frame` 的内容 |
| `test_mpp_run`，第 768 行 `encode_get_packet` | 每帧码流的大小（`len`） |

### 5.2 想单步进入 MPP 库的内部（进阶）
测试程序**链接的是自己编译的 MPP 库**，但在板子上运行时，加载的是**系统自带的** `/usr/lib/librockchip_mpp.so.1`，两者版本很接近，一般没问题。

要想按 F7 单步**进入** `encode_put_frame` 内部，看 MPP 库里面是怎么实现的，就要把自己编的库也传上去，并且让程序优先加载它：
```bash
scp cmake-build-rk_3588_debug/mpp/librockchip_mpp.so* root@$IP:/userdata/av/lib/
# 板子上运行时，指定库的路径：
LD_LIBRARY_PATH=/userdata/av/lib /tmp/CLion/debug/mpi_enc_test ...
```
在 CLion 的 Remote GDB Server 配置里，GDB Server 那一栏可以改成 `/usr/bin/env`，GDB Server args 改成 `LD_LIBRARY_PATH=/userdata/av/lib /usr/bin/gdbserver :1234 /tmp/CLion/debug/mpi_enc_test ...`。
> 这部分还没有实际验证过，用到的时候再测试。

---

## 六、常见问题
| 现象 | 原因 | 解决办法 |
|---|---|---|
| `Compatibility with CMake < 3.5 has been removed` | MPP 声明的最低 CMake 版本太老 | CMake options 加 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` |
| CLion 里代码不能跳转 | CMake 配置失败，或者在用 Mac 的 `Debug` 配置分析 | 先确认配置成功；删掉 `Debug` 配置 |
| `Could NOT find Threads` | Mac 上的 CMake 检测 pthread 的方式和交叉编译环境不太匹配 | 不影响编译和使用，忽略 |
| 板子上运行报 `cannot open shared object file` | 缺少 MPP 库 | 板子出厂系统里已经有 `librockchip_mpp.so.1`；用自己编的库时，要设置 `LD_LIBRARY_PATH` |
| 运行时内核日志出现 `unknow vpu service ioctl cmd` | MPP 会按不同版本的驱动接口逐个尝试 | 正常现象，可以忽略 |
