// Copyright 2024 Espressif Systems (Shanghai) CO., LTD.
// All rights reserved.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "avi_parser.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg/decoder.h"

static const char* TAG = "AVI_PRELOAD";

// 内部函数声明
static int parse_riff_header(uint8_t* data, size_t size, avi_container_t* container);
static int parse_list_hdrl(uint8_t* data, size_t size, avi_container_t* container);
static int parse_list_movi(uint8_t* data, size_t size, avi_container_t* container);
static int parse_avih_chunk(uint8_t* data, size_t size, avi_header_t* header);
static int parse_strl_chunk(uint8_t* data, size_t size, avi_container_t* container);
static int find_jpeg_frame_boundaries(uint8_t* data, size_t size, uint32_t* start, uint32_t* end);
static uint32_t read_uint32_le(uint8_t* data);
static uint32_t read_uint32_be(uint8_t* data);
static int parse_avi_stream(FILE* file, avi_container_t* container);
static int read_chunk_from_file(FILE* file, avi_chunk_t* chunk);
static int read_frame_data(FILE* file, avi_container_t* container);
static int parse_avi_structure(FILE* file, avi_container_t* container);
static int parse_header_list(FILE* file, avi_container_t* container);
static int parse_movi_list(FILE* file, avi_container_t* container);
static int parse_stream_list(FILE* file, avi_container_t* container);

// 预加载播放器内部函数声明
static int init_memory_pool(avi_memory_pool_t* pool, uint32_t max_jpeg_size, uint32_t max_rgb_size);
static void cleanup_memory_pool(avi_memory_pool_t* pool);
static uint8_t* allocate_jpeg_buffer(avi_memory_pool_t* pool);
static uint8_t* allocate_rgb_buffer(avi_memory_pool_t* pool);
static void release_jpeg_buffer(avi_memory_pool_t* pool, uint8_t* buffer);
static void release_rgb_buffer(avi_memory_pool_t* pool, uint8_t* buffer);
static int decode_frame_to_rgb(avi_preload_player_t* player, avi_preload_buffer_t* buffer);
static void preload_task_function(void* param);
static int load_frame_data(avi_preload_player_t* player, uint32_t frame_index, avi_preload_buffer_t* buffer);
static void cleanup_preload_buffer(avi_preload_buffer_t* buffer, avi_memory_pool_t* pool);

// 解析AVI文件
avi_container_t* avi_parse_file(const char* filename) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        return NULL;
    }
    
    // 分配容器结构
    avi_container_t* container = (avi_container_t*)calloc(1, sizeof(avi_container_t));
    if (!container) {
        fclose(file);
        return NULL;
    }
    
    container->is_valid = 1;
    
    // 边读边解析
    if (parse_avi_stream(file, container) != AVI_ERR_OK) {
        avi_free_container(container);
        fclose(file);
        return NULL;
    }
    
    fclose(file);
    return container;
}

// 解析AVI缓冲区数据
avi_container_t* avi_parse_buffer(uint8_t* data, size_t size) {
    if (!data || size < 12) {
        return NULL;
    }
    
    // 验证AVI格式
    if (!avi_validate_format(data, size)) {
        return NULL;
    }
    
    // 分配容器结构
    avi_container_t* container = (avi_container_t*)calloc(1, sizeof(avi_container_t));
    if (!container) {
        return NULL;
    }
    
    container->file_data = data;
    container->file_size = size;
    container->is_valid = 1;
    
    // 解析RIFF头部
    if (parse_riff_header(data, size, container) != AVI_ERR_OK) {
        avi_free_container(container);
        return NULL;
    }
    
    return container;
}

// 释放AVI容器资源
void avi_free_container(avi_container_t* container) {
    if (!container) {
        return;
    }
    
    if (container->frames) {
        free(container->frames);
    }
    
    free(container);
}

// 获取指定帧
avi_frame_t* avi_get_frame(avi_container_t* container, uint32_t frame_index) {
    if (!container || !container->is_valid || frame_index >= container->frame_count) {
        return NULL;
    }
    
    return &container->frames[frame_index];
}

// 获取帧数量
uint32_t avi_get_frame_count(avi_container_t* container) {
    if (!container || !container->is_valid) {
        return 0;
    }
    
    return container->frame_count;
}

// 获取视频宽度
uint32_t avi_get_width(avi_container_t* container) {
    if (!container || !container->is_valid) {
        return 0;
    }
    
    return container->header.width;
}

// 获取视频高度
uint32_t avi_get_height(avi_container_t* container) {
    if (!container || !container->is_valid) {
        return 0;
    }
    
    return container->header.height;
}

// 获取帧率
uint32_t avi_get_frame_rate(avi_container_t* container) {
    if (!container || !container->is_valid) {
        return 0;
    }
    
    return container->frame_rate;
}

// 验证AVI格式
int avi_validate_format(uint8_t* data, size_t size) {
    if (size < 12) {
        return 0;
    }
    
    // 检查RIFF标识
    if (read_uint32_be(data) != AVI_RIFF_ID) {
        return 0;
    }
    
    // 检查AVI标识
    if (read_uint32_be(data + 8) != AVI_AVI_ID) {
        return 0;
    }
    
    return 1;
}

// 查找下一个块
int avi_find_next_chunk(uint8_t* data, size_t size, uint32_t offset, avi_chunk_t* chunk) {
    if (!data || !chunk || offset >= size - 8) {
        return AVI_ERR_INVALID_PARAM;
    }
    
    chunk->offset = offset;
    chunk->id = read_uint32_be(data + offset);
    chunk->size = read_uint32_le(data + offset + 4);
    
    return AVI_ERR_OK;
}

// 解析RIFF头部
static int parse_riff_header(uint8_t* data, size_t size, avi_container_t* container) {
    uint32_t offset = 12; // 跳过RIFF头部
    
    while (offset < size - 8) {
        avi_chunk_t chunk;
        if (avi_find_next_chunk(data, size, offset, &chunk) != AVI_ERR_OK) {
            break;
        }
        
        if (chunk.id == AVI_LIST_ID) {
            uint32_t list_type = read_uint32_be(data + offset + 8);
            if (list_type == AVI_HDRL_ID) {
                // 解析头部信息
                if (parse_list_hdrl(data + offset, chunk.size, container) != AVI_ERR_OK) {
                    return AVI_ERR_INVALID_FORMAT;
                }
            } else if (list_type == AVI_MOVI_ID) {
                // 解析视频数据
                if (parse_list_movi(data + offset, chunk.size, container) != AVI_ERR_OK) {
                    return AVI_ERR_INVALID_FORMAT;
                }
            }
        }
        
        offset += 8 + chunk.size;
        if (chunk.size % 2) {
            offset++; // 对齐到字边界
        }
    }
    
    return AVI_ERR_OK;
}

// 解析LIST hdrl块
static int parse_list_hdrl(uint8_t* data, size_t size, avi_container_t* container) {
    uint32_t offset = 8; // 跳过LIST头部
    
    while (offset < size - 8) {
        avi_chunk_t chunk;
        if (avi_find_next_chunk(data, size, offset, &chunk) != AVI_ERR_OK) {
            break;
        }
        
        if (chunk.id == AVI_AVIH_ID) {
            // 解析主头部信息
            if (parse_avih_chunk(data + offset, chunk.size, &container->header) != AVI_ERR_OK) {
                return AVI_ERR_INVALID_FORMAT;
            }
        } else if (chunk.id == AVI_STRL_ID) {
            // 解析流信息
            if (parse_strl_chunk(data + offset, chunk.size, container) != AVI_ERR_OK) {
                return AVI_ERR_INVALID_FORMAT;
            }
        }
        
        offset += 8 + chunk.size;
        if (chunk.size % 2) {
            offset++; // 对齐到字边界
        }
    }
    
    return AVI_ERR_OK;
}

// 解析LIST movi块
static int parse_list_movi(uint8_t* data, size_t size, avi_container_t* container) {
    uint32_t offset = 8; // 跳过LIST头部
    uint32_t frame_count = 0;
    
    // 第一次遍历：计算帧数量
    uint32_t temp_offset = offset;
    while (temp_offset < size - 8) {
        avi_chunk_t chunk;
        if (avi_find_next_chunk(data, size, temp_offset, &chunk) != AVI_ERR_OK) {
            break;
        }
        
        if (chunk.id == AVI_00DC_ID) {
            frame_count++;
        }
        
        temp_offset += 8 + chunk.size;
        if (chunk.size % 2) {
            temp_offset++; // 对齐到字边界
        }
    }
    
    // 分配帧数组
    container->frames = (avi_frame_t*)calloc(frame_count, sizeof(avi_frame_t));
    if (!container->frames) {
        return AVI_ERR_NO_MEMORY;
    }
    
    container->frame_count = frame_count;
    
    // 第二次遍历：填充帧信息
    uint32_t frame_index = 0;
    while (offset < size - 8 && frame_index < frame_count) {
        avi_chunk_t chunk;
        if (avi_find_next_chunk(data, size, offset, &chunk) != AVI_ERR_OK) {
            break;
        }
        
        if (chunk.id == AVI_00DC_ID) {
            avi_frame_t* frame = &container->frames[frame_index];
            frame->offset = offset + 8; // 跳过块头部
            frame->size = chunk.size;
            frame->data = data + frame->offset;
            frame->timestamp = frame_index; // 简单的时间戳
            frame->is_key_frame = 1; // MJPEG中每帧都是关键帧
            
            frame_index++;
        }
        
        offset += 8 + chunk.size;
        if (chunk.size % 2) {
            offset++; // 对齐到字边界
        }
    }
    
    return AVI_ERR_OK;
}

// 解析avih块
static int parse_avih_chunk(uint8_t* data, size_t size, avi_header_t* header) {
    if (size < sizeof(avi_header_t)) {
        return AVI_ERR_INVALID_FORMAT;
    }
    
    memcpy(header, data, sizeof(avi_header_t));
    
    // 转换字节序（如果需要）
    header->micro_sec_per_frame = read_uint32_le((uint8_t*)&header->micro_sec_per_frame);
    header->max_bytes_per_sec = read_uint32_le((uint8_t*)&header->max_bytes_per_sec);
    header->padding_granularity = read_uint32_le((uint8_t*)&header->padding_granularity);
    header->flags = read_uint32_le((uint8_t*)&header->flags);
    header->total_frames = read_uint32_le((uint8_t*)&header->total_frames);
    header->initial_frames = read_uint32_le((uint8_t*)&header->initial_frames);
    header->streams = read_uint32_le((uint8_t*)&header->streams);
    header->suggested_buffer_size = read_uint32_le((uint8_t*)&header->suggested_buffer_size);
    header->width = read_uint32_le((uint8_t*)&header->width);
    header->height = read_uint32_le((uint8_t*)&header->height);
    
    return AVI_ERR_OK;
}

// 解析strl块
static int parse_strl_chunk(uint8_t* data, size_t size, avi_container_t* container) {
    uint32_t offset = 0;
    
    while (offset < size - 8) {
        avi_chunk_t chunk;
        if (avi_find_next_chunk(data, size, offset, &chunk) != AVI_ERR_OK) {
            break;
        }
        
        if (chunk.id == AVI_STRH_ID) {
            // 解析流头部
            uint32_t stream_type = read_uint32_be(data + offset + 8);
            if (stream_type == AVI_VIDS_ID) {
                // 视频流
                container->video_stream_id = read_uint32_le(data + offset + 12);
                
                // 计算帧率
                uint32_t scale = read_uint32_le(data + offset + 20);
                uint32_t rate = read_uint32_le(data + offset + 24);
                if (scale > 0) {
                    container->frame_rate = rate / scale;
                }
            }
        }
        
        offset += 8 + chunk.size;
        if (chunk.size % 2) {
            offset++; // 对齐到字边界
        }
    }
    
    return AVI_ERR_OK;
}

// 查找JPEG帧边界
static int find_jpeg_frame_boundaries(uint8_t* data, size_t size, uint32_t* start, uint32_t* end) {
    for (uint32_t i = 0; i < size - 1; i++) {
        if (data[i] == 0xFF && data[i+1] == 0xD8) {
            *start = i;
            // 查找帧结束
            for (uint32_t j = i + 2; j < size - 1; j++) {
                if (data[j] == 0xFF && data[j+1] == 0xD9) {
                    *end = j + 2;
                    return AVI_ERR_OK;
                }
            }
            break;
        }
    }
    
    return AVI_ERR_INVALID_FORMAT;
}

// 读取小端序32位整数
static uint32_t read_uint32_le(uint8_t* data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

// 读取大端序32位整数
static uint32_t read_uint32_be(uint8_t* data) {
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

// 流式解析AVI文件
static int parse_avi_stream(FILE* file, avi_container_t* container) {
    uint8_t header_buffer[12];
    
    // 读取RIFF头部
    if (fread(header_buffer, 1, 12, file) != 12) {
        return AVI_ERR_READ_FAILED;
    }
    
    // 验证AVI格式
    if (read_uint32_be(header_buffer) != AVI_RIFF_ID ||
        read_uint32_be(header_buffer + 8) != AVI_AVI_ID) {
        return AVI_ERR_INVALID_FORMAT;
    }
    
    // 解析文件结构
    return parse_avi_structure(file, container);
}

// 从文件读取块信息
static int read_chunk_from_file(FILE* file, avi_chunk_t* chunk) {
    uint8_t chunk_header[8];
    
    if (fread(chunk_header, 1, 8, file) != 8) {
        return AVI_ERR_READ_FAILED;
    }
    
    chunk->id = read_uint32_be(chunk_header);
    chunk->size = read_uint32_le(chunk_header + 4);
    
    return AVI_ERR_OK;
}

// 解析AVI文件结构
static int parse_avi_structure(FILE* file, avi_container_t* container) {
    avi_chunk_t chunk;
    
    while (!feof(file)) {
        if (read_chunk_from_file(file, &chunk) != AVI_ERR_OK) {
            break;
        }
        
        if (chunk.id == AVI_LIST_ID) {
            // 读取LIST类型
            uint8_t list_type[4];
            if (fread(list_type, 1, 4, file) != 4) {
                return AVI_ERR_READ_FAILED;
            }
            
            uint32_t list_id = read_uint32_be(list_type);
            if (list_id == AVI_HDRL_ID) {
                // 解析头部信息
                if (parse_header_list(file, container) != AVI_ERR_OK) {
                    return AVI_ERR_INVALID_FORMAT;
                }
            } else if (list_id == AVI_MOVI_ID) {
                // 解析视频数据
                if (parse_movi_list(file, container) != AVI_ERR_OK) {
                    return AVI_ERR_INVALID_FORMAT;
                }
            }
        }
        
        // 跳过剩余数据
        long current_pos = ftell(file);
        fseek(file, current_pos + chunk.size - 4, SEEK_SET);
        
        // 对齐到字边界
        if (chunk.size % 2) {
            fseek(file, 1, SEEK_CUR);
        }
    }
    
    return AVI_ERR_OK;
}

// 解析头部列表
static int parse_header_list(FILE* file, avi_container_t* container) {
    avi_chunk_t chunk;
    
    while (!feof(file)) {
        if (read_chunk_from_file(file, &chunk) != AVI_ERR_OK) {
            break;
        }
        
        if (chunk.id == AVI_AVIH_ID) {
            // 读取主头部信息
            uint8_t* header_data = (uint8_t*)malloc(chunk.size);
            if (!header_data) {
                return AVI_ERR_NO_MEMORY;
            }
            
            if (fread(header_data, 1, chunk.size, file) != chunk.size) {
                free(header_data);
                return AVI_ERR_READ_FAILED;
            }
            
            parse_avih_chunk(header_data, chunk.size, &container->header);
            free(header_data);
        } else if (chunk.id == AVI_STRL_ID) {
            // 解析流信息
            if (parse_stream_list(file, container) != AVI_ERR_OK) {
                return AVI_ERR_INVALID_FORMAT;
            }
        } else {
            // 跳过其他块
            fseek(file, chunk.size, SEEK_CUR);
        }
    }
    
    return AVI_ERR_OK;
}

// 解析视频数据列表
static int parse_movi_list(FILE* file, avi_container_t* container) {
    avi_chunk_t chunk;
    uint32_t frame_count = 0;
    
    // 第一次遍历：计算帧数量
    long start_pos = ftell(file);
    while (!feof(file)) {
        if (read_chunk_from_file(file, &chunk) != AVI_ERR_OK) {
            break;
        }
        
        if (chunk.id == AVI_00DC_ID) {
            frame_count++;
        }
        
        fseek(file, chunk.size, SEEK_CUR);
        if (chunk.size % 2) {
            fseek(file, 1, SEEK_CUR);
        }
    }
    
    // 分配帧数组
    container->frames = (avi_frame_t*)calloc(frame_count, sizeof(avi_frame_t));
    if (!container->frames) {
        return AVI_ERR_NO_MEMORY;
    }
    
    container->frame_count = frame_count;
    
    // 第二次遍历：填充帧信息
    fseek(file, start_pos, SEEK_SET);
    uint32_t frame_index = 0;
    
    while (!feof(file) && frame_index < frame_count) {
        if (read_chunk_from_file(file, &chunk) != AVI_ERR_OK) {
            break;
        }
        
        if (chunk.id == AVI_00DC_ID) {
            avi_frame_t* frame = &container->frames[frame_index];
            frame->offset = ftell(file);
            frame->size = chunk.size;
            frame->timestamp = frame_index;
            frame->is_key_frame = 1;
            
            // 读取帧数据到内存（可选，也可以延迟读取）
            frame->data = (uint8_t*)malloc(chunk.size);
            if (frame->data) {
                fread(frame->data, 1, chunk.size, file);
            } else {
                fseek(file, chunk.size, SEEK_CUR);
            }
            
            frame_index++;
        } else {
            fseek(file, chunk.size, SEEK_CUR);
        }
        
        if (chunk.size % 2) {
            fseek(file, 1, SEEK_CUR);
        }
    }
    
    return AVI_ERR_OK;
}

// 解析流列表
static int parse_stream_list(FILE* file, avi_container_t* container) {
    avi_chunk_t chunk;
    
    while (!feof(file)) {
        if (read_chunk_from_file(file, &chunk) != AVI_ERR_OK) {
            break;
        }
        
        if (chunk.id == AVI_STRH_ID) {
            uint8_t* stream_data = (uint8_t*)malloc(chunk.size);
            if (!stream_data) {
                return AVI_ERR_NO_MEMORY;
            }
            
            if (fread(stream_data, 1, chunk.size, file) != chunk.size) {
                free(stream_data);
                return AVI_ERR_READ_FAILED;
            }
            
            // 解析流头部
            uint32_t stream_type = read_uint32_be(stream_data);
            if (stream_type == AVI_VIDS_ID) {
                container->video_stream_id = read_uint32_le(stream_data + 4);
                
                // 计算帧率
                uint32_t scale = read_uint32_le(stream_data + 20);
                uint32_t rate = read_uint32_le(stream_data + 24);
                if (scale > 0) {
                    container->frame_rate = rate / scale;
                }
            }
            
            free(stream_data);
        } else {
            fseek(file, chunk.size, SEEK_CUR);
        }
    }
    
    return AVI_ERR_OK;
}

// =============================================================================
// 预加载播放器实现
// =============================================================================

// 创建预加载视频播放器
avi_preload_player_t* avi_create_preload_player(avi_container_t* container, int use_rgb565) {
    if (!container || !container->is_valid || container->frame_count == 0) {
        ESP_LOGE(TAG, "无效的AVI容器");
        return NULL;
    }
    
    // 分配播放器结构
    avi_preload_player_t* player = (avi_preload_player_t*)calloc(1, sizeof(avi_preload_player_t));
    if (!player) {
        ESP_LOGE(TAG, "播放器内存分配失败");
        return NULL;
    }
    
    // 初始化基本参数
    player->container = container;
    player->use_rgb565 = use_rgb565;
    player->frame_rate = container->frame_rate > 0 ? container->frame_rate : 15;  // 默认15fps
    player->frame_delay_ms = 1000 / player->frame_rate;
    player->enable_preload = 1;  // 默认启用预加载
    player->frame_skip_ratio = 1;  // 默认不跳帧
    player->current_frame_index = 0;
    player->is_playing = 0;
    
    // 计算最大帧大小
    uint32_t max_jpeg_size = 0;
    for (uint32_t i = 0; i < container->frame_count; i++) {
        if (container->frames[i].size > max_jpeg_size) {
            max_jpeg_size = container->frames[i].size;
        }
    }
    
    // 为安全起见，增加20%的缓冲
    max_jpeg_size = (max_jpeg_size * 120) / 100;
    
    // 计算RGB缓冲区大小
    uint32_t max_rgb_size;
    if (use_rgb565) {
        max_rgb_size = container->header.width * container->header.height * 2;  // RGB565: 2字节/像素
    } else {
        max_rgb_size = container->header.width * container->header.height * 3;  // RGB888: 3字节/像素
    }
    
    // 初始化内存池
    if (init_memory_pool(&player->memory_pool, max_jpeg_size, max_rgb_size) != AVI_ERR_OK) {
        ESP_LOGE(TAG, "内存池初始化失败");
        free(player);
        return NULL;
    }
    
    // 初始化JPEG解码器 (流式解码优化)
    player->jpeg_handle = (jpeg_handle_t*)malloc(sizeof(jpeg_handle_t));
    if (!player->jpeg_handle) {
        ESP_LOGE(TAG, "JPEG解码器句柄分配失败");
        cleanup_memory_pool(&player->memory_pool);
        free(player);
        return NULL;
    }
    
    jpeg_error_t jpeg_ret = esp_jpeg_stream_open(player->jpeg_handle);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG流式解码器初始化失败: %d", jpeg_ret);
        free(player->jpeg_handle);
        cleanup_memory_pool(&player->memory_pool);
        free(player);
        return NULL;
    }
    
    ESP_LOGI(TAG, "预加载播放器创建成功: %dx%d, %d帧, %dfps, JPEG最大%d字节, RGB最大%d字节 (使用流式解码器)", 
             container->header.width, container->header.height, 
             container->frame_count, player->frame_rate,
             max_jpeg_size, max_rgb_size);
    
    return player;
}

// 销毁预加载视频播放器
void avi_destroy_preload_player(avi_preload_player_t* player) {
    if (!player) {
        return;
    }
    
    // 停止播放
    avi_stop_preload_playback(player);
    
    // 清理预加载缓冲区
    cleanup_preload_buffer(&player->current_frame, &player->memory_pool);
    cleanup_preload_buffer(&player->next_frame, &player->memory_pool);
    
    // 清理JPEG解码器 (流式解码优化)
    if (player->jpeg_handle) {
        jpeg_error_t jpeg_ret = esp_jpeg_stream_close(player->jpeg_handle);
        if (jpeg_ret != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "JPEG流式解码器关闭失败: %d", jpeg_ret);
        }
        free(player->jpeg_handle);
        player->jpeg_handle = NULL;
    }
    
    // 清理内存池
    cleanup_memory_pool(&player->memory_pool);
    
    // 释放播放器结构
    free(player);
    
    ESP_LOGI(TAG, "预加载播放器已销毁 (包含流式解码器)");
}

// 开始预加载播放
int avi_start_preload_playback(avi_preload_player_t* player) {
    if (!player || !player->container) {
        return AVI_ERR_INVALID_PARAM;
    }
    
    if (player->is_playing) {
        ESP_LOGW(TAG, "播放器已在运行");
        return AVI_ERR_OK;
    }
    
    // 重置播放状态
    player->current_frame_index = 0;
    player->frames_decoded = 0;
    player->frames_skipped = 0;
    player->decode_errors = 0;
    
    // 加载第一帧
    if (load_frame_data(player, 0, &player->current_frame) != AVI_ERR_OK) {
        ESP_LOGE(TAG, "加载第一帧失败");
        return AVI_ERR_INVALID_FORMAT;
    }
    
    // 解码第一帧 (使用流式解码器)
    if (decode_frame_to_rgb(player, &player->current_frame) != AVI_ERR_OK) {
        ESP_LOGE(TAG, "流式解码第一帧失败");
        return AVI_ERR_INVALID_FORMAT;
    }
    
    player->current_frame.is_ready = 1;
    player->current_frame.is_valid = 1;
    player->frames_decoded++;
    
    // 启动播放
    player->is_playing = 1;
    
    // 如果启用预加载且有下一帧，创建预加载任务
    if (player->enable_preload && player->container->frame_count > 1) {
        xTaskCreate(preload_task_function, "avi_preload", 4096, player, 5, NULL);
        ESP_LOGI(TAG, "预加载任务已启动");
    }
    
    ESP_LOGI(TAG, "预加载播放开始: %d帧", player->container->frame_count);
    return AVI_ERR_OK;
}

// 停止预加载播放
void avi_stop_preload_playback(avi_preload_player_t* player) {
    if (!player) {
        return;
    }
    
    player->is_playing = 0;
    
    // 等待预加载任务结束 (任务会自动检查is_playing并退出)
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "预加载播放已停止");
}

// 获取当前帧的RGB数据
int avi_get_current_frame_rgb(avi_preload_player_t* player, uint32_t* width, uint32_t* height, 
                              uint8_t** rgb_data, uint32_t* rgb_size) {
    if (!player || !width || !height || !rgb_data || !rgb_size) {
        return AVI_ERR_INVALID_PARAM;
    }
    
    if (!player->current_frame.is_ready || !player->current_frame.is_valid) {
        ESP_LOGW(TAG, "当前帧未就绪");
        return AVI_ERR_INVALID_FORMAT;
    }
    
    *width = player->current_frame.width;
    *height = player->current_frame.height;
    *rgb_data = player->current_frame.rgb_data;
    *rgb_size = player->current_frame.rgb_size;
    
    return AVI_ERR_OK;
}

// 切换到下一帧
int avi_switch_to_next_frame(avi_preload_player_t* player) {
    if (!player) {
        return AVI_ERR_INVALID_PARAM;
    }
    
    // 检查是否到达末尾
    if (player->current_frame_index >= player->container->frame_count - 1) {
        ESP_LOGI(TAG, "播放结束");
        return AVI_ERR_INVALID_FORMAT;
    }
    
    // 如果启用预加载且下一帧就绪，直接切换
    if (player->enable_preload && player->next_frame.is_ready && player->next_frame.is_valid) {
        // 释放当前帧的RGB缓冲区
        if (player->current_frame.rgb_data) {
            release_rgb_buffer(&player->memory_pool, player->current_frame.rgb_data);
        }
        
        // 交换缓冲区
        avi_preload_buffer_t temp = player->current_frame;
        player->current_frame = player->next_frame;
        player->next_frame = temp;
        
        // 重置下一帧状态
        player->next_frame.is_ready = 0;
        player->next_frame.is_valid = 0;
        player->next_frame.is_decoding = 0;
        player->next_frame.jpeg_data = NULL;
        player->next_frame.rgb_data = NULL;
        
        player->current_frame_index++;
        ESP_LOGD(TAG, "切换到预加载帧 %d", player->current_frame_index);
    } else {
        // 没有预加载，直接加载下一帧
        uint32_t next_index = player->current_frame_index + 1;
        
        // 考虑跳帧
        if (player->frame_skip_ratio > 1) {
            next_index = player->current_frame_index + player->frame_skip_ratio;
            if (next_index >= player->container->frame_count) {
                next_index = player->container->frame_count - 1;
            }
            player->frames_skipped += (next_index - player->current_frame_index - 1);
        }
        
        // 清理当前帧
        cleanup_preload_buffer(&player->current_frame, &player->memory_pool);
        
        // 加载新帧
        if (load_frame_data(player, next_index, &player->current_frame) != AVI_ERR_OK) {
            ESP_LOGE(TAG, "加载帧 %d 失败", next_index);
            player->decode_errors++;
            return AVI_ERR_INVALID_FORMAT;
        }
        
        // 解码新帧 (使用流式解码器)
        if (decode_frame_to_rgb(player, &player->current_frame) != AVI_ERR_OK) {
            ESP_LOGE(TAG, "流式解码帧 %d 失败", next_index);
            player->decode_errors++;
            return AVI_ERR_INVALID_FORMAT;
        }
        
        player->current_frame.is_ready = 1;
        player->current_frame.is_valid = 1;
        player->current_frame_index = next_index;
        player->frames_decoded++;
        
        ESP_LOGD(TAG, "直接加载帧 %d", next_index);
    }
    
    return AVI_ERR_OK;
}

// 设置播放参数
void avi_set_playback_params(avi_preload_player_t* player, int enable_preload, int frame_skip_ratio) {
    if (!player) {
        return;
    }
    
    player->enable_preload = enable_preload;
    player->frame_skip_ratio = frame_skip_ratio > 0 ? frame_skip_ratio : 1;
    
    ESP_LOGI(TAG, "播放参数更新: 预加载=%s, 跳帧比例=%d", 
             enable_preload ? "启用" : "禁用", player->frame_skip_ratio);
}

// 获取播放统计信息
void avi_get_playback_stats(avi_preload_player_t* player, uint32_t* frames_decoded, 
                           uint32_t* frames_skipped, uint32_t* decode_errors) {
    if (!player) {
        return;
    }
    
    if (frames_decoded) *frames_decoded = player->frames_decoded;
    if (frames_skipped) *frames_skipped = player->frames_skipped;
    if (decode_errors) *decode_errors = player->decode_errors;
}

// 检查是否有新帧就绪
int avi_is_next_frame_ready(avi_preload_player_t* player) {
    if (!player) {
        return 0;
    }
    
    return (player->next_frame.is_ready && player->next_frame.is_valid) ? 1 : 0;
}

// 获取当前内存使用情况
void avi_get_memory_usage(avi_preload_player_t* player, size_t* heap_free, size_t* spiram_free) {
    if (heap_free) {
        *heap_free = esp_get_free_heap_size();
    }
    if (spiram_free) {
        *spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }
    
    if (player) {
        ESP_LOGD(TAG, "内存使用: 堆=%zu, SPIRAM=%zu, 当前帧索引=%d", 
                 heap_free ? *heap_free : 0, spiram_free ? *spiram_free : 0, 
                 player->current_frame_index);
    }
}

// =============================================================================
// 内部函数实现
// =============================================================================

// 初始化内存池
static int init_memory_pool(avi_memory_pool_t* pool, uint32_t max_jpeg_size, uint32_t max_rgb_size) {
    if (!pool) {
        return AVI_ERR_INVALID_PARAM;
    }
    
    memset(pool, 0, sizeof(avi_memory_pool_t));
    
    pool->max_jpeg_size = max_jpeg_size;
    pool->max_rgb_size = max_rgb_size;
    
    // 分配JPEG缓冲区池 (优先使用SPIRAM)
    for (int i = 0; i < 2; i++) {
        pool->jpeg_pool[i] = (uint8_t*)heap_caps_malloc(max_jpeg_size, MALLOC_CAP_SPIRAM);
        if (!pool->jpeg_pool[i]) {
            // SPIRAM不足，尝试使用普通堆内存
            pool->jpeg_pool[i] = (uint8_t*)malloc(max_jpeg_size);
            if (!pool->jpeg_pool[i]) {
                ESP_LOGE(TAG, "JPEG缓冲区 %d 分配失败", i);
                cleanup_memory_pool(pool);
                return AVI_ERR_NO_MEMORY;
            }
            ESP_LOGW(TAG, "JPEG缓冲区 %d 使用堆内存 (%d字节)", i, max_jpeg_size);
        } else {
            ESP_LOGI(TAG, "JPEG缓冲区 %d 使用SPIRAM (%d字节)", i, max_jpeg_size);
        }
        pool->pool_used[i] = 0;
    }
    
    // 分配RGB缓冲区池 (优先使用SPIRAM)
    for (int i = 0; i < 2; i++) {
        pool->rgb_pool[i] = (uint8_t*)heap_caps_malloc(max_rgb_size, MALLOC_CAP_SPIRAM);
        if (!pool->rgb_pool[i]) {
            // SPIRAM不足，尝试使用普通堆内存
            pool->rgb_pool[i] = (uint8_t*)malloc(max_rgb_size);
            if (!pool->rgb_pool[i]) {
                ESP_LOGE(TAG, "RGB缓冲区 %d 分配失败", i);
                cleanup_memory_pool(pool);
                return AVI_ERR_NO_MEMORY;
            }
            ESP_LOGW(TAG, "RGB缓冲区 %d 使用堆内存 (%d字节)", i, max_rgb_size);
        } else {
            ESP_LOGI(TAG, "RGB缓冲区 %d 使用SPIRAM (%d字节)", i, max_rgb_size);
        }
    }
    
    pool->is_initialized = 1;
    ESP_LOGI(TAG, "内存池初始化完成: JPEG=%d字节x2, RGB=%d字节x2", max_jpeg_size, max_rgb_size);
    
    return AVI_ERR_OK;
}

// 清理内存池
static void cleanup_memory_pool(avi_memory_pool_t* pool) {
    if (!pool) {
        return;
    }
    
    // 释放JPEG缓冲区
    for (int i = 0; i < 2; i++) {
        if (pool->jpeg_pool[i]) {
            free(pool->jpeg_pool[i]);
            pool->jpeg_pool[i] = NULL;
        }
    }
    
    // 释放RGB缓冲区
    for (int i = 0; i < 2; i++) {
        if (pool->rgb_pool[i]) {
            free(pool->rgb_pool[i]);
            pool->rgb_pool[i] = NULL;
        }
    }
    
    pool->is_initialized = 0;
    ESP_LOGI(TAG, "内存池已清理");
}

// 分配JPEG缓冲区
static uint8_t* allocate_jpeg_buffer(avi_memory_pool_t* pool) {
    if (!pool || !pool->is_initialized) {
        return NULL;
    }
    
    for (int i = 0; i < 2; i++) {
        if (!pool->pool_used[i] && pool->jpeg_pool[i]) {
            pool->pool_used[i] = 1;
            ESP_LOGD(TAG, "分配JPEG缓冲区 %d", i);
            return pool->jpeg_pool[i];
        }
    }
    
    ESP_LOGW(TAG, "没有可用的JPEG缓冲区");
    return NULL;
}

// 分配RGB缓冲区
static uint8_t* allocate_rgb_buffer(avi_memory_pool_t* pool) {
    if (!pool || !pool->is_initialized) {
        return NULL;
    }
    
    for (int i = 0; i < 2; i++) {
        if (pool->rgb_pool[i]) {
            ESP_LOGD(TAG, "分配RGB缓冲区 %d", i);
            return pool->rgb_pool[i];
        }
    }
    
    ESP_LOGW(TAG, "没有可用的RGB缓冲区");
    return NULL;
}

// 释放JPEG缓冲区
static void release_jpeg_buffer(avi_memory_pool_t* pool, uint8_t* buffer) {
    if (!pool || !buffer) {
        return;
    }
    
    for (int i = 0; i < 2; i++) {
        if (pool->jpeg_pool[i] == buffer) {
            pool->pool_used[i] = 0;
            ESP_LOGD(TAG, "释放JPEG缓冲区 %d", i);
            return;
        }
    }
    
    ESP_LOGW(TAG, "尝试释放未知的JPEG缓冲区");
}

// 释放RGB缓冲区 (RGB缓冲区不需要标记，因为每次都重新分配)
static void release_rgb_buffer(avi_memory_pool_t* pool, uint8_t* buffer) {
    // RGB缓冲区在内存池中是永久分配的，这里只是标记释放，实际不做任何操作
    ESP_LOGD(TAG, "释放RGB缓冲区");
}

// 解码帧到RGB (流式解码优化)
static int decode_frame_to_rgb(avi_preload_player_t* player, avi_preload_buffer_t* buffer) {
    if (!player || !player->jpeg_handle || !buffer || !buffer->jpeg_data || buffer->jpeg_size == 0) {
        return AVI_ERR_INVALID_PARAM;
    }
    
    buffer->is_decoding = 1;
    
    // 使用流式JPEG解码器解码 (性能优化)
    uint8_t* output_buf = NULL;
    int output_len = 0;
    
    ESP_LOGD(TAG, "开始流式解码帧: %d字节", buffer->jpeg_size);
    
    jpeg_error_t result = esp_jpeg_stream_decode(player->jpeg_handle, 
                                                buffer->jpeg_data, buffer->jpeg_size, 
                                                &output_buf, &output_len);
    
    if (result != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "流式JPEG解码失败: %d", result);
        buffer->is_decoding = 0;
        return AVI_ERR_INVALID_FORMAT;
    }
    
    // 复制解码数据到RGB缓冲区
    if (buffer->rgb_data && output_buf) {
        memcpy(buffer->rgb_data, output_buf, output_len);
        buffer->rgb_size = output_len;
        
        // BGR到RGB转换 (如果需要)
        if (!player->use_rgb565) {  // RGB888格式需要转换
            uint8_t* pixel_data = buffer->rgb_data;
            for (int i = 0; i < output_len; i += 3) {
                uint8_t temp = pixel_data[i];
                pixel_data[i] = pixel_data[i + 2];
                pixel_data[i + 2] = temp;
            }
            ESP_LOGD(TAG, "BGR转RGB转换完成");
        }
    }
    
    // 注意：流式解码器的内存由解码器管理，不需要手动释放output_buf
    // 这是相比esp_jpeg_decode_one_picture的另一个优势
    
    buffer->is_decoding = 0;
    ESP_LOGD(TAG, "流式解码完成: %dx%d, %d字节 (RGB%s)", 
             buffer->width, buffer->height, buffer->rgb_size,
             player->use_rgb565 ? "565" : "888");
    
    return AVI_ERR_OK;
}

// 预加载任务函数
static void preload_task_function(void* param) {
    avi_preload_player_t* player = (avi_preload_player_t*)param;
    
    ESP_LOGI(TAG, "预加载任务开始运行");
    
    while (player->is_playing) {
        // 检查下一帧是否已经就绪或正在解码
        if (player->next_frame.is_ready || player->next_frame.is_decoding) {
            vTaskDelay(pdMS_TO_TICKS(10));  // 短暂延迟
            continue;
        }
        
        // 计算下一帧索引
        uint32_t next_index = player->current_frame_index + 1;
        
        // 考虑跳帧
        if (player->frame_skip_ratio > 1) {
            next_index = player->current_frame_index + player->frame_skip_ratio;
        }
        
        // 检查是否到达末尾
        if (next_index >= player->container->frame_count) {
            ESP_LOGD(TAG, "已到达视频末尾，预加载任务结束");
            break;
        }
        
        // 检查内存使用情况
        size_t free_heap = esp_get_free_heap_size();
        if (free_heap < 5000) {  // 少于5KB时停止预加载
            ESP_LOGW(TAG, "内存不足 (%zu字节)，暂停预加载", free_heap);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // 加载下一帧数据
        if (load_frame_data(player, next_index, &player->next_frame) != AVI_ERR_OK) {
            ESP_LOGE(TAG, "预加载帧 %d 失败", next_index);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // 解码下一帧 (使用流式解码器)
        if (decode_frame_to_rgb(player, &player->next_frame) != AVI_ERR_OK) {
            ESP_LOGE(TAG, "流式预解码帧 %d 失败", next_index);
            cleanup_preload_buffer(&player->next_frame, &player->memory_pool);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // 标记为就绪
        player->next_frame.is_ready = 1;
        player->next_frame.is_valid = 1;
        
        ESP_LOGD(TAG, "预加载帧 %d 完成", next_index);
        
        // 控制预加载频率，避免过度消耗CPU
        vTaskDelay(pdMS_TO_TICKS(player->frame_delay_ms / 2));
    }
    
    ESP_LOGI(TAG, "预加载任务结束");
    vTaskDelete(NULL);
}

// 加载帧数据
static int load_frame_data(avi_preload_player_t* player, uint32_t frame_index, avi_preload_buffer_t* buffer) {
    if (!player || !buffer || frame_index >= player->container->frame_count) {
        return AVI_ERR_INVALID_PARAM;
    }
    
    avi_frame_t* frame = &player->container->frames[frame_index];
    
    // 分配JPEG缓冲区
    buffer->jpeg_data = allocate_jpeg_buffer(&player->memory_pool);
    if (!buffer->jpeg_data) {
        ESP_LOGE(TAG, "分配JPEG缓冲区失败");
        return AVI_ERR_NO_MEMORY;
    }
    
    // 分配RGB缓冲区
    buffer->rgb_data = allocate_rgb_buffer(&player->memory_pool);
    if (!buffer->rgb_data) {
        ESP_LOGE(TAG, "分配RGB缓冲区失败");
        release_jpeg_buffer(&player->memory_pool, buffer->jpeg_data);
        buffer->jpeg_data = NULL;
        return AVI_ERR_NO_MEMORY;
    }
    
    // 复制JPEG数据
    memcpy(buffer->jpeg_data, frame->data, frame->size);
    buffer->jpeg_size = frame->size;
    buffer->frame_index = frame_index;
    buffer->width = player->container->header.width;
    buffer->height = player->container->header.height;
    buffer->is_ready = 0;
    buffer->is_valid = 0;
    buffer->is_decoding = 0;
    
    ESP_LOGD(TAG, "加载帧 %d 数据: %d字节", frame_index, frame->size);
    
    return AVI_ERR_OK;
}

// 清理预加载缓冲区
static void cleanup_preload_buffer(avi_preload_buffer_t* buffer, avi_memory_pool_t* pool) {
    if (!buffer) {
        return;
    }
    
    if (buffer->jpeg_data) {
        release_jpeg_buffer(pool, buffer->jpeg_data);
        buffer->jpeg_data = NULL;
    }
    
    if (buffer->rgb_data) {
        release_rgb_buffer(pool, buffer->rgb_data);
        buffer->rgb_data = NULL;
    }
    
    buffer->jpeg_size = 0;
    buffer->rgb_size = 0;
    buffer->is_ready = 0;
    buffer->is_valid = 0;
    buffer->is_decoding = 0;
    
    ESP_LOGD(TAG, "清理预加载缓冲区");
}

// =============================================================================
// 文件加载和播放函数
// =============================================================================

// 加载并播放AVI文件 (完整示例)
int avi_load_and_play_file(const char* avi_file_path, int use_rgb565, int enable_preload, int frame_skip_ratio) {
    if (!avi_file_path) {
        ESP_LOGE(TAG, "无效的文件路径");
        return AVI_ERR_INVALID_PARAM;
    }
    
    ESP_LOGI(TAG, "开始加载AVI文件: %s", avi_file_path);
    
    // 1. 解析AVI文件
    avi_container_t* container = avi_parse_file(avi_file_path);
    if (!container) {
        ESP_LOGE(TAG, "AVI文件解析失败: %s", avi_file_path);
        return AVI_ERR_INVALID_FORMAT;
    }
    
    ESP_LOGI(TAG, "AVI文件解析成功: %dx%d, %d帧, %dfps", 
             container->header.width, container->header.height,
             container->frame_count, container->frame_rate);
    
    // 2. 创建预加载播放器
    avi_preload_player_t* player = avi_create_preload_player(container, use_rgb565);
    if (!player) {
        ESP_LOGE(TAG, "播放器创建失败");
        avi_free_container(container);
        return AVI_ERR_NO_MEMORY;
    }
    
    // 3. 设置播放参数
    avi_set_playback_params(player, enable_preload, frame_skip_ratio);
    
    // 4. 开始播放
    if (avi_start_preload_playback(player) != AVI_ERR_OK) {
        ESP_LOGE(TAG, "播放启动失败");
        avi_destroy_preload_player(player);
        avi_free_container(container);
        return AVI_ERR_INVALID_FORMAT;
    }
    
    // 5. 播放循环
    ESP_LOGI(TAG, "开始播放循环");
    uint32_t frame_count = 0;
    
    while (player->is_playing && frame_count < container->frame_count) {
        // 获取当前帧数据
        uint32_t width, height, rgb_size;
        uint8_t* rgb_data;
        
        if (avi_get_current_frame_rgb(player, &width, &height, &rgb_data, &rgb_size) == AVI_ERR_OK) {
            // 这里可以处理帧数据，比如显示到屏幕
            ESP_LOGD(TAG, "显示帧 %d: %dx%d, %d字节", 
                     frame_count, width, height, rgb_size);
            
            // TODO: 在这里添加显示代码，例如：
            // display_frame_on_screen(rgb_data, width, height);
            
            frame_count++;
        } else {
            ESP_LOGW(TAG, "获取帧 %d 失败", frame_count);
        }
        
        // 切换到下一帧
        if (avi_switch_to_next_frame(player) != AVI_ERR_OK) {
            ESP_LOGI(TAG, "播放结束或切换帧失败");
            break;
        }
        
        // 帧率控制
        vTaskDelay(pdMS_TO_TICKS(player->frame_delay_ms));
        
        // 内存监控
        if (frame_count % 30 == 0) {  // 每30帧检查一次内存
            size_t heap_free, spiram_free;
            avi_get_memory_usage(player, &heap_free, &spiram_free);
            ESP_LOGI(TAG, "播放进度: %d/%d, 内存: 堆=%zu, SPIRAM=%zu", 
                     frame_count, container->frame_count, heap_free, spiram_free);
        }
    }
    
    // 6. 获取播放统计
    uint32_t frames_decoded, frames_skipped, decode_errors;
    avi_get_playback_stats(player, &frames_decoded, &frames_skipped, &decode_errors);
    ESP_LOGI(TAG, "播放完成: 解码=%d帧, 跳过=%d帧, 错误=%d帧", 
             frames_decoded, frames_skipped, decode_errors);
    
    // 7. 清理资源
    avi_destroy_preload_player(player);
    avi_free_container(container);
    
    ESP_LOGI(TAG, "AVI文件播放完成: %s", avi_file_path);
    return AVI_ERR_OK;
}

// 简单的AVI文件播放示例 (使用默认参数)
int avi_play_file_simple(const char* avi_file_path) {
    // 使用默认参数：RGB565格式，启用预加载，不跳帧
    return avi_load_and_play_file(avi_file_path, 1, 1, 1);
}

// 逐帧处理AVI文件 (不使用预加载)
int avi_process_frames(const char* avi_file_path, avi_frame_callback_t frame_callback, void* user_data) {
    if (!avi_file_path || !frame_callback) {
        ESP_LOGE(TAG, "无效的参数");
        return AVI_ERR_INVALID_PARAM;
    }
    
    ESP_LOGI(TAG, "开始逐帧处理AVI文件: %s", avi_file_path);
    
    // 1. 解析AVI文件
    avi_container_t* container = avi_parse_file(avi_file_path);
    if (!container) {
        ESP_LOGE(TAG, "AVI文件解析失败: %s", avi_file_path);
        return AVI_ERR_INVALID_FORMAT;
    }
    
    ESP_LOGI(TAG, "AVI文件解析成功: %dx%d, %d帧", 
             container->header.width, container->header.height, container->frame_count);
    
    // 2. 逐帧处理
    uint32_t processed_frames = 0;
    uint32_t error_frames = 0;
    
    for (uint32_t i = 0; i < container->frame_count; i++) {
        avi_frame_t* frame = avi_get_frame(container, i);
        if (!frame || !frame->data) {
            ESP_LOGW(TAG, "获取帧 %d 失败", i);
            error_frames++;
            continue;
        }
        
        // 检查帧大小限制
        if (frame->size > 1024 * 1024) {  // 1MB限制
            ESP_LOGW(TAG, "帧 %d 太大 (%d字节)，跳过", i, frame->size);
            error_frames++;
            continue;
        }
        
        // 解码JPEG帧
        uint8_t* output_buf = NULL;
        int output_len = 0;
        
        jpeg_error_t result = esp_jpeg_decode_one_picture(frame->data, frame->size, 
                                                          &output_buf, &output_len);
        
        if (result != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "帧 %d JPEG解码失败: %d", i, result);
            error_frames++;
            continue;
        }
        
        // 调用回调函数处理帧
        int callback_result = frame_callback(i, output_buf, 
                                           container->header.width, 
                                           container->header.height, 
                                           user_data);
        
        if (callback_result != AVI_ERR_OK) {
            ESP_LOGW(TAG, "帧 %d 回调处理失败: %d", i, callback_result);
            error_frames++;
        } else {
            processed_frames++;
        }
        
        // 释放解码器分配的内存
        if (output_buf) {
            jpeg_free_align(output_buf);
        }
        
        // 内存监控
        if (i % 30 == 0) {
            size_t free_heap = esp_get_free_heap_size();
            size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            ESP_LOGI(TAG, "处理进度: %d/%d, 内存: 堆=%zu, SPIRAM=%zu", 
                     i, container->frame_count, free_heap, free_spiram);
            
            if (free_heap < 5000) {
                ESP_LOGW(TAG, "内存不足，停止处理");
                break;
            }
        }
    }
    
    // 3. 清理资源
    avi_free_container(container);
    
    ESP_LOGI(TAG, "逐帧处理完成: 成功=%d帧, 失败=%d帧", processed_frames, error_frames);
    return AVI_ERR_OK;
}

// =============================================================================
// 使用示例和辅助函数
// =============================================================================

// 示例：帧处理回调函数
static int example_frame_callback(uint32_t frame_index, uint8_t* rgb_data, uint32_t width, uint32_t height, void* user_data) {
    ESP_LOGI(TAG, "处理帧 %d: %dx%d, %d字节", frame_index, width, height, width * height * 3);
    
    // 这里可以添加具体的帧处理逻辑，比如：
    // - 显示到屏幕
    // - 保存到文件
    // - 图像处理
    // - 发送到网络
    
    return AVI_ERR_OK;
}

// 示例：播放AVI文件到屏幕
int avi_play_to_display(const char* avi_file_path) {
    ESP_LOGI(TAG, "播放AVI文件到屏幕: %s", avi_file_path);
    
    // 使用逐帧处理方式，每帧都显示到屏幕
    return avi_process_frames(avi_file_path, example_frame_callback, NULL);
}

// 示例：检查AVI文件信息
int avi_check_file_info(const char* avi_file_path) {
    if (!avi_file_path) {
        return AVI_ERR_INVALID_PARAM;
    }
    
    avi_container_t* container = avi_parse_file(avi_file_path);
    if (!container) {
        ESP_LOGE(TAG, "无法解析AVI文件: %s", avi_file_path);
        return AVI_ERR_INVALID_FORMAT;
    }
    
    ESP_LOGI(TAG, "AVI文件信息:");
    ESP_LOGI(TAG, "  文件路径: %s", avi_file_path);
    ESP_LOGI(TAG, "  分辨率: %dx%d", container->header.width, container->header.height);
    ESP_LOGI(TAG, "  总帧数: %d", container->frame_count);
    ESP_LOGI(TAG, "  帧率: %d fps", container->frame_rate);
    ESP_LOGI(TAG, "  时长: %.2f 秒", (float)container->frame_count / container->frame_rate);
    ESP_LOGI(TAG, "  每帧微秒: %d", container->header.micro_sec_per_frame);
    ESP_LOGI(TAG, "  最大字节/秒: %d", container->header.max_bytes_per_sec);
    
    // 分析帧大小分布
    uint32_t total_size = 0;
    uint32_t max_frame_size = 0;
    uint32_t min_frame_size = 0xFFFFFFFF;
    
    for (uint32_t i = 0; i < container->frame_count; i++) {
        avi_frame_t* frame = avi_get_frame(container, i);
        if (frame) {
            total_size += frame->size;
            if (frame->size > max_frame_size) max_frame_size = frame->size;
            if (frame->size < min_frame_size) min_frame_size = frame->size;
        }
    }
    
    ESP_LOGI(TAG, "  总大小: %d 字节 (%.2f MB)", total_size, total_size / 1024.0 / 1024.0);
    ESP_LOGI(TAG, "  平均帧大小: %d 字节", total_size / container->frame_count);
    ESP_LOGI(TAG, "  最大帧大小: %d 字节", max_frame_size);
    ESP_LOGI(TAG, "  最小帧大小: %d 字节", min_frame_size);
    
    avi_free_container(container);
    return AVI_ERR_OK;
} 