#include <esp_random.h>
#include <cstdio>
#include "board.h"
#include "display.h"
#include "settings.h"

// Minimal Board defaults linked only by the dedicated reader variant.
Board::Board() {
    Settings settings("board", true);
    uuid_ = settings.GetString("uuid");
    if (uuid_.empty()) {
        uuid_ = GenerateUuid();
        settings.SetString("uuid", uuid_);
    }
}
std::string Board::GenerateUuid() {
    char value[37];
    snprintf(value, sizeof(value), "%08lx-%04lx-4%03lx-8%03lx-%08lx%04lx", esp_random(),
             esp_random() & 0xffff, esp_random() & 0xfff, esp_random() & 0xfff, esp_random(),
             esp_random() & 0xffff);
    return value;
}
Led* Board::GetLed() {
    static NoLed led;
    return &led;
}
Display* Board::GetDisplay() { return nullptr; }
Camera* Board::GetCamera() { return nullptr; }
bool Board::GetTemperature(float&) { return false; }
bool Board::GetBatteryLevel(int&, bool&, bool&) { return false; }
bool Board::GetBatteryInfo(BatteryInfo& info) {
    info = {};
    return false;
}
std::string Board::GetSystemInfoJson() { return "{\"product\":\"tab5-reader\"}"; }
