#ifndef HOME_ASSISTANT_H
#define HOME_ASSISTANT_H

#include <string>
#include <vector>
#include <esp_http_client.h>
#include <esp_log.h>
#include "settings.h"
#include "mcp_server.h"

class HomeAssistant {
public:
    static HomeAssistant& GetInstance() {
        static HomeAssistant instance;
        return instance;
    }

    void Initialize();
    void RegisterMcpTools();

    std::string CallService(const std::string& domain, const std::string& service, const std::string& entity_id = "", const std::string& data_json = "");
    std::string GetStates(const std::string& entity_id = "");
    std::string TestConnection();

    void SetConfig(const std::string& url, const std::string& token);
    std::string GetUrl() const { return url_; }
    bool IsConfigured() const { return !url_.empty() && !token_.empty(); }

private:
    HomeAssistant();
    ~HomeAssistant() = default;

    std::string url_;
    std::string token_;

    std::string PerformHttpRequest(esp_http_client_method_t method, const std::string& path, const std::string& post_data = "");
};

#endif // HOME_ASSISTANT_H
