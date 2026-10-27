#include "reader_application.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <memory>
#include "board.h"
#include "reader_display.h"

Application::Application() { tasks_ = xQueueCreate(12, sizeof(std::function<void()>*)); }
bool Application::Schedule(std::function<void()> task) {
    auto owned = new (std::nothrow) std::function<void()>(std::move(task));
    if (!owned)
        return false;
    if (xQueueSend(tasks_, &owned, 0) != pdTRUE) {
        delete owned;
        return false;
    }
    return true;
}
void Application::Initialize() {
    ESP_ERROR_CHECK(tasks_ ? ESP_OK : ESP_ERR_NO_MEM);
    SetDeviceState(kDeviceStateStarting);
    Board::GetInstance().GetDisplay()->SetupUI();
    SetDeviceState(kDeviceStateIdle);
    ESP_LOGI("ReaderApp",
             "Offline reader ready: PSRAM=%u; audio=off camera=off cloud=off Wi-Fi=off",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
void Application::Run() {
    for (;;) {
        std::function<void()>* task = nullptr;
        if (xQueueReceive(tasks_, &task, portMAX_DELAY) == pdTRUE) {
            std::unique_ptr<std::function<void()>> owned(task);
            (*owned)();
        }
    }
}
