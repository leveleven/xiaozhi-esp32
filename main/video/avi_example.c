// Copyright 2024 Espressif Systems (Shanghai) CO., LTD.
// All rights reserved.

#include "avi_parser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "AVI_EXAMPLE";

// 简单的AVI播放示例
void example_simple_playback(void) {
    const char* avi_file = "/storage/video.avi";
    
    // 使用默认参数播放
    int result = avi_play_file_simple(avi_file);
    
    if (result == AVI_ERR_OK) {
        ESP_LOGI(TAG, "AVI文件播放成功");
    } else {
        ESP_LOGE(TAG, "AVI文件播放失败: %d", result);
    }
}

// 检查AVI文件信息
void example_check_file_info(void) {
    const char* avi_file = "/storage/video.avi";
    
    int result = avi_check_file_info(avi_file);
    
    if (result == AVI_ERR_OK) {
        ESP_LOGI(TAG, "AVI文件信息检查完成");
    } else {
        ESP_LOGE(TAG, "AVI文件信息检查失败: %d", result);
    }
}

// 自定义帧处理回调
static int custom_frame_callback(uint32_t frame_index, uint8_t* rgb_data, uint32_t width, uint32_t height, void* user_data) {
    ESP_LOGI(TAG, "处理帧 %d: %dx%d", frame_index, width, height);
    return AVI_ERR_OK;
}

void example_custom_frame_processing(void) {
    const char* avi_file = "/storage/video.avi";
    
    // 使用自定义回调函数逐帧处理
    int result = avi_process_frames(avi_file, custom_frame_callback, NULL);
    
    if (result == AVI_ERR_OK) {
        ESP_LOGI(TAG, "自定义帧处理完成");
    } else {
        ESP_LOGE(TAG, "自定义帧处理失败: %d", result);
    }
}

// 主示例函数
void run_avi_examples(void) {
    ESP_LOGI(TAG, "开始运行AVI播放器示例");
    
    example_check_file_info();
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    example_simple_playback();
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    example_custom_frame_processing();
    
    ESP_LOGI(TAG, "所有AVI播放器示例运行完成");
}

// 任务函数
void avi_example_task(void* param) {
    ESP_LOGI(TAG, "AVI示例任务启动");
    vTaskDelay(pdMS_TO_TICKS(2000));
    run_avi_examples();
    ESP_LOGI(TAG, "AVI示例任务结束");
    vTaskDelete(NULL);
}

// 启动AVI示例任务
void start_avi_examples(void) {
    xTaskCreate(avi_example_task, "avi_example", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "AVI示例任务已创建");
}
