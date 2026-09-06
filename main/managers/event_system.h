#ifndef _EVENT_SYSTEM_H_
#define _EVENT_SYSTEM_H_

#include <cstdint>

enum EventType {
    EVENT_NONE = 0,
    EVENT_BUTTON_CLICK,
    EVENT_TOUCH_TAP,
    EVENT_VOICE_START,
    EVENT_VOICE_END,
    EVENT_AI_RESPONSE_START,
    EVENT_AI_RESPONSE_END,
    EVENT_AUDIO_START,
    EVENT_AUDIO_END,
    EVENT_WIFI_CONNECTED,
    EVENT_WIFI_DISCONNECTED,
    EVENT_APP_CHANGED,
    EVENT_GAMEPAD_CONNECTED,
    EVENT_GAMEPAD_DISCONNECTED
};

struct SystemEvent {
    EventType type;
    int32_t arg1;
    int32_t arg2;
    void* ptr_data;
};

#endif // _EVENT_SYSTEM_H_
