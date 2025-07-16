#include "test.h"
#include "decoder.h"
#include "image_io.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <string.h>

static const char* TAG = "JpegTest";

esp_err_t jpeg_test_init() {
    ESP_LOGI(TAG, "Initializing JPEG test...");
    return ESP_OK;
}

esp_err_t jpeg_test_decode() {
    ESP_LOGI(TAG, "Test 1: Testing JPEG decode functionality");
    
    uint8_t *output_buf = NULL;
    int out_len = 0;
    
    jpeg_error_t ret = esp_jpeg_decode_one_picture((uint8_t*)test_jpeg_data, sizeof(test_jpeg_data), &output_buf, &out_len);
    if (ret == JPEG_ERR_OK) {
        ESP_LOGI(TAG, "✓ JPEG decode test passed:");
        ESP_LOGI(TAG, "  Output size: %d bytes", out_len);
        ESP_LOGI(TAG, "  Expected size: %d bytes (%dx%dx3)", TEST_JPEG_X, TEST_JPEG_Y, TEST_JPEG_X * TEST_JPEG_Y * 3);
        
        // 验证解码结果
        if (out_len == TEST_JPEG_X * TEST_JPEG_Y * 3) {
            ESP_LOGI(TAG, "  ✓ Size verification passed");
        } else {
            ESP_LOGW(TAG, "  ✗ Size verification failed");
        }
        
        // 释放输出缓冲区
        if (output_buf) {
            free(output_buf);
        }
    } else {
        ESP_LOGE(TAG, "✗ JPEG decode test failed: %d", ret);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t jpeg_test_stream_decode() {
    ESP_LOGI(TAG, "Test 2: Testing JPEG stream decode functionality");
    
    // 创建流解码器句柄
    esp_jpeg_stream_handle_t stream_handle = (esp_jpeg_stream_handle_t)malloc(sizeof(struct esp_jpeg_stream));
    if (!stream_handle) {
        ESP_LOGE(TAG, "✗ Failed to allocate stream handle");
        return ESP_ERR_NO_MEM;
    }
    
    jpeg_error_t ret = esp_jpeg_stream_open(stream_handle);
    if (ret == JPEG_ERR_OK) {
        ESP_LOGI(TAG, "✓ Stream decoder opened successfully");
        
        // 解码测试数据
        uint8_t *stream_output = NULL;
        int stream_out_len = 0;
        ret = esp_jpeg_stream_decode(stream_handle, (uint8_t*)test_jpeg_data, sizeof(test_jpeg_data), &stream_output, &stream_out_len);
        if (ret == JPEG_ERR_OK) {
            ESP_LOGI(TAG, "✓ Stream decode test passed:");
            ESP_LOGI(TAG, "  Output size: %d bytes", stream_out_len);
            
            if (stream_output) {
                free(stream_output);
            }
        } else {
            ESP_LOGE(TAG, "✗ Stream decode failed: %d", ret);
            esp_jpeg_stream_close(stream_handle);
            free(stream_handle);
            return ESP_FAIL;
        }
        
        esp_jpeg_stream_close(stream_handle);
    } else {
        ESP_LOGE(TAG, "✗ Failed to open stream decoder: %d", ret);
        free(stream_handle);
        return ESP_FAIL;
    }
    
    free(stream_handle);
    return ESP_OK;
}

esp_err_t jpeg_test_block_decode() {
    ESP_LOGI(TAG, "Test 3: Testing JPEG block decode functionality");
    
    jpeg_error_t ret = esp_jpeg_decode_one_picture_block((unsigned char*)test_jpeg_data, sizeof(test_jpeg_data));
    if (ret == JPEG_ERR_OK) {
        ESP_LOGI(TAG, "✓ Block decode test passed");
    } else {
        ESP_LOGE(TAG, "✗ Block decode test failed: %d", ret);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t jpeg_test_memory_usage() {
    ESP_LOGI(TAG, "Test 4: Testing memory usage");
    
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    
    ESP_LOGI(TAG, "Memory usage:");
    ESP_LOGI(TAG, "  Total heap: %zu bytes", total_heap);
    ESP_LOGI(TAG, "  Free heap: %zu bytes", free_heap);
    ESP_LOGI(TAG, "  Min free heap: %zu bytes", min_free_heap);
    ESP_LOGI(TAG, "  Used heap: %zu bytes (%.1f%%)", 
             total_heap - free_heap, 
             (float)(total_heap - free_heap) / total_heap * 100);
    
    // 检查内存使用是否合理
    if (free_heap > total_heap * 0.1) { // 至少10%可用内存
        ESP_LOGI(TAG, "✓ Memory usage is reasonable");
    } else {
        ESP_LOGW(TAG, "⚠ Memory usage is high");
    }
    
    return ESP_OK;
} 