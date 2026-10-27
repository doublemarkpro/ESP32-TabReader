#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "reader/reader_display.h"

namespace {
std::vector<uint32_t> pixels(1280 * 1280);
std::vector<uint8_t> buffer(1280 * 1280 * 4);
void Require(bool value, const char* description) {
    if (!value)
        throw std::runtime_error(description);
}
void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* colors) {
    const int width = lv_display_get_horizontal_resolution(display);
    auto source = reinterpret_cast<uint32_t*>(colors);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            pixels[y * width + x] = *source++;
    lv_display_flush_ready(display);
}
void Shot(const std::string& name) {
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(nullptr);
    const int width = lv_display_get_horizontal_resolution(nullptr);
    const int height = lv_display_get_vertical_resolution(nullptr);
    FILE* file = fopen(name.c_str(), "wb");
    if (!file)
        throw std::runtime_error("screenshot file");
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        uint8_t rgb[] = {static_cast<uint8_t>(pixels[i] >> 16),
                         static_cast<uint8_t>(pixels[i] >> 8), static_cast<uint8_t>(pixels[i])};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
lv_obj_t* Find(lv_obj_t* parent, int action) {
    for (uint32_t i = 0; i < lv_obj_get_event_count(parent); ++i) {
        auto descriptor = lv_obj_get_event_dsc(parent, i);
        if (reinterpret_cast<intptr_t>(lv_event_dsc_get_user_data(descriptor)) == action)
            return parent;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i)
        if (auto found = Find(lv_obj_get_child(parent, i), action))
            return found;
    return nullptr;
}
void Click(int action) {
    auto button = Find(lv_screen_active(), action);
    Require(button != nullptr, "action button absent");
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
}
}  // namespace
struct ReaderUiSimulation {
    static void Run(const std::string& directory) {
        ReaderDisplay display(nullptr, nullptr);
        display.SetupUI();
        display.reader_loading_ = false;
        han::reader::EpubBook book;
        book.title = "中文书名：阅读测试";
        book.path = "/sdcard/handict/books/test.epub";
        book.spine = {"first.xhtml", "second.xhtml"};
        for (int i = 0; i < 7; ++i)
            display.reader_books_.push_back(book);
        display.reader_book_progress_.assign(7, 375);
        display.Render(ReaderDisplay::Page::Shelf);
        Shot(directory + "/shelf-portrait.ppm");
        Click(31);
        Require(display.shelf_page_ == 1, "shelf pagination");
        Click(30);
        Click(12);
        Shot(directory + "/settings-portrait.ppm");
        Click(1405);
        Require(display.reader_dark_, "night selection");
        Shot(directory + "/settings-night.ppm");
        Click(1345);
        Require(!display.reader_portrait_, "landscape toggle");
        Shot(directory + "/settings-landscape.ppm");
        Click(11);
        Shot(directory + "/transfer-landscape.ppm");
        Click(10);
        Shot(directory + "/shelf-landscape.ppm");
        display.reader_selected_book_ = 0;
        display.reader_book_ = book;
        display.reader_open_ = true;
        for (int i = 0; i < 90; ++i)
            display.reader_text_ += "　　中文阅读测试，首行缩进，点击左侧上一页，右侧下一页。\n";
        display.reader_spine_index_ = 0;
        display.BuildReaderPages();
        display.Render(ReaderDisplay::Page::Reader);
        Require(!Find(lv_screen_active(), 1332), "reading controls initially hidden");
        Click(1382);
        Require(display.reader_page_index_ == 1, "right tap next page");
        Click(1380);
        Require(display.reader_page_index_ == 0, "left tap previous page");
        Click(1381);
        Require(Find(lv_screen_active(), 1332), "center reveals shelf button");
        Shot(directory + "/reader-landscape-menu.ppm");
        Click(1390);
        Shot(directory + "/reader-screen-menu.ppm");
        Click(1391);
        Click(1345);
        Shot(directory + "/reader-portrait-menu.ppm");
        Click(1381);
        Shot(directory + "/reader-portrait.ppm");
        display.SaveReaderProgress(true);
        size_t spine, offset;
        uint16_t progress;
        display.LoadReaderProgress(book.path, spine, offset, progress);
        Require(spine == 0 && offset == 0, "saved progress loads");
        Click(1381);
        Click(1332);
        Require(display.page_ == ReaderDisplay::Page::Shelf, "back to shelf");
        display.lan_setup_ = display.importing_ = true;
        display.pending_ssid_ = "Home-test";
        display.Render(ReaderDisplay::Page::Transfer);
        Require(display.wifi_name_ && display.wifi_password_ && display.wifi_keyboard_,
                "LAN inputs");
        Click(1501);
        Require(std::string(lv_textarea_get_text(display.wifi_name_)) == "Reader-test",
                "select scanned Wi-Fi");
        lv_textarea_set_text(display.wifi_password_, "testpassword");
        Shot(directory + "/transfer-lan-portrait.ppm");
        Click(65);
        Require(
            display.pending_ssid_ == "Reader-test" && display.pending_password_ == "testpassword",
            "connect credentials");
        display.reader_loading_ = false;
        display.reader_portrait_ = false;
        display.Render(ReaderDisplay::Page::Transfer);
        Shot(directory + "/transfer-lan-landscape.ppm");
        display.importing_ = display.lan_setup_ = false;
        display.RequestSleep();
        Require(display.asleep_, "sleep suspends interaction");
        std::cout << "Dedicated reader UI checks passed\n";
    }
};
int main(int argc, char** argv) {
    try {
        std::string directory = argc > 1 ? argv[1] : "reader-shots";
        std::filesystem::create_directories(directory);
        lv_init();
        auto display = lv_display_create(720, 1280);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);
        lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size(),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, Flush);
        ReaderUiSimulation::Run(directory);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
