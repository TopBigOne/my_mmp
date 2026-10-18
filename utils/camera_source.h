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
 * @file camera_source.h
 * @brief V4L2 摄像头采集模块 —— 头文件
 *
 * 本模块封装了 Linux V4L2（Video for Linux 2）接口，为 MPP 编码器提供摄像头帧数据。
 * 采集到的帧通过 DMA-BUF 导出为 MppBuffer，可以零拷贝地直接送给 MPP 编码器。
 *
 * 典型使用流程：
 *   1. camera_source_init()    —— 打开设备、协商格式、申请缓冲区、启动采集流
 *   2. camera_source_get_frame() —— 从驱动取出一帧（DQBUF）
 *   3. camera_frame_to_buf()     —— 将该帧转成 MppBuffer 供编码器使用
 *   4. camera_source_put_frame() —— 用完后归还给驱动（QBUF），让驱动可以继续往里写
 *   5. camera_source_deinit()    —— 停止采集、释放所有资源
 */

#ifndef __CAMERA_SOURCE_H__
#define __CAMERA_SOURCE_H__

#include "mpp_frame.h"

/* 前向声明摄像头上下文结构体，具体定义在 camera_source.c 中 */
typedef struct CamSource CamSource;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化摄像头采集上下文
 *
 * 打开指定的 V4L2 设备，设置采集分辨率和像素格式，申请内核缓冲区并 mmap 到用户空间，
 * 将缓冲区通过 VIDIOC_EXPBUF 导出为 DMA-BUF fd，最后启动视频流。
 *
 * @param device  设备节点路径，例如 "/dev/video0"
 * @param bufcnt  申请的 V4L2 缓冲区数量（一般 3~5 个，实现采集和编码的流水线）
 * @param width   期望的采集宽度（像素）
 * @param height  期望的采集高度（像素）
 * @param fmt     期望的像素格式（MPP 格式，如 MPP_FMT_YUV420SP 即 NV12）
 * @return        成功返回 CamSource 指针；失败返回 NULL
 */
CamSource* camera_source_init(const char *device, RK_U32 bufcnt, RK_U32 width, RK_U32 height, MppFrameFormat fmt);

/**
 * @brief 反初始化摄像头，释放所有资源
 *
 * 停止视频流（STREAMOFF），解除所有缓冲区的 mmap 映射，
 * 关闭 DMA-BUF fd 和设备 fd，释放上下文内存。
 *
 * @param ctx  由 camera_source_init() 返回的上下文指针
 * @return     MPP_OK
 */
MPP_RET camera_source_deinit(CamSource *ctx);

/**
 * @brief 从摄像头获取一帧（出队）
 *
 * 调用 VIDIOC_DQBUF 从驱动的"已填充队列"中取出一个缓冲区。
 * 取出后该缓冲区归用户所有，驱动不会再往里写数据，直到调用 put_frame() 归还。
 *
 * @param ctx  摄像头上下文
 * @return     成功返回缓冲区索引（0 ~ bufcnt-1）；失败返回 -1
 */
RK_S32 camera_source_get_frame(CamSource *ctx);

/**
 * @brief 将帧缓冲区归还给摄像头驱动（入队）
 *
 * 调用 VIDIOC_QBUF 把缓冲区放回驱动的"空闲队列"，驱动后续会往里写新的帧数据。
 * 注意：归还后不能再访问该缓冲区的数据！
 *
 * @param ctx  摄像头上下文
 * @param idx  由 camera_source_get_frame() 返回的缓冲区索引
 * @return     MPP_OK
 */
MPP_RET camera_source_put_frame(CamSource *ctx, RK_S32 idx);

/**
 * @brief 将摄像头帧转换为 MppBuffer
 *
 * 根据缓冲区索引获取对应的 MppBuffer（底层是 DMA-BUF），
 * 并调用 mpp_buffer_sync_end() 确保 CPU 缓存与硬件一致（缓存同步）。
 * 返回的 MppBuffer 可以直接设置到 MppFrame 里送给 MPP 编码器，实现零拷贝编码。
 *
 * @param ctx  摄像头上下文
 * @param idx  由 camera_source_get_frame() 返回的缓冲区索引
 * @return     对应的 MppBuffer；idx 无效时返回 NULL
 */
MppBuffer camera_frame_to_buf(CamSource *ctx, RK_S32 idx);

#ifdef __cplusplus
}
#endif

#endif /* __CAMERA_SOURCE_H__ */