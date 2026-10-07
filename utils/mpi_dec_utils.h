/*
 * Copyright 2020 Rockchip Electronics Co. LTD
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

#ifndef __MPI_DEC_UTILS_H__
#define __MPI_DEC_UTILS_H__

#include <stdio.h>
#include "utils.h"

#define MAX_FILE_NAME_LENGTH        256
#define MPI_DEC_STREAM_SIZE         (SZ_4K)
#define MPI_DEC_LOOP_COUNT          4

/*
 * NOTE: We can choose decoder's buffer mode here.
 * There are three mode that decoder can support:
 *
 * Mode 1: Pure internal mode
 * In the mode user will NOT call MPP_DEC_SET_EXT_BUF_GROUP
 * control to decoder. Only call MPP_DEC_SET_INFO_CHANGE_READY
 * to let decoder go on. Then decoder will use create buffer
 * internally and user need to release each frame they get.
 *
 * Advantage:
 * Easy to use and get a demo quickly
 * Disadvantage:
 * 1. The buffer from decoder may not be return before
 * decoder is close. So memroy leak or crash may happen.
 * 2. The decoder memory usage can not be control. Decoder
 * is on a free-to-run status and consume all memory it can
 * get.
 * 3. Difficult to implement zero-copy display path.
 *
 * Mode 2: Half internal mode
 * This is the mode current test code using. User need to
 * create MppBufferGroup according to the returned info
 * change MppFrame. User can use mpp_buffer_group_limit_config
 * function to limit decoder memory usage.
 *
 * Advantage:
 * 1. Easy to use
 * 2. User can release MppBufferGroup after decoder is closed.
 *    So memory can stay longer safely.
 * 3. Can limit the memory usage by mpp_buffer_group_limit_config
 * Disadvantage:
 * 1. The buffer limitation is still not accurate. Memory usage
 * is 100% fixed.
 * 2. Also difficult to implement zero-copy display path.
 *
 * Mode 3: Pure external mode
 * In this mode use need to create empty MppBufferGroup and
 * import memory from external allocator by file handle.
 * On Android surfaceflinger will create buffer. Then
 * mediaserver get the file handle from surfaceflinger and
 * commit to decoder's MppBufferGroup.
 *
 * Advantage:
 * 1. Most efficient way for zero-copy display
 * Disadvantage:
 * 1. Difficult to learn and use.
 * 2. Player work flow may limit this usage.
 * 3. May need a external parser to get the correct buffer
 * size for the external allocator.
 *
 * The required buffer size caculation:
 * hor_stride * ver_stride * 3 / 2 for pixel data
 * hor_stride * ver_stride / 2 for extra info
 * Total hor_stride * ver_stride * 2 will be enough.
 *
 * For H.264/H.265 20+ buffers will be enough.
 * For other codec 10 buffers will be enough.
 */
typedef enum MppDecBufMode_e {
    MPP_DEC_BUF_HALF_INT,
    MPP_DEC_BUF_INTERNAL,
    MPP_DEC_BUF_EXTERNAL,
    MPP_DEC_BUF_MODE_BUTT,
} MppDecBufMode;

typedef void* FileReader;   /* 文件读取器句柄（内部实现在 mpi_dec_utils.c） */
typedef void* DecBufMgr;    /* 解码 buffer 管理器句柄 */

/*
 * FileBufSlot - reader 返回的码流数据槽
 *
 * reader_read() 每次返回一个 FileBufSlot，包含一包码流数据。
 * dec_simple 通过 data/size 直接访问数据；
 * dec_advanced 通过 buf（MppBuffer）做零拷贝传递。
 */
typedef struct FileBufSlot_t {
    RK_S32          index;  /* 槽位索引（reader 内部管理的环形 buffer 编号） */
    MppBuffer       buf;    /* 码流数据对应的 MppBuffer（DMA buffer，advanced 模式用） */
    size_t          size;   /* 本包码流数据的有效字节数 */
    RK_U32          eos;    /* End Of Stream 标志：1=文件已读完，这是最后一包 */
    char            *data;  /* 码流数据指针（simple 模式直接用这个读数据） */
} FileBufSlot;

/*
 * MpiDecTestCmd - 解码测试的命令行参数和运行时配置
 *
 * 由 main() 中 mpi_dec_test_cmd_init() 从命令行参数解析填充，
 * 然后传给 dec_decode() 驱动整个解码流程。
 */
typedef struct MpiDecTestCmd_t {
    char            file_input[MAX_FILE_NAME_LENGTH];   /* -i: 输入码流文件路径 */
    char            file_output[MAX_FILE_NAME_LENGTH];  /* -o: 输出 YUV 文件路径 */

    MppCodingType   type;       /* -t: 编码类型（7=H.264, 16777220=H.265 等） */
    MppFrameFormat  format;     /* -f: 输出帧格式（JPEG 模式可指定 YUV/RGB） */
    RK_U32          width;      /* -w: 视频宽（JPEG 必须指定，H.264 可不指定） */
    RK_U32          height;     /* -h: 视频高 */

    RK_U32          have_input;     /* 是否指定了输入文件 */
    RK_U32          have_output;    /* 是否指定了输出文件 */

    RK_U32          simple;     /* 解码模式：1=simple（非JPEG），0=advanced（JPEG） */
    RK_S32          timeout;    /* 超时时间 */
    RK_S32          frame_num;  /* -n: 解码帧数（-1=无限循环, 0=到EOS, >0=指定帧数） */
    size_t          pkt_size;   /* 每次读取的码流块大小 */
    MppDecBufMode   buf_mode;   /* buffer 模式（内部/外部分配） */

    RK_S32          nthreads;   /* 线程数（mpi_dec_multi_test 多路解码用） */
    size_t          max_usage;  /* 输出：帧 buffer 内存峰值使用量（字节） */

    FileReader      reader;     /* 文件读取器（内部管理读取和分包） */
    FpsCalc         fps;        /* 帧率计算器 */

    RK_U32          quiet;      /* 静默模式：减少日志输出 */
    RK_U32          trace_fps;  /* 是否追踪帧率 */
    char            *file_slt;  /* CRC 校验输出文件路径 */
} MpiDecTestCmd;

RK_S32  mpi_dec_test_cmd_init(MpiDecTestCmd* cmd, int argc, char **argv);
RK_S32  mpi_dec_test_cmd_deinit(MpiDecTestCmd* cmd);
void    mpi_dec_test_cmd_options(MpiDecTestCmd* cmd);

void    reader_init(FileReader* reader, char* file_in, MppCodingType type);
void    reader_deinit(FileReader reader);

void    reader_start(FileReader reader);
void    reader_sync(FileReader reader);
void    reader_stop(FileReader reader);
size_t  reader_size(FileReader reader);
MPP_RET reader_read(FileReader reader, FileBufSlot **buf);
MPP_RET reader_index_read(FileReader reader, RK_S32 index, FileBufSlot **buf);
void    reader_rewind(FileReader reader);

MPP_RET dec_buf_mgr_init(DecBufMgr *mgr);
void dec_buf_mgr_deinit(DecBufMgr mgr);
MppBufferGroup dec_buf_mgr_setup(DecBufMgr mgr, RK_U32 size, RK_U32 count, MppDecBufMode mode);

void show_dec_fps(RK_S64 total_time, RK_S64 total_count, RK_S64 last_time, RK_S64 last_count);

#endif