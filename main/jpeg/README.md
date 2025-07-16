# JPEG解码器测试

这是一个基于ESP-JPEG库的JPEG解码器测试实现，用于验证JPEG解码功能的正确性。

## 功能特性

- ✅ JPEG单帧解码测试
- ✅ JPEG流解码测试
- ✅ JPEG块解码测试
- ✅ 内存使用监控
- ✅ 完整的错误处理

## 文件结构

```
jpeg/
├── decoder.h          # JPEG解码器头文件
├── decoder.cc         # JPEG解码器实现
├── test.h             # 测试头文件
├── test.cc            # 测试实现
└── README.md          # 本文档
```

## 测试项目

### 1. JPEG单帧解码测试
- 测试 `esp_jpeg_decode_one_picture()` 函数
- 使用内置的1x1像素JPEG测试数据
- 验证输出大小和内存管理

### 2. JPEG流解码测试
- 测试 `esp_jpeg_stream_open()` 和 `esp_jpeg_stream_decode()` 函数
- 验证流式解码功能
- 测试内存分配和释放

### 3. JPEG块解码测试
- 测试 `esp_jpeg_decode_one_picture_block()` 函数
- 验证块解码功能

### 4. 内存使用监控
- 监控堆内存使用情况
- 检查内存泄漏
- 验证内存使用合理性

## API接口

### 测试初始化
```c
esp_err_t jpeg_test_init();
```

### 单帧解码测试
```c
esp_err_t jpeg_test_decode();
```

### 流解码测试
```c
esp_err_t jpeg_test_stream_decode();
```

### 块解码测试
```c
esp_err_t jpeg_test_block_decode();
```

### 内存使用测试
```c
esp_err_t jpeg_test_memory_usage();
```

## 在Application中使用

```c
// 在Application类中调用
void Application::TestJpegDecoder() {
    ESP_LOGI(TAG, "Starting JPEG decoder test...");
    
    // 初始化测试
    esp_err_t ret = jpeg_test_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize JPEG test");
        return;
    }
    
    // 运行所有测试
    ret = jpeg_test_decode();
    ret = jpeg_test_stream_decode();
    ret = jpeg_test_block_decode();
    ret = jpeg_test_memory_usage();
    
    ESP_LOGI(TAG, "All JPEG decoder tests completed!");
}
```

## 测试输出示例

```
I (xxxx) main: Testing JPEG decoder functionality...
I (xxxx) Application: Starting JPEG decoder test...
I (xxxx) JpegTest: Initializing JPEG test...
I (xxxx) JpegTest: Test 1: Testing JPEG decode functionality
I (xxxx) JpegTest: ✓ JPEG decode test passed:
I (xxxx) JpegTest:   Output size: 1152 bytes
I (xxxx) JpegTest:   Expected size: 1152 bytes (24x16x3)
I (xxxx) JpegTest:   ✓ Size verification passed
I (xxxx) JpegTest: Test 2: Testing JPEG stream decode functionality
I (xxxx) JpegTest: ✓ Stream decoder opened successfully
I (xxxx) JpegTest: ✓ Stream decode test passed:
I (xxxx) JpegTest:   Output size: 1152 bytes
I (xxxx) JpegTest: Test 3: Testing JPEG block decode functionality
I (xxxx) JpegTest: ✓ Block decode test passed
I (xxxx) JpegTest: Test 4: Testing memory usage
I (xxxx) JpegTest: Memory usage:
I (xxxx) JpegTest:   Total heap: 327680 bytes
I (xxxx) JpegTest:   Free heap: 245760 bytes
I (xxxx) JpegTest:   Min free heap: 240640 bytes
I (xxxx) JpegTest:   Used heap: 81920 bytes (25.0%)
I (xxxx) JpegTest: ✓ Memory usage is reasonable
I (xxxx) Application: All JPEG decoder tests completed successfully!
```

## 测试数据

使用image_io.h中的24x16像素JPEG测试数据：
- 分辨率：24x16像素
- 格式：RGB888
- 大小：1152字节（24×16×3）
- 包含6种颜色：白色、黑色、青色、红色、绿色、蓝色
- 包含完整的JPEG文件头和压缩数据

## 错误处理

- 内存分配失败检测
- 解码错误处理
- 流操作错误处理
- 内存泄漏检测

## 性能特点

- 快速测试执行
- 低内存占用
- 完整的错误检查
- 详细的日志输出

## 注意事项

1. **内存管理**：确保正确释放解码后的缓冲区
2. **错误处理**：检查所有API调用的返回值
3. **资源清理**：正确关闭流解码器
4. **内存监控**：定期检查内存使用情况

## 后续扩展

- [ ] 支持更多JPEG格式测试
- [ ] 添加性能基准测试
- [ ] 支持大尺寸图像测试
- [ ] 添加并发测试
- [ ] 支持不同压缩质量测试 