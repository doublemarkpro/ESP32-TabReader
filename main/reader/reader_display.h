#pragma once
#ifdef READER_UI_HOST_SIM
#include "reader_platform.h"
#else
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "display.h"
#endif
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "han_dictionary/reader/epub_reader.h"

class ReaderDisplay : public Display {
public:
    ReaderDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);
    void AttachTouch(esp_lcd_touch_handle_t touch);
    void SetupUI() override;
    void SetUsbActions(std::function<std::string()> start, std::function<std::string()> stop) {
        usb_start_ = std::move(start);
        usb_stop_ = std::move(stop);
    }
    void SetSleepAction(std::function<void()> action) { sleep_action_ = std::move(action); }

private:
#ifdef READER_UI_HOST_SIM
    friend struct ReaderUiSimulation;
#endif
    enum class Page { Shelf, Reader, Transfer, Settings, Fonts };
    struct Job {
        int type;
        int value;
    };
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;
    static void OnClick(lv_event_t* event);
    static void OnSettingSliderChanged(lv_event_t* event);
    static void OnSettingSliderReleased(lv_event_t* event);
    static void Tick(lv_timer_t* timer);
    static void OnTouch(lv_event_t* event);
    static void OnRefresh(lv_event_t* event);
    static void OnWifiInput(lv_event_t* event);
    void RequestSleep();
    static void Worker(void* context);
    void Action(int action);
    void Render(Page page);
    void DrawShelf();
    void DrawReader();
    void DrawSettings();
    void DrawTransfer();
    void DrawFonts();
    void Navigation();
    bool Queue(int type, int value = 0);
    void BuildReaderPages(size_t preferred_offset = 0);
    void EnsureReaderFont();
    void ReleaseReaderFont();
    void LoadReaderProgress(const std::string& path, size_t& spine, size_t& offset,
                            uint16_t& progress) const;
    bool SaveReaderProgress(bool force);
    void SaveReaderPreferences();
    void SaveAutoLockSetting();
    void UpdateSettingLabels();
    void Toast(const char* text);
    lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color);
    lv_obj_t* Label(lv_obj_t* parent, const char* text, int x, int y, int w,
                    const lv_font_t* font = nullptr);
    lv_obj_t* Button(lv_obj_t* parent, const char* text, int x, int y, int w, int h, uint32_t color,
                     int action);
    const lv_font_t* DictionaryTextFont() const;
    const lv_font_t* DynamicTextFont() const;
    lv_display_t* display_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* brightness_value_ = nullptr;
    lv_obj_t* brightness_slider_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* wifi_name_ = nullptr;
    lv_obj_t* wifi_password_ = nullptr;
    lv_obj_t* wifi_keyboard_ = nullptr;
    QueueHandle_t jobs_ = nullptr;
    std::function<std::string()> usb_start_;
    std::function<std::string()> usb_stop_;
    std::function<void()> sleep_action_;
    std::string pending_ssid_, pending_password_;
    bool lan_setup_ = false;
    bool resume_after_sleep_ = false;
    Page page_ = Page::Shelf;
    Page font_return_page_ = Page::Settings;
    int shelf_page_ = 0;
    int brightness_setting_ = 60;
    int auto_lock_minutes_ = 10;
    bool asleep_ = false;
    bool importing_ = false;
    bool usb_active_ = false;
    bool initialized_ = false;
    int64_t suppress_touch_until_ = 0;
    std::string last_path_;
    std::string notice_;
    int64_t render_started_us_ = 0;
    bool render_pending_ = false;
    std::vector<han::reader::EpubBook> reader_books_;
    std::vector<std::string> reader_fonts_;
    std::vector<uint16_t> reader_book_progress_;
    han::reader::EpubBook reader_book_;
    std::string reader_text_;
    std::string reader_error_;
    std::string reader_font_preference_;
    std::vector<size_t> reader_page_offsets_;
    size_t reader_selected_book_ = 0;
    size_t reader_spine_index_ = 0;
    size_t reader_page_index_ = 0;
    int reader_font_index_ = -1;
    int reader_font_size_ = 32;
    int reader_line_spacing_ = 10;
    int reader_margin_ = 48;
    int reader_background_index_ = 0;
    bool reader_open_ = false;
    bool reader_loading_ = false;
    bool reader_scanned_ = false;
    bool reader_font_builtin_ = false;
    bool reader_dark_ = false;
    bool reader_font_picker_open_ = false;
    bool reader_screen_settings_open_ = false;
    bool reader_controls_visible_ = false;
    bool reader_portrait_ = true;
    int reader_pages_since_progress_save_ = 0;
    lv_font_t* reader_font_ = nullptr;
    lv_font_t* reader_ui_font_ = nullptr;
    std::string reader_font_loaded_path_;
    int reader_font_loaded_size_ = 0;
};
