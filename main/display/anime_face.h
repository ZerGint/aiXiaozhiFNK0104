#pragma once

#include <cstddef>
#include <lvgl.h>
#include <cstdint>

#include "face_reaction.h"

class AnimeFace {
public:
    enum class ApplicationState : uint8_t { Idle, Listening, Thinking, Speaking };
    enum class FaceEmotion : uint8_t {
        Neutral,
        Happy,
        Sad,
        Angry,
        Surprised,
        Sleepy,
        Crying,
        Worried,
        Scared,
        Focused,
        Confused,
        Shy,
        Exhausted,
        Laughing,
        Love,
        Nervous,
        Playful,
        Yawn,
    };
    enum class MouthExpression : uint8_t { Closed, Small, Medium, Wide, O };

    struct FaceExpression {
        int8_t gaze_x;
        int8_t gaze_y;
        int8_t left_brow_x;
        int8_t left_brow_y;
        int8_t right_brow_x;
        int8_t right_brow_y;
        int8_t eyelid_bias;
        uint8_t base_eyelid_frame;
        MouthExpression mouth;
    };

    bool Initialize(lv_obj_t* parent, int screen_width, int screen_height);
    void Update(uint32_t now_ms);
    void SetNeutral();
    void SetApplicationState(ApplicationState state);
    void SetEmotion(FaceEmotion emotion);
    // Close and reopen one eyelid without affecting the other eye.
    void PlayWink(bool left_eye = false, uint32_t duration_ms = 500);
    void PlayReaction(FaceReaction reaction, FaceReactionSource source = FaceReactionSource::Unknown);
    // Explicit blink remains available to callers even when an emotion owns
    // a partially closed eyelid base frame.
    void ForceBlink();
    void SetSpeechLevel(uint8_t level, uint32_t now_ms);
    bool IsInitialized() const { return initialized_; }
    lv_obj_t* Root() const { return root_; }

private:
    struct Point {
        int16_t x;
        int16_t y;
    };

    static constexpr int kCanvasWidth = 240;
    static constexpr int kCanvasHeight = 240;
    static constexpr int kEyelidFrameCount = 5;
    static constexpr int kMouthFrameCount = 5;
    static constexpr int kSadMouthFrameCount = 4;
    static constexpr int kAngryMouthFrameCount = 4;
    static constexpr int kHappyMouthFrameCount = 4;
    static constexpr int kHappyEyeFrameCount = 3;
    // 28 ms per step; frame 4 is held for two steps (56 ms).
    static constexpr int kBlinkSequence[] = {0, 1, 2, 3, 4, 4, 3, 2, 1, 0};
    // Smile-eye blink uses only the smile assets. Frame 1 is open,
    // frame 0 is closed, and frame 2 is slightly closed.
    static constexpr int kHappyEyeBlinkSequence[] = {1, 2, 0, 0, 2, 1};
    // Laughing stays in a strongly squinted pose; frame 2 and the closed
    // frame alternate. Love periodically opens fully and returns closed.
    static constexpr int kLaughingSmileBlinkSequence[] = {2, 0, 0, 2};
    static constexpr int kLoveSmileBlinkSequence[] = {0, 2, 1, 1, 2, 0};

    lv_obj_t* root_ = nullptr;
    lv_obj_t* iris_clip_[2] = {};
    lv_obj_t* iris_[2] = {};
    lv_obj_t* eyelid_[2][kEyelidFrameCount] = {};
    lv_obj_t* eye_smile_[2][kHappyEyeFrameCount] = {};
    lv_obj_t* brow_[2] = {};
    lv_obj_t* mouth_[kMouthFrameCount] = {};
    lv_obj_t* sad_mouth_[kSadMouthFrameCount] = {};
    lv_obj_t* angry_mouth_frames_[kAngryMouthFrameCount] = {};
    lv_obj_t* happy_mouth_frames_[kHappyMouthFrameCount] = {};
    lv_obj_t* angry_mouth_ = nullptr;
    lv_obj_t* crying_mouth_ = nullptr;
    lv_obj_t* open_smile_mouth_ = nullptr;
    lv_obj_t* tongue_mouth_ = nullptr;
    lv_obj_t* yawn_mouth_ = nullptr;
    lv_obj_t* tear_[2] = {};
    lv_obj_t* blush_[2] = {};
    lv_obj_t* heart_ = nullptr;
    lv_obj_t* irritation_ = nullptr;
    lv_obj_t* sparkle_ = nullptr;
    lv_obj_t* sweat_[2] = {};
    lv_obj_t* base_ = nullptr;
    lv_obj_t* eye_white_[2] = {};
    lv_img_dsc_t descriptors_[56] = {};
    size_t descriptor_count_ = 0;

    int screen_width_ = 0;
    int screen_height_ = 0;
    int gaze_x_ = 0;
    int gaze_target_x_ = 0;
    uint32_t gaze_started_ms_ = 0;
    uint32_t gaze_transition_ms_ = 0;
    uint32_t gaze_hold_until_ms_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t blink_started_ms_ = 0;
    int blink_frame_ = 0;
    bool blinking_ = false;
    bool wink_left_eye_ = false;
    uint32_t wink_started_ms_ = 0;
    uint32_t wink_duration_ms_ = 500;
    bool winking_ = false;
    ApplicationState application_state_ = ApplicationState::Idle;
    FaceEmotion emotion_ = FaceEmotion::Neutral;
    FaceExpression expression_{};
    uint8_t base_eyelid_frame_ = 0;
    int brow_left_x_current_ = 0;
    int brow_left_y_current_ = 0;
    int brow_right_x_current_ = 0;
    int brow_right_y_current_ = 0;
    int brow_left_x_target_ = 0;
    int brow_left_y_target_ = 0;
    int brow_right_x_target_ = 0;
    int brow_right_y_target_ = 0;
    int mouth_frame_ = 0;
    int expression_mouth_frame_ = 0;
    enum class SpecialMouth : uint8_t {
        None,
        Sad,
        Angry,
        Crying,
        OpenSmile,
        Tongue,
        Yawn,
    };
    SpecialMouth special_mouth_ = SpecialMouth::None;
    bool tears_visible_ = false;
    uint32_t tear_motion_started_ms_ = 0;
    bool eye_smile_visible_ = false;
    int happy_eye_frame_ = 0;
    bool blush_visible_ = false;
    bool heart_visible_ = false;
    bool irritation_visible_ = false;
    bool sparkle_visible_ = false;
    bool sweat_visible_ = false;
    uint32_t next_mouth_ms_ = 0;
    bool mouth_animating_ = false;
    uint8_t speech_level_target_ = 0;
    uint8_t speech_level_current_ = 0;
    uint32_t last_speech_pcm_ms_ = 0;
    uint32_t speech_state_enter_ms_ = 0;
    bool speech_pcm_seen_ = false;
    bool speech_silence_logged_ = false;
    bool mouth_open_logged_ = false;
    bool speech_mouth_rendering_ = false;
    bool initialized_ = false;
    bool first_update_ = true;

    enum class ReactionState : uint8_t { None, Notice, Attention, Settle };
    ReactionState reaction_state_ = ReactionState::None;
    FaceReactionSource reaction_source_ = FaceReactionSource::Unknown;
    uint32_t reaction_started_ms_ = 0;
    uint32_t reaction_phase_started_ms_ = 0;
    int reaction_gaze_before_ = 0;
    bool reaction_center_logged_ = false;
    bool reaction_mouth_logged_ = false;
    bool reaction_blink_was_active_ = false;

#if CONFIG_FNK_ANIME_FACE_EMOTION_DEMO
    uint8_t demo_emotion_index_ = 0;
    uint32_t demo_next_ms_ = 0;
#endif

    void SetImage(lv_obj_t* object, const uint8_t* data, size_t size);
    void SetImagePosition(lv_obj_t* object, int x, int y);
    void SetEyelidFrame(int frame);
    void SetEyelidFrameForEye(int eye, int frame);
    void ApplyEyeVisibility();
    void SetWinkMouthPose(int offset_x, int angle_degrees);
    void SetMouthFrame(int frame);
    void ApplyMouthVisibility();
    void SetTearsVisible(bool visible);
    void UpdateTears(uint32_t now_ms);
    void SetFaceOverlays(bool blush, bool heart, bool irritation, bool sparkle, bool sweat);
    void SetBrowPose(ApplicationState state);
    void UpdateBrowTransition();
    void SetGaze(int x);
    void StartBlink(uint32_t now_ms);
    void UpdateGaze(uint32_t now_ms);
    void UpdateBlink(uint32_t now_ms);
    void UpdateWink(uint32_t now_ms);
    void UpdateMouth(uint32_t now_ms);
    void UpdateReaction(uint32_t now_ms);
    void FinishReaction(uint32_t now_ms);
    void UpdateEmotionDemo(uint32_t now_ms);
    int ExpressionMouthFrame() const;
    int SpeakingEyelidFrame() const;
    int EyelidRestFrame() const;
    int SpeechMouthFrame(uint8_t level) const;
    static FaceExpression ExpressionFor(FaceEmotion emotion);
    static uint32_t RandomRange(uint32_t min_value, uint32_t max_value);
    static int Interpolate(int from, int to, uint32_t elapsed, uint32_t duration);
};
