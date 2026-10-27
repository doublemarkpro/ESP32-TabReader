#pragma once
#include <esp_event.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

// Explicit, foreground-only import session. The UI suspends all card clients before Start().
class ReaderTransferServer {
public:
    struct Network {
        std::string ssid;
        int rssi;
        bool secured;
    };
    static ReaderTransferServer& GetInstance() {
        static ReaderTransferServer server;
        return server;
    }
    std::string Start();
    std::string PrepareLan();
    std::string StartLan(const std::string& ssid, const std::string& password);
    const std::vector<Network>& Networks() const { return networks_; }
    std::string PreferredSsid() const;
    bool IsLan() const { return lan_; }
    const std::string& Address() const { return address_; }
    void SetRadioPowerAction(std::function<void(bool)> action) { radio_power_ = std::move(action); }
    void Stop();
    bool Active() const { return active_.load(); }
    const std::string& Ssid() const { return ssid_; }
    const std::string& Password() const { return password_; }
    const std::string& Token() const { return token_; }
    std::string Url() const { return "http://" + address_ + "/?token=" + token_; }
    bool Busy() const { return busy_.load(); }
    static bool ValidFilename(const std::string& name, bool font);

private:
    static esp_err_t Root(httpd_req_t* request);
    static esp_err_t Upload(httpd_req_t* request);
    static esp_err_t Firmware(httpd_req_t* request);
    bool Authorized(httpd_req_t* request) const;
    std::string InitializeWifi();
    std::string StartHttp();
    void NewToken();
    static void WifiEvent(void* context, esp_event_base_t base, int32_t id, void* data);
    httpd_handle_t server_ = nullptr;
    void* ap_netif_ = nullptr;
    void* sta_netif_ = nullptr;
    EventGroupHandle_t connection_events_ = nullptr;
    esp_event_handler_instance_t ip_handler_ = nullptr;
    esp_event_handler_instance_t disconnect_handler_ = nullptr;
    std::function<void(bool)> radio_power_;
    std::vector<Network> networks_;
    bool lan_ = false;
    std::string address_ = "192.168.4.1";
    std::atomic<uint32_t> received_ip_{0};
    bool wifi_initialized_ = false;
    // The constructor creates dormant transport resources. Only tear down a session
    // after it has been configured: the upstream pre-connect teardown is unsafe.
    bool hosted_initialized_ = false;
    std::atomic<bool> busy_{false};
    std::atomic<bool> active_{false};
    std::string ssid_, password_, token_;
};
