#include "home_assistant_settings_server.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <wifi_manager.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "home_assistant.h"

namespace {

constexpr char TAG[] = "HASettings";
constexpr size_t kMaxRequestBody = 2048;
constexpr size_t kMaxUrlLength = 256;
constexpr size_t kMaxTokenLength = 1024;

constexpr char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>FNK0104S · Home Assistant Settings</title>
<style>
:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#111827;color:#f3f4f6}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:1rem;box-sizing:border-box}main{width:min(100%,34rem);background:#1f2937;border:1px solid #374151;border-radius:1rem;padding:1.25rem;box-sizing:border-box;box-shadow:0 1rem 3rem #0004}h1{font-size:1.35rem;margin:0 0 .35rem}p{color:#9ca3af;margin:.35rem 0 1.2rem;font-size:.92rem}label{display:block;margin:.9rem 0 .35rem;font-size:.9rem;color:#d1d5db}input{width:100%;box-sizing:border-box;padding:.7rem .75rem;border-radius:.55rem;border:1px solid #4b5563;background:#111827;color:#f9fafb;font:inherit}.buttons{display:flex;gap:.65rem;margin-top:1.2rem;flex-wrap:wrap}button{border:0;border-radius:.55rem;padding:.7rem 1rem;background:#2563eb;color:#fff;font:inherit;cursor:pointer}button.secondary{background:#4b5563}button:disabled{opacity:.55;cursor:wait}#status{min-height:1.3rem;margin-top:1rem;font-size:.9rem;color:#93c5fd}small{display:block;color:#9ca3af;margin-top:.9rem}
</style></head><body><main>
<h1>FNK0104S</h1><p>Home Assistant Settings</p>
<form id="form"><label for="url">Home Assistant URL</label>
<input id="url" name="url" type="text" inputmode="url" placeholder="http://192.168.1.100:8123" autocomplete="url" required>
<label for="token">Long-Lived Access Token</label>
<input id="token" name="token" type="password" placeholder="Leave blank to keep the current token" autocomplete="new-password">
<div class="buttons"><button id="test" type="button" class="secondary">Test connection</button><button id="save" type="submit">Save</button></div>
</form><div id="status" role="status"></div><small>The token is never displayed or returned by this page.</small></main>
<script>
const url=document.getElementById('url'),token=document.getElementById('token'),status=document.getElementById('status'),test=document.getElementById('test'),save=document.getElementById('save');
const setStatus=(s,ok=false)=>{status.textContent=s;status.style.color=ok?'#86efac':'#93c5fd'};
async function load(){try{const r=await fetch('/api/ha',{cache:'no-store'});const d=await r.json();if(!r.ok)throw Error();url.value=d.url||'';setStatus(d.token_configured?'Token is configured.':'No token is configured.')}catch(e){setStatus('Could not load settings.')}}
async function send(path){const body={url:url.value.trim(),token:token.value};test.disabled=true;save.disabled=true;setStatus('Working…');try{const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const d=await r.json();if(!r.ok||!d.success)throw Error();setStatus(path.endsWith('/test')?'Connection successful.':'Settings saved.',true);if(path.endsWith('/save'))token.value=''}catch(e){setStatus(path.endsWith('/test')?'Connection failed.':'Could not save settings.')}finally{test.disabled=false;save.disabled=false}}
test.onclick=()=>send('/api/ha/test');document.getElementById('form').onsubmit=e=>{e.preventDefault();send('/api/ha/save')};load();
</script></body></html>)HTML";

void LogMemory(const char* stage) {
    ESP_LOGI(TAG,
             "HA_SETTINGS_MEM stage=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u min=%u",
             stage,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
}

bool ReadBody(httpd_req_t* request, std::string& body) {
    if (request->content_len <= 0 || request->content_len > static_cast<int>(kMaxRequestBody)) return false;
    body.resize(static_cast<size_t>(request->content_len));
    size_t received = 0;
    while (received < body.size()) {
        int count = httpd_req_recv(request, body.data() + received, body.size() - received);
        if (count <= 0) { body.clear(); return false; }
        received += static_cast<size_t>(count);
    }
    return true;
}

bool GetString(cJSON* root, const char* name, std::string& value, size_t max_length, bool required) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) return !required;
    if (!cJSON_IsString(item) || item->valuestring == nullptr || std::strlen(item->valuestring) > max_length) return false;
    value = item->valuestring;
    return true;
}

esp_err_t SendJson(httpd_req_t* request, const char* json, const char* status = "200 OK") {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, status);
    return httpd_resp_sendstr(request, json);
}

esp_err_t SendError(httpd_req_t* request, const char* status, const char* message) {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_status(request, status);
    return httpd_resp_sendstr(request, message);
}

} // namespace

HomeAssistantSettingsServer& HomeAssistantSettingsServer::GetInstance() {
    static HomeAssistantSettingsServer instance;
    return instance;
}

void HomeAssistantSettingsServer::Start() {
    if (!WifiManager::GetInstance().IsConnected()) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (server_ != nullptr || starting_) return;
        starting_ = true;
        stop_requested_ = false;
    }

    LogMemory("before_start");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.task_priority = 2;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;

    httpd_handle_t handle = nullptr;
    if (httpd_start(&handle, &config) != ESP_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        starting_ = false;
        ESP_LOGE(TAG, "Failed to start Home Assistant settings server");
        return;
    }

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = HandleIndex, .user_ctx = this},
        {.uri = "/api/ha", .method = HTTP_GET, .handler = HandleGetConfig, .user_ctx = this},
        {.uri = "/api/ha/test", .method = HTTP_POST, .handler = HandleTest, .user_ctx = this},
        {.uri = "/api/ha/save", .method = HTTP_POST, .handler = HandleSave, .user_ctx = this},
    };
    bool registration_failed = false;
    for (const auto& handler : handlers) {
        if (httpd_register_uri_handler(handle, &handler) != ESP_OK) {
            registration_failed = true;
            break;
        }
    }

    bool should_stop = registration_failed || !WifiManager::GetInstance().IsConnected();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_stop = should_stop || stop_requested_;
        starting_ = false;
        if (!should_stop) server_ = handle;
    }

    if (should_stop) {
        httpd_stop(handle);
        if (registration_failed) ESP_LOGE(TAG, "Failed to register Home Assistant settings endpoint");
        return;
    }

    ESP_LOGI(TAG, "Home Assistant settings server started at http://%s/", WifiManager::GetInstance().GetIpAddress().c_str());
    LogMemory("after_start");
}

void HomeAssistantSettingsServer::Stop() {
    httpd_handle_t handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
        handle = server_;
        server_ = nullptr;
    }
    if (handle != nullptr) {
        httpd_stop(handle);
        ESP_LOGI(TAG, "Home Assistant settings server stopped");
        LogMemory("after_stop");
    }
}

bool HomeAssistantSettingsServer::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_ != nullptr;
}

esp_err_t HomeAssistantSettingsServer::HandleIndex(httpd_req_t* request) {
    return static_cast<HomeAssistantSettingsServer*>(request->user_ctx)->SendIndex(request);
}

esp_err_t HomeAssistantSettingsServer::HandleGetConfig(httpd_req_t* request) {
    return static_cast<HomeAssistantSettingsServer*>(request->user_ctx)->SendConfig(request);
}

esp_err_t HomeAssistantSettingsServer::HandleTest(httpd_req_t* request) {
    return static_cast<HomeAssistantSettingsServer*>(request->user_ctx)->TestConfig(request);
}

esp_err_t HomeAssistantSettingsServer::HandleSave(httpd_req_t* request) {
    return static_cast<HomeAssistantSettingsServer*>(request->user_ctx)->SaveConfig(request);
}

esp_err_t HomeAssistantSettingsServer::SendIndex(httpd_req_t* request) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HomeAssistantSettingsServer::SendConfig(httpd_req_t* request) {
    auto config = HomeAssistant::GetInstance().GetConfigSnapshot();
    cJSON* root = cJSON_CreateObject();
    if (!root) return SendError(request, "500 Internal Server Error", "{\"error\":\"out_of_memory\"}");
    cJSON_AddStringToObject(root, "url", config.url.c_str());
    cJSON_AddBoolToObject(root, "token_configured", !config.token.empty());
    char* rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) return SendError(request, "500 Internal Server Error", "{\"error\":\"out_of_memory\"}");
    esp_err_t result = SendJson(request, rendered);
    free(rendered);
    return result;
}

esp_err_t HomeAssistantSettingsServer::TestConfig(httpd_req_t* request) {
    std::string body;
    if (!ReadBody(request, body)) return SendError(request, "400 Bad Request", "{\"success\":false,\"error\":\"invalid_body\"}");

    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return SendError(request, "400 Bad Request", "{\"success\":false,\"error\":\"invalid_json\"}");
    }
    std::string url;
    std::string token;
    bool valid = GetString(root, "url", url, kMaxUrlLength, false) && GetString(root, "token", token, kMaxTokenLength, false);
    cJSON_Delete(root);
    if (!valid) return SendError(request, "400 Bad Request", "{\"success\":false,\"error\":\"invalid_config\"}");

    LogMemory("before_test");
    bool success = HomeAssistant::GetInstance().TestConnection(url, token);
    LogMemory("after_test");
    return SendJson(request, success ? "{\"success\":true}" : "{\"success\":false,\"error\":\"connection_failed\"}",
                    success ? "200 OK" : "502 Bad Gateway");
}

esp_err_t HomeAssistantSettingsServer::SaveConfig(httpd_req_t* request) {
    std::string body;
    if (!ReadBody(request, body)) return SendError(request, "400 Bad Request", "{\"success\":false,\"error\":\"invalid_body\"}");

    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return SendError(request, "400 Bad Request", "{\"success\":false,\"error\":\"invalid_json\"}");
    }
    std::string url;
    std::string token;
    bool valid = GetString(root, "url", url, kMaxUrlLength, true) && GetString(root, "token", token, kMaxTokenLength, false);
    cJSON_Delete(root);
    if (!valid || url.empty()) return SendError(request, "400 Bad Request", "{\"success\":false,\"error\":\"url_required\"}");

    auto current = HomeAssistant::GetInstance().GetConfigSnapshot();
    if (token.empty()) token = current.token;
    HomeAssistant::GetInstance().SetConfig(url, token);
    LogMemory("after_save");
    return SendJson(request, "{\"success\":true}");
}
