#ifndef _GAMEPAD_MANAGER_H_
#define _GAMEPAD_MANAGER_H_

#include <cstdint>

struct GamepadState {
    bool connected = false;
    uint16_t buttons = 0;
    int16_t left_stick_x = 0;
    int16_t left_stick_y = 0;
    int16_t right_stick_x = 0;
    int16_t right_stick_y = 0;
};

class GamepadManager {
public:
    static GamepadManager& GetInstance() {
        static GamepadManager instance;
        return instance;
    }

    void StartScanAndConnect();
    void StopScan();
    bool IsGamepadConnected() const { return state_.connected; }
    GamepadState GetState() const { return state_; }

private:
    GamepadManager() = default;
    GamepadState state_;
};

#endif // _GAMEPAD_MANAGER_H_
