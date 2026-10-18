/*
 * Copyright 2015 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file camera_source.c
 * @brief V4L2 摄像头采集模块 —— 实现文件
 *
 * 本文件封装了 Linux V4L2 摄像头采集的完整流程：
 *   打开设备 → 查询能力 → 协商格式 → 申请缓冲区 → mmap 映射 →
 *   导出 DMA-BUF → 入队 → 启动流 → 采集/归还循环 → 停流 → 释放资源
 *
 * 关键设计：
 * - 同时支持单平面（CAPTURE）和多平面（CAPTURE_MPLANE）两种 V4L2 接口
 * - 通过 VIDIOC_EXPBUF 将 V4L2 缓冲区导出为 DMA-BUF fd，
 *   再包装成 MppBuffer，这样编码器可以零拷贝直接使用摄像头数据
 */

#define MODULE_TAG "camera_source"

/* ==================== 标准库头文件 ==================== */
#include <stdio.h>      /* printf 等标准 I/O */
#include <fcntl.h>      /* open() 的 O_RDWR、O_CLOEXEC 等标志 */
#include <unistd.h>     /* close()、read() */
#include <errno.h>      /* errno 错误码 */
#include <stdlib.h>     /* malloc/free 等内存管理 */
#include <string.h>     /* memset/memcpy */
#include <time.h>       /* nanosleep() 用的 timespec */

/* ==================== 系统/内核头文件 ==================== */
#include <sys/select.h> /* select()，本文件未直接使用，但可能被间接依赖 */
#include <sys/mman.h>   /* mmap()、munmap() —— 将内核缓冲区映射到用户空间 */
#include <sys/ioctl.h>  /* ioctl() —— 与 V4L2 驱动通信的核心系统调用 */
#include <linux/videodev2.h>  /* V4L2 全部数据结构和 ioctl 命令定义 */

/* ==================== MPP 内部头文件 ==================== */
#include "mpp_log.h"    /* mpp_log()、mpp_err_f() 日志宏 */
#include "mpp_mem.h"    /* mpp_calloc()、MPP_FREE() 内存工具 */
#include "camera_source.h"

/**
 * @brief 单个帧缓冲区的描述信息
 *
 * 每个 V4L2 缓冲区被 mmap 到用户空间后，用这个结构来记录它的地址、大小、
 * DMA-BUF fd 和对应的 MppBuffer。
 */
typedef struct CamFrame_t {
    void        *start;      /* mmap 后的用户空间起始地址 */
    size_t      length;      /* 缓冲区大小（字节），munmap 时需要 */
    RK_S32      export_fd;   /* VIDIOC_EXPBUF 导出的 DMA-BUF 文件描述符 */
    RK_S32      sequence;    /* 帧序号（预留字段，当前未使用） */
    MppBuffer   buffer;      /* 由 DMA-BUF fd 导入生成的 MppBuffer，可直接送编码器 */
} CamFrame;

/**
 * @brief 摄像头采集上下文
 *
 * 保存 V4L2 设备句柄、缓冲区数组、采集类型等运行时状态。
 * 对外通过 CamSource* 不透明指针暴露，隐藏内部细节。
 */
struct CamSource {
    RK_S32              fd;       /* V4L2 设备文件描述符（open 返回） */
    RK_U32              bufcnt;   /* 缓冲区数量 */
    enum v4l2_buf_type  type;     /* 采集类型：CAPTURE 或 CAPTURE_MPLANE */
    MppFrameFormat      fmt;      /* MPP 像素格式（如 NV12、NV21 等） */
    CamFrame            fbuf[10]; /* 帧缓冲区数组，最多支持 10 个 */
};

/**
 * @brief MPP YUV 格式 → V4L2 像素格式 的映射表
 *
 * 数组下标对应 (MppFrameFormat - MPP_FRAME_FMT_YUV) 的偏移，
 * 值为对应的 V4L2_PIX_FMT_xxx 宏。0 表示该格式没有对应的 V4L2 格式。
 *
 * 常用映射：
 *   MPP_FMT_YUV420SP     (NV12)  → V4L2_PIX_FMT_NV12
 *   MPP_FMT_YUV420SP_VU  (NV21)  → V4L2_PIX_FMT_NV21
 *   MPP_FMT_YUV420P      (I420)  → V4L2_PIX_FMT_YVU420
 *   MPP_FMT_YUV422_YUYV          → V4L2_PIX_FMT_YUYV
 */
static RK_U32 V4L2_yuv_cfg[MPP_FMT_YUV_BUTT] = {
    V4L2_PIX_FMT_NV12,     /* MPP_FMT_YUV420SP      - NV12: Y 平面 + UV 交织平面 */
    0,                       /* MPP_FMT_YUV420SP_10BIT - 10bit NV12，V4L2 无直接对应 */
    V4L2_PIX_FMT_NV16,     /* MPP_FMT_YUV422SP      - NV16: Y 平面 + UV 交织（4:2:2） */
    0,                       /* MPP_FMT_YUV422SP_10BIT */
    V4L2_PIX_FMT_YVU420,   /* MPP_FMT_YUV420P       - YV12/I420: Y/U/V 三个独立平面 */
    V4L2_PIX_FMT_NV21,     /* MPP_FMT_YUV420SP_VU   - NV21: Y 平面 + VU 交织（Android 常用） */
    V4L2_PIX_FMT_YUV422P,  /* MPP_FMT_YUV422P       - YUV422 三平面 */
    V4L2_PIX_FMT_NV61,     /* MPP_FMT_YUV422SP_VU   - NV61: NV16 的 VU 版本 */
    V4L2_PIX_FMT_YUYV,     /* MPP_FMT_YUV422_YUYV   - YUYV packed 格式 */
    V4L2_PIX_FMT_YVYU,     /* MPP_FMT_YUV422_YVYU   - YVYU packed 格式 */
    V4L2_PIX_FMT_UYVY,     /* MPP_FMT_YUV422_UYVY   - UYVY packed 格式 */
    V4L2_PIX_FMT_VYUY,     /* MPP_FMT_YUV422_VYUY   - VYUY packed 格式 */
    V4L2_PIX_FMT_GREY,     /* MPP_FMT_YUV400        - 灰度图（仅 Y 分量） */
    0,                       /* 预留 */
    0,                       /* 预留 */
    0,                       /* 预留 */
};

/**
 * @brief MPP RGB 格式 → V4L2 像素格式 的映射表
 *
 * 数组下标对应 (MppFrameFormat - MPP_FRAME_FMT_RGB) 的偏移。
 */
static RK_U32 V4L2_RGB_cfg[MPP_FMT_RGB_BUTT - MPP_FRAME_FMT_RGB] = {
    V4L2_PIX_FMT_RGB565,   /* MPP_FMT_RGB565   - 16bit RGB */
    0,                       /* MPP_FMT_BGR565   */
    V4L2_PIX_FMT_RGB555,   /* MPP_FMT_RGB555   - 15bit RGB */
    0,                       /* MPP_FMT_BGR555   */
    V4L2_PIX_FMT_RGB444,   /* MPP_FMT_RGB444   - 12bit RGB */
    0,                       /* MPP_FMT_BGR444   */
    V4L2_PIX_FMT_RGB24,    /* MPP_FMT_RGB888   - 24bit RGB */
    V4L2_PIX_FMT_BGR24,    /* MPP_FMT_BGR888   - 24bit BGR */
    0,                       /* MPP_FMT_RGB101010 */
    0,                       /* MPP_FMT_BGR101010 */
    V4L2_PIX_FMT_RGB32,    /* MPP_FMT_ARGB8888 - 32bit ARGB */
    V4L2_PIX_FMT_BGR32,    /* MPP_FMT_ABGR8888 - 32bit ABGR */
    0,                       /* 预留 */
    0,                       /* 预留 */
};

/** 多平面模式下的平面数量，摄像头采集一般只有 1 个平面 */
#define FMT_NUM_PLANES 1

/**
 * @brief 带重试的 ioctl 封装
 *
 * V4L2 的 ioctl 可能因为信号中断（EINTR）或设备繁忙（EAGAIN）而返回失败，
 * 这并不是真正的错误，需要重试。本函数会在这两种情况下每隔 10ms 重试一次，
 * 直到成功或遇到真正的错误。
 *
 * @param fd   设备文件描述符
 * @param req  ioctl 请求码（如 VIDIOC_QUERYCAP、VIDIOC_S_FMT 等）
 * @param arg  请求参数指针
 * @return     成功返回 0；失败返回 -1
 */
static RK_S32 camera_source_ioctl(RK_S32 fd, RK_S32 req, void* arg)
{
    struct timespec poll_time;
    RK_S32 ret;

    while ((ret = ioctl(fd, req, arg))) {
        if (ret == -1 && (EINTR != errno && EAGAIN != errno)) {
            /* 遇到非 EINTR/EAGAIN 的错误，属于真正的失败，直接退出 */
            break;
        }
        /* 被信号中断或设备暂时繁忙，等待 10ms 后重试 */
        poll_time.tv_sec = 0;
        poll_time.tv_nsec = 10000000;  /* 10ms = 10,000,000 纳秒 */
        nanosleep(&poll_time, NULL);
    }

    return ret;
}

/**
 * @brief 初始化摄像头采集上下文
 *
 * 完整流程：
 *   1. 分配 CamSource 上下文
 *   2. 打开 V4L2 设备节点
 *   3. 枚举输入设备（打印日志）
 *   4. VIDIOC_QUERYCAP 检查设备能力（是否支持采集、是否支持流式 I/O）
 *   5. VIDIOC_S_FMT / VIDIOC_G_FMT 协商分辨率和像素格式
 *   6. VIDIOC_REQBUFS 申请内核缓冲区（mmap 模式）
 *   7. 逐个缓冲区：VIDIOC_QUERYBUF → mmap → VIDIOC_EXPBUF 导出 DMA-BUF
 *   8. 将 DMA-BUF fd 导入为 MppBuffer（mpp_buffer_import）
 *   9. VIDIOC_QBUF 把所有缓冲区入队（驱动可以开始往里写）
 *  10. VIDIOC_STREAMON 启动视频流
 *  11. 跳过前几帧（摄像头刚启动时画面可能不稳定）
 *
 * @param device  设备节点路径，如 "/dev/video0"
 * @param bufcnt  缓冲区数量
 * @param width   期望宽度
 * @param height  期望高度
 * @param format  期望的 MPP 像素格式
 * @return        成功返回 CamSource*；失败返回 NULL（资源会被自动清理）
 */
CamSource *camera_source_init(const char *device, RK_U32 bufcnt, RK_U32 width, RK_U32 height, MppFrameFormat format)
{
    struct v4l2_capability     cap;   /* 设备能力描述 */
    struct v4l2_format         vfmt;  /* 视频格式描述 */
    struct v4l2_requestbuffers req;   /* 缓冲区申请请求 */
    struct v4l2_buffer         buf;   /* 单个缓冲区描述 */
    enum   v4l2_buf_type       type;  /* 采集类型 */
    RK_U32 i;
    RK_U32 buf_len = 0;              /* 单个缓冲区的数据长度 */
    CamSource *ctx;

    /* ---- 第 1 步：分配上下文 ---- */
    ctx = mpp_calloc(CamSource, 1);
    if (!ctx)
        return NULL;

    ctx->bufcnt = bufcnt;

    /* ---- 第 2 步：打开 V4L2 设备 ---- */
    /* O_RDWR: 读写方式打开（V4L2 要求）
     * O_CLOEXEC: fork/exec 后自动关闭，防止子进程泄漏 fd */
    ctx->fd = open(device, O_RDWR | O_CLOEXEC, 0);
    if (ctx->fd < 0) {
        mpp_err_f("Cannot open device\n");
        goto FAIL;
    }

    /* ---- 第 3 步：枚举输入设备（仅打印日志，了解摄像头有哪些输入源） ---- */
    {
        struct v4l2_input input;

        input.index = 0;
        while (!camera_source_ioctl(ctx->fd, VIDIOC_ENUMINPUT, &input)) {
            mpp_log("input devices:%s\n", input.name);
            ++input.index;
        }
    }

    /* ---- 第 4 步：查询设备能力 ---- */
    /* VIDIOC_QUERYCAP 返回设备的驱动名、卡名和支持的功能标志位 */
    if (0 != camera_source_ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap)) {
        mpp_err_f("Not v4l2 compatible\n");
        goto FAIL;
    }

    /* 检查是否支持视频采集（单平面或多平面） */
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) && !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        mpp_err_f("Capture not supported\n");
        goto FAIL;
    }

    /* 检查是否支持流式 I/O（mmap/userptr/dmabuf），这是必须的 */
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        mpp_err_f("Streaming IO Not Supported\n");
        goto FAIL;
    }

    /* ---- 第 5 步：设置视频格式 ---- */
    vfmt = (struct v4l2_format) {0};
    /* 默认用单平面采集模式 */
    vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    /* 如果设备支持多平面模式（MPLANE），则切换到多平面模式
     * 多平面模式常见于 ISP 输出的设备（如 rkisp），可以同时输出多个平面（Y/UV 分开） */
    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    vfmt.fmt.pix.width = width;
    vfmt.fmt.pix.height = height;

    /* 枚举设备支持的所有像素格式（仅打印日志，帮助调试） */
    {
        struct v4l2_fmtdesc fmtdesc;

        fmtdesc.index = 0;
        fmtdesc.type = vfmt.type;
        while (!camera_source_ioctl(ctx->fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
            mpp_log("fmt name: [%s]\n", fmtdesc.description);
            /* 将 pixelformat 的 4 字节 FourCC 逐字节打印出来，如 "NV12" */
            mpp_log("fmt pixelformat: '%c%c%c%c', description = '%s'\n", fmtdesc.pixelformat & 0xFF,
                    (fmtdesc.pixelformat >> 8) & 0xFF, (fmtdesc.pixelformat >> 16) & 0xFF,
                    (fmtdesc.pixelformat >> 24) & 0xFF, fmtdesc.description);
            fmtdesc.index++;
        }
    }

    /* 将 MPP 格式转换为 V4L2 格式 */
    if (MPP_FRAME_FMT_IS_YUV(format)) {
        vfmt.fmt.pix.pixelformat = V4L2_yuv_cfg[format - MPP_FRAME_FMT_YUV];
    } else if (MPP_FRAME_FMT_IS_RGB(format)) {
        vfmt.fmt.pix.pixelformat = V4L2_RGB_cfg[format - MPP_FRAME_FMT_RGB];
    }

    /* 如果映射表中没有对应格式（值为 0），默认使用 NV12 */
    if (!vfmt.fmt.pix.pixelformat)
        vfmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;

    type = vfmt.type;
    ctx->type = vfmt.type;

    /* VIDIOC_S_FMT: 告诉驱动我们想要的格式（驱动可能会调整为最接近的支持格式） */
    if (-1 == camera_source_ioctl(ctx->fd, VIDIOC_S_FMT, &vfmt)) {
        mpp_err_f("VIDIOC_S_FMT\n");
        goto FAIL;
    }

    /* VIDIOC_G_FMT: 回读驱动实际协商好的格式（可能与请求的不完全一致） */
    if (-1 == camera_source_ioctl(ctx->fd, VIDIOC_G_FMT, &vfmt)) {
        mpp_err_f("VIDIOC_G_FMT\n");
        goto FAIL;
    }

    mpp_log("get width %d height %d", vfmt.fmt.pix.width, vfmt.fmt.pix.height);

    /* ---- 第 6 步：申请内核缓冲区 ---- */
    /* V4L2_MEMORY_MMAP 表示由内核分配缓冲区，用户通过 mmap 访问
     * 这是最常用的方式，比 USERPTR 更高效（减少内存拷贝） */
    req = (struct v4l2_requestbuffers) {0};
    req.count  = ctx->bufcnt;
    req.type   = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (-1 == camera_source_ioctl(ctx->fd, VIDIOC_REQBUFS, &req)) {
        mpp_err_f("Device does not support mmap\n");
        goto FAIL;
    }

    /* 驱动实际分配的缓冲区数量可能少于请求的，这里要求必须完全匹配 */
    if (req.count != ctx->bufcnt) {
        mpp_err_f("Device buffer count mismatch\n");
        goto FAIL;
    }

    /* ---- 第 7 步：逐个缓冲区 mmap + 导出 DMA-BUF ---- */
    for (i = 0 ; i < ctx->bufcnt; i++) {
        /* 7a. VIDIOC_QUERYBUF: 查询第 i 个缓冲区的信息（偏移量、大小等） */
        buf = (struct v4l2_buffer) {0};
        buf.type    = type;
        buf.memory  = V4L2_MEMORY_MMAP;
        buf.index   = i;
        struct v4l2_plane planes[FMT_NUM_PLANES];
        buf.memory = V4L2_MEMORY_MMAP;
        /* 多平面模式需要额外设置 planes 数组 */
        if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == type) {
            buf.m.planes = planes;
            buf.length = FMT_NUM_PLANES;
        }

        if (-1 == camera_source_ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf)) {
            mpp_err_f("ERROR: VIDIOC_QUERYBUF\n");
            goto FAIL;
        }

        /* 7b. mmap: 将内核缓冲区映射到用户空间
         * - PROT_READ | PROT_WRITE: 可读可写
         * - MAP_SHARED: 多进程共享（V4L2 要求）
         * - 最后一个参数是内核缓冲区的偏移量，由 QUERYBUF 返回 */
        if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buf.type) {
            /* 多平面模式：大小和偏移量在 planes[0] 中 */
            buf_len = buf.m.planes[0].length;
            ctx->fbuf[i].start =
                mmap(NULL /* start anywhere */,
                     buf.m.planes[0].length,
                     PROT_READ | PROT_WRITE /* required */,
                     MAP_SHARED /* recommended */,
                     ctx->fd,
                     buf.m.planes[0].m.mem_offset);
        } else {
            /* 单平面模式：大小和偏移量直接在 buf 中 */
            buf_len = buf.length;
            ctx->fbuf[i].start =
                mmap(NULL /* start anywhere */,
                     buf.length,
                     PROT_READ | PROT_WRITE /* required */,
                     MAP_SHARED /* recommended */,
                     ctx->fd,
                     buf.m.offset);
        }
        if (MAP_FAILED == ctx->fbuf[i].start) {
            mpp_err_f("ERROR: Failed to map device frame buffers\n");
            goto FAIL;
        }

        /* 记录缓冲区长度，munmap 时需要 */
        ctx->fbuf[i].length = buf_len;

        /* 7c. VIDIOC_EXPBUF: 将 V4L2 缓冲区导出为 DMA-BUF 文件描述符，
         * DMA-BUF 是 Linux 内核的跨设备缓冲区共享机制，
         * 有了 DMA-BUF fd，就可以把摄像头的缓冲区直接交给 VPU 编码器使用，
         * 不需要任何内存拷贝（零拷贝）。这是整个模块最核心的优化点！ */
        struct v4l2_exportbuffer expbuf = (struct v4l2_exportbuffer) {0} ;
        expbuf.type = type;
        expbuf.index = i;
        expbuf.flags = O_CLOEXEC;  /* fork 后自动关闭 */
        if (camera_source_ioctl(ctx->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
            mpp_err_f("get dma buf failed\n");
            goto FAIL;
        } else {
            mpp_log("get dma buf(%d)-fd: %d\n", i, expbuf.fd);
            /* 7d. 将 DMA-BUF fd 导入为 MppBuffer
             * MPP_BUFFER_TYPE_EXT_DMA: 表示这是外部导入的 DMA-BUF
             * info.size 的低 27 位存实际大小，高 5 位存 index 信息 */
            MppBufferInfo info;
            memset(&info, 0, sizeof(MppBufferInfo));
            info.type = MPP_BUFFER_TYPE_EXT_DMA;
            info.fd =  expbuf.fd;
            info.size = buf_len & 0x07ffffff;           /* 取低 27 位作为 size */
            info.index = (buf_len & 0xf8000000) >> 27;  /* 取高 5 位作为 index */
            mpp_buffer_import(&ctx->fbuf[i].buffer, &info);
        }
        ctx->fbuf[i].export_fd = expbuf.fd;
    }

    /* ---- 第 8 步：将所有缓冲区入队（QBUF） ---- */
    /* 入队后驱动就可以往这些缓冲区里写入采集到的帧数据 */
    for (i = 0; i < ctx->bufcnt; i++ ) {
        struct v4l2_plane planes[FMT_NUM_PLANES];

        buf = (struct v4l2_buffer) {0};
        buf.type    = type;
        buf.memory  = V4L2_MEMORY_MMAP;
        buf.index   = i;
        buf.memory = V4L2_MEMORY_MMAP;

        if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == type) {
            buf.m.planes = planes;
            buf.length = FMT_NUM_PLANES;
        }

        if (-1 == camera_source_ioctl(ctx->fd, VIDIOC_QBUF, &buf)) {
            mpp_err_f("ERROR: VIDIOC_QBUF %d\n", i);
            goto FAIL;
        }
    }

    /* ---- 第 9 步：启动视频流 ---- */
    /* STREAMON 之后驱动才会真正开始采集，摄像头 sensor 开始输出数据 */
    if (-1 == camera_source_ioctl(ctx->fd, VIDIOC_STREAMON, &type)) {
        mpp_err_f("ERROR: VIDIOC_STREAMON\n");
        goto FAIL;
    }

    /* ---- 第 10 步：跳过开头几帧 ---- */
    /* 摄像头刚启动时，自动曝光/白平衡还没有稳定，前几帧画面质量较差，
     * 这里取出并立即归还 bufcnt 帧，相当于丢弃它们 */
    for (i = 0; i < ctx->bufcnt; i++ ) {
        RK_S32 idx = camera_source_get_frame(ctx);
        if (idx >= 0)
            camera_source_put_frame(ctx, idx);
    }

    return ctx;

FAIL:
    /* 任何步骤失败都走这里，统一清理已分配的资源 */
    camera_source_deinit(ctx);
    return NULL;
}

/**
 * @brief 反初始化摄像头，释放所有资源
 *
 * 执行顺序：停流 → 释放 MppBuffer → 解除 mmap → 关闭 DMA-BUF fd → 关闭设备 fd → 释放上下文
 *
 * @param ctx  摄像头上下文，可以为 NULL（安全调用）
 * @return     MPP_OK
 */
MPP_RET camera_source_deinit(CamSource *ctx)
{
    struct v4l2_buffer buf;
    enum v4l2_buf_type type;
    RK_U32 i;

    if (NULL == ctx)
        return MPP_OK;

    if (ctx->fd < 0)
        return MPP_OK;

    /* 停止视频流，驱动不再往缓冲区写数据 */
    type = ctx->type;
    camera_source_ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);

    /* 逐个释放缓冲区 */
    for (i = 0 ; i < ctx->bufcnt; i++) {
        buf = (struct v4l2_buffer) {0};
        buf.type    = type;
        buf.memory  = V4L2_MEMORY_MMAP;
        buf.index   = i;
        camera_source_ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf);

        /* 释放 MppBuffer（内部会减少引用计数） */
        if (ctx->fbuf[buf.index].buffer) {
            mpp_buffer_put(ctx->fbuf[buf.index].buffer);
        }
        /* 解除 mmap 映射 */
        munmap(ctx->fbuf[buf.index].start, ctx->fbuf[buf.index].length);
        /* 关闭 DMA-BUF 文件描述符 */
        close(ctx->fbuf[i].export_fd);
    }

    /* 关闭 V4L2 设备 */
    close(ctx->fd);
    /* 释放上下文结构体内存 */
    MPP_FREE(ctx);
    return MPP_OK;
}

/**
 * @brief 从摄像头获取一帧（出队操作）
 *
 * 调用 VIDIOC_DQBUF 从驱动的"已填充队列"中取出一个缓冲区。
 *
 * V4L2 缓冲区有两个队列：
 * - 空闲队列（incoming）：通过 QBUF 入队，驱动可以往里写数据
 * - 已填充队列（outgoing）：驱动写完数据后放到这里，用户通过 DQBUF 取出
 *
 * 注意：此函数不是线程安全的！
 *
 * @param ctx  摄像头上下文
 * @return     成功返回缓冲区索引（0 ~ bufcnt-1）；失败返回 -1
 */
RK_S32 camera_source_get_frame(CamSource *ctx)
{
    struct v4l2_buffer buf;
    enum v4l2_buf_type type;

    type = ctx->type;
    buf = (struct v4l2_buffer) {0};
    buf.type   = type;
    buf.memory = V4L2_MEMORY_MMAP;

    /* 多平面模式需要提供 planes 数组给驱动填写 */
    struct v4l2_plane planes[FMT_NUM_PLANES];
    if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == type) {
        buf.m.planes = planes;
        buf.length = FMT_NUM_PLANES;
    }

    /* VIDIOC_DQBUF: 出队（dequeue），取出一个驱动已经填好数据的缓冲区
     * 如果队列为空（所有缓冲区还在被驱动使用），此调用会阻塞等待 */
    if (-1 == camera_source_ioctl(ctx->fd, VIDIOC_DQBUF, &buf)) {
        mpp_err_f("VIDIOC_DQBUF\n");
        return -1;
    }

    /* 安全检查：确保返回的索引在合法范围内 */
    if (buf.index > ctx->bufcnt) {
        mpp_err_f("buffer index out of bounds\n");
        return -1;
    }

    /* 多平面模式下，bytesused 在 planes[0] 中，统一放到 buf.bytesused */
    if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == type)
        buf.bytesused = buf.m.planes[0].bytesused;

    return buf.index;
}

/**
 * @brief 将帧缓冲区归还给摄像头驱动（入队操作）
 *
 * 调用 VIDIOC_QBUF 将用完的缓冲区放回驱动的"空闲队列"，
 * 驱动后续会用它来存放新采集到的帧。
 *
 * 归还后不能再访问该缓冲区的数据！如果编码器还在使用，必须等编码完成后再归还。
 *
 * @param ctx  摄像头上下文
 * @param idx  要归还的缓冲区索引（由 get_frame 返回）
 * @return     MPP_OK
 */
MPP_RET camera_source_put_frame(CamSource *ctx, RK_S32 idx)
{
    struct v4l2_buffer buf;
    enum v4l2_buf_type type;

    /* idx < 0 表示之前 get_frame 失败，直接返回 */
    if (idx < 0)
        return MPP_OK;

    type = ctx->type;
    buf = (struct v4l2_buffer) {0};
    buf.type   = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = idx;

    struct v4l2_plane planes[FMT_NUM_PLANES];
    if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == type) {
        buf.m.planes = planes;
        buf.length = FMT_NUM_PLANES;
    }

    /* VIDIOC_QBUF: 入队（enqueue），将缓冲区归还给驱动 */
    if (-1 == camera_source_ioctl(ctx->fd, VIDIOC_QBUF, &buf)) {
        mpp_err_f("VIDIOC_QBUF\n");
        return MPP_OK;
    }

    return MPP_OK;
}

/**
 * @brief 将摄像头帧转换为 MppBuffer（零拷贝）
 *
 * 在 init 阶段，每个 V4L2 缓冲区已经通过 VIDIOC_EXPBUF 导出为 DMA-BUF，
 * 并通过 mpp_buffer_import() 包装成了 MppBuffer。
 * 这里根据索引直接返回对应的 MppBuffer，不涉及任何数据拷贝。
 *
 * mpp_buffer_sync_end() 的作用：
 * - 摄像头（DMA 设备）写入数据后，CPU 缓存中可能还有旧数据
 * - sync_end 会使 CPU 缓存失效，确保后续读取能看到 DMA 写入的最新数据
 * - 这对于 ARM 平台非常重要，因为 ARM 的缓存不是硬件一致的
 *
 * @param ctx  摄像头上下文
 * @param idx  缓冲区索引
 * @return     对应的 MppBuffer；idx 无效时返回 NULL
 */
MppBuffer camera_frame_to_buf(CamSource *ctx, RK_S32 idx)
{
    MppBuffer buf = NULL;

    if (idx < 0)
        return buf;

    buf = ctx->fbuf[idx].buffer;
    if (buf)
        mpp_buffer_sync_end(buf);  /* CPU 缓存同步：使缓存失效，读到 DMA 最新写入的数据 */

    return buf;
}