#include "reader_display.h"
#ifndef READER_UI_HOST_SIM
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <src/libs/freetype/lv_freetype_private.h>
#include "board.h"
#include "reader_application.h"
#include "settings.h"
#include "transfer_server.h"
#endif
#include <src/misc/cache/instance/lv_image_cache.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <new>
extern "C" {
LV_FONT_DECLARE(reader_ui_24);
}
namespace {
#if LV_USE_FREETYPE
struct ReaderFontStreamCache {
    static constexpr size_t kBlockSize = 1024;
    static constexpr size_t kWays = 4;
    static constexpr size_t kSlots = 1024;
    struct Block {
        unsigned long offset = ULONG_MAX;
        size_t size = 0;
        uint64_t used = 0;
    };
    FT_StreamDesc descriptor;
    FT_Stream_IoFunc read;
    FT_Stream_CloseFunc close;
    uint8_t* data = nullptr;
    Block blocks[kSlots];
    uint64_t tick = 0;
    unsigned reads = 0;
    unsigned hits = 0;
    int64_t read_us = 0;

    static unsigned long Read(FT_Stream stream, unsigned long offset, unsigned char* buffer,
                              unsigned long count) {
        if (count == 0)
            return offset > stream->size ? 1 : 0;
        if (offset >= stream->size || !buffer)
            return 0;
        auto cache = static_cast<ReaderFontStreamCache*>(stream->descriptor.pointer);
        const size_t wanted = std::min(count, stream->size - offset);
        size_t done = 0;
        while (done < wanted) {
            const unsigned long position = offset + done;
            const unsigned long block_offset = position / kBlockSize * kBlockSize;
            const size_t first = (block_offset / kBlockSize % (kSlots / kWays)) * kWays;
            size_t slot = first;
            bool hit = false;
            for (size_t way = first; way < first + kWays; ++way) {
                if (cache->blocks[way].offset == block_offset) {
                    slot = way;
                    hit = true;
                    break;
                }
                if (cache->blocks[way].used < cache->blocks[slot].used)
                    slot = way;
            }
            auto& block = cache->blocks[slot];
            if (!hit) {
                // The backing FILE position differs from FreeType's logical position after a
                // cache hit/read-ahead. Force the original reader to seek on every miss.
                FT_StreamRec backing = *stream;
                backing.descriptor = cache->descriptor;
                backing.pos = ULONG_MAX;
                const int64_t started = esp_timer_get_time();
                const size_t size =
                    cache->read(&backing, block_offset, cache->data + slot * kBlockSize,
                                std::min<unsigned long>(kBlockSize, stream->size - block_offset));
                cache->read_us += esp_timer_get_time() - started;
                ++cache->reads;
                block.offset = block_offset;
                block.size = size;
            } else {
                ++cache->hits;
            }
            block.used = ++cache->tick;
            const size_t within = position - block_offset;
            if (within >= block.size)
                break;
            const size_t size = std::min(wanted - done, block.size - within);
            memcpy(buffer + done, cache->data + slot * kBlockSize + within, size);
            done += size;
        }
        return done;
    }

    static void Close(FT_Stream stream) {
        auto cache = static_cast<ReaderFontStreamCache*>(stream->descriptor.pointer);
        stream->descriptor = cache->descriptor;
        stream->read = cache->read;
        stream->close = cache->close;
        if (stream->close)
            stream->close(stream);
        heap_caps_free(cache->data);
        delete cache;
    }
};

ReaderFontStreamCache* ReaderStreamCache(const lv_font_t* font) {
    if (!font)
        return nullptr;
    auto dsc = static_cast<const lv_freetype_font_dsc_t*>(font->dsc);
    auto stream = dsc->cache_node->face->stream;
    return stream->read == ReaderFontStreamCache::Read
               ? static_cast<ReaderFontStreamCache*>(stream->descriptor.pointer)
               : nullptr;
}

void AccelerateReaderFont(lv_font_t* font, int size) {
    auto dsc = static_cast<const lv_freetype_font_dsc_t*>(font->dsc);
    auto node = dsc->cache_node;
    // A Chinese page can exceed the default 256 entries before it is even drawn. Keep
    // metrics for several pages and bound bitmap storage to about 2 MiB at the selected size.
    lv_cache_set_max_size(node->glyph_cache, 8192, nullptr);
    const size_t bitmaps = std::clamp<size_t>(2 * 1024 * 1024 / (size * size + 128), 256, 2048);
    lv_cache_set_max_size(node->draw_data_cache, bitmaps, nullptr);
    auto stream = node->face->stream;
    if (!stream->read || ReaderStreamCache(font))
        return;
    auto cache = new (std::nothrow) ReaderFontStreamCache;
    if (!cache)
        return;
    cache->data = static_cast<uint8_t*>(
        heap_caps_malloc(ReaderFontStreamCache::kSlots * ReaderFontStreamCache::kBlockSize,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!cache->data) {
        delete cache;
        return;
    }
    cache->descriptor = stream->descriptor;
    cache->read = stream->read;
    cache->close = stream->close;
    stream->descriptor.pointer = cache;
    stream->read = ReaderFontStreamCache::Read;
    stream->close = ReaderFontStreamCache::Close;
    ESP_LOGI("HanDisplay", "Reader font cache: 1024 KiB in 1 KiB blocks, 8192 metrics, %u bitmaps",
             static_cast<unsigned>(bitmaps));
}

#endif
constexpr uint32_t kBlue = 0xe5e5df, kMuted = 0x74756d;
constexpr uint32_t kReaderBackgrounds[] = {0xfffdf7, 0xffffff, 0xe7efdf, 0xf5e6c8, 0xe2e8ee};
constexpr const char* kReaderBackgroundNames[] = {"米白", "纸白", "浅绿", "暖黄", "灰蓝", "夜间"};
constexpr int kReaderScreenTimeouts[] = {1, 3, 5, 10, 20, 30};
std::string ReaderFontDisplayName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    auto name = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name.resize(dot);
    return name.empty() ? "未命名字体" : name;
}

uint32_t ReaderBookId(const std::string& path) {
    uint32_t hash = 2166136261u;
    for (unsigned char ch : path) {
        hash ^= ch;
        hash *= 16777619u;
    }
    return hash;
}

std::string ReaderProgressKey(char prefix, const std::string& path) {
    char key[16];
    snprintf(key, sizeof(key), "%c%08x", prefix, static_cast<unsigned>(ReaderBookId(path)));
    return key;
}

}  // namespace
ReaderDisplay::ReaderDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel) {
#ifdef READER_UI_HOST_SIM
    display_ = lv_display_get_default();
#else
    lv_init();
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    // FreeType's grayscale rasterizer has a 16 KiB local pool. Match the proven
    // Tab5 reading task budget, leaving room for LVGL and the font/cache call chain.
    port.task_stack = 32 * 1024;
    port.task_priority = 5;
    port.task_affinity = 1;
    port.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_ERROR_CHECK(lvgl_port_init(&port));
    lvgl_port_display_cfg_t config{};
    config.io_handle = io;
    config.panel_handle = panel;
    config.buffer_size = 720 * 1280;
    config.hres = 720;
    config.vres = 1280;
    config.flags.buff_dma = true;
    config.flags.buff_spiram = true;
    config.flags.sw_rotate = true;
    lvgl_port_display_dsi_cfg_t dsi{};
    display_ = lvgl_port_add_disp_dsi(&config, &dsi);
    ESP_ERROR_CHECK(display_ ? ESP_OK : ESP_ERR_NO_MEM);
#endif
    lv_display_add_event_cb(display_, OnRefresh, LV_EVENT_REFR_READY, this);
}
bool ReaderDisplay::Lock(int timeout_ms) {
#ifdef READER_UI_HOST_SIM
    return true;
#else
    return lvgl_port_lock(timeout_ms);
#endif
}
void ReaderDisplay::Unlock() {
#ifndef READER_UI_HOST_SIM
    lvgl_port_unlock();
#endif
}
void ReaderDisplay::AttachTouch(esp_lcd_touch_handle_t touch) {
#ifndef READER_UI_HOST_SIM
    lvgl_port_touch_cfg_t config{};
    config.disp = display_;
    config.handle = touch;
    auto input = lvgl_port_add_touch(&config);
    if (input)
        lv_indev_add_event_cb(input, OnTouch, LV_EVENT_PRESSED, this);
#endif
}
const lv_font_t* ReaderDisplay::DictionaryTextFont() const { return &reader_ui_24; }
const lv_font_t* ReaderDisplay::DynamicTextFont() const {
    return reader_ui_font_ ? reader_ui_font_ : &reader_ui_24;
}
lv_obj_t* ReaderDisplay::Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    if (reader_dark_ && (color & 0xff) > 0x80 && ((color >> 16) & 0xff) > 0x80)
        color = 0x273343;
    auto obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}
lv_obj_t* ReaderDisplay::Label(lv_obj_t* parent, const char* text, int x, int y, int w,
                               const lv_font_t* font) {
    auto label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_font(label, font ? font : DynamicTextFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(reader_dark_ ? 0xf3f0e8 : 0x252720), 0);
    lv_label_set_text(label, text);
    return label;
}
lv_obj_t* ReaderDisplay::Button(lv_obj_t* parent, const char* text, int x, int y, int w, int h,
                                uint32_t color, int action) {
    auto button =
        Box(parent, x, y, w, h, reader_dark_ ? (color == 0xd8e2d0 ? 0x426b5a : 0x28323b) : color);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(reader_dark_ ? 0x4c565c : 0xcecec4), 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(button, this);
    lv_obj_add_event_cb(button, OnClick, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(action)));
    auto label = Label(button, text, 0, 0, w - 12);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_center(label);
    return button;
}
void ReaderDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    root_ = lv_screen_active();
    setup_ui_called_ = true;
    lv_obj_remove_style_all(root_);
    Settings prefs("han_reader");
    reader_font_size_ = std::clamp<int>(prefs.GetInt("font_size", 32), 22, 48);
    reader_line_spacing_ = std::clamp<int>(prefs.GetInt("line_space", 10), 2, 24);
    reader_margin_ = std::clamp<int>(prefs.GetInt("margin", 48), 32, 72);
    reader_background_index_ = std::clamp<int>(prefs.GetInt("paper", 0), 0, 4);
    reader_dark_ = prefs.GetBool("dark", false);
    reader_portrait_ = prefs.GetBool("portrait", true);
    reader_font_preference_ = prefs.GetString("font_path");
    reader_font_builtin_ = reader_font_preference_.empty() && prefs.GetInt("font", -2) == -1;
    last_path_ = prefs.GetString("last_path");
    resume_after_sleep_ = prefs.GetBool("sleep_resume", false);
    Settings screen("display");
    brightness_setting_ = std::clamp<int>(screen.GetInt("brightness", 60), 10, 100);
    auto_lock_minutes_ = std::clamp<int>(screen.GetInt("lock_min", 10), 1, 30);
    if (auto backlight = Board::GetInstance().GetBacklight())
        backlight->SetBrightness(brightness_setting_);
    jobs_ = xQueueCreate(4, sizeof(Job));
    ESP_ERROR_CHECK(jobs_ ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(Worker, "reader_card", 16 * 1024, this, 2, nullptr) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    lv_timer_create(Tick, 1000, this);
    initialized_ = true;
    reader_loading_ = true;
    Render(Page::Shelf);
    Queue(1);
}
bool ReaderDisplay::Queue(int type, int value) {
    Job job{type, value};
    if (xQueueSend(jobs_, &job, 0) == pdTRUE)
        return true;
    notice_ = "操作队列已满，请稍后重试";
    reader_loading_ = false;
    return false;
}
void ReaderDisplay::OnClick(lv_event_t* event) {
    auto self = static_cast<ReaderDisplay*>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (!self || esp_timer_get_time() < self->suppress_touch_until_)
        return;
    self->Action(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event))));
}
void ReaderDisplay::OnTouch(lv_event_t* event) {
    auto self = static_cast<ReaderDisplay*>(lv_event_get_user_data(event));
    if (!self || !self->asleep_ || self->sleep_action_)
        return;
    self->asleep_ = false;
    self->suppress_touch_until_ = esp_timer_get_time() + 400000;
    if (auto backlight = Board::GetInstance().GetBacklight())
        backlight->SetBrightness(self->brightness_setting_);
    lv_display_trigger_activity(self->display_);
}
void ReaderDisplay::RequestSleep() {
    if (asleep_ || usb_active_ || reader_loading_)
        return;
    if (!SaveReaderProgress(true))
        return;
    asleep_ = true;
    const bool resume = reader_open_;
    // FIFO application work commits the position before the worker removes power.
    if (!Application::GetInstance().Schedule([this, resume] {
            {
                Settings prefs("han_reader", true);
                prefs.SetBool("sleep_resume", resume);
            }
            if (!Queue(11)) {
                DisplayLockGuard lock(this);
                asleep_ = false;
            }
        }))
        asleep_ = false;
}
void ReaderDisplay::Tick(lv_timer_t* timer) {
    auto self = static_cast<ReaderDisplay*>(lv_timer_get_user_data(timer));
    if (ReaderTransferServer::GetInstance().Busy()) {
        lv_display_trigger_activity(self->display_);
        return;
    }
    if (!self->asleep_ && !self->usb_active_ && !self->reader_loading_ &&
        lv_display_get_inactive_time(self->display_) >= self->auto_lock_minutes_ * 60000U) {
        self->RequestSleep();
    }
}
void ReaderDisplay::OnRefresh(lv_event_t* event) {
    auto self = static_cast<ReaderDisplay*>(lv_event_get_user_data(event));
    if (!self->render_pending_)
        return;
    ESP_LOGI("ReaderUI", "Page %d ready in %lld ms; PSRAM=%u", static_cast<int>(self->page_),
             (esp_timer_get_time() - self->render_started_us_) / 1000,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    self->render_pending_ = false;
}
void ReaderDisplay::Render(Page page) {
    render_started_us_ = esp_timer_get_time();
    render_pending_ = true;
    page_ = page;
    lv_obj_clean(root_);
    brightness_value_ = brightness_slider_ = status_ = nullptr;
    wifi_name_ = wifi_password_ = wifi_keyboard_ = nullptr;
    lv_display_set_rotation(display_,
                            reader_portrait_ ? LV_DISPLAY_ROTATION_0 : LV_DISPLAY_ROTATION_90);
    width_ = reader_portrait_ ? 720 : 1280;
    height_ = reader_portrait_ ? 1280 : 720;
    lv_obj_set_size(root_, width_, height_);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(reader_dark_ ? 0x101722 : 0xf7f5ed), 0);
    if (!importing_)
        EnsureReaderFont();
    body_ = Box(root_, 0, 0, width_, height_, reader_dark_ ? 0x101722 : 0xf7f5ed);
    if (page == Page::Reader)
        DrawReader();
    else {
        if (page == Page::Shelf)
            DrawShelf();
        else if (page == Page::Settings)
            DrawSettings();
        else if (page == Page::Fonts)
            DrawFonts();
        else
            DrawTransfer();
        if (!lan_setup_ || page != Page::Transfer || reader_loading_)
            Navigation();
    }
    if (!notice_.empty()) {
        auto message =
            Label(root_, notice_.c_str(), 24,
                  (lan_setup_ && page == Page::Transfer) ? 428 : height_ - 120, width_ - 48);
        lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_invalidate(root_);
}
void ReaderDisplay::Navigation() {
    const int step = (width_ - 48) / 3;
    Button(root_, "书架", 24, height_ - 78, step - 12, 56, 0xefeee5, 10);
    Button(root_, "传书", 24 + step, height_ - 78, step - 12, 56, 0xefeee5, 11);
    Button(root_, "设置", 24 + 2 * step, height_ - 78, step - 12, 56, 0xefeee5, 12);
}
void ReaderDisplay::DrawShelf() {
    Label(body_, "Tab5 Reader", 24, 20, 300);
    Label(body_, "我的书架", 24, 74, 300);
    Button(body_, "继续阅读", width_ - 308, 18, 136, 52, 0xffffff, 15);
    Button(body_, "扫描", width_ - 160, 18, 136, 52, 0xffffff, 16);
    if (reader_loading_) {
        Label(body_, "正在整理书架…", 28, 200, width_ - 56);
        return;
    }
    if (reader_books_.empty()) {
        Label(body_,
              "书架还是空的\n\n将 EPUB 放入 TF 卡的 /handict/books\n或打开底部传书，使用手机上传。",
              28, 200, width_ - 56);
        return;
    }
    const int columns = reader_portrait_ ? 2 : 3;
    const int rows = reader_portrait_ ? 3 : 2;
    const int count = columns * rows;
    const int card_width = (width_ - 48 - (columns - 1) * 16) / columns;
    const int card_height = (height_ - 270 - (rows - 1) * 16) / rows;
    const size_t begin = shelf_page_ * count;
    for (int slot = 0; slot < count && begin + slot < reader_books_.size(); ++slot) {
        const auto index = begin + slot;
        const auto& book = reader_books_[index];
        auto card = Button(body_, "", 24 + slot % columns * (card_width + 16),
                           124 + slot / columns * (card_height + 16), card_width, card_height,
                           0xfffdf7, 100 + index);
        if (!book.cover_path.empty()) {
            auto cover = lv_image_create(card);
            const std::string path = "S:" + book.cover_path;
            lv_image_set_src(cover, path.c_str());
            lv_obj_set_pos(cover, 12, 16);
            lv_obj_set_size(cover, 76, 112);
            lv_image_set_inner_align(cover, LV_IMAGE_ALIGN_CONTAIN);
        } else
            Label(card, "EPUB", 10, 44, 80);
        auto title = Label(card, book.title.c_str(), 104, 14, card_width - 116, DynamicTextFont());
        lv_obj_set_height(title, std::min(100, card_height - 90));
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        std::string progress = book.readable()
                                   ? std::to_string(reader_book_progress_[index] / 10) + "% · " +
                                         std::to_string(book.spine.size()) + " 章"
                                   : "无法打开";
        Label(card, progress.c_str(), 104, card_height - 70, card_width - 116);
        auto bar = lv_bar_create(card);
        lv_obj_set_pos(bar, 12, card_height - 24);
        lv_obj_set_size(bar, card_width - 24, 6);
        lv_bar_set_range(bar, 0, 1000);
        lv_bar_set_value(bar, reader_book_progress_[index], LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x646f60), LV_PART_INDICATOR);
    }
    Button(body_, "上一组", 24, height_ - 140, 120, 44, 0xefeee5, 30);
    const std::string page = std::to_string(shelf_page_ + 1) + " / " +
                             std::to_string((reader_books_.size() + count - 1) / count);
    auto position = Label(body_, page.c_str(), width_ / 2 - 60, height_ - 134, 120);
    lv_obj_set_style_text_align(position, LV_TEXT_ALIGN_CENTER, 0);
    Button(body_, "下一组", width_ - 144, height_ - 140, 120, 44, 0xefeee5, 31);
}
void ReaderDisplay::DrawSettings() {
    Label(body_, "阅读设置", 24, 24, width_ - 48);
    Label(body_, "亮度", 28, 98, 100);
    brightness_value_ = Label(body_, "", width_ - 120, 98, 90);
    brightness_slider_ = lv_slider_create(body_);
    lv_obj_set_pos(brightness_slider_, 140, 108);
    lv_obj_set_size(brightness_slider_, width_ - 304, 20);
    lv_slider_set_range(brightness_slider_, 10, 100);
    lv_slider_set_value(brightness_slider_, brightness_setting_, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider_, OnSettingSliderChanged, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(brightness_slider_, OnSettingSliderReleased, LV_EVENT_RELEASED, this);
    UpdateSettingLabels();
    const int columns = 3, step = (width_ - 56) / columns;
    Label(body_, "纸张背景", 28, 165, width_ - 56);
    for (int i = 0; i < 6; ++i) {
        const bool selected =
            i == 5 ? reader_dark_ : !reader_dark_ && reader_background_index_ == i;
        Button(body_, kReaderBackgroundNames[i], 28 + i % columns * step, 208 + i / columns * 60,
               step - 12, 50, selected ? 0xd8e2d0 : 0xffffff, 1400 + i);
    }
    Label(body_, "自动休眠", 28, 340, width_ - 56);
    for (int i = 0; i < 6; ++i) {
        const auto label = std::to_string(kReaderScreenTimeouts[i]) + " 分钟";
        Button(body_, label.c_str(), 28 + i % columns * step, 382 + i / columns * 60, step - 12, 50,
               auto_lock_minutes_ == kReaderScreenTimeouts[i] ? 0xd8e2d0 : 0xffffff, 1410 + i);
    }
    Button(body_, reader_portrait_ ? "横屏" : "竖屏", 28, 514, step - 12, 50, 0xffffff, 1345);
    Button(body_, "字体", 28 + step, 514, step - 12, 50, 0xffffff, 40);
    Button(body_, "立即休眠", 28 + 2 * step, 514, step - 12, 50, 0xffffff, 41);
    if (reader_portrait_)
        Label(body_, "休眠后按电源键开机\n阅读中点中央展开菜单，左翻前页，右翻后页。", 28, 610,
              width_ - 56);
}
void ReaderDisplay::DrawFonts() {
    Label(body_, "选择阅读字体", 24, 24, width_ - 48);
    Button(body_, "内置字体", 28, 90, width_ - 56, 50, 0xffffff, 1360);
    const int columns = reader_portrait_ ? 2 : 3;
    const int step = (width_ - 56) / columns;
    for (size_t i = 0; i < reader_fonts_.size(); ++i) {
        const auto name = ReaderFontDisplayName(reader_fonts_[i]);
        Button(body_, name.c_str(), 28 + i % columns * step, 156 + i / columns * 62, step - 12, 52,
               reader_font_index_ == static_cast<int>(i) ? 0xd8e2d0 : 0xffffff, 1361 + i);
    }
}
void ReaderDisplay::OnWifiInput(lv_event_t* event) {
    auto self = static_cast<ReaderDisplay*>(lv_event_get_user_data(event));
    if (self->wifi_keyboard_)
        lv_keyboard_set_textarea(self->wifi_keyboard_, lv_event_get_target_obj(event));
}
void ReaderDisplay::DrawTransfer() {
    Label(body_, "导入书籍与字体", 24, 24, width_ - 48);
    auto& server = ReaderTransferServer::GetInstance();
    if (reader_loading_) {
        Label(body_, "正在准备，请稍候…", 28, 150, width_ - 56);
        return;
    }
    if (lan_setup_) {
        Label(body_, "连接局域网 · 仅在传书时启用 Wi-Fi", 28, 70, width_ - 56);
        auto input = [&](int y, const char* placeholder, const std::string& value, bool password) {
            auto obj = lv_textarea_create(body_);
            lv_obj_set_pos(obj, 28, y);
            lv_obj_set_size(obj, width_ - 56, 52);
            lv_obj_set_style_text_font(obj, DictionaryTextFont(), 0);
            lv_textarea_set_one_line(obj, true);
            lv_textarea_set_max_length(obj, password ? 64 : 32);
            lv_textarea_set_placeholder_text(obj, placeholder);
            lv_textarea_set_password_mode(obj, password);
            lv_textarea_set_text(obj, value.c_str());
            lv_obj_add_event_cb(obj, OnWifiInput, LV_EVENT_FOCUSED, this);
            lv_obj_add_event_cb(obj, OnWifiInput, LV_EVENT_CLICKED, this);
            return obj;
        };
        wifi_name_ = input(112, "Wi-Fi 名称（2.4GHz）", pending_ssid_, false);
        wifi_password_ = input(176, "密码：留空使用已保存的密码", pending_password_, true);
        const auto& networks = server.Networks();
        for (size_t i = 0; i < std::min<size_t>(networks.size(), 4); ++i)
            Button(body_, networks[i].ssid.c_str(), 28 + (i % 2) * (width_ / 2 - 14),
                   244 + (i / 2) * 54, width_ / 2 - 42, 46, 0xffffff, 1500 + i);
        Button(body_, "连接并开启传书", 28, 364, width_ / 2 - 42, 56, 0xd8e2d0, 65);
        Button(body_, "取消", width_ / 2 + 14, 364, width_ / 2 - 42, 56, 0xffffff, 62);
        wifi_keyboard_ = lv_keyboard_create(root_);
        lv_obj_set_size(wifi_keyboard_, width_, reader_portrait_ ? 310 : 240);
        lv_obj_align(wifi_keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_textarea(wifi_keyboard_, wifi_password_);
    } else if (usb_active_) {
        Label(body_, "USB 读卡器已开启\n请在电脑安全弹出 TF 卡后，再退出。", 28, 140, width_ - 56);
        Button(body_, "已安全弹出，退出并重启", 28, 340, width_ - 56, 64, 0xffffff, 64);
    } else if (server.Active()) {
        std::string info = server.IsLan()
                               ? "已连接 Wi-Fi：" + server.Ssid() + "\n手机、电脑连接同一局域网。"
                               : "手机连接热点：" + server.Ssid() + "\n密码：" + server.Password();
        info += "\n\n浏览器打开：http://" + server.Address() + "\n网页连接码：" + server.Token() +
                "\n\n支持 EPUB、TTF；传输期间暂停阅读。\n停止传书或休眠后断开 Wi-Fi。";
        Label(body_, info.c_str(), 28, 108, width_ - 56);
        Button(body_, "停止传书", 28, reader_portrait_ ? 660 : 470, width_ - 56, 64, 0xffffff, 62);
    } else {
        Label(body_, "选择传书方式，网络只在传书期间开启。", 28, 130, width_ - 56);
        Button(body_, "局域网传书", 28, 230, width_ - 56, 66, 0xd8e2d0, 61);
        Button(body_, "Wi-Fi 热点传书", 28, 316, width_ - 56, 66, 0xffffff, 60);
        Button(body_, "USB 磁盘模式", 28, 402, width_ - 56, 66, 0xffffff, 63);
    }
}
void ReaderDisplay::UpdateSettingLabels() {
    if (brightness_value_) {
        const auto value = std::to_string(brightness_setting_) + "%";
        lv_label_set_text(brightness_value_, value.c_str());
    }
}
void ReaderDisplay::OnSettingSliderChanged(lv_event_t* event) {
    auto self = static_cast<ReaderDisplay*>(lv_event_get_user_data(event));
    self->brightness_setting_ = lv_slider_get_value(lv_event_get_target_obj(event));
    self->UpdateSettingLabels();
}
void ReaderDisplay::OnSettingSliderReleased(lv_event_t* event) {
    auto self = static_cast<ReaderDisplay*>(lv_event_get_user_data(event));
    const int value = self->brightness_setting_;
    Application::GetInstance().Schedule([value] {
        if (auto backlight = Board::GetInstance().GetBacklight())
            backlight->SetBrightness(value, true);
    });
}
void ReaderDisplay::SaveAutoLockSetting() {
    const int value = auto_lock_minutes_;
    Application::GetInstance().Schedule([value] {
        Settings settings("display", true);
        settings.SetInt("lock_min", value);
    });
}
void ReaderDisplay::Toast(const char* text) {
    notice_ = text;
    Render(page_);
}
void ReaderDisplay::Action(int action) {
    if (asleep_ || reader_loading_)
        return;
    notice_.clear();
    if (importing_ && action != 62 && action != 64 && action != 65 &&
        !(action >= 1500 && action < 1516))
        return;
    if (action == 10 || action == 1332) {
        SaveReaderProgress(true);
        reader_open_ = false;
        reader_screen_settings_open_ = reader_font_picker_open_ = false;
        Render(Page::Shelf);
    } else if (action == 11) {
        SaveReaderProgress(true);
        Render(Page::Transfer);
    } else if (action == 12)
        Render(Page::Settings);
    else if (action == 15) {
        auto it = std::find_if(reader_books_.begin(), reader_books_.end(),
                               [this](const auto& book) { return book.path == last_path_; });
        if (it == reader_books_.end()) {
            Toast("打开一本书后，即可在这里继续阅读");
            return;
        }
        Action(100 + static_cast<int>(it - reader_books_.begin()));
    } else if (action == 16) {
        reader_loading_ = true;
        Render(Page::Shelf);
        Queue(1);
    } else if (action == 30 || action == 31) {
        const int max_page = std::max(0, static_cast<int>((reader_books_.size() + 5) / 6) - 1);
        shelf_page_ = std::clamp(shelf_page_ + (action == 30 ? -1 : 1), 0, max_page);
        Render(Page::Shelf);
    } else if (action >= 100 && action < 164) {
        const size_t index = action - 100;
        if (index >= reader_books_.size())
            return;
        if (!reader_books_[index].readable()) {
            Toast(reader_books_[index].error.c_str());
            return;
        }
        reader_selected_book_ = index;
        reader_book_ = reader_books_[index];
        reader_loading_ = true;
        reader_open_ = true;
        reader_controls_visible_ = false;
        reader_error_.clear();
        Render(Page::Reader);
        Queue(2, index);
    } else if (action == 1380 || action == 1382)
        Action(action == 1380 ? 1330 : 1331);
    else if (action == 1330 || action == 1331) {
        if (reader_page_offsets_.empty())
            return;
        if (action == 1330 && reader_page_index_ > 0) {
            --reader_page_index_;
            SaveReaderProgress(false);
            Render(Page::Reader);
        } else if (action == 1331 && reader_page_index_ + 1 < reader_page_offsets_.size()) {
            ++reader_page_index_;
            SaveReaderProgress(false);
            Render(Page::Reader);
        } else {
            const bool previous = action == 1330;
            if ((previous && reader_spine_index_ == 0) ||
                (!previous && reader_spine_index_ + 1 >= reader_book_.spine.size())) {
                Toast(previous ? "已经是全书开头" : "已经是全书末尾");
                return;
            }
            SaveReaderProgress(true);
            reader_loading_ = true;
            Render(Page::Reader);
            Queue(previous ? 4 : 3);
        }
    } else if (action == 1381) {
        reader_controls_visible_ = !reader_controls_visible_;
        reader_font_picker_open_ = reader_screen_settings_open_ = false;
        Render(Page::Reader);
    } else if (action == 1390 || action == 1391) {
        reader_screen_settings_open_ = action == 1390;
        Render(Page::Reader);
    } else if (action == 1346 || action == 40) {
        font_return_page_ = page_;
        if (page_ == Page::Reader)
            reader_font_picker_open_ = true;
        Render(page_ == Page::Reader ? Page::Reader : Page::Fonts);
    } else if (action == 1379) {
        reader_font_picker_open_ = false;
        Render(Page::Reader);
    } else if (action >= 1360 && action <= 1376) {
        const int index = action - 1361;
        if (index >= static_cast<int>(reader_fonts_.size()))
            return;
        reader_font_index_ = index;
        reader_font_builtin_ = index < 0;
        reader_font_preference_ = index < 0 ? "" : reader_fonts_[index];
        reader_font_picker_open_ = false;
        SaveReaderPreferences();
        Render(font_return_page_);
    } else if (action >= 1400 && action <= 1405) {
        reader_dark_ = action == 1405;
        if (!reader_dark_)
            reader_background_index_ = action - 1400;
        SaveReaderPreferences();
        Render(page_);
    } else if (action >= 1410 && action <= 1415) {
        auto_lock_minutes_ = kReaderScreenTimeouts[action - 1410];
        lv_display_trigger_activity(display_);
        SaveAutoLockSetting();
        Render(page_);
    } else if (action >= 1340 && action <= 1345) {
        const size_t offset =
            reader_page_offsets_.empty() ? 0 : reader_page_offsets_[reader_page_index_];
        if (action == 1340)
            reader_font_size_ = std::max(22, reader_font_size_ - 2);
        if (action == 1341)
            reader_font_size_ = std::min(48, reader_font_size_ + 2);
        if (action == 1342)
            reader_line_spacing_ = std::max(2, reader_line_spacing_ - 2);
        if (action == 1343)
            reader_line_spacing_ = std::min(24, reader_line_spacing_ + 2);
        if (action == 1344)
            reader_margin_ = reader_margin_ >= 72 ? 32 : reader_margin_ + 20;
        if (action == 1345)
            reader_portrait_ = !reader_portrait_;
        BuildReaderPages(offset);
        SaveReaderPreferences();
        Render(page_);
    } else if (action == 1347) {
        reader_dark_ = !reader_dark_;
        SaveReaderPreferences();
        Render(Page::Reader);
    } else if (action == 41) {
        RequestSleep();
    } else if (action >= 1500 && action < 1516 && wifi_name_) {
        const auto& networks = ReaderTransferServer::GetInstance().Networks();
        const size_t index = action - 1500;
        if (index < networks.size())
            lv_textarea_set_text(wifi_name_, networks[index].ssid.c_str());
    } else if (action == 65 && wifi_name_ && wifi_password_) {
        pending_ssid_ = lv_textarea_get_text(wifi_name_);
        pending_password_ = lv_textarea_get_text(wifi_password_);
        reader_loading_ = true;
        Render(Page::Transfer);
        Queue(10);
    } else if (action == 60 || action == 61 || action == 63) {
        SaveReaderProgress(true);
        lv_obj_clean(root_);
        // Close every font/image file before either browser writes or USB ownership begins.
        ReleaseReaderFont();
        lv_image_cache_drop(nullptr);
        importing_ = true;
        reader_loading_ = true;
        Render(Page::Transfer);
        Queue(action == 60 ? 5 : (action == 61 ? 9 : 7));
    } else if (action == 62 || action == 64) {
        reader_loading_ = true;
        Render(Page::Transfer);
        Queue(action == 62 ? 6 : 8);
    }
}
void ReaderDisplay::Worker(void* context) {
    auto self = static_cast<ReaderDisplay*>(context);
    Job job{};
    for (;;) {
        if (xQueueReceive(self->jobs_, &job, portMAX_DELAY) != pdTRUE)
            continue;
        if (job.type == 1) {
            auto fonts = han::reader::ScanTtfFonts("/sdcard/handict/fonts");
            auto books = han::reader::ScanLibrary("/sdcard/handict/books");
            DisplayLockGuard lock(self);
            self->reader_fonts_ = std::move(fonts);
            self->reader_books_ = std::move(books);
            self->reader_book_progress_.clear();
            for (const auto& book : self->reader_books_) {
                size_t spine, offset;
                uint16_t progress;
                self->LoadReaderProgress(book.path, spine, offset, progress);
                self->reader_book_progress_.push_back(progress);
            }
            auto font = std::find(self->reader_fonts_.begin(), self->reader_fonts_.end(),
                                  self->reader_font_preference_);
            if (font != self->reader_fonts_.end())
                self->reader_font_index_ = font - self->reader_fonts_.begin();
            else if (!self->reader_font_builtin_ && self->reader_font_preference_.empty() &&
                     !self->reader_fonts_.empty()) {
                self->reader_font_index_ = 0;
                self->reader_font_preference_ = self->reader_fonts_[0];
            } else
                self->reader_font_index_ = -1;
            self->reader_loading_ = false;
            self->reader_scanned_ = true;
            self->shelf_page_ = 0;
            self->Render(Page::Shelf);
            if (self->resume_after_sleep_) {
                self->resume_after_sleep_ = false;
                Application::GetInstance().Schedule([] {
                    Settings prefs("han_reader", true);
                    prefs.SetBool("sleep_resume", false);
                });
                self->Action(15);
            }
        } else if (job.type >= 2 && job.type <= 4) {
            han::reader::EpubBook book;
            size_t start = 0, offset = 0;
            {
                DisplayLockGuard lock(self);
                book = self->reader_book_;
                if (job.type == 2) {
                    uint16_t progress;
                    self->LoadReaderProgress(book.path, start, offset, progress);
                    start = std::min(start, book.spine.size() - 1);
                } else
                    start = job.type == 4 ? self->reader_spine_index_ - 1
                                          : self->reader_spine_index_ + 1;
            }
            std::string text, error;
            size_t actual = start;
            han::reader::EpubReader::FindReadableSpineText(book, start, job.type == 4 ? -1 : 1,
                                                           actual, text, error);
            DisplayLockGuard lock(self);
            self->reader_text_ = std::move(text);
            self->reader_error_ = std::move(error);
            self->reader_spine_index_ = actual;
            self->BuildReaderPages(actual == start ? offset : 0);
            if (job.type == 4 && !self->reader_page_offsets_.empty())
                self->reader_page_index_ = self->reader_page_offsets_.size() - 1;
            self->reader_loading_ = false;
            self->last_path_ = book.path;
            self->SaveReaderProgress(true);
            self->Render(Page::Reader);
        } else {
            std::string error;
            if (job.type == 11) {
                ReaderTransferServer::GetInstance().Stop();
                {
                    DisplayLockGuard lock(self);
                    lv_obj_clean(self->root_);
                    self->ReleaseReaderFont();
                    lv_image_cache_drop(nullptr);
                }
                if (self->sleep_action_)
                    self->sleep_action_();
                continue;
            }
            if (job.type == 9)
                error = ReaderTransferServer::GetInstance().PrepareLan();
            if (job.type == 10)
                error = ReaderTransferServer::GetInstance().StartLan(self->pending_ssid_,
                                                                     self->pending_password_);
            if (job.type == 5)
                error = ReaderTransferServer::GetInstance().Start();
            if (job.type == 6)
                ReaderTransferServer::GetInstance().Stop();
            if (job.type == 7)
                error = self->usb_start_ ? self->usb_start_() : "USB 不可用";
            if (job.type == 8)
                error = self->usb_stop_ ? self->usb_stop_() : "USB 不可用";
            {
                DisplayLockGuard lock(self);
                self->reader_loading_ = false;
                self->notice_ = error;
                if (job.type == 9) {
                    self->lan_setup_ = true;
                    self->pending_ssid_ = ReaderTransferServer::GetInstance().PreferredSsid();
                    self->pending_password_.clear();
                }
                if (job.type == 10 && error.empty())
                    self->lan_setup_ = false;
                if (job.type == 6)
                    self->lan_setup_ = false;
                // Even a failed USB start may already have unmounted FAT. Keep card readers
                // suspended until the explicit exit/restart restores ownership.
                if (job.type == 7)
                    self->usb_active_ = true;
                if (job.type == 6 || (!error.empty() && !self->usb_active_ && !self->lan_setup_))
                    self->importing_ = false;
                self->Render(Page::Transfer);
                if (job.type == 6) {
                    self->reader_loading_ = true;
                    self->Render(Page::Shelf);
                    self->Queue(1);
                }
            }
            if (job.type == 8 && error.empty())
                esp_restart();
        }
    }
}
void ReaderDisplay::BuildReaderPages(size_t preferred_offset) {
    reader_page_offsets_.clear();
    reader_page_offsets_.push_back(0);
    reader_page_index_ = 0;
    if (reader_text_.empty())
        return;

    // This is the phase-1 paginator: it uses the selected font metrics as a conservative cell
    // estimate, then prefers paragraph boundaries. The next phase replaces this estimate with
    // glyph-by-glyph LVGL measurement while preserving these stable page offsets in the UI layer.
    const size_t page_width = reader_portrait_ ? 720 : 1280;
    const size_t page_height = reader_portrait_ ? 1280 : 720;
    const size_t columns =
        std::max<size_t>(10, (page_width - 2 * reader_margin_) / reader_font_size_);
    const size_t rows =
        std::max<size_t>(8, (page_height - 48) / (reader_font_size_ + reader_line_spacing_));
    const size_t characters_per_page = std::max<size_t>(80, columns * rows);
    size_t cursor = 0;
    while (cursor < reader_text_.size()) {
        const size_t start = cursor;
        size_t characters = 0;
        size_t last_paragraph = std::string::npos;
        while (cursor < reader_text_.size() && characters < characters_per_page) {
            if (reader_text_[cursor] == '\n')
                last_paragraph = cursor + 1;
            const unsigned char lead = static_cast<unsigned char>(reader_text_[cursor]);
            size_t width = lead < 0x80 ? 1 : (lead < 0xe0 ? 2 : (lead < 0xf0 ? 3 : 4));
            cursor = std::min(reader_text_.size(), cursor + width);
            ++characters;
        }
        if (cursor < reader_text_.size() && last_paragraph != std::string::npos &&
            last_paragraph > start + 16)
            cursor = last_paragraph;
        if (cursor <= start)
            break;
        if (cursor < reader_text_.size())
            reader_page_offsets_.push_back(cursor);
    }
    const auto page = std::upper_bound(reader_page_offsets_.begin(), reader_page_offsets_.end(),
                                       std::min(preferred_offset, reader_text_.size()));
    if (page != reader_page_offsets_.begin())
        reader_page_index_ = static_cast<size_t>(page - reader_page_offsets_.begin() - 1);
}

void ReaderDisplay::LoadReaderProgress(const std::string& path, size_t& spine, size_t& offset,
                                       uint16_t& progress) const {
    spine = 0;
    offset = 0;
    progress = 0;
    Settings settings("han_reader");
    spine =
        static_cast<size_t>(std::max<int32_t>(0, settings.GetInt(ReaderProgressKey('s', path))));
    offset =
        static_cast<size_t>(std::max<int32_t>(0, settings.GetInt(ReaderProgressKey('o', path))));
    progress = static_cast<uint16_t>(
        std::clamp<int32_t>(settings.GetInt(ReaderProgressKey('p', path)), 0, 1000));
}

bool ReaderDisplay::SaveReaderProgress(bool force) {
    if (!reader_open_ || reader_book_.path.empty() || reader_book_.spine.empty())
        return true;
    const size_t offset =
        reader_page_offsets_.empty()
            ? 0
            : reader_page_offsets_[std::min(reader_page_index_, reader_page_offsets_.size() - 1)];
    const uint32_t chapter_progress = reader_text_.empty()
                                          ? 0
                                          : static_cast<uint32_t>(std::min<uint64_t>(
                                                1000, offset * 1000ULL / reader_text_.size()));
    const uint16_t progress = static_cast<uint16_t>(std::min<uint64_t>(
        1000, (reader_spine_index_ * 1000ULL + chapter_progress) / reader_book_.spine.size()));
    if (reader_selected_book_ < reader_book_progress_.size())
        reader_book_progress_[reader_selected_book_] = progress;
    if (!force && ++reader_pages_since_progress_save_ < 5)
        return true;
    reader_pages_since_progress_save_ = 0;
    const std::string path = reader_book_.path;
    const int32_t saved_spine = static_cast<int32_t>(reader_spine_index_);
    const int32_t saved_offset = static_cast<int32_t>(
        std::min<size_t>(offset, static_cast<size_t>(std::numeric_limits<int32_t>::max())));
    return Application::GetInstance().Schedule([path, saved_spine, saved_offset, progress] {
        Settings settings("han_reader", true);
        settings.SetInt(ReaderProgressKey('s', path), saved_spine);
        settings.SetInt(ReaderProgressKey('o', path), saved_offset);
        settings.SetInt(ReaderProgressKey('p', path), progress);
        settings.SetString("last_path", path);
    });
}

void ReaderDisplay::SaveReaderPreferences() {
    const int font_size = reader_font_size_;
    const int line_spacing = reader_line_spacing_;
    const int margin = reader_margin_;
    const int font_index = reader_font_index_;
    const std::string font_path =
        font_index >= 0 && font_index < static_cast<int>(reader_fonts_.size())
            ? reader_fonts_[font_index]
            : std::string{};
    const bool dark = reader_dark_;
    const int background = reader_background_index_;
    const bool portrait = reader_portrait_;
    Application::GetInstance().Schedule(
        [font_size, line_spacing, margin, font_index, font_path, dark, background, portrait] {
            Settings settings("han_reader", true);
            settings.SetInt("font_size", font_size);
            settings.SetInt("line_space", line_spacing);
            settings.SetInt("margin", margin);
            settings.SetInt("font", font_index);
            settings.SetString("font_path", font_path);
            settings.SetBool("dark", dark);
            settings.SetInt("paper", background);
            settings.SetBool("portrait", portrait);
        });
}

void ReaderDisplay::EnsureReaderFont() {
#if LV_USE_FREETYPE
    if (importing_)
        return;
    const std::string path =
        reader_font_index_ >= 0 && reader_font_index_ < static_cast<int>(reader_fonts_.size())
            ? reader_fonts_[reader_font_index_]
            : reader_font_preference_;
    if (reader_font_ && reader_font_loaded_path_ == path &&
        reader_font_loaded_size_ == reader_font_size_)
        return;
    ReleaseReaderFont();
    if (path.empty())
        return;
    // FreeType keeps the font on the SD card and reads only the tables/glyphs it needs. This is
    // what lets a 26 MiB CJK font work without requiring a 26 MiB contiguous PSRAM allocation.
    reader_font_ = lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                           reader_font_size_, LV_FREETYPE_FONT_STYLE_NORMAL);
    reader_ui_font_ = lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 24,
                                              LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!reader_font_) {
        if (reader_ui_font_)
            lv_freetype_font_delete(reader_ui_font_);
        reader_ui_font_ = nullptr;
        ESP_LOGW("HanDisplay", "Unable to stream reader font with FreeType: %s", path.c_str());
        return;
    }
    AccelerateReaderFont(reader_font_, reader_font_size_);
    reader_font_->fallback = const_cast<lv_font_t*>(DictionaryTextFont());
    if (reader_ui_font_)
        reader_ui_font_->fallback = const_cast<lv_font_t*>(DictionaryTextFont());
    reader_font_loaded_path_ = path;
    reader_font_loaded_size_ = reader_font_size_;
    ESP_LOGI("HanDisplay", "Reader font opened with FreeType streaming: %s at %d px", path.c_str(),
             reader_font_size_);
#endif
}
void ReaderDisplay::ReleaseReaderFont() {
#if LV_USE_FREETYPE
    if (reader_ui_font_)
        lv_freetype_font_delete(reader_ui_font_);
    if (reader_font_)
        lv_freetype_font_delete(reader_font_);
    reader_ui_font_ = reader_font_ = nullptr;
    reader_font_loaded_path_.clear();
    reader_font_loaded_size_ = 0;
#endif
}

void ReaderDisplay::DrawReader() {
    const lv_font_t* shelf_title_font = DynamicTextFont();
    const lv_font_t* shelf_ui_font = DynamicTextFont();
    const int reader_width = reader_portrait_ ? 720 : 1280;
    const int reader_height = reader_portrait_ ? 1280 : 720;
    const uint32_t page_background =
        reader_dark_ ? 0x101722 : kReaderBackgrounds[reader_background_index_];
    const uint32_t page_text = reader_dark_ ? 0xf3f6fa : 0x263247;
    const uint32_t chrome_background = reader_dark_ ? 0x202c3d : 0xf4ecdf;
    const uint32_t chrome_text = reader_dark_ ? 0xf5f7fb : 0x263247;
    const uint32_t chrome_muted = reader_dark_ ? 0xbec9d8 : 0x61708f;
    auto paper = Box(body_, 0, 0, reader_width, reader_height, page_background);
    lv_obj_set_style_radius(paper, 0, 0);
    // Reader day/night colors are independent from the global UI theme. Explicit colors prevent
    // the shared theme mapper from darkening an already-dark reader palette a second time.
    lv_obj_set_style_bg_color(paper, lv_color_hex(page_background), 0);
    if (reader_loading_) {
        auto loading = Label(paper, "正在解压并整理章节…", 0, reader_height / 2 - 20, reader_width,
                             shelf_title_font);
        lv_obj_set_style_text_align(loading, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(loading, lv_color_hex(page_text), 0);
        return;
    }
    if (!reader_error_.empty()) {
        auto error = Label(paper, reader_error_.c_str(), 48, reader_height / 2 - 90,
                           reader_width - 96, shelf_title_font);
        lv_obj_set_style_text_align(error, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(error, lv_color_hex(0xc0434f), 0);
        auto back = Button(paper, "返回书架", (reader_width - 260) / 2, reader_height / 2 + 30, 260,
                           70, kBlue, 1332);
        if (auto label = lv_obj_get_child(back, 0))
            lv_obj_set_style_text_font(label, shelf_ui_font, 0);
        return;
    }

    EnsureReaderFont();
    const lv_font_t* reader_control_font = reader_ui_font_ ? reader_ui_font_ : DictionaryTextFont();
    const size_t start =
        reader_page_offsets_.empty() ? 0 : reader_page_offsets_[reader_page_index_];
    const size_t end = reader_page_index_ + 1 < reader_page_offsets_.size()
                           ? reader_page_offsets_[reader_page_index_ + 1]
                           : reader_text_.size();
    ESP_LOGI("HanDisplay", "Reader position: spine=%u page=%u/%u bytes=%u..%u",
             static_cast<unsigned>(reader_spine_index_),
             static_cast<unsigned>(reader_page_index_ + 1),
             static_cast<unsigned>(reader_page_offsets_.size()), static_cast<unsigned>(start),
             static_cast<unsigned>(end));
    const auto page_text_value = reader_text_.substr(start, end - start);
    auto text = Label(paper, page_text_value.c_str(), reader_margin_, 24,
                      reader_width - 2 * reader_margin_);
    lv_obj_set_height(text, reader_height - 48);
    lv_obj_set_style_text_font(text, reader_font_ ? reader_font_ : DictionaryTextFont(), 0);
    lv_obj_set_style_text_color(text, lv_color_hex(page_text), 0);
    lv_obj_set_style_text_line_space(text, reader_line_spacing_, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);

    const int left_width = reader_width * 32 / 100;
    const int center_width = reader_width * 36 / 100;
    const auto tap_zone = [&](int x, int width, int action) {
        auto zone = Box(paper, x, 0, width, reader_height, page_background);
        lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(zone, this);
        lv_obj_add_event_cb(zone, OnClick, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(action)));
    };
    tap_zone(0, left_width, 1380);
    tap_zone(left_width, center_width, 1381);
    tap_zone(left_width + center_width, reader_width - left_width - center_width, 1382);

    if (!reader_controls_visible_ && !reader_font_picker_open_ && !reader_screen_settings_open_)
        return;

    const auto reader_button = [&](lv_obj_t* parent, const char* label, int x, int y, int width,
                                   int height, int action, bool selected = false) {
        auto button = Button(parent, label, x, y, width, height, kBlue, action);
        lv_obj_set_style_bg_color(button,
                                  lv_color_hex(selected ? (reader_dark_ ? 0x426b5a : 0xccebd8)
                                                        : (reader_dark_ ? 0x33445b : 0xfffaf0)),
                                  0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(reader_dark_ ? 0x566981 : 0xd9cfc0), 0);
        if (auto label_object = lv_obj_get_child(button, 0)) {
            lv_obj_set_style_text_font(label_object, reader_control_font, 0);
            lv_obj_set_style_text_color(label_object, lv_color_hex(chrome_text), 0);
            lv_label_set_long_mode(label_object, LV_LABEL_LONG_DOT);
        }
        return button;
    };

    const std::string progress = std::to_string(reader_page_index_ + 1) + "/" +
                                 std::to_string(std::max<size_t>(1, reader_page_offsets_.size()));
    auto top_controls =
        Box(paper, 0, 0, reader_width, reader_portrait_ ? 88 : 80, chrome_background);
    lv_obj_set_style_bg_color(top_controls, lv_color_hex(chrome_background), 0);
    lv_obj_set_style_bg_opa(top_controls, LV_OPA_90, 0);
    lv_obj_set_style_radius(top_controls, 0, 0);
    reader_button(top_controls, "书架", 16, 12, 104, reader_portrait_ ? 64 : 56, 1332);
    auto book_title = Label(top_controls, reader_book_.title.c_str(), 136, 18, reader_width - 440,
                            reader_control_font);
    lv_obj_set_height(book_title, 42);
    lv_label_set_long_mode(book_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(book_title, lv_color_hex(chrome_text), 0);
    auto progress_label =
        Label(top_controls, progress.c_str(), reader_width - 280, 22, 132, reader_control_font);
    lv_obj_set_style_text_align(progress_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(progress_label, lv_color_hex(chrome_muted), 0);
    reader_button(top_controls, "屏幕", reader_width - 136, 12, 120, reader_portrait_ ? 64 : 56,
                  1390);

    const int bottom_height = reader_portrait_ ? 190 : 96;
    auto controls = Box(paper, 0, reader_height - bottom_height, reader_width, bottom_height,
                        chrome_background);
    lv_obj_set_style_bg_color(controls, lv_color_hex(chrome_background), 0);
    lv_obj_set_style_bg_opa(controls, LV_OPA_90, 0);
    lv_obj_set_style_radius(controls, 0, 0);
    const char* labels[] = {"A-",
                            "A+",
                            "行-",
                            "行+",
                            "边距",
                            "字体",
                            reader_dark_ ? "日间" : "夜间",
                            reader_portrait_ ? "横屏" : "竖屏"};
    const int actions[] = {1340, 1341, 1342, 1343, 1344, 1346, 1347, 1345};
    for (int index = 0; index < 8; ++index) {
        if (reader_portrait_) {
            const int column = index % 4;
            const int row = index / 4;
            reader_button(controls, labels[index], 24 + column * 168, 16 + row * 82, 144, 66,
                          actions[index], index == 6 && reader_dark_);
        } else {
            reader_button(controls, labels[index], 20 + index * 157, 15, 137, 66, actions[index],
                          index == 6 && reader_dark_);
        }
    }

    if (reader_screen_settings_open_) {
        auto dim = Box(paper, 0, 0, reader_width, reader_height, 0x172033);
        lv_obj_set_style_bg_opa(dim, LV_OPA_70, 0);
        lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
        const int panel_width = reader_portrait_ ? reader_width - 48 : reader_width - 160;
        const int panel_height = reader_portrait_ ? 660 : 500;
        auto panel = Box(dim, (reader_width - panel_width) / 2, (reader_height - panel_height) / 2,
                         panel_width, panel_height, chrome_background);
        lv_obj_set_style_bg_color(panel, lv_color_hex(chrome_background), 0);
        lv_obj_set_style_radius(panel, 24, 0);
        lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
        const auto setting_label = [&](const char* value, int x, int y, int width) {
            auto label = Label(panel, value, x, y, width, reader_control_font);
            lv_obj_set_style_text_color(label, lv_color_hex(chrome_text), 0);
            return label;
        };
        setting_label("阅读屏幕设置", 28, 24, panel_width - 200);
        reader_button(panel, "关闭", panel_width - 148, 14, 120, 58, 1391);
        setting_label("亮度", 28, 98, 100);
        brightness_value_ = setting_label("", panel_width - 132, 98, 104);
        lv_obj_set_style_text_align(brightness_value_, LV_TEXT_ALIGN_RIGHT, 0);
        brightness_slider_ = lv_slider_create(panel);
        lv_obj_set_pos(brightness_slider_, 140, 110);
        lv_obj_set_size(brightness_slider_, panel_width - 304, 18);
        lv_slider_set_range(brightness_slider_, 10, 100);
        lv_slider_set_value(brightness_slider_, brightness_setting_, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0x87958d), LV_PART_MAIN);
        lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0x529379), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0x529379), LV_PART_KNOB);
        lv_obj_add_event_cb(brightness_slider_, OnSettingSliderChanged, LV_EVENT_VALUE_CHANGED,
                            this);
        lv_obj_add_event_cb(brightness_slider_, OnSettingSliderReleased, LV_EVENT_RELEASED, this);
        setting_label("纸张背景", 28, 168, panel_width - 56);
        const int columns = reader_portrait_ ? 3 : 6;
        const int step = (panel_width - 56) / columns;
        for (int index = 0; index < 6; ++index) {
            const bool selected =
                index == 5 ? reader_dark_ : !reader_dark_ && reader_background_index_ == index;
            reader_button(panel, kReaderBackgroundNames[index], 28 + index % columns * step,
                          216 + index / columns * 68, step - 12, 56, 1400 + index, selected);
        }
        const int timeout_y = reader_portrait_ ? 370 : 306;
        setting_label("自动休眠（与系统共用）", 28, timeout_y, panel_width - 56);
        for (int index = 0; index < 6; ++index) {
            const std::string label = std::to_string(kReaderScreenTimeouts[index]) + " 分钟";
            reader_button(panel, label.c_str(), 28 + index % columns * step,
                          timeout_y + 48 + index / columns * 68, step - 12, 56, 1410 + index,
                          auto_lock_minutes_ == kReaderScreenTimeouts[index]);
        }
        setting_label("亮度松手生效，设置自动保存", 28, panel_height - 58, panel_width - 230);
        reader_button(panel, "立即休眠", panel_width - 180, panel_height - 74, 152, 56, 41);
        UpdateSettingLabels();
    }

    if (reader_font_picker_open_) {
        auto dim = Box(paper, 0, 0, reader_width, reader_height, 0x172033);
        lv_obj_set_style_bg_opa(dim, LV_OPA_70, 0);
        lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
        const int panel_x = reader_portrait_ ? 24 : 80;
        const int panel_y = reader_portrait_ ? 32 : 40;
        const int panel_width = reader_width - 2 * panel_x;
        const int panel_height = reader_height - 2 * panel_y;
        auto panel = Box(dim, panel_x, panel_y, panel_width, panel_height,
                         reader_dark_ ? 0x253247 : 0xfffdf7);
        lv_obj_set_style_bg_color(panel, lv_color_hex(reader_dark_ ? 0x253247 : 0xfffdf7), 0);
        lv_obj_set_style_radius(panel, 24, 0);
        lv_obj_set_style_border_width(panel, 2, 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(reader_dark_ ? 0x50647e : 0xe2d7c8), 0);
        lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
        auto title = Label(panel, "选择阅读字体", 28, 20, panel_width - 210, reader_control_font);
        lv_obj_set_style_text_color(title, lv_color_hex(page_text), 0);
        reader_button(panel, "关闭", panel_width - 148, 14, 120, 58, 1379);

        const size_t choice_count = std::min<size_t>(reader_fonts_.size() + 1, 17);
        const int columns = reader_portrait_ ? 2 : 3;
        const int button_width = reader_portrait_ ? 300 : 336;
        const int column_step = reader_portrait_ ? 316 : 354;
        const int row_step = reader_portrait_ ? 82 : 68;
        for (size_t choice = 0; choice < choice_count; ++choice) {
            const int font_index = static_cast<int>(choice) - 1;
            const bool selected = reader_font_index_ == font_index;
            std::string name =
                choice == 0 ? "内置字体" : ReaderFontDisplayName(reader_fonts_[choice - 1]);
            if (selected)
                name = "● " + name;
            const int column = static_cast<int>(choice % columns);
            const int row = static_cast<int>(choice / columns);
            auto button = reader_button(
                panel, name.c_str(), 24 + column * column_step, 92 + row * row_step, button_width,
                62, choice == 0 ? 1360 : 1360 + static_cast<int>(choice), selected);
            auto label = lv_obj_get_child(button, 0);
            if (label) {
                lv_obj_set_width(label, button_width - 24);
                lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            }
        }
        auto hint = Label(panel, "把 .ttf 文件复制到 /handict/fonts，重新扫描后即可选择", 24,
                          panel_height - 62, panel_width - 48, reader_control_font);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(reader_dark_ ? 0xb9c7da : kMuted), 0);
    }
}
