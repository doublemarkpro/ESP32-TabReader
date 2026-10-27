#pragma once
// Hardware doubles for the dedicated reader's real LVGL pages.
#include <lvgl.h>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
using esp_lcd_panel_io_handle_t = void*;
using esp_lcd_panel_handle_t = void*;
using esp_lcd_touch_handle_t = void*;
using QueueHandle_t = void*;
constexpr int pdTRUE = 1, pdPASS = 1, portMAX_DELAY = 0, ESP_OK = 0, ESP_ERR_NO_MEM = -2;
constexpr int MALLOC_CAP_SPIRAM = 1;
#define pdMS_TO_TICKS(x) (x)
#define ESP_ERROR_CHECK(x)                               \
    do {                                                 \
        if ((x) != 0)                                    \
            throw std::runtime_error("ESP stub failed"); \
    } while (0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
inline int64_t esp_timer_get_time() { return lv_tick_get() * 1000LL; }
inline unsigned heap_caps_get_free_size(int) { return 16 * 1024 * 1024; }
inline QueueHandle_t xQueueCreate(int, int) { return reinterpret_cast<void*>(1); }
inline int xQueueSend(QueueHandle_t, const void*, int) { return 1; }
inline int xQueueReceive(QueueHandle_t, void*, int) { return 0; }
inline int xTaskCreate(void (*)(void*), const char*, int, void*, int, void*) { return 1; }
inline void esp_restart() { throw std::runtime_error("simulated reboot"); }
struct Display {
    int width_ = 720, height_ = 1280;
    bool setup_ui_called_ = false;
    virtual ~Display() = default;
    virtual void SetupUI() {}
    virtual bool Lock(int = 0) = 0;
    virtual void Unlock() = 0;
};
struct DisplayLockGuard {
    explicit DisplayLockGuard(Display*) {}
};
struct Backlight {
    int value = 60;
    void SetBrightness(int level, bool = false) { value = level; }
};
struct Board {
    static Board& GetInstance() {
        static Board board;
        return board;
    }
    Backlight* GetBacklight() { return &backlight; }
    Backlight backlight;
};
struct Application {
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    bool Schedule(std::function<void()> task) {
        task();
        return true;
    }
};
struct Settings {
    inline static std::map<std::string, std::string> values;
    std::string prefix;
    explicit Settings(const std::string& ns, bool = false) : prefix(ns + "/") {}
    std::string GetString(const std::string& key, const std::string& fallback = "") {
        auto found = values.find(prefix + key);
        return found == values.end() ? fallback : found->second;
    }
    int GetInt(const std::string& key, int fallback = 0) {
        auto value = GetString(key);
        return value.empty() ? fallback : std::stoi(value);
    }
    bool GetBool(const std::string& key, bool fallback = false) {
        return GetInt(key, fallback) != 0;
    }
    void SetString(const std::string& key, const std::string& value) {
        values[prefix + key] = value;
    }
    void SetInt(const std::string& key, int value) { SetString(key, std::to_string(value)); }
    void SetBool(const std::string& key, bool value) { SetInt(key, value); }
};
struct ReaderTransferServer {
    struct Network {
        std::string ssid;
        int rssi;
        bool secured;
    };
    const std::vector<Network>& Networks() const {
        static const std::vector<Network> networks{{"Home-test", -40, true},
                                                   {"Reader-test", -60, true}};
        return networks;
    }
    bool Busy() const { return false; }
    bool IsLan() const { return true; }
    const std::string& Address() const {
        static const std::string value = "192.168.1.20";
        return value;
    }
    std::string PreferredSsid() const { return "Home-test"; }
    std::string PrepareLan() { return {}; }
    std::string StartLan(const std::string&, const std::string&) {
        active = true;
        return {};
    }
    bool active = false;
    static ReaderTransferServer& GetInstance() {
        static ReaderTransferServer server;
        return server;
    }
    bool Active() const { return active; }
    std::string Start() {
        active = true;
        return {};
    }
    void Stop() { active = false; }
    const std::string& Ssid() const {
        static const std::string value = "Tab5Reader-test";
        return value;
    }
    const std::string& Password() const {
        static const std::string value = "abcdef123456";
        return value;
    }
    const std::string& Token() const {
        static const std::string value = "abcd1234";
        return value;
    }
};
