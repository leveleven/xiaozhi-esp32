#include "lcd_display.h"

#include <font_awesome_symbols.h>
#include <esp_log.h>
#include <esp_err.h>
#include <driver/ledc.h>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <algorithm>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

#include "board.h"
#include "display/lv_display.h"
#include "jpeg/decoder.h"
#include "avi_player.h"

#define TAG "LcdDisplay"
#define LCD_LEDC_CH LEDC_CHANNEL_0

LV_FONT_DECLARE(font_awesome_30_4);

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           gpio_num_t backlight_pin, bool backlight_output_invert,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy,
                           DisplayFonts fonts)
    : panel_io_(panel_io), panel_(panel), backlight_pin_(backlight_pin), backlight_output_invert_(backlight_output_invert),
      fonts_(fonts) {
    width_ = width;
    height_ = height;

    // 创建背光渐变定时器
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->OnBacklightTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "backlight_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &backlight_timer_));
    InitializeBacklight(backlight_pin);

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 10),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    SetupUI();

    SetBacklight(brightness_);
    
    // 初始化官方AVI播放器
    avi_player_config_t avi_config = {
        .buffer_size = 50 * 1024,  // 50KB缓冲区
        .video_cb = VideoFrameCallback,
        .audio_cb = AudioFrameCallback,
        .audio_set_clock_cb = nullptr,  // 不需要音频时钟设置
        .avi_play_end_cb = PlayEndCallback,
        .priority = 5,
        .coreID = 0,
        .user_data = this,
        .stack_size = 4096,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        .stack_in_psram = false
#endif
    };
    
    esp_err_t ret = avi_player_init(avi_config, &avi_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVI播放器初始化失败: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "AVI播放器初始化成功");
    }
}

LcdDisplay::~LcdDisplay() {
    if (backlight_timer_ != nullptr) {
        esp_timer_stop(backlight_timer_);
        esp_timer_delete(backlight_timer_);
    }
    
    // 停止AVI播放
    if (avi_handle_ != nullptr) {
        avi_player_play_stop(avi_handle_);
        avi_player_deinit(avi_handle_);
        avi_handle_ = nullptr;
        ESP_LOGI(TAG, "AVI播放器已清理");
    }
    
    // 清理图片资源
    CleanupCurrentImage();
    
    // 删除LVGL图片对象
    if (emotion_img_) {
        lv_obj_del(emotion_img_);
        emotion_img_ = nullptr;
        ESP_LOGI(TAG, "LVGL图片对象已删除");
    }
    
    // 然后再清理其他 LVGL 对象
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

void LcdDisplay::InitializeBacklight(gpio_num_t backlight_pin) {
    if (backlight_pin == GPIO_NUM_NC) {
        return;
    }

    // Setup LEDC peripheral for PWM backlight control
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = backlight_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = backlight_output_invert_,
        }
    };
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000, //背光pwm频率需要高一点，防止电感啸叫
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };

    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));
}

void LcdDisplay::OnBacklightTimer() {
    if (current_brightness_ < brightness_) {
        current_brightness_++;
    } else if (current_brightness_ > brightness_) {
        current_brightness_--;
    }
    
    // LEDC resolution set to 10bits, thus: 100% = 1023
    uint32_t duty_cycle = (1023 * current_brightness_) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH);
    
    if (current_brightness_ == brightness_) {
        esp_timer_stop(backlight_timer_);
    }
}

void LcdDisplay::SetBacklight(uint8_t brightness) {
    if (backlight_pin_ == GPIO_NUM_NC) {
        return;
    }

    if (brightness > 100) {
        brightness = 100;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness);
    // 停止现有的定时器（如果正在运行）
    esp_timer_stop(backlight_timer_);

    Display::SetBacklight(brightness);
    // 启动定时器，每 5ms 更新一次
    ESP_ERROR_CHECK(esp_timer_start_periodic(backlight_timer_, 5 * 1000));
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, fonts_.text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, fonts_.text_font->line_height);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    
    /* Content */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);

    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN); // 垂直布局（从上到下）
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY); // 子对象居中对齐，等距分布

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);

    chat_message_label_ = lv_label_create(content_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.9); // 限制宽度为屏幕宽度的 90%
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP); // 设置为自动换行模式
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0); // 设置文本居中对齐

    /* Status bar */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_left(status_bar_, 2, 0);
    lv_obj_set_style_pad_right(status_bar_, 2, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, fonts_.icon_font, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "通知");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(status_label_, "正在初始化");
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);

    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, fonts_.icon_font, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, fonts_.icon_font, 0);
}

void LcdDisplay::SetChatMessage(const std::string &role, const std::string &content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }
    lv_label_set_text(chat_message_label_, content.c_str());
}

void LcdDisplay::SetEmotion(const std::string &emotion) {
    struct Emotion {
        const char* icon;
        const char* text;
    };

    static const std::vector<Emotion> emotions = {
        {"😶", "neutral"},
        {"🙂", "happy"},
        {"😆", "laughing"},
        {"😂", "funny"},
        {"😔", "sad"},
        {"😠", "angry"},
        {"😭", "crying"},
        {"😍", "loving"},
        {"😳", "embarrassed"},
        {"😯", "surprised"},
        {"😱", "shocked"},
        {"🤔", "thinking"},
        {"😉", "winking"},
        {"😎", "cool"},
        {"😌", "relaxed"},
        {"🤤", "delicious"},
        {"😘", "kissy"},
        {"😏", "confident"},
        {"😴", "sleepy"},
        {"😜", "silly"},
        {"🙄", "confused"}
    };
    
    // 查找匹配的表情
    auto it = std::find_if(emotions.begin(), emotions.end(),
        [&emotion](const Emotion& e) { return e.text == emotion; });

    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }

    // 如果找到匹配的表情就显示对应图标，否则显示默认的neutral表情
    lv_obj_set_style_text_font(emotion_label_, fonts_.emoji_font, 0);
    if (it != emotions.end()) {
        lv_label_set_text(emotion_label_, it->icon);
    } else {
        lv_label_set_text(emotion_label_, "😶");
    }
}

void LcdDisplay::SetIcon(const char* icon) {
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_label_set_text(emotion_label_, icon);
}

void LcdDisplay::SetJpgEmotionLVGL(const char* jpg_path) {
    if (!jpg_path) {
        ESP_LOGE(TAG, "图片路径为空");
        return;
    }

    ESP_LOGI(TAG, "显示JPEG图像: %s", jpg_path);
    
    DisplayLockGuard lock(this);
    
    // 1. 清理之前的图片资源
    CleanupCurrentImage();
    
    // 2. 读取JPEG文件
    FILE* fp = fopen(jpg_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "无法打开文件: %s", jpg_path);
        return;
    }
    
    // 获取文件大小
    fseek(fp, 0, SEEK_END);
    long jpg_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (jpg_len <= 0 || jpg_len > 512 * 1024) {  // 限制512KB
        ESP_LOGE(TAG, "文件大小异常: %ld 字节", jpg_len);
        fclose(fp);
        return;
    }

    ESP_LOGI(TAG, "JPEG文件大小: %ld 字节", jpg_len);

    // 3. 分配并读取JPEG数据（优先使用SPIRAM）
    uint8_t* jpg_buf = (uint8_t*)heap_caps_malloc(jpg_len, 
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpg_buf) {
        jpg_buf = (uint8_t*)malloc(jpg_len);  // 回退到内部RAM
        if (!jpg_buf) {
            ESP_LOGE(TAG, "内存分配失败: %ld 字节", jpg_len);
            fclose(fp);
            return;
        }
        ESP_LOGI(TAG, "使用内部RAM分配JPEG缓冲区");
    } else {
        ESP_LOGI(TAG, "使用SPIRAM分配JPEG缓冲区");
    }
    
    if (fread(jpg_buf, 1, jpg_len, fp) != jpg_len) {
        ESP_LOGE(TAG, "文件读取失败");
        free(jpg_buf);
        fclose(fp);
        return;
    }
    fclose(fp);

    // 4. JPEG解码
    uint8_t* out_buf = nullptr;
    int out_len = 0;
    jpeg_dec_header_info_t out_info = {};
    jpeg_error_t ret = esp_jpeg_decode_one_picture(jpg_buf, jpg_len, &out_buf, &out_len, &out_info);
    
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG解码失败: %d", ret);
        jpeg_free_align(out_buf);
        return;
    }

    ESP_LOGI(TAG, "JPEG解码成功，RGB数据大小: %d 字节", out_len);

    // 5. 验证解码数据
    if (out_len != out_info.width * out_info.height * 3) {
        ESP_LOGE(TAG, "数据大小不匹配: 期望 %d, 实际 %d", out_info.width * out_info.height * 3, out_len);
        jpeg_free_align(out_buf);
        return;
    }
    
    // 6. 颜色格式转换：BGR -> RGB
    uint8_t* pixel_data = out_buf;
    ESP_LOGI(TAG, "执行BGR到RGB颜色转换...");
    for (int i = 0; i < out_len; i += 3) {
        // 交换B和R通道 (BGR -> RGB)
        uint8_t temp = pixel_data[i];      // 保存B
        pixel_data[i] = pixel_data[i + 2]; // B = R
        pixel_data[i + 2] = temp;          // R = B
        // G通道保持不变
    }

    // 7. 使用LVGL显示图片
    DisplayImageWithLVGL(out_buf, out_info.width, out_info.height);
    
    // 8. 保存数据引用用于后续清理
    current_img_data_ = out_buf;

    ESP_LOGI(TAG, "图片显示完成");
    
    // 9. 打印内存使用情况
    size_t free_heap = esp_get_free_heap_size();
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "当前可用内存 - 堆: %zu 字节, SPIRAM: %zu 字节", free_heap, free_spiram);
}

void LcdDisplay::CleanupCurrentImage() {
    // 隐藏图片对象（但不删除，重复使用）
    if (emotion_img_) {
        lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "图片对象已隐藏");
    }
    
    // 释放图片数据
    if (current_img_data_) {
        jpeg_free_align(current_img_data_);
        current_img_data_ = nullptr;
        ESP_LOGI(TAG, "图片数据已释放");
    }
    
    // 恢复其他UI元素的显示
    if (emotion_label_) {
        lv_obj_clear_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "恢复表情标签显示");
    }
    if (chat_message_label_) {
        lv_obj_clear_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "恢复聊天消息标签显示");
    }
}

void LcdDisplay::SetAviEmotionLVGL(const char* avi_file_path) {
    if (!avi_file_path) {
        ESP_LOGE(TAG, "无效的文件路径");
        return;
    }
    
    if (!avi_handle_) {
        ESP_LOGE(TAG, "AVI播放器未初始化");
        return;
    }
    
    ESP_LOGI(TAG, "开始播放AVI文件: %s", avi_file_path);
    
    // 检查文件是否存在
    FILE* test_file = fopen(avi_file_path, "rb");
    if (!test_file) {
        ESP_LOGE(TAG, "AVI文件不存在或无法访问: %s", avi_file_path);
        return;
    }
    fclose(test_file);
    
    // 使用官方AVI播放器播放文件
    esp_err_t ret = avi_player_play_from_file(avi_handle_, avi_file_path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVI播放启动失败: %s", esp_err_to_name(ret));
        return;
    }
    
    avi_playing_ = true;
    ESP_LOGI(TAG, "AVI播放已启动，使用异步播放模式");
}

// AVI播放器回调函数实现
void LcdDisplay::VideoFrameCallback(frame_data_t *data, void *arg) {
    LcdDisplay* display = static_cast<LcdDisplay*>(arg);
    if (!display || !data || data->type != FRAME_TYPE_VIDEO) {
        return;
    }
    
    ESP_LOGD(TAG, "收到视频帧: %dx%d, %zu字节", 
             data->video_info.width, data->video_info.height, data->data_bytes);
    
    // 使用LVGL显示视频帧
    display->DisplayImageWithLVGL(data->data, data->video_info.width, data->video_info.height);
}

void LcdDisplay::AudioFrameCallback(frame_data_t *data, void *arg) {
    // 音频帧回调，目前不需要处理
    ESP_LOGD(TAG, "收到音频帧: %zu字节", data->data_bytes);
}

void LcdDisplay::PlayEndCallback(void *arg) {
    LcdDisplay* display = static_cast<LcdDisplay*>(arg);
    if (!display) {
        return;
    }
    
    display->avi_playing_ = false;
    ESP_LOGI(TAG, "AVI播放结束");
}

void LcdDisplay::DisplayImageWithLVGL(uint8_t* rgb_data, int width, int height) {
    // 1. 创建LVGL图片对象（如果还未创建）
    if (emotion_img_ == nullptr) {
        emotion_img_ = lv_img_create(lv_screen_active());
        if (!emotion_img_) {
            ESP_LOGE(TAG, "创建图片对象失败");
            return;
        }
        ESP_LOGI(TAG, "创建LVGL图片对象成功");
    }

    // 2. 创建LVGL图片描述符（使用静态变量避免动态分配）
    static lv_img_dsc_t img_dsc;
    memset(&img_dsc, 0, sizeof(lv_img_dsc_t));
    
    img_dsc.header.w = width;
    img_dsc.header.h = height;
    img_dsc.header.cf = LV_COLOR_FORMAT_RGB888;  // 匹配RGB888解码器输出
    img_dsc.data_size = width * height * 3;
    img_dsc.data = rgb_data;

    ESP_LOGI(TAG, "图片描述符创建完成 - 尺寸: %dx%d, 格式: RGB888, 数据: %p", 
             width, height, rgb_data);

    // 3. 设置图片源并显示
    lv_img_set_src(emotion_img_, &img_dsc);
    
    // 4. 获取屏幕区域尺寸
    lv_coord_t content_width = lv_obj_get_width(lv_screen_active());
    lv_coord_t content_height = lv_obj_get_height(lv_screen_active());

    ESP_LOGI(TAG, "显示区域分析 - 内容: %dx%d", content_width, content_height);
    ESP_LOGI(TAG, "原始图片尺寸: %dx%d", width, height);
    
    // 5. 计算缩放比例，确保图片完全显示（不裁剪）
    float scale_x = (float)content_width / width;
    float scale_y = (float)content_height / height;
    float scale = 1.0f;  // 默认不缩放
    
    // 如果图片超出显示区域，进行等比例缩放
    if (width > content_width || height > content_height) {
        // 选择较小的缩放比例，确保图片完全显示
        scale = (scale_x < scale_y) ? scale_x : scale_y;
        ESP_LOGI(TAG, "图片需要缩放 - 水平缩放比例: %.3f, 垂直缩放比例: %.3f, 选择: %.3f", 
                 scale_x, scale_y, scale);
    }
    
    // 6. 计算缩放后的尺寸
    lv_coord_t display_width = (lv_coord_t)(width * scale);
    lv_coord_t display_height = (lv_coord_t)(height * scale);
    
    // 7. 设置图片显示尺寸
    lv_obj_set_size(emotion_img_, display_width, display_height);
    
    if (scale < 1.0f) {
        ESP_LOGI(TAG, "图片缩放显示: %dx%d -> %dx%d (缩放比例: %.3f)", 
                 width, height, display_width, display_height, scale);
    } else {
        ESP_LOGI(TAG, "图片原尺寸显示: %dx%d", display_width, display_height);
    }
    
    // 8. 居中显示并设置样式
    lv_obj_center(emotion_img_);
    lv_obj_clear_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "LVGL图片显示完成");
}