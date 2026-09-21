#ifndef HOME_ASSISTANT_SETTINGS_SERVER_H
#define HOME_ASSISTANT_SETTINGS_SERVER_H

#include <esp_http_server.h>
#include <mutex>

class HomeAssistantSettingsServer {
public:
    static HomeAssistantSettingsServer& GetInstance();
    void Start();
    void Stop();
    bool IsRunning() const;

private:
    HomeAssistantSettingsServer() = default;
    ~HomeAssistantSettingsServer() = default;
    HomeAssistantSettingsServer(const HomeAssistantSettingsServer&) = delete;
    HomeAssistantSettingsServer& operator=(const HomeAssistantSettingsServer&) = delete;

    static esp_err_t HandleIndex(httpd_req_t* request);
    static esp_err_t HandleGetConfig(httpd_req_t* request);
    static esp_err_t HandleTest(httpd_req_t* request);
    static esp_err_t HandleSave(httpd_req_t* request);

    esp_err_t SendIndex(httpd_req_t* request);
    esp_err_t SendConfig(httpd_req_t* request);
    esp_err_t TestConfig(httpd_req_t* request);
    esp_err_t SaveConfig(httpd_req_t* request);

    mutable std::mutex mutex_;
    httpd_handle_t server_ = nullptr;
    bool starting_ = false;
    bool stop_requested_ = false;
};

#endif // HOME_ASSISTANT_SETTINGS_SERVER_H
