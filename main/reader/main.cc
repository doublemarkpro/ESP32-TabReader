#include <nvs_flash.h>
#include "reader_application.h"
extern "C" void app_main() {
    // Preserve existing reading positions and network settings on migration errors.
    ESP_ERROR_CHECK(nvs_flash_init());
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();
}
