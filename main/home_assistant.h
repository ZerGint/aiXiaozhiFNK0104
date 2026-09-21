#ifndef HOME_ASSISTANT_H
#define HOME_ASSISTANT_H

#include <string>
#include <vector>
#include <mutex>
#include <esp_http_client.h>
#include <esp_log.h>
#include "settings.h"
#include "mcp_server.h"

class HomeAssistant {
public:
    struct ConfigSnapshot {
        std::string url;
        std::string token;
    };

    static HomeAssistant& GetInstance() {
        static HomeAssistant instance;
        return instance;
    }

    void Initialize();
    void RegisterMcpTools();

    std::string CallService(const std::string& domain, const std::string& service, const std::string& entity_id = "", const std::string& data_json = "");
    std::string GetStates(const std::string& entity_id = "");
    std::string TestConnection();
    bool TestConnection(const std::string& url, const std::string& token);

    void SetConfig(const std::string& url, const std::string& token);
    ConfigSnapshot GetConfigSnapshot() const;
    std::string GetUrl() const;
    bool IsConfigured() const;

private:
    HomeAssistant();
    ~HomeAssistant() = default;

    std::string url_;
    std::string token_;
    mutable std::mutex config_mutex_;

    std::string PerformHttpRequest(esp_http_client_method_t method, const std::string& path, const std::string& post_data = "");
    std::string PerformHttpRequest(esp_http_client_method_t method, const std::string& path,
                                   const std::string& post_data, const ConfigSnapshot& config);
    bool TestConnectionForConfig(const ConfigSnapshot& config);
};

#endif // HOME_ASSISTANT_H
