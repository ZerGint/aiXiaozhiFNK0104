#include "anime_face.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <utility>

#define DECLARE_ASSET(name) \
    extern "C" const uint8_t _binary_##name##_start[]; \
    extern "C" const uint8_t _binary_##name##_end[];

DECLARE_ASSET(anime_base_240_png)
DECLARE_ASSET(anime_eye_white_left_png)
DECLARE_ASSET(anime_eye_white_right_png)
DECLARE_ASSET(anime_iris_left_png)
DECLARE_ASSET(anime_iris_right_png)
DECLARE_ASSET(anime_eyelid_left_0_png)
DECLARE_ASSET(anime_eyelid_left_1_png)
DECLARE_ASSET(anime_eyelid_left_2_png)
DECLARE_ASSET(anime_eyelid_left_3_png)
DECLARE_ASSET(anime_eyelid_left_4_png)
DECLARE_ASSET(anime_eyelid_right_0_png)
DECLARE_ASSET(anime_eyelid_right_1_png)
DECLARE_ASSET(anime_eyelid_right_2_png)
DECLARE_ASSET(anime_eyelid_right_3_png)
DECLARE_ASSET(anime_eyelid_right_4_png)
DECLARE_ASSET(anime_eye_smile_left_png)
DECLARE_ASSET(anime_eye_smile_right_png)
DECLARE_ASSET(anime_brow_left_png)
DECLARE_ASSET(anime_brow_right_png)
DECLARE_ASSET(anime_mouth_0_closed_png)
DECLARE_ASSET(anime_mouth_1_small_png)
DECLARE_ASSET(anime_mouth_2_medium_png)
DECLARE_ASSET(anime_mouth_3_wide_png)
DECLARE_ASSET(anime_mouth_4_o_png)
DECLARE_ASSET(anime_mouth_sad_png)
DECLARE_ASSET(anime_mouth_angry_png)
DECLARE_ASSET(anime_mouth_crying_png)
DECLARE_ASSET(anime_mouth_open_smile_png)
DECLARE_ASSET(anime_mouth_tongue_png)
DECLARE_ASSET(anime_mouth_yawn_png)
DECLARE_ASSET(anime_tear_left_png)
DECLARE_ASSET(anime_tear_right_png)
DECLARE_ASSET(anime_blush_left_png)
DECLARE_ASSET(anime_blush_right_png)
DECLARE_ASSET(anime_effect_heart_png)
DECLARE_ASSET(anime_effect_irritation_png)
DECLARE_ASSET(anime_effect_sparkle_png)
DECLARE_ASSET(anime_effect_sweat_left_png)
DECLARE_ASSET(anime_effect_sweat_right_png)

namespace {
constexpr char TAG[] = "AnimeFace";

struct Asset {
    const uint8_t* data;
    size_t size;
};

#define ASSET(name) { _binary_##name##_start, \
                      static_cast<size_t>(_binary_##name##_end - _binary_##name##_start) }

constexpr int kEyeWhiteX[] = {56, 143};
constexpr int kEyeWhiteY = 101;
constexpr int kEyeWhiteW = 41;
constexpr int kEyeWhiteH = 38;
constexpr int kIrisX[] = {60, 139};
constexpr int kIrisY = 99;
constexpr int kEyelidX[] = {44, 138};
constexpr int kEyelidY = 88;
constexpr int kBrowX[] = {47, 137};
constexpr int kBrowY = 69;
constexpr int kMouthX = 103;
constexpr int kMouthY = 155;
constexpr int kTearX[] = {63, 165};
constexpr int kTearY = 137;
constexpr int kTearTravelPx = 6;
constexpr uint32_t kTearPeriodMs = 1000;
constexpr uint32_t kTearPhaseOffsetMs = 180;
constexpr int kBlushX[] = {57, 153};
constexpr int kBlushY = 139;
constexpr int kSweatX[] = {43, 185};
constexpr int kSweatY = 92;
constexpr int kHeartX = 30;
constexpr int kHeartY = 58;
constexpr int kIrritationX = 175;
constexpr int kIrritationY = 66;
constexpr int kSparkleX = 193;
constexpr int kSparkleY = 125;

const Asset kBase = ASSET(anime_base_240_png);
const Asset kEyeWhite[] = {ASSET(anime_eye_white_left_png), ASSET(anime_eye_white_right_png)};
const Asset kIris[] = {ASSET(anime_iris_left_png), ASSET(anime_iris_right_png)};
const Asset kEyelid[][5] = {
    {ASSET(anime_eyelid_left_0_png), ASSET(anime_eyelid_left_1_png),
     ASSET(anime_eyelid_left_2_png), ASSET(anime_eyelid_left_3_png),
     ASSET(anime_eyelid_left_4_png)},
    {ASSET(anime_eyelid_right_0_png), ASSET(anime_eyelid_right_1_png),
     ASSET(anime_eyelid_right_2_png), ASSET(anime_eyelid_right_3_png),
     ASSET(anime_eyelid_right_4_png)},
};
const Asset kEyeSmile[] = {ASSET(anime_eye_smile_left_png), ASSET(anime_eye_smile_right_png)};
const Asset kBrow[] = {ASSET(anime_brow_left_png), ASSET(anime_brow_right_png)};
const Asset kMouth[] = {
    ASSET(anime_mouth_0_closed_png), ASSET(anime_mouth_1_small_png),
    ASSET(anime_mouth_2_medium_png), ASSET(anime_mouth_3_wide_png),
    ASSET(anime_mouth_4_o_png),
};
const Asset kSpecialMouth[] = {
    ASSET(anime_mouth_sad_png), ASSET(anime_mouth_angry_png),
    ASSET(anime_mouth_crying_png), ASSET(anime_mouth_open_smile_png),
    ASSET(anime_mouth_tongue_png), ASSET(anime_mouth_yawn_png),
};
const Asset kTear[] = {ASSET(anime_tear_left_png), ASSET(anime_tear_right_png)};
const Asset kBlush[] = {ASSET(anime_blush_left_png), ASSET(anime_blush_right_png)};
const Asset kSweat[] = {ASSET(anime_effect_sweat_left_png), ASSET(anime_effect_sweat_right_png)};
const Asset kHeart = ASSET(anime_effect_heart_png);
const Asset kIrritation = ASSET(anime_effect_irritation_png);
const Asset kSparkle = ASSET(anime_effect_sparkle_png);

void LogMemory(const char* marker) {
    ESP_LOGI(TAG,
             "ANIME_FACE_MEM %s internal_free=%u internal_largest=%u internal_min=%u "
             "dma_free=%u dma_largest=%u spiram_free=%u spiram_largest=%u",
             marker,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
}  // namespace

AnimeFace::FaceExpression AnimeFace::ExpressionFor(FaceEmotion emotion) {
    switch (emotion) {
    case FaceEmotion::Happy:
        return {0, 0, 0, -1, 0, -1, 0, 0, MouthExpression::Small};
    case FaceEmotion::Sad:
        return {0, 0, 0, 1, 0, 1, 1, 1, MouthExpression::Closed};
    case FaceEmotion::Angry:
        return {0, 0, 0, 1, 0, 1, 0, 0, MouthExpression::Small};
    case FaceEmotion::Crying:
        return {0, 0, 0, 1, 0, 1, 1, 1, MouthExpression::Closed};
    case FaceEmotion::Worried:
        return {-1, 0, 0, 2, 0, 2, 1, 1, MouthExpression::Closed};
    case FaceEmotion::Scared:
        return {-1, 0, -1, -1, 1, -2, 0, 0, MouthExpression::O};
    case FaceEmotion::Focused:
        return {0, 0, 0, 1, 0, 1, 2, 2, MouthExpression::Closed};
    case FaceEmotion::Confused:
        return {2, 0, -1, -2, 1, 1, 1, 1, MouthExpression::Small};
    case FaceEmotion::Shy:
        return {-2, 1, 0, -1, 0, -1, 1, 1, MouthExpression::Small};
    case FaceEmotion::Exhausted:
        return {-1, 0, 0, 1, 0, 1, 2, 2, MouthExpression::Closed};
    case FaceEmotion::Laughing:
        return {0, 0, 0, -2, 0, -2, 2, 2, MouthExpression::Wide};
    case FaceEmotion::Love:
        return {0, 0, 0, -1, 0, -1, 2, 2, MouthExpression::Small};
    case FaceEmotion::Nervous:
        return {-1, 0, 0, 2, 0, 2, 1, 1, MouthExpression::Closed};
    case FaceEmotion::Playful:
        return {1, 0, 0, -1, 0, -1, 0, 0, MouthExpression::Small};
    case FaceEmotion::Yawn:
        return {0, 0, 0, 0, 0, 0, 2, 2, MouthExpression::Closed};
    case FaceEmotion::Surprised:
        return {0, 0, 0, -2, 0, -2, 0, 0, MouthExpression::O};
    case FaceEmotion::Sleepy:
        return {0, 0, 0, 0, 0, 0, 1, 1, MouthExpression::Closed};
    case FaceEmotion::Neutral:
    default:
        return {0, 0, 0, 0, 0, 0, 0, 0, MouthExpression::Closed};
    }
}

int AnimeFace::ExpressionMouthFrame() const {
    switch (expression_.mouth) {
    case MouthExpression::Small: return 1;
    case MouthExpression::Medium: return 2;
    case MouthExpression::Wide: return 3;
    case MouthExpression::O: return 4;
    case MouthExpression::Closed:
    default: return 0;
    }
}

uint32_t AnimeFace::RandomRange(uint32_t min_value, uint32_t max_value) {
    if (max_value <= min_value) return min_value;
    return min_value + static_cast<uint32_t>(std::rand()) % (max_value - min_value + 1);
}

int AnimeFace::Interpolate(int from, int to, uint32_t elapsed, uint32_t duration) {
    if (duration == 0 || elapsed >= duration) return to;
    return from + static_cast<int>((static_cast<int64_t>(to - from) * elapsed) / duration);
}

void AnimeFace::SetImage(lv_obj_t* object, const uint8_t* data, size_t size) {
    if (descriptor_count_ >= sizeof(descriptors_) / sizeof(descriptors_[0])) return;
    lv_img_dsc_t& dsc = descriptors_[descriptor_count_++];
    dsc = {};
    dsc.data_size = size;
    dsc.data = const_cast<uint8_t*>(data);
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    dsc.header.w = 0;
    dsc.header.h = 0;
    lv_image_set_src(object, &dsc);
}

void AnimeFace::SetImagePosition(lv_obj_t* object, int x, int y) {
    lv_obj_set_pos(object, x, y);
}

void AnimeFace::ApplyMouthVisibility() {
    for (int candidate = 0; candidate < kMouthFrameCount; ++candidate) {
        const bool show_normal_mouth = speech_mouth_rendering_ || winking_ ||
                                       special_mouth_ == SpecialMouth::None;
        const bool show = show_normal_mouth && candidate == mouth_frame_;
        if (show) {
            lv_obj_remove_flag(mouth_[candidate], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(mouth_[candidate], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (sad_mouth_ != nullptr) {
        if (!speech_mouth_rendering_ && special_mouth_ == SpecialMouth::Sad) {
            lv_obj_remove_flag(sad_mouth_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sad_mouth_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (angry_mouth_ != nullptr) {
        if (!speech_mouth_rendering_ && special_mouth_ == SpecialMouth::Angry) {
            lv_obj_remove_flag(angry_mouth_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(angry_mouth_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (crying_mouth_ != nullptr) {
        if (!speech_mouth_rendering_ && special_mouth_ == SpecialMouth::Crying) {
            lv_obj_remove_flag(crying_mouth_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(crying_mouth_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (open_smile_mouth_ != nullptr) {
        if (!speech_mouth_rendering_ && special_mouth_ == SpecialMouth::OpenSmile) {
            lv_obj_remove_flag(open_smile_mouth_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(open_smile_mouth_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (tongue_mouth_ != nullptr) {
        if (!speech_mouth_rendering_ && special_mouth_ == SpecialMouth::Tongue) {
            lv_obj_remove_flag(tongue_mouth_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tongue_mouth_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (yawn_mouth_ != nullptr) {
        if (!speech_mouth_rendering_ && special_mouth_ == SpecialMouth::Yawn) {
            lv_obj_remove_flag(yawn_mouth_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(yawn_mouth_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AnimeFace::ApplyEyeVisibility() {
    const bool show_smile = eye_smile_visible_ && !blinking_ && !winking_ &&
                            reaction_state_ == ReactionState::None;
    for (int eye = 0; eye < 2; ++eye) {
        for (int candidate = 0; candidate < kEyelidFrameCount; ++candidate) {
            const bool show = !show_smile && candidate == blink_frame_;
            if (show) {
                lv_obj_remove_flag(eyelid_[eye][candidate], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(eyelid_[eye][candidate], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (eye_smile_[eye] != nullptr) {
            if (show_smile) {
                lv_obj_remove_flag(eye_smile_[eye], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(eye_smile_[eye], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void AnimeFace::SetMouthFrame(int frame) {
    mouth_frame_ = std::clamp(frame, 0, kMouthFrameCount - 1);
    ApplyMouthVisibility();
}

void AnimeFace::SetTearsVisible(bool visible) {
    if (visible && !tears_visible_) {
        tear_motion_started_ms_ = lv_tick_get();
    }
    tears_visible_ = visible;
    for (int eye = 0; eye < 2; ++eye) {
        lv_obj_t* tear = tear_[eye];
        if (tear == nullptr) continue;
        if (visible) {
            lv_obj_remove_flag(tear, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tear, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(tear, kTearY);
        }
    }
}

void AnimeFace::SetFaceOverlays(bool blush, bool heart, bool irritation, bool sparkle,
                                bool sweat) {
    blush_visible_ = blush;
    heart_visible_ = heart;
    irritation_visible_ = irritation;
    sparkle_visible_ = sparkle;
    sweat_visible_ = sweat;
    for (lv_obj_t* object : blush_) {
        if (object == nullptr) continue;
        if (blush_visible_) {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (lv_obj_t* object : sweat_) {
        if (object == nullptr) continue;
        if (sweat_visible_) {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }
    const std::pair<lv_obj_t*, bool> singles[] = {
        {heart_, heart_visible_}, {irritation_, irritation_visible_},
        {sparkle_, sparkle_visible_},
    };
    for (const auto& item : singles) {
        if (item.first == nullptr) continue;
        if (item.second) {
            lv_obj_remove_flag(item.first, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(item.first, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AnimeFace::UpdateTears(uint32_t now_ms) {
    if (!tears_visible_) return;

    for (int eye = 0; eye < 2; ++eye) {
        if (tear_[eye] == nullptr) continue;
        const uint32_t phase =
            (now_ms - tear_motion_started_ms_ + eye * kTearPhaseOffsetMs) % kTearPeriodMs;
        const uint32_t half_period = kTearPeriodMs / 2;
        const uint32_t distance = phase <= half_period ? phase : kTearPeriodMs - phase;
        const int offset = static_cast<int>((distance * kTearTravelPx) / half_period);
        lv_obj_set_y(tear_[eye], kTearY + offset);
    }
}

void AnimeFace::SetWinkMouthPose(int offset_x, int angle_degrees) {
    // The closed-mouth sprite is a persistent image; move and rotate it around
    // its own center so the smile leans naturally toward the closing eye.
    lv_obj_set_pos(mouth_[0], kMouthX + offset_x, kMouthY);
    lv_image_set_pivot(mouth_[0], 17, 11);
    lv_image_set_rotation(mouth_[0], angle_degrees * 10);
}

void AnimeFace::SetBrowPose(ApplicationState state) {
    int left_x = expression_.left_brow_x;
    int left_y = expression_.left_brow_y;
    int right_x = expression_.right_brow_x;
    int right_y = expression_.right_brow_y;
    // Preserve the existing application-state cues only for the neutral
    // expression. Explicit emotions own their brow parameters.
    if (emotion_ == FaceEmotion::Neutral) {
        switch (state) {
        case ApplicationState::Listening:
            --left_y;
            --right_y;
            break;
        case ApplicationState::Thinking:
            --left_y;
            ++right_y;
            break;
        case ApplicationState::Speaking:
            ++left_y;
            ++right_y;
            break;
        case ApplicationState::Idle:
            break;
        }
    }
    brow_left_x_target_ = left_x;
    brow_left_y_target_ = left_y;
    brow_right_x_target_ = right_x;
    brow_right_y_target_ = right_y;
}

void AnimeFace::UpdateBrowTransition() {
    auto approach = [](int current, int target) {
        if (current == target) return current;
        const int delta = target - current;
        const int step = std::max(1, std::abs(delta) / 2);
        return current + (delta > 0 ? step : -step);
    };
    brow_left_x_current_ = approach(brow_left_x_current_, brow_left_x_target_);
    brow_left_y_current_ = approach(brow_left_y_current_, brow_left_y_target_);
    brow_right_x_current_ = approach(brow_right_x_current_, brow_right_x_target_);
    brow_right_y_current_ = approach(brow_right_y_current_, brow_right_y_target_);
    SetImagePosition(brow_[0], kBrowX[0] + brow_left_x_current_,
                     kBrowY + brow_left_y_current_);
    SetImagePosition(brow_[1], kBrowX[1] + brow_right_x_current_,
                     kBrowY + brow_right_y_current_);
}

bool AnimeFace::Initialize(lv_obj_t* parent, int screen_width, int screen_height) {
    if (parent == nullptr) return false;
    descriptor_count_ = 0;
    screen_width_ = screen_width;
    screen_height_ = screen_height;
    emotion_ = FaceEmotion::Neutral;
    expression_ = ExpressionFor(emotion_);
    expression_mouth_frame_ = ExpressionMouthFrame();
    base_eyelid_frame_ = expression_.base_eyelid_frame;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, kCanvasWidth, kCanvasHeight);
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(root_, LV_ALIGN_CENTER, 0, 0);

    base_ = lv_image_create(root_);
    SetImage(base_, kBase.data, kBase.size);
    SetImagePosition(base_, 0, 0);

    for (int eye = 0; eye < 2; ++eye) {
        eye_white_[eye] = lv_image_create(root_);
        SetImage(eye_white_[eye], kEyeWhite[eye].data, kEyeWhite[eye].size);
        SetImagePosition(eye_white_[eye], kEyeWhiteX[eye], kEyeWhiteY);

        // The manifest gives the iris a 5,3 transparent padding. A fixed eye
        // rectangle is the safe clipping region when no alpha mask API is
        // available in the current LVGL build.
        iris_clip_[eye] = lv_obj_create(root_);
        lv_obj_set_size(iris_clip_[eye], kEyeWhiteW, kEyeWhiteH);
        lv_obj_set_pos(iris_clip_[eye], kEyeWhiteX[eye], kEyeWhiteY);
        lv_obj_set_style_bg_opa(iris_clip_[eye], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(iris_clip_[eye], 0, 0);
        lv_obj_set_style_pad_all(iris_clip_[eye], 0, 0);
        lv_obj_clear_flag(iris_clip_[eye], LV_OBJ_FLAG_SCROLLABLE);

        iris_[eye] = lv_image_create(iris_clip_[eye]);
        SetImage(iris_[eye], kIris[eye].data, kIris[eye].size);
        SetImagePosition(iris_[eye], kIrisX[eye] - kEyeWhiteX[eye], kIrisY - kEyeWhiteY);

        for (int frame = 0; frame < kEyelidFrameCount; ++frame) {
            eyelid_[eye][frame] = lv_image_create(root_);
            SetImage(eyelid_[eye][frame], kEyelid[eye][frame].data, kEyelid[eye][frame].size);
            SetImagePosition(eyelid_[eye][frame], kEyelidX[eye], kEyelidY);
        }

        eye_smile_[eye] = lv_image_create(root_);
        SetImage(eye_smile_[eye], kEyeSmile[eye].data, kEyeSmile[eye].size);
        SetImagePosition(eye_smile_[eye], kEyelidX[eye], kEyelidY);

        brow_[eye] = lv_image_create(root_);
        SetImage(brow_[eye], kBrow[eye].data, kBrow[eye].size);
        SetImagePosition(brow_[eye], kBrowX[eye], kBrowY);
    }

    for (int frame = 0; frame < kMouthFrameCount; ++frame) {
        mouth_[frame] = lv_image_create(root_);
        SetImage(mouth_[frame], kMouth[frame].data, kMouth[frame].size);
        SetImagePosition(mouth_[frame], kMouthX, kMouthY);
    }

    sad_mouth_ = lv_image_create(root_);
    SetImage(sad_mouth_, kSpecialMouth[0].data, kSpecialMouth[0].size);
    SetImagePosition(sad_mouth_, kMouthX, kMouthY);
    angry_mouth_ = lv_image_create(root_);
    SetImage(angry_mouth_, kSpecialMouth[1].data, kSpecialMouth[1].size);
    SetImagePosition(angry_mouth_, kMouthX, kMouthY);
    crying_mouth_ = lv_image_create(root_);
    SetImage(crying_mouth_, kSpecialMouth[2].data, kSpecialMouth[2].size);
    SetImagePosition(crying_mouth_, kMouthX, kMouthY);
    open_smile_mouth_ = lv_image_create(root_);
    SetImage(open_smile_mouth_, kSpecialMouth[3].data, kSpecialMouth[3].size);
    SetImagePosition(open_smile_mouth_, kMouthX, kMouthY);
    tongue_mouth_ = lv_image_create(root_);
    SetImage(tongue_mouth_, kSpecialMouth[4].data, kSpecialMouth[4].size);
    SetImagePosition(tongue_mouth_, kMouthX, kMouthY);
    yawn_mouth_ = lv_image_create(root_);
    SetImage(yawn_mouth_, kSpecialMouth[5].data, kSpecialMouth[5].size);
    SetImagePosition(yawn_mouth_, kMouthX, kMouthY);

    for (int eye = 0; eye < 2; ++eye) {
        tear_[eye] = lv_image_create(root_);
        SetImage(tear_[eye], kTear[eye].data, kTear[eye].size);
        SetImagePosition(tear_[eye], kTearX[eye], kTearY);
        blush_[eye] = lv_image_create(root_);
        SetImage(blush_[eye], kBlush[eye].data, kBlush[eye].size);
        SetImagePosition(blush_[eye], kBlushX[eye], kBlushY);
        sweat_[eye] = lv_image_create(root_);
        SetImage(sweat_[eye], kSweat[eye].data, kSweat[eye].size);
        SetImagePosition(sweat_[eye], kSweatX[eye], kSweatY);
    }

    heart_ = lv_image_create(root_);
    SetImage(heart_, kHeart.data, kHeart.size);
    SetImagePosition(heart_, kHeartX, kHeartY);
    irritation_ = lv_image_create(root_);
    SetImage(irritation_, kIrritation.data, kIrritation.size);
    SetImagePosition(irritation_, kIrritationX, kIrritationY);
    sparkle_ = lv_image_create(root_);
    SetImage(sparkle_, kSparkle.data, kSparkle.size);
    SetImagePosition(sparkle_, kSparkleX, kSparkleY);

    eye_smile_visible_ = false;
    SetEyelidFrame(0);
    special_mouth_ = SpecialMouth::None;
    SetTearsVisible(false);
    SetFaceOverlays(false, false, false, false, false);
    SetMouthFrame(0);
    SetBrowPose(ApplicationState::Idle);
    UpdateBrowTransition();
    SetGaze(0);
    gaze_hold_until_ms_ = lv_tick_get() + RandomRange(1200, 3500);
    next_blink_ms_ = lv_tick_get() + RandomRange(3000, 7000);
    next_mouth_ms_ = lv_tick_get();
#if CONFIG_FNK_ANIME_FACE_EMOTION_DEMO
    demo_emotion_index_ = 0;
    demo_next_ms_ = lv_tick_get();
#endif
    initialized_ = true;
    ESP_LOGI(TAG, "ANIME_FACE_INIT canvas=%dx%d screen=%dx%d assets=embedded layers=persistent",
             kCanvasWidth, kCanvasHeight, screen_width_, screen_height_);
    LogMemory("after_init");
    return true;
}

void AnimeFace::SetNeutral() {
    if (!initialized_) return;
    emotion_ = FaceEmotion::Neutral;
    expression_ = ExpressionFor(emotion_);
    expression_mouth_frame_ = ExpressionMouthFrame();
    base_eyelid_frame_ = expression_.base_eyelid_frame;
    special_mouth_ = SpecialMouth::None;
    eye_smile_visible_ = false;
    ApplyEyeVisibility();
    SetTearsVisible(false);
    SetFaceOverlays(false, false, false, false, false);
    application_state_ = ApplicationState::Idle;
    mouth_animating_ = false;
    speech_level_target_ = 0;
    speech_level_current_ = 0;
    speech_mouth_rendering_ = false;
    SetMouthFrame(expression_mouth_frame_);
    SetBrowPose(application_state_);
    UpdateBrowTransition();
    SetEyelidFrame(0);
    SetGaze(gaze_x_);
}

void AnimeFace::SetApplicationState(ApplicationState state) {
    if (!initialized_ || state == application_state_) return;
    const ApplicationState previous = application_state_;
    application_state_ = state;
    const char* names[] = {"IDLE", "LISTENING", "THINKING", "SPEAKING"};
    ESP_LOGI(TAG, "ANIME_FACE_STATE: %s", names[static_cast<int>(state)]);
    if (state == ApplicationState::Speaking) {
        speech_state_enter_ms_ = lv_tick_get();
        speech_level_target_ = 0;
        speech_level_current_ = 0;
        speech_pcm_seen_ = false;
        speech_silence_logged_ = false;
        mouth_open_logged_ = false;
        mouth_animating_ = true;
        speech_mouth_rendering_ = false;
        next_mouth_ms_ = speech_state_enter_ms_;
        SetMouthFrame(expression_mouth_frame_);
        ESP_LOGI(TAG, "SPEAKING_STATE_ENTER t=%u",
                 static_cast<unsigned>(speech_state_enter_ms_));
    } else if (previous == ApplicationState::Speaking) {
        mouth_animating_ = false;
        speech_level_target_ = 0;
        speech_level_current_ = 0;
        const bool was_speech_mouth_rendering = speech_mouth_rendering_;
        speech_mouth_rendering_ = false;
        if (speech_pcm_seen_) {
            ESP_LOGI(TAG, "LAST_SPEECH_PCM t=%u last_update=%u",
                     static_cast<unsigned>(lv_tick_get()),
                     static_cast<unsigned>(last_speech_pcm_ms_));
        }
        SetMouthFrame(expression_mouth_frame_);
        if (was_speech_mouth_rendering) {
            ESP_LOGI(TAG, "MOUTH_CLOSED t=%u", static_cast<unsigned>(lv_tick_get()));
            ESP_LOGI(TAG, "ANIME_FACE_MOUTH: stop");
        }
    }
    SetBrowPose(state);
    gaze_target_x_ = state == ApplicationState::Thinking ? 2 : expression_.gaze_x;
    gaze_started_ms_ = lv_tick_get();
    gaze_transition_ms_ = 180;
    gaze_hold_until_ms_ = gaze_started_ms_ + 900;
}

void AnimeFace::SetEmotion(FaceEmotion emotion) {
    if (!initialized_) return;
    if (emotion_ == emotion) return;
    emotion_ = emotion;
    expression_ = ExpressionFor(emotion_);
    expression_mouth_frame_ = ExpressionMouthFrame();
    base_eyelid_frame_ = expression_.base_eyelid_frame;
    if (emotion_ == FaceEmotion::Sad || emotion_ == FaceEmotion::Worried ||
        emotion_ == FaceEmotion::Exhausted) {
        special_mouth_ = SpecialMouth::Sad;
    } else if (emotion_ == FaceEmotion::Crying || emotion_ == FaceEmotion::Nervous) {
        special_mouth_ = SpecialMouth::Crying;
    } else if (emotion_ == FaceEmotion::Angry) {
        special_mouth_ = SpecialMouth::Angry;
    } else if (emotion_ == FaceEmotion::Happy || emotion_ == FaceEmotion::Laughing ||
               emotion_ == FaceEmotion::Love) {
        special_mouth_ = SpecialMouth::OpenSmile;
    } else if (emotion_ == FaceEmotion::Playful) {
        special_mouth_ = SpecialMouth::Tongue;
    } else if (emotion_ == FaceEmotion::Yawn) {
        special_mouth_ = SpecialMouth::Yawn;
    } else {
        special_mouth_ = SpecialMouth::None;
    }
    eye_smile_visible_ = emotion_ == FaceEmotion::Laughing || emotion_ == FaceEmotion::Love;
    ApplyEyeVisibility();
    SetTearsVisible(emotion_ == FaceEmotion::Crying);
    SetFaceOverlays(emotion_ == FaceEmotion::Laughing || emotion_ == FaceEmotion::Love ||
                        emotion_ == FaceEmotion::Shy,
                    emotion_ == FaceEmotion::Love, emotion_ == FaceEmotion::Angry,
                    emotion_ == FaceEmotion::Happy || emotion_ == FaceEmotion::Laughing ||
                        emotion_ == FaceEmotion::Playful,
                    emotion_ == FaceEmotion::Nervous || emotion_ == FaceEmotion::Scared);
    const char* names[] = {"NEUTRAL", "HAPPY", "SAD", "ANGRY", "SURPRISED", "SLEEPY",
                           "CRYING", "WORRIED", "SCARED", "FOCUSED", "CONFUSED", "SHY",
                           "EXHAUSTED", "LAUGHING", "LOVE", "NERVOUS", "PLAYFUL", "YAWN"};
    ESP_LOGI(TAG, "ANIME_EMOTION %s", names[static_cast<int>(emotion_)]);
    SetBrowPose(application_state_);
    gaze_target_x_ = application_state_ == ApplicationState::Thinking ? 2 : expression_.gaze_x;
    gaze_started_ms_ = lv_tick_get();
    gaze_transition_ms_ = 160;
    gaze_hold_until_ms_ = gaze_started_ms_ + 900;
    if (!blinking_) SetEyelidFrame(base_eyelid_frame_);
    if (!speech_mouth_rendering_ &&
        (application_state_ != ApplicationState::Speaking || speech_level_current_ <= 7)) {
        SetMouthFrame(expression_mouth_frame_);
    }
    if (emotion_ == FaceEmotion::Sleepy) {
        next_blink_ms_ = UINT32_MAX;
    } else {
        next_blink_ms_ = lv_tick_get() + RandomRange(3000, 7000);
    }
}

void AnimeFace::ForceBlink() {
    if (!initialized_) return;
    StartBlink(lv_tick_get());
}

void AnimeFace::PlayWink(bool left_eye, uint32_t duration_ms) {
    if (!initialized_) return;

    wink_left_eye_ = left_eye;
    wink_started_ms_ = lv_tick_get();
    wink_duration_ms_ = std::clamp<uint32_t>(duration_ms, 260, 1200);
    winking_ = true;
    blinking_ = false;
    next_blink_ms_ = wink_started_ms_ + wink_duration_ms_ + RandomRange(3000, 7000);

    const int wink_eye = wink_left_eye_ ? 0 : 1;
    const int open_eye = wink_left_eye_ ? 1 : 0;
    SetEyelidFrameForEye(wink_eye, 0);
    SetEyelidFrameForEye(open_eye, base_eyelid_frame_);
    if (application_state_ != ApplicationState::Speaking && !speech_mouth_rendering_) {
        SetMouthFrame(0);
        SetWinkMouthPose(0, 0);
    }
    ESP_LOGI(TAG, "ANIME_FACE_WINK start eye=%s duration_ms=%u",
             wink_left_eye_ ? "left" : "right", static_cast<unsigned>(wink_duration_ms_));
}

void AnimeFace::PlayReaction(FaceReaction reaction, FaceReactionSource source) {
    if (!initialized_ || reaction != FaceReaction::WakeAttention) return;

    const uint32_t now_ms = lv_tick_get();
    reaction_source_ = source;
    reaction_state_ = ReactionState::Notice;
    reaction_started_ms_ = now_ms;
    reaction_phase_started_ms_ = now_ms;
    reaction_gaze_before_ = gaze_x_;
    reaction_center_logged_ = false;
    reaction_mouth_logged_ = false;
    reaction_blink_was_active_ = blinking_;
    gaze_target_x_ = 0;
    gaze_started_ms_ = now_ms;
    gaze_transition_ms_ = std::clamp<uint32_t>(
        100 + static_cast<uint32_t>(std::abs(reaction_gaze_before_)) * 15, 100, 160);
    gaze_hold_until_ms_ = UINT32_MAX;
    // Do not start a second blink. If one is already in progress, UpdateReaction
    // lets it finish before the attention pose takes over.
    if (!blinking_) {
        SetEyelidFrame(0);
    }

    const char* source_name = "UNKNOWN";
    if (source == FaceReactionSource::Touch) source_name = "TOUCH";
    else if (source == FaceReactionSource::WakeWord) source_name = "WAKE_WORD";
    ESP_LOGI(TAG, "ANIME_WAKE START source=%s gaze_before=%d", source_name,
             reaction_gaze_before_);
}

void AnimeFace::FinishReaction(uint32_t now_ms) {
    reaction_state_ = ReactionState::None;
    reaction_source_ = FaceReactionSource::Unknown;
    reaction_center_logged_ = false;
    reaction_mouth_logged_ = false;
    SetBrowPose(application_state_);
    UpdateBrowTransition();
    if (!blinking_) SetEyelidFrame(base_eyelid_frame_);

    // Listening owns a centered gaze. Idle may resume its random gaze only after
    // a short hold, so the attention reaction does not snap away on its last frame.
    gaze_target_x_ = application_state_ == ApplicationState::Listening
                         ? 0
                         : (application_state_ == ApplicationState::Thinking ? 2
                                                                              : expression_.gaze_x);
    gaze_started_ms_ = now_ms;
    gaze_transition_ms_ = application_state_ == ApplicationState::Listening ? 0 : 160;
    gaze_hold_until_ms_ = now_ms + 1200;
    if (!speech_mouth_rendering_ && application_state_ != ApplicationState::Speaking) {
        SetMouthFrame(expression_mouth_frame_);
    }
    const char* state_names[] = {"IDLE", "LISTENING", "THINKING", "SPEAKING"};
    ESP_LOGI(TAG, "ANIME_WAKE END base=%s", state_names[static_cast<int>(application_state_)]);
}

void AnimeFace::UpdateReaction(uint32_t now_ms) {
    if (reaction_state_ == ReactionState::None) return;

    if (blinking_) {
        UpdateBlink(now_ms);
        if (blinking_) return;
        // A pre-existing blink has completed. Start the notice timing now so the
        // reaction still presents a full, readable attention pose.
        reaction_phase_started_ms_ = now_ms;
        reaction_started_ms_ = now_ms;
        reaction_state_ = ReactionState::Notice;
        SetEyelidFrame(0);
    }

    const uint32_t elapsed = now_ms - reaction_started_ms_;
    if (elapsed < 140) {
        reaction_state_ = ReactionState::Notice;
        SetGaze(Interpolate(reaction_gaze_before_, 0, elapsed,
                            std::max<uint32_t>(100, gaze_transition_ms_)));
        SetEyelidFrame(0);
        return;
    }

    if (!reaction_center_logged_) {
        reaction_center_logged_ = true;
        SetGaze(0);
        ESP_LOGI(TAG, "ANIME_WAKE CENTERED");
    }

    if (elapsed < 400) {
        reaction_state_ = ReactionState::Attention;
        SetGaze(0);
        // A barely raised attention brow, deliberately much softer than Surprised.
        brow_left_x_target_ = 0;
        brow_right_x_target_ = 0;
        brow_left_y_target_ = -1;
        brow_right_y_target_ = -1;
        UpdateBrowTransition();
        if (!speech_mouth_rendering_) {
            SetMouthFrame(1);
            if (!reaction_mouth_logged_) {
                reaction_mouth_logged_ = true;
                ESP_LOGI(TAG, "ANIME_WAKE MOUTH_SMALL");
            }
        }
        return;
    }

    if (elapsed < 550) {
        reaction_state_ = ReactionState::Settle;
        SetGaze(0);
        if (!speech_mouth_rendering_) SetMouthFrame(0);
        return;
    }

    if (elapsed < 750) {
        reaction_state_ = ReactionState::Settle;
        SetGaze(0);
        SetBrowPose(application_state_);
        UpdateBrowTransition();
        if (!speech_mouth_rendering_) SetMouthFrame(0);
        return;
    }

    FinishReaction(now_ms);
}

void AnimeFace::SetSpeechLevel(uint8_t level, uint32_t now_ms) {
    speech_level_target_ = std::min<uint8_t>(level, 100);
    if (application_state_ != ApplicationState::Speaking) return;
    if (speech_level_target_ > 0) {
        last_speech_pcm_ms_ = now_ms;
        if (!speech_pcm_seen_) {
            speech_pcm_seen_ = true;
            speech_silence_logged_ = false;
            ESP_LOGI(TAG, "FIRST_SPEECH_PCM t=%u level=%u", static_cast<unsigned>(now_ms),
                     static_cast<unsigned>(speech_level_target_));
        }
    } else if (speech_pcm_seen_ && !speech_silence_logged_) {
        speech_silence_logged_ = true;
        ESP_LOGI(TAG, "LAST_SPEECH_PCM t=%u last_update=%u",
                 static_cast<unsigned>(now_ms), static_cast<unsigned>(last_speech_pcm_ms_));
    }
}

void AnimeFace::SetGaze(int x) {
    gaze_x_ = std::clamp(x, -4, 4);
    for (int eye = 0; eye < 2; ++eye) {
        lv_obj_set_x(iris_[eye], kIrisX[eye] - kEyeWhiteX[eye] + gaze_x_);
    }
}

void AnimeFace::SetEyelidFrame(int frame) {
    frame = std::clamp(frame, 0, kEyelidFrameCount - 1);
    blink_frame_ = frame;
    ApplyEyeVisibility();
}

void AnimeFace::SetEyelidFrameForEye(int eye, int frame) {
    if (eye < 0 || eye > 1) return;
    frame = std::clamp(frame, 0, kEyelidFrameCount - 1);
    blink_frame_ = frame;
    for (int candidate = 0; candidate < kEyelidFrameCount; ++candidate) {
        // During a wink this helper is called for each eye explicitly. Keep
        // the requested frame visible even though the global winking_ flag is
        // set; suppressing it here made both eyelids disappear.
        const bool explicit_wink_frame = winking_;
        if ((explicit_wink_frame ||
             (!eye_smile_visible_ && !blinking_ &&
              reaction_state_ == ReactionState::None)) &&
            candidate == frame) {
            lv_obj_remove_flag(eyelid_[eye][candidate], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(eyelid_[eye][candidate], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (eye_smile_[eye] != nullptr) {
        if (eye_smile_visible_ && !blinking_ && !winking_ &&
            reaction_state_ == ReactionState::None) {
            lv_obj_remove_flag(eye_smile_[eye], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(eye_smile_[eye], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AnimeFace::StartBlink(uint32_t now_ms) {
    blinking_ = true;
    blink_started_ms_ = now_ms;
    blink_frame_ = base_eyelid_frame_;
    SetEyelidFrame(base_eyelid_frame_);
    ESP_LOGI(TAG, "ANIME_FACE_BLINK start gaze_x=%d", gaze_x_);
}

void AnimeFace::UpdateGaze(uint32_t now_ms) {
    if (blinking_) return;
    if (gaze_transition_ms_ != 0) {
        const uint32_t elapsed = now_ms - gaze_started_ms_;
        SetGaze(Interpolate(gaze_x_, gaze_target_x_, elapsed, gaze_transition_ms_));
        if (elapsed >= gaze_transition_ms_) {
            gaze_transition_ms_ = 0;
            if (application_state_ == ApplicationState::Idle) {
                gaze_hold_until_ms_ = now_ms + RandomRange(1200, 3500);
            } else if (application_state_ == ApplicationState::Thinking) {
                gaze_hold_until_ms_ = now_ms + RandomRange(900, 1600);
            } else {
                gaze_hold_until_ms_ = now_ms + RandomRange(900, 1800);
            }
        }
        return;
    }
    if (now_ms < gaze_hold_until_ms_) return;
    if (application_state_ == ApplicationState::Listening) {
        gaze_target_x_ = 0;
    } else if (application_state_ == ApplicationState::Thinking) {
        gaze_target_x_ = (gaze_target_x_ >= 0) ? -2 : 2;
    } else if (application_state_ == ApplicationState::Speaking) {
        gaze_target_x_ = expression_.gaze_x;
    } else if (emotion_ == FaceEmotion::Surprised || emotion_ == FaceEmotion::Scared ||
               emotion_ == FaceEmotion::Angry || emotion_ == FaceEmotion::Sad ||
               emotion_ == FaceEmotion::Crying || emotion_ == FaceEmotion::Worried ||
               emotion_ == FaceEmotion::Focused || emotion_ == FaceEmotion::Confused ||
               emotion_ == FaceEmotion::Shy || emotion_ == FaceEmotion::Exhausted ||
               emotion_ == FaceEmotion::Nervous || emotion_ == FaceEmotion::Yawn) {
        gaze_target_x_ = expression_.gaze_x;
    } else if (emotion_ == FaceEmotion::Happy || emotion_ == FaceEmotion::Laughing ||
               emotion_ == FaceEmotion::Love || emotion_ == FaceEmotion::Playful) {
        gaze_target_x_ = static_cast<int>(RandomRange(3, 5)) - 4;
    } else if (emotion_ == FaceEmotion::Sleepy) {
        gaze_target_x_ = static_cast<int>(RandomRange(3, 5)) - 4;
    } else {
        gaze_target_x_ = static_cast<int>(RandomRange(0, 8)) - 4;
    }
    gaze_started_ms_ = now_ms;
    gaze_transition_ms_ = (emotion_ == FaceEmotion::Sleepy || emotion_ == FaceEmotion::Yawn ||
                           emotion_ == FaceEmotion::Exhausted)
                              ? RandomRange(220, 320)
                              : RandomRange(100, 220);
    ESP_LOGI(TAG, "ANIME_FACE_GAZE target_x=%d transition_ms=%u", gaze_target_x_,
             static_cast<unsigned>(gaze_transition_ms_));
}

void AnimeFace::UpdateMouth(uint32_t now_ms) {
    if (!mouth_animating_ || application_state_ != ApplicationState::Speaking) return;

    // Smooth the cheap PCM envelope with integer attack/release. The audio
    // task publishes zero after its stale window, so a real pause closes the
    // mouth without relying on a guessed speaking timeout.
    if (speech_level_target_ > speech_level_current_) {
        const uint8_t delta = static_cast<uint8_t>(speech_level_target_ - speech_level_current_);
        speech_level_current_ = static_cast<uint8_t>(speech_level_current_ + std::max<uint8_t>(1, delta / 2));
    } else if (speech_level_target_ < speech_level_current_) {
        const uint8_t delta = static_cast<uint8_t>(speech_level_current_ - speech_level_target_);
        speech_level_current_ = static_cast<uint8_t>(speech_level_current_ - std::max<uint8_t>(1, delta / 3));
    }

    if (now_ms < next_mouth_ms_) return;
    constexpr uint8_t kOpenThreshold = 12;
    constexpr uint8_t kCloseThreshold = 7;
    const bool speech_frame_hold = speech_level_target_ > 0 || speech_level_current_ > kCloseThreshold;
    const bool speech_rendering = speech_frame_hold &&
                                  (speech_level_current_ >= kOpenThreshold ||
                                   speech_level_target_ >= kOpenThreshold);
    int desired_frame = expression_mouth_frame_;
    if (speech_rendering) {
        if (speech_level_current_ >= 66) {
            desired_frame = 3;
        } else if (speech_level_current_ >= 36) {
            desired_frame = 2;
        } else {
            desired_frame = 1;
        }
    }
    const bool was_speech_mouth_rendering = speech_mouth_rendering_;
    if (speech_mouth_rendering_ && !speech_rendering) {
        ESP_LOGI(TAG, "MOUTH_CLOSED t=%u", static_cast<unsigned>(now_ms));
        ESP_LOGI(TAG, "ANIME_FACE_MOUTH: stop");
        speech_mouth_rendering_ = false;
    }
    if (speech_rendering) speech_mouth_rendering_ = true;
    if (was_speech_mouth_rendering != speech_mouth_rendering_) ApplyMouthVisibility();
    if (desired_frame != mouth_frame_) {
        SetMouthFrame(desired_frame);
        if (speech_rendering && desired_frame != 0 && !mouth_open_logged_) {
            mouth_open_logged_ = true;
            ESP_LOGI(TAG, "MOUTH_FIRST_OPEN t=%u frame=%d level=%u",
                     static_cast<unsigned>(now_ms), desired_frame,
                     static_cast<unsigned>(speech_level_current_));
            ESP_LOGI(TAG, "ANIME_FACE_MOUTH: start");
        } else if (speech_rendering && desired_frame == 0) {
            ESP_LOGI(TAG, "MOUTH_CLOSED t=%u", static_cast<unsigned>(now_ms));
            ESP_LOGI(TAG, "ANIME_FACE_MOUTH: stop");
        }
    }
    next_mouth_ms_ = now_ms + 40;
}

void AnimeFace::UpdateBlink(uint32_t now_ms) {
    if (!blinking_) {
        if (emotion_ == FaceEmotion::Sleepy) return;
        if (now_ms >= next_blink_ms_) StartBlink(now_ms);
        return;
    }
    const uint32_t elapsed = now_ms - blink_started_ms_;
    int sequence_index = static_cast<int>(elapsed / 28);
    const int sequence_length = base_eyelid_frame_ == 2 ? 6 :
                                (base_eyelid_frame_ == 1 ? 8 : 10);
    if (sequence_index >= sequence_length) {
        blinking_ = false;
        SetEyelidFrame(base_eyelid_frame_);
        next_blink_ms_ = now_ms + RandomRange(3000, 7000);
        ESP_LOGI(TAG, "ANIME_FACE_BLINK complete gaze_x=%d next_ms=%u", gaze_x_,
                 static_cast<unsigned>(next_blink_ms_ - now_ms));
        return;
    }
    if (base_eyelid_frame_ == 2) {
        static constexpr int kSleepyBlink[] = {2, 3, 4, 4, 3, 2};
        SetEyelidFrame(kSleepyBlink[sequence_index]);
    } else if (base_eyelid_frame_ == 1) {
        static constexpr int kRelaxedBlink[] = {1, 2, 3, 4, 4, 3, 2, 1};
        SetEyelidFrame(kRelaxedBlink[sequence_index]);
    } else {
        SetEyelidFrame(kBlinkSequence[sequence_index]);
    }
}

void AnimeFace::UpdateWink(uint32_t now_ms) {
    if (!winking_) return;

    const uint32_t elapsed = now_ms - wink_started_ms_;
    const uint32_t close_ms = std::min<uint32_t>(120, wink_duration_ms_ / 3);
    const uint32_t open_ms = std::min<uint32_t>(160, wink_duration_ms_ / 3);
    const uint32_t hold_ms = wink_duration_ms_ - close_ms - open_ms;
    const int wink_eye = wink_left_eye_ ? 0 : 1;
    const int max_offset = wink_left_eye_ ? -3 : 3;
    const int max_angle = wink_left_eye_ ? -6 : 6;

    const bool mouth_available = application_state_ != ApplicationState::Speaking &&
                                 !speech_mouth_rendering_;
    if (mouth_available && mouth_frame_ != 0) SetMouthFrame(0);

    if (elapsed < close_ms) {
        const int frame = Interpolate(0, kEyelidFrameCount - 1, elapsed,
                                      std::max<uint32_t>(1, close_ms));
        SetEyelidFrameForEye(wink_eye, frame);
        if (mouth_available) {
            const int offset = Interpolate(0, max_offset, elapsed,
                                           std::max<uint32_t>(1, close_ms));
            const int angle = Interpolate(0, max_angle, elapsed,
                                          std::max<uint32_t>(1, close_ms));
            SetWinkMouthPose(offset, angle);
        }
        return;
    }
    if (elapsed < close_ms + hold_ms) {
        SetEyelidFrameForEye(wink_eye, kEyelidFrameCount - 1);
        if (mouth_available) SetWinkMouthPose(max_offset, max_angle);
        return;
    }
    if (elapsed < wink_duration_ms_) {
        const uint32_t opening_elapsed = elapsed - close_ms - hold_ms;
        const int frame = Interpolate(kEyelidFrameCount - 1, base_eyelid_frame_,
                                      opening_elapsed, std::max<uint32_t>(1, open_ms));
        SetEyelidFrameForEye(wink_eye, frame);
        if (mouth_available) {
            const int offset = Interpolate(max_offset, 0, opening_elapsed,
                                           std::max<uint32_t>(1, open_ms));
            const int angle = Interpolate(max_angle, 0, opening_elapsed,
                                          std::max<uint32_t>(1, open_ms));
            SetWinkMouthPose(offset, angle);
        }
        return;
    }

    SetEyelidFrameForEye(wink_eye, base_eyelid_frame_);
    SetWinkMouthPose(0, 0);
    winking_ = false;
    ApplyEyeVisibility();
    ApplyMouthVisibility();
    ESP_LOGI(TAG, "ANIME_FACE_WINK complete eye=%s", wink_left_eye_ ? "left" : "right");
}

void AnimeFace::UpdateEmotionDemo(uint32_t now_ms) {
#if CONFIG_FNK_ANIME_FACE_EMOTION_DEMO
    if (now_ms < demo_next_ms_) return;
    static constexpr FaceEmotion kDemoEmotions[] = {
        FaceEmotion::Neutral, FaceEmotion::Happy, FaceEmotion::Sad,
        FaceEmotion::Angry, FaceEmotion::Surprised, FaceEmotion::Sleepy,
        FaceEmotion::Crying, FaceEmotion::Worried, FaceEmotion::Scared,
        FaceEmotion::Focused, FaceEmotion::Confused, FaceEmotion::Shy,
        FaceEmotion::Exhausted, FaceEmotion::Laughing, FaceEmotion::Love,
        FaceEmotion::Nervous, FaceEmotion::Playful, FaceEmotion::Yawn,
    };
    const FaceEmotion next_emotion = kDemoEmotions[demo_emotion_index_];
    if (next_emotion == emotion_) {
        ESP_LOGI(TAG, "ANIME_EMOTION NEUTRAL");
    } else {
        SetEmotion(next_emotion);
    }
    demo_emotion_index_ = static_cast<uint8_t>((demo_emotion_index_ + 1) %
                                                (sizeof(kDemoEmotions) / sizeof(kDemoEmotions[0])));
    demo_next_ms_ = now_ms + 5000;
#else
    (void)now_ms;
#endif
}

void AnimeFace::Update(uint32_t now_ms) {
    if (!initialized_) return;
    if (first_update_) {
        first_update_ = false;
        LogMemory("first_update");
    }
    UpdateEmotionDemo(now_ms);
    if (reaction_state_ != ReactionState::None) {
        UpdateReaction(now_ms);
    } else {
        UpdateBrowTransition();
        UpdateGaze(now_ms);
        if (winking_) {
            UpdateWink(now_ms);
        } else {
            UpdateBlink(now_ms);
        }
    }
    UpdateTears(now_ms);
    UpdateMouth(now_ms);
}
