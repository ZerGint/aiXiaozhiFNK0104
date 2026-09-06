#include "home_assistant.h"
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <cJSON.h>

#define TAG "HomeAssistant"

HomeAssistant::HomeAssistant() {
    Settings settings("ha", false);
    url_ = settings.GetString("url", "");
    token_ = settings.GetString("token", "");
}

void HomeAssistant::Initialize() {
    ESP_LOGI(TAG, "Initializing Home Assistant integration...");
    Settings settings("ha", false);
    url_ = settings.GetString("url", "");
    token_ = settings.GetString("token", "");
    RegisterMcpTools();
}

void HomeAssistant::SetConfig(const std::string& url, const std::string& token) {
    url_ = url;
    token_ = token;
    Settings settings("ha", true);
    settings.SetString("url", url_);
    settings.SetString("token", token_);
    ESP_LOGI(TAG, "Home Assistant configuration saved: URL=%s", url_.c_str());
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        std::string* response_body = static_cast<std::string*>(evt->user_data);
        if (response_body && evt->data && evt->data_len > 0) {
            response_body->append((char*)evt->data, evt->data_len);
        }
    }
    return ESP_OK;
}

std::string HomeAssistant::PerformHttpRequest(esp_http_client_method_t method, const std::string& path, const std::string& post_data) {
    if (!IsConfigured()) {
        return "{\"error\": \"Home Assistant URL or Token is not configured. Use homeassistant.set_config tool first.\"}";
    }

    std::string base_url = url_;
    if (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }

    auto try_request = [this, method, path, post_data](const std::string& target_url) -> std::pair<esp_err_t, std::string> {
        std::string full_url = target_url + path;
        std::string response_body;

        esp_http_client_config_t config = {};
        config.url = full_url.c_str();
        config.method = method;
        config.timeout_ms = 10000;
        config.event_handler = _http_event_handler;
        config.user_data = &response_body;
        config.buffer_size = 4096;
        config.buffer_size_tx = 2048;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.skip_cert_common_name_check = true;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            return {ESP_FAIL, "{\"error\": \"Failed to initialize HTTP client\"}"};
        }

        std::string auth_header = "Bearer " + token_;
        esp_http_client_set_header(client, "Authorization", auth_header.c_str());
        esp_http_client_set_header(client, "Content-Type", "application/json");

        if (method == HTTP_METHOD_POST && !post_data.empty()) {
            esp_http_client_set_post_field(client, post_data.c_str(), post_data.length());
        }

        esp_err_t err = esp_http_client_perform(client);
        int status_code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK && (status_code >= 200 && status_code < 300)) {
            ESP_LOGI(TAG, "HTTP %s %s -> Status: %d, Response Len: %d",
                     method == HTTP_METHOD_POST ? "POST" : "GET", full_url.c_str(), status_code, (int)response_body.length());
            return {ESP_OK, response_body};
        }

        ESP_LOGE(TAG, "HTTP Request to %s failed: err=%s (%d), status=%d", full_url.c_str(), esp_err_to_name(err), err, status_code);
        return {err, "{\"error\": \"Request to " + full_url + " failed with status " + std::to_string(status_code) + "\","
                    "\"esp_err\": \"" + std::string(esp_err_to_name(err)) + "\","
                    "\"status_code\": " + std::to_string(status_code) + "}"};
    };

    // First attempt with configured URL
    auto [err, res] = try_request(base_url);
    if (err == ESP_OK) return res;

    // Retry with port :8123 if omitted in base_url (e.g. http://192.168.31.19 -> http://192.168.31.19:8123)
    size_t proto_end = base_url.find("://");
    if (proto_end != std::string::npos) {
        std::string host_part = base_url.substr(proto_end + 3);
        if (host_part.find(':') == std::string::npos) {
            std::string url_with_port = base_url + ":8123";
            ESP_LOGW(TAG, "Retrying Home Assistant request with port 8123: %s", url_with_port.c_str());
            auto [p_err, p_res] = try_request(url_with_port);
            if (p_err == ESP_OK) return p_res;
        }
    }

    // If HTTP failed, automatically try HTTPS (or vice versa)
    std::string alt_url = base_url;
    if (alt_url.rfind("http://", 0) == 0) {
        alt_url.replace(0, 7, "https://");
    } else if (alt_url.rfind("https://", 0) == 0) {
        alt_url.replace(0, 8, "http://");
    }

    if (alt_url != base_url) {
        ESP_LOGW(TAG, "Retrying Home Assistant request with alternate protocol: %s", alt_url.c_str());
        auto [alt_err, alt_res] = try_request(alt_url);
        if (alt_err == ESP_OK) return alt_res;
    }

    return res;
}

std::string HomeAssistant::TestConnection() {
    std::string res = PerformHttpRequest(HTTP_METHOD_GET, "/api/");
    if (res.find("API running") != std::string::npos || res.find("message") != std::string::npos) {
        return "OK";
    }
    std::string res_cfg = PerformHttpRequest(HTTP_METHOD_GET, "/api/config");
    if (res_cfg.find("location_name") != std::string::npos || res_cfg.find("version") != std::string::npos) {
        return "OK";
    }
    return res;
}

std::string HomeAssistant::CallService(const std::string& domain, const std::string& service, const std::string& entity_id, const std::string& data_json) {
    if (!entity_id.empty()) {
        std::string state_res = PerformHttpRequest(HTTP_METHOD_GET, "/api/states/" + entity_id);
        if (state_res.find("\"unavailable\"") != std::string::npos || state_res.find("\"unknown\"") != std::string::npos) {
            ESP_LOGW(TAG, "Device entity '%s' is unavailable or offline in Home Assistant", entity_id.c_str());
            return "{\"error\": \"Device entity '" + entity_id + "' is currently UNAVAILABLE / OFFLINE in Home Assistant. The physical device is turned off or disconnected from power/network. Inform the user that the device is turned off or offline.\"}";
        }
    }

    std::string path = "/api/services/" + domain + "/" + service;

    cJSON* json_root = cJSON_CreateObject();
    if (!entity_id.empty()) {
        cJSON_AddStringToObject(json_root, "entity_id", entity_id.c_str());
    }

    if (!data_json.empty()) {
        cJSON* extra_data = cJSON_Parse(data_json.c_str());
        if (extra_data) {
            cJSON* item = extra_data->child;
            while (item) {
                cJSON_AddItemToObject(json_root, item->string, cJSON_Duplicate(item, 1));
                item = item->next;
            }
            cJSON_Delete(extra_data);
        }
    }

    char* rendered = cJSON_PrintUnformatted(json_root);
    std::string post_data = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(json_root);

    return PerformHttpRequest(HTTP_METHOD_POST, path, post_data);
}

static std::string FilterEntitiesJson(const std::string& raw_json, const std::string& search_keyword) {
    cJSON* root = cJSON_Parse(raw_json.c_str());
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return raw_json;
    }

    cJSON* result_array = cJSON_CreateArray();
    int size = cJSON_GetArraySize(root);

    std::string kw = search_keyword;
    for (auto& c : kw) c = tolower(c);

    for (int i = 0; i < size; i++) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        cJSON* ent_id = cJSON_GetObjectItem(item, "entity_id");
        cJSON* state = cJSON_GetObjectItem(item, "state");
        cJSON* attrs = cJSON_GetObjectItem(item, "attributes");

        if (!ent_id || !ent_id->valuestring || !state) continue;

        std::string eid = ent_id->valuestring;
        std::string fname = "";
        std::string unit = "";
        if (attrs && cJSON_IsObject(attrs)) {
            cJSON* fn = cJSON_GetObjectItem(attrs, "friendly_name");
            if (fn && fn->valuestring) fname = fn->valuestring;
            cJSON* u = cJSON_GetObjectItem(attrs, "unit_of_measurement");
            if (u && u->valuestring) unit = u->valuestring;
        }

        std::string sub_kw = kw;
        size_t dot_pos = sub_kw.find('.');
        if (dot_pos != std::string::npos) {
            sub_kw = sub_kw.substr(dot_pos + 1);
        }

        std::string eid_lower = eid;
        for (auto& c : eid_lower) c = tolower(c);
        std::string fname_lower = fname;
        for (auto& c : fname_lower) c = tolower(c);

        bool is_all_request = sub_kw.empty() || 
            sub_kw == "all" || sub_kw == "devices" || sub_kw == "device" || 
            sub_kw == "entities" || sub_kw == "entity" || sub_kw == "list" || 
            sub_kw == "homeassistant" || sub_kw == "show" || sub_kw == "everything" || 
            sub_kw == "get_all" || sub_kw == "get_states" || sub_kw == "states" || 
            sub_kw == "get" || sub_kw == "status" || sub_kw == "все" || sub_kw == "всё" ||
            sub_kw.find("устройств") != std::string::npos || sub_kw.find("девайс") != std::string::npos || 
            sub_kw.find("список") != std::string::npos || sub_kw.find("прибор") != std::string::npos;

        bool match = false;
        if (is_all_request) {
            // Skip noisy system entities in general device list
            if (eid_lower.rfind("update.", 0) == 0 ||
                eid_lower.rfind("automation.", 0) == 0 ||
                eid_lower.rfind("scene.", 0) == 0 ||
                eid_lower.rfind("zone.", 0) == 0 ||
                eid_lower.rfind("sun.", 0) == 0 ||
                eid_lower.rfind("person.", 0) == 0 ||
                eid_lower.rfind("tts.", 0) == 0 ||
                eid_lower.rfind("stt.", 0) == 0) {
                continue;
            }
            // Skip secondary diagnostic sub-entities in general device list
            if (eid_lower.find("_battery") != std::string::npos ||
                eid_lower.find("_linkquality") != std::string::npos ||
                eid_lower.find("_signal") != std::string::npos ||
                eid_lower.find("_voltage") != std::string::npos ||
                eid_lower.find("_rssi") != std::string::npos ||
                eid_lower.find("_lqi") != std::string::npos) {
                continue;
            }
            // Collapse repetitive glass sub-entities for dispenser in general list
            if (eid_lower.find("glass_2") != std::string::npos ||
                eid_lower.find("glass_3") != std::string::npos ||
                eid_lower.find("glass_4") != std::string::npos) {
                continue;
            }
            match = true;
        } else {
            if (eid_lower.find(sub_kw) != std::string::npos || fname_lower.find(sub_kw) != std::string::npos) {
                match = true;
            } else if (sub_kw.find("step") != std::string::npos || sub_kw.find("шаг") != std::string::npos || sub_kw.find("walk") != std::string::npos) {
                if (eid_lower.find("step") != std::string::npos || eid_lower.find("walk") != std::string::npos ||
                    fname_lower.find("step") != std::string::npos || fname_lower.find("шаг") != std::string::npos || fname_lower.find("шагов") != std::string::npos) {
                    match = true;
                }
            } else if (sub_kw.find("heart") != std::string::npos || sub_kw.find("pulse") != std::string::npos || sub_kw.find("пульс") != std::string::npos) {
                if (eid_lower.find("heart") != std::string::npos || fname_lower.find("heart") != std::string::npos || fname_lower.find("pulse") != std::string::npos) {
                    match = true;
                }
            } else if (sub_kw.find("amazfit") != std::string::npos || sub_kw.find("watch") != std::string::npos) {
                if (eid_lower.find("amazfit") != std::string::npos || eid_lower.find("watch") != std::string::npos) {
                    match = true;
                }
            }
        }

        if (match) {
            cJSON* match_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(match_obj, "entity_id", eid.c_str());
            cJSON_AddStringToObject(match_obj, "state", state->valuestring ? state->valuestring : "");
            if (!fname.empty()) cJSON_AddStringToObject(match_obj, "name", fname.c_str());
            if (!unit.empty()) cJSON_AddStringToObject(match_obj, "unit", unit.c_str());
            cJSON_AddItemToArray(result_array, match_obj);

            if (is_all_request && cJSON_GetArraySize(result_array) >= 30) {
                break;
            }
        }
    }

    cJSON_Delete(root);
    char* rendered = cJSON_PrintUnformatted(result_array);
    std::string res = rendered ? rendered : "[]";
    if (rendered) free(rendered);
    cJSON_Delete(result_array);

    return res;
}

std::string HomeAssistant::GetStates(const std::string& entity_id) {
    if (!entity_id.empty()) {
        std::string res = PerformHttpRequest(HTTP_METHOD_GET, "/api/states/" + entity_id);
        if (res.find("404") == std::string::npos && res.find("error") == std::string::npos && res.find("Entity not found") == std::string::npos) {
            return res;
        }
        ESP_LOGW(TAG, "Entity '%s' not found directly, searching all Home Assistant entities...", entity_id.c_str());
    }

    std::string all_states = PerformHttpRequest(HTTP_METHOD_GET, "/api/states");
    return FilterEntitiesJson(all_states, entity_id);
}

void HomeAssistant::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("homeassistant.call_service",
        "Call any Home Assistant service to control smart home devices, run scripts, or pour drinks from Smart Naliwator Dispenser.\n"
        "IMPORTANT: If a device is physically turned off or unavailable, CallService will return an UNAVAILABLE error. If you receive an UNAVAILABLE error, DO NOT attempt to pour or control! Tell the user: 'Устройство (или наливатор) сейчас отключено или не в сети'!\n"
        "CRITICAL FOR NALIWATOR DISPENSER (наливатор / рюмки / налей):\n"
        "1) SAY FIRST to user: 'Наливаю X мл в N-ю рюмку...'\n"
        "2) Execute Step 1: Set volume: domain='number', service='set_value', entity_id='number.smart_naliwator_dispenser_glass_N_volume', data_json='{\"value\": X}'\n"
        "3) Execute Step 2: TRIGGER POUR: domain='button', service='press', entity_id='button.gostinaia_smart_naliwator_dispenser_pour_glass_N'\n"
        "4) If sensor.smart_naliwator_dispenser_status or binary_sensor.smart_naliwator_dispenser_pouring exists, poll get_states until state is 'Idle'/'off'.\n"
        "5) SAY FINALLY when done: 'Готово! Налив завершён.'\n"
        "Args:\n"
        "  `domain`: Service domain (e.g. 'number', 'button', 'switch', 'light', 'script')\n"
        "  `service`: Service name (e.g. 'set_value', 'press', 'turn_on', 'turn_off')\n"
        "  `entity_id`: Entity ID (e.g. 'number.smart_naliwator_dispenser_glass_4_volume', 'button.gostinaia_smart_naliwator_dispenser_pour_glass_4')\n"
        "  `data_json`: Extra JSON parameters (optional, e.g. '{\"value\": 20}')",
        PropertyList({
            Property("domain", kPropertyTypeString, std::string("")),
            Property("service", kPropertyTypeString, std::string("")),
            Property("entity_id", kPropertyTypeString, std::string("")),
            Property("data_json", kPropertyTypeString, std::string(""))
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string domain = properties["domain"].value<std::string>();
            std::string service = properties["service"].value<std::string>();
            std::string entity_id = properties.HasProperty("entity_id") ? properties["entity_id"].value<std::string>() : "";
            std::string data_json = properties.HasProperty("data_json") ? properties["data_json"].value<std::string>() : "";

            return CallService(domain, service, entity_id, data_json);
        });

    mcp.AddTool("homeassistant.get_states",
        "Get current real-time sensor data, smart watch metrics (steps count, heart rate, sleep), temperatures, battery levels, or device states from Home Assistant.\n"
        "To get all devices or entities list in Home Assistant, pass empty entity_id string '' or 'all' or 'devices'.\n"
        "IMPORTANT WHEN ANSWERING USER: Group and summarize main physical devices by their friendly names (e.g., 'Наливатор, Светильник, Умные часы, Датчики'). Do NOT read out every technical sub-entity, sensor parameter, or glass number one by one!\n"
        "ALWAYS call this tool when user asks about devices in Home Assistant, steps walked, smart watch stats, health data, temperature, humidity, or any home status.\n"
        "Args:\n"
        "  `entity_id`: Entity ID, domain, or keyword (optional, e.g. '', 'all', 'devices', 'sensor.steps', 'light', 'switch'). If empty or 'all'/'devices', fetches all states.",
        PropertyList({
            Property("entity_id", kPropertyTypeString, std::string(""))
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string entity_id = properties.HasProperty("entity_id") ? properties["entity_id"].value<std::string>() : "";
            return GetStates(entity_id);
        });

    mcp.AddUserOnlyTool("homeassistant.set_config",
        "Configure Home Assistant connection URL and Long-Lived Access Token\n"
        "Args:\n"
        "  `url`: Base URL of Home Assistant (e.g. 'http://192.168.1.100:8123')\n"
        "  `token`: Long-Lived Access Token from Home Assistant profile",
        PropertyList({
            Property("url", kPropertyTypeString),
            Property("token", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string url = properties["url"].value<std::string>();
            std::string token = properties["token"].value<std::string>();
            SetConfig(url, token);
            return "Home Assistant configuration saved successfully.";
        });
}
