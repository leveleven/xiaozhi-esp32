#ifndef _JPEG_TEST_H
#define _JPEG_TEST_H

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化JPEG测试
 * 
 * @return esp_err_t 
 */
esp_err_t jpeg_test_init();

/**
 * @brief 测试JPEG解码功能
 * 
 * @return esp_err_t 
 */
esp_err_t jpeg_test_decode();

/**
 * @brief 测试JPEG流解码功能
 * 
 * @return esp_err_t 
 */
esp_err_t jpeg_test_stream_decode();

/**
 * @brief 测试JPEG块解码功能
 * 
 * @return esp_err_t 
 */
esp_err_t jpeg_test_block_decode();

/**
 * @brief 测试内存使用情况
 * 
 * @return esp_err_t 
 */
esp_err_t jpeg_test_memory_usage();

#ifdef __cplusplus
}
#endif

#endif // _JPEG_TEST_H 