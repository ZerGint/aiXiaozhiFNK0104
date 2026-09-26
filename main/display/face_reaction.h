#ifndef FACE_REACTION_H
#define FACE_REACTION_H

#include <cstdint>

enum class FaceReaction : uint8_t {
    WakeAttention,
};

enum class FaceReactionSource : uint8_t {
    Unknown,
    Touch,
    WakeWord,
};

#endif
