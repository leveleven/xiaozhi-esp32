#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "display.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <font_emoji.h>

#include <atomic>

// 官方AVI播放器头文件
#include "avi_player.h"

class LcdDisplay : public Display {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    gpio_num_t backlight_pin_ = GPIO_NUM_NC;
    bool backlight_output_invert_ = false;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;

    DisplayFonts fonts_;

    esp_timer_handle_t backlight_timer_ = nullptr;
    uint8_t current_brightness_ = 0;
    void OnBacklightTimer();
    void InitializeBacklight(gpio_num_t backlight_pin);

    // 图片显示相关成员
    lv_obj_t* emotion_img_ = nullptr;          // LVGL图片对象
    uint8_t* current_img_data_ = nullptr;      // 当前图片数据缓冲区
    
    // 官方AVI播放器相关成员
    avi_player_handle_t avi_handle_ = nullptr;  // AVI播放器句柄
    bool avi_playing_ = false;                  // 播放状态
    
    void CleanupCurrentImage();                // 清理当前图片资源
    void DisplayImageWithLVGL(uint8_t* rgb_data, int width, int height);  // LVGL图片显示逻辑
    
    // AVI播放器回调函数
    static void VideoFrameCallback(frame_data_t *data, void *arg);
    static void AudioFrameCallback(frame_data_t *data, void *arg);
    static void PlayEndCallback(void *arg);

protected:

    virtual void SetupUI();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

public:
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  gpio_num_t backlight_pin, bool backlight_output_invert,
                  int width, int height,  int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy,
                  DisplayFonts fonts);
    ~LcdDisplay();

    virtual void SetChatMessage(const std::string &role, const std::string &content) override;
    virtual void SetEmotion(const std::string &emotion) override;
    virtual void SetJpgEmotionLVGL(const char* jpg_path) override;
    virtual void SetAviEmotionLVGL(const char* avi_path) override;
    virtual void SetIcon(const char* icon) override;
    virtual void SetBacklight(uint8_t brightness) override;
};

#endif // LCD_DISPLAY_H
