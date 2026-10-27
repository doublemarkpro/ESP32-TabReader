#include "transfer_server.h"
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_event.h>
#include <esp_hosted.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <esp_wifi.h>
#include <ft2build.h>
#include <ssid_manager.h>
#include "file_transfer_policy.h"
#include "han_dictionary/reader/epub_reader.h"
#include "reader_application.h"
#include FT_FREETYPE_H
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {
constexpr size_t kBufferSize = 16 * 1024;
struct BusyGuard {
    std::atomic<bool>& flag;
    ~BusyGuard() { flag = false; }
};
esp_err_t Reply(httpd_req_t* req, const char* status, const char* message) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, message);
}
std::string Header(httpd_req_t* req, const char* key) {
    const size_t length = httpd_req_get_hdr_value_len(req, key);
    if (!length || length > 1024)
        return {};
    std::string result(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, key, result.data(), result.size()) != ESP_OK)
        return {};
    result.resize(length);
    return result;
}
int Receive(httpd_req_t* req, char* buffer, size_t size) {
    for (int retry = 0; retry < 3; ++retry) {
        if (!ReaderTransferServer::GetInstance().Active())
            return -1;
        int count = httpd_req_recv(req, buffer, size);
        if (count != HTTPD_SOCK_ERR_TIMEOUT)
            return count;
    }
    return -1;
}
bool ValidFont(const std::string& path) {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    if (FT_Init_FreeType(&library))
        return false;
    const bool valid = FT_New_Face(library, path.c_str(), 0, &face) == 0;
    if (face)
        FT_Done_Face(face);
    FT_Done_FreeType(library);
    return valid;
}
constexpr char kPage[] = R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Tab5 Reader 传书</title>
<style>body{font:16px system-ui;background:#f7f5ed;color:#252720;max-width:650px;margin:32px auto;padding:20px}h1{font-size:28px}section{background:#fffdf7;padding:24px;border:1px solid #d4d4c8;border-radius:14px;margin:18px 0}button,input{font:inherit;padding:12px;border:1px solid #aaa;border-radius:8px;max-width:100%;box-sizing:border-box}button{cursor:pointer;background:#e8eddf}input{width:100%;margin:8px 0}progress{width:100%}#message{white-space:pre-wrap}small{color:#666}</style>
<h1>Tab5 Reader</h1><p>把书放进设备，接着读。</p>
<section><label>设备屏幕上的网页连接码<input id="token" autocomplete="off"></label>
<p>选择 EPUB 图书或 TTF 字体，逐个上传。已有同名文件不会被覆盖。</p>
<input id="files" type="file" accept=".epub,.ttf" multiple><button id="send">上传到设备</button>
<p><progress id="progress" value="0" max="100"></progress></p><p id="message" aria-live="polite"></p></section>
<section><details><summary>本地固件升级</summary><p>仅接受本项目 Tab5 Reader 的应用 BIN。不要选择整机合并镜像。</p>
<input id="firmware" type="file" accept=".bin"><button id="upgrade">写入固件</button>
<button id="reboot" hidden>重启设备</button></details></section>
<small>传完后，在设备上点“停止传书”，书架和字体会重新扫描。局域网模式请连接同一网络；热点模式请保持设备热点连接。</small>
<script>
const q=document.querySelector.bind(document);q('#token').value=new URLSearchParams(location.search).get('token')||'';
let active=false;function upload(file,url){return new Promise((resolve,reject)=>{const x=new XMLHttpRequest();x.open('POST',url);x.setRequestHeader('X-Reader-Token',q('#token').value.trim());x.setRequestHeader('X-Filename',encodeURIComponent(file.name));x.timeout=300000;x.upload.onprogress=e=>{if(e.lengthComputable)q('#progress').value=e.loaded*100/e.total};x.onload=()=>x.status===200?resolve(x.responseText):reject(new Error(x.responseText||('HTTP '+x.status)));x.onerror=()=>reject(new Error('连接中断，请保持同一 Wi-Fi 网络连接后重试'));x.ontimeout=()=>reject(new Error('上传超时'));x.send(file)})}
async function run(firmware){if(active)return;const files=Array.from(q(firmware?'#firmware':'#files').files);if(!files.length)return;q('#send').disabled=q('#upgrade').disabled=true;active=true;try{for(const file of files){const font=/\.ttf$/i.test(file.name);if(!firmware&&!font&&!/\.epub$/i.test(file.name))throw new Error('仅支持 EPUB 或 TTF');q('#message').textContent='正在上传：'+file.name;const text=await upload(file,firmware?'/api/firmware':'/api/upload?type='+(font?'font':'book'));q('#message').textContent=file.name+'：'+text}if(firmware)q('#reboot').hidden=false}catch(e){q('#message').textContent=e.message}finally{active=false;q('#send').disabled=q('#upgrade').disabled=false}}
q('#send').onclick=()=>run(false);q('#upgrade').onclick=()=>run(true);q('#reboot').onclick=async()=>{await fetch('/api/reboot',{method:'POST',headers:{'X-Reader-Token':q('#token').value.trim()}});q('#message').textContent='设备正在重启'};
</script></html>)HTML";
}  // namespace

bool ReaderTransferServer::ValidFilename(const std::string& name, bool font) {
    return reader::ValidUploadName(name, font);
}
bool ReaderTransferServer::Authorized(httpd_req_t* req) const {
    return Active() && !token_.empty() && Header(req, "X-Reader-Token") == token_;
}
void ReaderTransferServer::NewToken() {
    char random[17];
    snprintf(random, sizeof(random), "%08lx%08lx", esp_random(), esp_random());
    token_ = std::string(random, 8);
    password_ = std::string(random + 4, 12);
    ssid_ = "Tab5Reader-" + std::string(random + 12, 4);
}
std::string ReaderTransferServer::InitializeWifi() {
    if (!wifi_initialized_) {
        auto error = esp_netif_init();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
            return esp_err_to_name(error);
        error = esp_event_loop_create_default();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
            return esp_err_to_name(error);
        if (radio_power_)
            radio_power_(true);
        if (!hosted_initialized_) {
            error = esp_hosted_init();
            if (error != ESP_OK) {
                if (radio_power_)
                    radio_power_(false);
                return "无法启动无线通信";
            }
        }
        ap_netif_ = esp_netif_create_default_wifi_ap();
        sta_netif_ = esp_netif_create_default_wifi_sta();
        if (!ap_netif_ || !sta_netif_) {
            Stop();
            return "无法创建 Wi-Fi 接口";
        }
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        error = esp_wifi_init(&config);
        if (error != ESP_OK) {
            Stop();
            return std::string("Wi-Fi 初始化失败：") + esp_err_to_name(error);
        }
        wifi_initialized_ = true;
        hosted_initialized_ = true;
    }
    return {};
}
std::string ReaderTransferServer::Start() {
    if (Active())
        return {};
    NewToken();
    lan_ = false;
    address_ = "192.168.4.1";
    auto init_error = InitializeWifi();
    if (!init_error.empty())
        return init_error;
    wifi_config_t config{};
    memcpy(config.ap.ssid, ssid_.data(), ssid_.size());
    config.ap.ssid_len = ssid_.size();
    memcpy(config.ap.password, password_.data(), password_.size());
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 2;
    config.ap.channel = 6;
    esp_err_t error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK)
        error = esp_wifi_set_mode(WIFI_MODE_AP);
    if (error == ESP_OK)
        error = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (error == ESP_OK)
        error = esp_wifi_start();
    if (error != ESP_OK) {
        Stop();
        return std::string("热点启动失败：") + esp_err_to_name(error);
    }
    return StartHttp();
}
std::string ReaderTransferServer::PreferredSsid() const {
    const auto& saved = SsidManager::GetInstance().GetSsidList();
    return saved.empty() ? std::string{} : saved.front().ssid;
}
std::string ReaderTransferServer::PrepareLan() {
    lan_ = true;
    networks_.clear();
    auto error = InitializeWifi();
    if (!error.empty())
        return error;
    auto result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK)
        result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK)
        result = esp_wifi_start();
    if (result != ESP_OK) {
        Stop();
        return std::string("Wi-Fi 启动失败：") + esp_err_to_name(result);
    }
    wifi_scan_config_t scan{};
    result = esp_wifi_scan_start(&scan, true);
    if (result != ESP_OK)
        return "扫描失败，可手动输入 Wi-Fi 名称";
    std::array<wifi_ap_record_t, 16> records{};
    uint16_t count = records.size();
    if (esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK)
        return "无法读取 Wi-Fi 列表";
    for (size_t i = 0; i < count; ++i) {
        std::string name(reinterpret_cast<const char*>(records[i].ssid),
                         strnlen(reinterpret_cast<const char*>(records[i].ssid), 32));
        if (!name.empty() &&
            std::none_of(networks_.begin(), networks_.end(),
                         [&](const auto& network) { return network.ssid == name; }))
            networks_.push_back(
                {std::move(name), records[i].rssi, records[i].authmode != WIFI_AUTH_OPEN});
    }
    return {};
}
void ReaderTransferServer::WifiEvent(void* context, esp_event_base_t base, int32_t id, void* data) {
    auto self = static_cast<ReaderTransferServer*>(context);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto event = static_cast<ip_event_got_ip_t*>(data);
        self->received_ip_ = event->ip_info.ip.addr;
        xEventGroupSetBits(self->connection_events_, BIT0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(self->connection_events_, BIT1);
    }
}
std::string ReaderTransferServer::StartLan(const std::string& name,
                                           const std::string& supplied_password) {
    if (name.empty() || name.size() > 32 || supplied_password.size() > 64)
        return "Wi-Fi 名称或密码长度不正确";
    auto password = supplied_password;
    if (password.empty()) {
        for (const auto& saved : SsidManager::GetInstance().GetSsidList())
            if (saved.ssid == name) {
                password = saved.password;
                break;
            }
    }
    if (!password.empty() && password.size() < 8)
        return "Wi-Fi 密码至少需要 8 个字符";
    auto error = wifi_initialized_ ? std::string{} : PrepareLan();
    if (!error.empty())
        return error;
    lan_ = true;
    NewToken();
    ssid_ = name;
    address_.clear();
    if (!connection_events_)
        connection_events_ = xEventGroupCreate();
    if (!connection_events_)
        return "连接资源不足";
    esp_wifi_disconnect();
    // Drain a previous attempt's disconnect event before waiting on this connection.
    vTaskDelay(pdMS_TO_TICKS(150));
    xEventGroupClearBits(connection_events_, BIT0 | BIT1);
    esp_err_t result = ESP_OK;
    if (!ip_handler_)
        result = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, WifiEvent, this,
                                                     &ip_handler_);
    if (result == ESP_OK && !disconnect_handler_)
        result = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                     WifiEvent, this, &disconnect_handler_);
    wifi_config_t config{};
    memcpy(config.sta.ssid, name.data(), name.size());
    memcpy(config.sta.password, password.data(), password.size());
    config.sta.pmf_cfg.capable = true;
    if (result == ESP_OK)
        result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result == ESP_OK)
        result = esp_wifi_connect();
    if (result != ESP_OK)
        return std::string("连接失败：") + esp_err_to_name(result);
    auto bits =
        xEventGroupWaitBits(connection_events_, BIT0 | BIT1, pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
    if (!(bits & BIT0)) {
        esp_wifi_disconnect();
        return "连接失败或超时，请检查 Wi-Fi 密码与 2.4GHz 网络";
    }
    SsidManager::GetInstance().AddSsid(name, password);
    esp_ip4_addr_t ip{.addr = received_ip_.load()};
    char address[16];
    snprintf(address, sizeof(address), IPSTR, IP2STR(&ip));
    address_ = address;
    return StartHttp();
}
std::string ReaderTransferServer::StartHttp() {
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 12 * 1024;
    http.max_open_sockets = 3;
    http.recv_wait_timeout = 5;
    http.send_wait_timeout = 5;
    http.lru_purge_enable = true;
    auto error = httpd_start(&server_, &http);
    if (error != ESP_OK) {
        Stop();
        return "无法启动传书网页";
    }
    const httpd_uri_t root{.uri = "/", .method = HTTP_GET, .handler = Root, .user_ctx = this};
    const httpd_uri_t upload{
        .uri = "/api/upload", .method = HTTP_POST, .handler = Upload, .user_ctx = this};
    const httpd_uri_t firmware{
        .uri = "/api/firmware", .method = HTTP_POST, .handler = Firmware, .user_ctx = this};
    const httpd_uri_t reboot{.uri = "/api/reboot",
                             .method = HTTP_POST,
                             .handler = [](httpd_req_t* req) -> esp_err_t {
                                 auto& self = GetInstance();
                                 if (!self.Authorized(req))
                                     return Reply(req, "403 Forbidden", "连接码不正确");
                                 if (self.Busy())
                                     return Reply(req, "409 Conflict", "请等待传输完成");
                                 Reply(req, "200 OK", "正在重启");
                                 esp_timer_handle_t timer = nullptr;
                                 esp_timer_create_args_t args{};
                                 args.callback = [](void*) { esp_restart(); };
                                 args.name = "reader_reboot";
                                 if (esp_timer_create(&args, &timer) == ESP_OK)
                                     esp_timer_start_once(timer, 200000);
                                 return ESP_OK;
                             },
                             .user_ctx = this};
    for (const auto* route : {&root, &upload, &firmware, &reboot}) {
        if (httpd_register_uri_handler(server_, route) != ESP_OK) {
            Stop();
            return "网页接口注册失败";
        }
    }
    active_ = true;
    ESP_LOGI("ReaderTransfer", "Foreground %s session ready at %s; Wi-Fi SSID=%s",
             lan_ ? "LAN" : "AP", address_.c_str(), ssid_.c_str());
    return {};
}
void ReaderTransferServer::Stop() {
    active_ = false;
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    if (ip_handler_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_);
        ip_handler_ = nullptr;
    }
    if (disconnect_handler_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              disconnect_handler_);
        disconnect_handler_ = nullptr;
    }
    if (wifi_initialized_) {
        esp_wifi_stop();
        esp_wifi_deinit();
        wifi_initialized_ = false;
    }
    if (hosted_initialized_) {
        esp_hosted_deinit();
        hosted_initialized_ = false;
    }
    if (ap_netif_) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }
    if (sta_netif_) {
        esp_netif_destroy_default_wifi(sta_netif_);
        sta_netif_ = nullptr;
    }
    if (connection_events_) {
        vEventGroupDelete(connection_events_);
        connection_events_ = nullptr;
    }
    if (radio_power_)
        radio_power_(false);
    busy_ = false;
    ESP_LOGI("ReaderTransfer", "Import session closed; Wi-Fi radio stopped");
}
esp_err_t ReaderTransferServer::Root(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kPage, sizeof(kPage) - 1);
}
esp_err_t ReaderTransferServer::Upload(httpd_req_t* req) {
    auto& self = GetInstance();
    if (!self.Authorized(req))
        return Reply(req, "403 Forbidden", "连接码不正确");
    if (self.busy_.exchange(true))
        return Reply(req, "409 Conflict", "正在处理其他文件");
    BusyGuard busy{self.busy_};
    char query[64]{};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    const bool font = std::string(query) == "type=font";
    const auto name = reader::DecodeUploadName(Header(req, "X-Filename"));
    if (!ValidFilename(name, font))
        return Reply(req, "400 Bad Request", "文件名或类型不正确");
    if (req->content_len == 0 || req->content_len > (font ? 64U : 128U) * 1024 * 1024)
        return Reply(req, "413 Content Too Large", "文件大小超出限制");
    const std::string directory = font ? "/sdcard/handict/fonts" : "/sdcard/handict/books";
    mkdir("/sdcard/handict", 0755);
    mkdir(directory.c_str(), 0755);
    const auto target = directory + "/" + name;
    struct stat existing{};
    if (stat(target.c_str(), &existing) == 0)
        return Reply(req, "409 Conflict", "同名文件已存在，请先改名");
    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes) == ESP_OK &&
        free_bytes < req->content_len + 65536ULL)
        return Reply(req, "507 Insufficient Storage", "TF 卡剩余空间不足");
    const auto temporary = target + ".part-" + self.token_;
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return Reply(req, "500 Internal Server Error", "无法创建临时文件，请检查 TF 卡");
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(temporary.c_str());
        return Reply(req, "500 Internal Server Error", "无法打开文件");
    }
    auto buffer = std::make_unique<char[]>(kBufferSize);
    size_t remaining = req->content_len;
    bool valid = true;
    while (remaining && valid) {
        int count = Receive(req, buffer.get(), std::min(remaining, kBufferSize));
        if (count <= 0 || fwrite(buffer.get(), 1, count, file) != static_cast<size_t>(count))
            valid = false;
        else
            remaining -= count;
    }
    if (fflush(file) != 0 || ferror(file))
        valid = false;
    if (fclose(file) != 0)
        valid = false;
    std::string error;
    if (valid && font)
        valid = ValidFont(temporary);
    else if (valid) {
        han::reader::EpubBook book;
        valid = han::reader::EpubReader::ParsePackage(temporary, book);
        error = book.error;
    }
    if (valid && self.Active())
        valid = rename(temporary.c_str(), target.c_str()) == 0;
    else
        valid = false;
    if (!valid) {
        unlink(temporary.c_str());
        return Reply(req, "400 Bad Request",
                     error.empty() ? "上传中断、文件无效或写卡失败" : error.c_str());
    }
    ESP_LOGI("ReaderTransfer", "Imported %u bytes: %s", static_cast<unsigned>(req->content_len),
             name.c_str());
    return Reply(req, "200 OK", "上传完成；退出传书后书架会自动刷新");
}
esp_err_t ReaderTransferServer::Firmware(httpd_req_t* req) {
    auto& self = GetInstance();
    if (!self.Authorized(req))
        return Reply(req, "403 Forbidden", "连接码不正确");
    if (self.busy_.exchange(true))
        return Reply(req, "409 Conflict", "正在处理其他文件");
    BusyGuard busy{self.busy_};
    const auto partition = esp_ota_get_next_update_partition(nullptr);
    constexpr size_t metadata_size =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (!partition || req->content_len < metadata_size || req->content_len > partition->size)
        return Reply(req, "413 Content Too Large", "固件大小不正确");
    std::array<char, metadata_size> header{};
    size_t have = 0;
    while (have < header.size()) {
        int count = Receive(req, header.data() + have, header.size() - have);
        if (count <= 0)
            return Reply(req, "400 Bad Request", "无法读取固件头");
        have += count;
    }
    esp_app_desc_t description{};
    memcpy(&description,
           header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           sizeof(description));
    if (static_cast<uint8_t>(header[0]) != ESP_IMAGE_HEADER_MAGIC ||
        description.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(description.project_name, "tab5_reader", sizeof(description.project_name)) != 0)
        return Reply(req, "400 Bad Request", "不是 Tab5 Reader 的应用固件，请勿上传合并镜像");
    esp_ota_handle_t handle = 0;
    auto error = esp_ota_begin(partition, req->content_len, &handle);
    if (error != ESP_OK)
        return Reply(req, "500 Internal Server Error", esp_err_to_name(error));
    Application::GetInstance().Schedule(
        [] { Application::GetInstance().SetDeviceState(kDeviceStateUpgrading); });
    error = esp_ota_write(handle, header.data(), header.size());
    auto buffer = std::make_unique<char[]>(kBufferSize);
    size_t remaining = req->content_len - header.size();
    while (remaining && error == ESP_OK) {
        int count = Receive(req, buffer.get(), std::min(remaining, kBufferSize));
        if (count <= 0) {
            error = ESP_FAIL;
            break;
        }
        error = esp_ota_write(handle, buffer.get(), count);
        remaining -= count;
    }
    if (error == ESP_OK)
        error = esp_ota_end(handle);
    else
        esp_ota_abort(handle);
    if (error == ESP_OK)
        error = esp_ota_set_boot_partition(partition);
    Application::GetInstance().Schedule(
        [] { Application::GetInstance().SetDeviceState(kDeviceStateIdle); });
    return Reply(req, error == ESP_OK ? "200 OK" : "500 Internal Server Error",
                 error == ESP_OK ? "固件已验证并写入，点击重启完成更新" : esp_err_to_name(error));
}
