#ifndef _DEVICE_STATE_H_
#define _DEVICE_STATE_H_

enum DeviceState {
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateWifiConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateSpeaking,
    kDeviceStateNotifying,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
    kDeviceStateAudioTesting,
    kDeviceStateFatalError
};

// States in which the device plays or captures voice (assistant speech,
// microphone streaming, server notifications). Local media must be silent.
inline bool IsVoiceAudioState(DeviceState state) {
    return state == kDeviceStateListening || state == kDeviceStateSpeaking ||
           state == kDeviceStateNotifying;
}

#endif // _DEVICE_STATE_H_ 