#ifndef _APP_MANAGER_H_
#define _APP_MANAGER_H_

#include <cstdint>
#include <memory>
#include "event_system.h"

enum AppId {
    kAppLauncher = 0,
    kAppAiAssistant,
    kAppMediaPlayer,
    kAppInternetRadio,
    kAppSegaEmulator,
    kAppSettings
};

class AppBase {
public:
    virtual ~AppBase() = default;
    virtual AppId GetId() const = 0;
    virtual void OnStart() = 0;
    virtual void OnStop() = 0;
    virtual void OnPause() {}
    virtual void OnResume() {}
    virtual void OnUpdate() {}
    virtual void OnTouchEvent(int x, int y, bool pressed) {}
    virtual void OnEvent(const SystemEvent& event) {}
};

class AppManager {
public:
    static AppManager& GetInstance() {
        static AppManager instance;
        return instance;
    }

    void Initialize();
    bool SwitchToApp(AppId id);
    AppId GetCurrentAppId() const { return current_app_id_; }
    AppBase* GetCurrentApp() { return current_app_.get(); }
    void DispatchTouchEvent(int x, int y, bool pressed);
    void DispatchEvent(const SystemEvent& event);
    void OnUpdate();

private:
    AppManager() = default;
    AppId current_app_id_ = kAppAiAssistant; // Default to AI Assistant
    std::unique_ptr<AppBase> current_app_;
};

#endif // _APP_MANAGER_H_
