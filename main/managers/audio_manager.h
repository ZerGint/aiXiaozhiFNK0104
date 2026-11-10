#ifndef _AUDIO_MANAGER_H_
#define _AUDIO_MANAGER_H_

#include <atomic>
#include <cstdint>
#include <mutex>

enum AudioSource {
    kAudioSourceNone = 0,
    kAudioSourceXiaoZhiTts,
    kAudioSourceMp3Player,
    kAudioSourceInternetRadio,
    kAudioSourceEmulator
};

class AudioManager {
public:
    static AudioManager& GetInstance() {
        static AudioManager instance;
        return instance;
    }

    bool RequestAudioFocus(AudioSource source);
    void ReleaseAudioFocus(AudioSource source);
    AudioSource GetCurrentAudioSource() const { return current_source_.load(); }
    
    void SetMasterVolume(uint8_t volume);
    uint8_t GetMasterVolume() const { return volume_; }

    void Mute(bool mute);
    bool IsMuted() const { return is_muted_; }

private:
    AudioManager() = default;
    // Written by the radio/SD tasks, read lock-free by the audio output task.
    std::atomic<AudioSource> current_source_{kAudioSourceNone};
    AudioSource paused_source_ = kAudioSourceNone;  // guarded by mutex_
    std::mutex mutex_;
    uint8_t volume_ = 70;
    bool is_muted_ = false;
};

#endif // _AUDIO_MANAGER_H_
