// Copyright 2024 Espressif Systems (Shanghai) CO., LTD.
// All rights reserved.

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// AVI块标识符
#define AVI_RIFF_ID       0x52494646  // "RIFF" - 文件格式标识符，AVI文件必须以RIFF开头
#define AVI_AVI_ID        0x41564920  // "AVI " - AVI文件类型标识符，跟在RIFF后面
#define AVI_LIST_ID       0x5453494C  // "LIST" - 列表块标识符，用于组织相关数据块
#define AVI_HDRL_ID       0x6864726C  // "hdrl" - 头部列表标识符，包含文件头部信息
#define AVI_MOVI_ID       0x69766F6D  // "movi" - 视频数据列表标识符，包含所有媒体数据
#define AVI_AVIH_ID       0x61697668  // "avih" - AVI主头部标识符，包含视频基本信息
#define AVI_STRL_ID       0x7374726C  // "strl" - 流列表标识符，包含音频/视频流信息
#define AVI_STRH_ID       0x73747268  // "strh" - 流头部标识符，包含流的详细信息
#define AVI_STRF_ID       0x66727473  // "strf" - 流格式标识符，包含编码格式信息
#define AVI_VIDS_ID       0x76696473  // "vids" - 视频流标识符，表示这是视频数据
#define AVI_MJPG_ID       0x4D4A5047  // "MJPG" - Motion JPEG编码格式标识符
#define AVI_00DC_ID       0x30306463  // "00dc" - 视频帧数据块标识符，00=流编号，dc=数据类型
#define AVI_01WB_ID       0x62773130  // "01wb" - 音频帧数据块标识符，01=流编号，wb=音频数据
#define AVI_IDX1_ID       0x31786469  // "idx1" - 索引块标识符，包含所有帧的位置信息
#define AVI_JUNK_ID       0x4B4E554A  // "JUNK" - 填充块标识符，用于数据对齐
#define AVI_JPEG_START_ID 0xFFD8FFE0  // JPEG Start of Image - JPEG图像开始标记
#define AVI_JPEG_END_ID   0xFFD9      // JPEG End of Image - JPEG图像结束标记

// 错误代码
typedef enum {
    AVI_ERR_OK = 0,
    AVI_ERR_INVALID_FORMAT = -1,
    AVI_ERR_NO_MEMORY = -2,
    AVI_ERR_INVALID_PARAM = -3,
    AVI_ERR_FILE_NOT_FOUND = -4,
    AVI_ERR_READ_FAILED = -5,
    AVI_ERR_UNSUPPORTED = -6,
} avi_error_t;

// AVI主头部信息
typedef struct {
    uint32_t micro_sec_per_frame;  // 每帧微秒数
    uint32_t max_bytes_per_sec;    // 最大字节/秒
    uint32_t padding_granularity;  // 填充粒度
    uint32_t flags;                // 标志位
    uint32_t total_frames;         // 总帧数
    uint32_t initial_frames;       // 初始帧数
    uint32_t streams;              // 流数量
    uint32_t suggested_buffer_size; // 建议缓冲区大小
    uint32_t width;                // 视频宽度
    uint32_t height;               // 视频高度
    uint32_t reserved[4];          // 保留字段
} avi_header_t;

// AVI帧信息
typedef struct {
    uint32_t offset;               // 文件偏移
    uint32_t size;                 // 帧大小
    uint32_t timestamp;            // 时间戳
    uint8_t *data;                 // 帧数据指针
    int is_key_frame;              // 是否为关键帧
} avi_frame_t;

// AVI容器结构
typedef struct {
    avi_header_t header;           // 主头部信息
    avi_frame_t *frames;           // 帧数组
    uint32_t frame_count;          // 帧数量
    uint8_t *file_data;            // 文件数据
    size_t file_size;              // 文件大小
    uint32_t video_stream_id;      // 视频流ID
    uint32_t frame_rate;           // 帧率
    int is_valid;                  // 有效性标志
} avi_container_t;

// 块信息结构
typedef struct {
    uint32_t id;                   // 块ID
    uint32_t size;                 // 块大小
    uint32_t offset;               // 文件偏移
} avi_chunk_t;

// 预加载缓冲区结构
typedef struct {
    uint8_t* jpeg_data;            // JPEG帧数据缓冲区
    uint8_t* rgb_data;             // 解码后的RGB数据缓冲区
    uint32_t jpeg_size;            // JPEG数据大小
    uint32_t rgb_size;             // RGB数据大小
    uint32_t width;                // 图像宽度
    uint32_t height;               // 图像高度
    uint32_t frame_index;          // 帧索引
    volatile int is_ready;         // 是否解码完成 (volatile for thread safety)
    volatile int is_valid;         // 数据是否有效 (volatile for thread safety)
    volatile int is_decoding;      // 是否正在解码 (volatile for thread safety)
} avi_preload_buffer_t;

// 内存池结构
typedef struct {
    uint8_t* jpeg_pool[2];         // JPEG缓冲区池 (双缓冲)
    uint8_t* rgb_pool[2];          // RGB缓冲区池 (双缓冲)
    volatile int pool_used[2];     // 缓冲区使用状态 (volatile for thread safety)
    uint32_t max_jpeg_size;        // 最大JPEG帧大小
    uint32_t max_rgb_size;         // 最大RGB帧大小
    int is_initialized;            // 是否已初始化
} avi_memory_pool_t;

// 预加载视频播放器结构
typedef struct {
    avi_container_t* container;         // AVI容器
    avi_preload_buffer_t current_frame; // 当前显示帧
    avi_preload_buffer_t next_frame;    // 预加载的下一帧
    avi_memory_pool_t memory_pool;      // 内存池
    
    // 播放控制
    volatile int is_playing;            // 是否正在播放 (volatile for thread safety)
    volatile int current_frame_index;   // 当前帧索引 (volatile for thread safety)
    uint32_t frame_rate;                // 播放帧率
    uint32_t frame_delay_ms;            // 帧间延迟毫秒
    
    // 性能控制
    int enable_preload;                 // 是否启用预加载
    int frame_skip_ratio;               // 跳帧比例 (1=不跳帧, 2=跳1帧, 3=跳2帧)
    int use_rgb565;                     // 是否使用RGB565格式 (否则RGB888)
    
    // 统计信息
    uint32_t frames_decoded;            // 已解码帧数
    uint32_t frames_skipped;            // 跳过帧数
    uint32_t decode_errors;             // 解码错误数
} avi_preload_player_t;

// 函数声明

/**
 * @brief 解析AVI文件
 * @param filename 文件名
 * @return AVI容器指针，失败返回NULL
 */
avi_container_t* avi_parse_file(const char* filename);

/**
 * @brief 解析AVI缓冲区数据
 * @param data 数据缓冲区
 * @param size 数据大小
 * @return AVI容器指针，失败返回NULL
 */
avi_container_t* avi_parse_buffer(uint8_t* data, size_t size);

/**
 * @brief 释放AVI容器资源
 * @param container AVI容器指针
 */
void avi_free_container(avi_container_t* container);

/**
 * @brief 获取指定帧
 * @param container AVI容器指针
 * @param frame_index 帧索引
 * @return 帧指针，失败返回NULL
 */
avi_frame_t* avi_get_frame(avi_container_t* container, uint32_t frame_index);

/**
 * @brief 获取帧数量
 * @param container AVI容器指针
 * @return 帧数量
 */
uint32_t avi_get_frame_count(avi_container_t* container);

/**
 * @brief 获取视频宽度
 * @param container AVI容器指针
 * @return 视频宽度
 */
uint32_t avi_get_width(avi_container_t* container);

/**
 * @brief 获取视频高度
 * @param container AVI容器指针
 * @return 视频高度
 */
uint32_t avi_get_height(avi_container_t* container);

/**
 * @brief 获取帧率
 * @param container AVI容器指针
 * @return 帧率
 */
uint32_t avi_get_frame_rate(avi_container_t* container);

/**
 * @brief 验证AVI格式
 * @param data 数据缓冲区
 * @param size 数据大小
 * @return 1表示有效，0表示无效
 */
int avi_validate_format(uint8_t* data, size_t size);

/**
 * @brief 查找下一个块
 * @param data 数据缓冲区
 * @param size 数据大小
 * @param offset 开始偏移
 * @param chunk 输出块信息
 * @return 成功返回0，失败返回错误代码
 */
int avi_find_next_chunk(uint8_t* data, size_t size, uint32_t offset, avi_chunk_t* chunk);

// 预加载播放器相关函数

/**
 * @brief 创建预加载视频播放器
 * @param container AVI容器指针
 * @param use_rgb565 是否使用RGB565格式 (否则RGB888)
 * @return 播放器指针，失败返回NULL
 */
avi_preload_player_t* avi_create_preload_player(avi_container_t* container, int use_rgb565);

/**
 * @brief 销毁预加载视频播放器
 * @param player 播放器指针
 */
void avi_destroy_preload_player(avi_preload_player_t* player);

/**
 * @brief 开始预加载播放
 * @param player 播放器指针
 * @return 成功返回0，失败返回错误代码
 */
int avi_start_preload_playback(avi_preload_player_t* player);

/**
 * @brief 停止预加载播放
 * @param player 播放器指针
 */
void avi_stop_preload_playback(avi_preload_player_t* player);

/**
 * @brief 获取当前帧的RGB数据
 * @param player 播放器指针
 * @param width 输出图像宽度
 * @param height 输出图像高度
 * @param rgb_data 输出RGB数据指针
 * @param rgb_size 输出RGB数据大小
 * @return 成功返回0，失败返回错误代码
 */
int avi_get_current_frame_rgb(avi_preload_player_t* player, uint32_t* width, uint32_t* height, 
                              uint8_t** rgb_data, uint32_t* rgb_size);

/**
 * @brief 切换到下一帧
 * @param player 播放器指针
 * @return 成功返回0，失败返回错误代码
 */
int avi_switch_to_next_frame(avi_preload_player_t* player);

/**
 * @brief 设置播放参数
 * @param player 播放器指针
 * @param enable_preload 是否启用预加载
 * @param frame_skip_ratio 跳帧比例 (1=不跳帧)
 */
void avi_set_playback_params(avi_preload_player_t* player, int enable_preload, int frame_skip_ratio);

/**
 * @brief 获取播放统计信息
 * @param player 播放器指针
 * @param frames_decoded 输出已解码帧数
 * @param frames_skipped 输出跳过帧数
 * @param decode_errors 输出解码错误数
 */
void avi_get_playback_stats(avi_preload_player_t* player, uint32_t* frames_decoded, 
                           uint32_t* frames_skipped, uint32_t* decode_errors);

/**
 * @brief 检查是否有新帧就绪
 * @param player 播放器指针
 * @return 1表示有新帧，0表示没有
 */
int avi_is_next_frame_ready(avi_preload_player_t* player);

/**
 * @brief 获取当前内存使用情况
 * @param player 播放器指针
 * @param heap_free 输出剩余堆内存
 * @param spiram_free 输出剩余SPIRAM内存
 */
void avi_get_memory_usage(avi_preload_player_t* player, size_t* heap_free, size_t* spiram_free);

/**
 * @brief 加载并播放AVI文件 (完整示例)
 * @param avi_file_path AVI文件路径
 * @param use_rgb565 是否使用RGB565格式 (否则RGB888)
 * @param enable_preload 是否启用预加载
 * @param frame_skip_ratio 跳帧比例 (1=不跳帧)
 * @return 成功返回0，失败返回错误代码
 */
int avi_load_and_play_file(const char* avi_file_path, int use_rgb565, int enable_preload, int frame_skip_ratio);

/**
 * @brief 简单的AVI文件播放示例 (使用默认参数)
 * @param avi_file_path AVI文件路径
 * @return 成功返回0，失败返回错误代码
 */
int avi_play_file_simple(const char* avi_file_path);

/**
 * @brief 逐帧处理AVI文件 (不使用预加载)
 * @param avi_file_path AVI文件路径
 * @param frame_callback 帧处理回调函数
 * @param user_data 用户数据
 * @return 成功返回0，失败返回错误代码
 */
typedef int (*avi_frame_callback_t)(uint32_t frame_index, uint8_t* rgb_data, uint32_t width, uint32_t height, void* user_data);
int avi_process_frames(const char* avi_file_path, avi_frame_callback_t frame_callback, void* user_data);

#ifdef __cplusplus
}
#endif 