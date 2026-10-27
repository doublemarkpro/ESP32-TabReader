#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <functional>
#include "device_state_machine.h"

// Bounded offline event loop, without assistant, audio or activation objects.
class Application {
public:
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    void Initialize();
    void Run();
    bool Schedule(std::function<void()> task);
    bool SetDeviceState(DeviceState state) { return states_.TransitionTo(state); }
    DeviceState GetDeviceState() const { return states_.GetState(); }

private:
    Application();
    QueueHandle_t tasks_ = nullptr;
    DeviceStateMachine states_;
};
