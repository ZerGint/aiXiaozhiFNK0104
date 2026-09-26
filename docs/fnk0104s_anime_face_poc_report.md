# FNK0104S layered anime face PoC

## Scope

This PoC is isolated behind `CONFIG_FNK_ANIME_FACE_POC` (default `n`). With the option disabled, the existing RoboEyes canvas path and its 240x120 layout remain selected.

The supplied manifest was used without changing the PNGs: base `240x240` at
`(0,0)`; eye whites `41x38` at `(56,101)` and `(143,101)`; irises `41x42` at
`(60,99)` and `(139,99)` with the manifest's `(-4..+4, 0)` travel; eyelids
`58x56` at `(44,88)` and `(138,88)`; brows `56x23` at `(47,69)` and
`(137,69)`; and mouth frames `34x22` at `(103,155)`.

## Implemented

- Persistent LVGL image objects for the supplied 240x240 layered assets.
- Layer order: base, eye whites, clipped movable irises, one eyelid frame, mouth, brows.
- All five manifest mouth sprites are embedded as persistent LVGL image objects at
  the manifest anchor `(103,155)`; only the selected frame is visible.
- Blink animation uses eyelid frames 0..4..0 and preserves iris position.
- Idle gaze moves the irises only within the manifest travel range (`x=-4..4`, `y=0`).
- No per-frame allocation, PNG decode, framebuffer, or heavy expression system
  is used; mouth timing consumes only the existing lock-free PCM envelope.
- Rate-limited diagnostic markers: `ANIME_FACE_INIT`, `ANIME_FACE_GAZE`, `ANIME_FACE_BLINK`, `ANIME_FACE_MEM`.
- Chat subtitle bar is an opaque one-line strip over the lower face and remains
  independent of the face animation.
- The AnimeFace view now has a clickable face/panel hit area that forwards taps
  to `Application::ToggleChatState(true)`, which uses the same `OGG_POPUP`
  notification cue as wake-word activation.
- A compact opaque black 248x34 subtitle strip overlays the lower face. It is
  one line with circular scrolling and is shown while chat content is present.
- The experimental LVGL task stack is 12288 bytes when this PoC is enabled. This
  avoids the stack overflow triggered by LVGL's PNG image composition while
  leaving the production/default stack setting unchanged.

## Emotion system PoC

The emotion layer uses only the existing embedded sprites. Each sprite is a
persistent LVGL image object; changing an emotion selects a frame or moves a
brow/iris object and does not allocate or decode a new image.

| Layer | Existing assets | Runtime use |
| --- | --- | --- |
| Base | `base_240.png` (106079 B) | persistent background/face |
| Eye whites | left 263 B, right 283 B | persistent white eye layer |
| Irises | left 2859 B, right 2853 B | persistent clipped layer, X travel -4..+4 |
| Eyelids | 10 frames, 1884..2289 B | one visible frame per eye |
| Brows | left 1093 B, right 1128 B | persistent objects with small offsets |
| Mouths | closed 434 B, small 490 B, medium 721 B, wide 934 B, O 565 B | one visible expression/speech frame |

`FaceExpression` contains gaze X/Y, per-brow X/Y offsets, eyelid bias and
base frame, plus a mouth frame. The six supported emotions are:

| Emotion | Brows | Base eyelid | Expression mouth |
| --- | --- | ---: | --- |
| NEUTRAL | level | 0 | closed |
| HAPPY | slight upward | 0 | small |
| SAD | slight downward | 1 | closed |
| ANGRY | downward | 0 | small |
| SURPRISED | raised | 0 | O |
| SLEEPY | relaxed | 1 | closed |

Speech PCM has priority over the expression mouth. While fresh AI PCM is
above the existing hysteresis threshold, the mouth uses small/medium/wide
speech frames. When PCM becomes stale or speaking ends, the selected emotion
mouth is restored. Sleepy starts from eyelid frame 1 and suppresses cyclic
automatic blinks; a blink sequence, when explicitly started, still begins and
ends at that base frame. The supplied manifest has no Y iris travel, so
`gaze_y` remains zero until an asset with vertical travel is provided.

`CONFIG_FNK_ANIME_FACE_EMOTION_DEMO` defaults to OFF. When temporarily enabled
for a hardware check it cycles six emotions for five seconds each and emits
only transition markers of the form `ANIME_EMOTION ...`. The existing LLM
semantic-emotion callback is explicitly ignored in the AnimeFace PoC; the
typed `AnimeFace::SetEmotion(FaceEmotion)` API remains available for a future
explicit owner. The final worktree configuration is OFF.

Memory markers from the clean experimental run were approximately 214 KiB
internal free after initialization and 166 KiB at the first update (DMA free
207/159 KiB respectively); later application services account for the lower
runtime minimum. No per-frame allocations were introduced.

The demo image compiled successfully and was flashed app-only to COM13. The
42-second capture [`anime_face_com13_emotion_demo_runtime.log`](../anime_face_com13_emotion_demo_runtime.log)
contains NEUTRAL, HAPPY, SAD, ANGRY, SURPRISED and SLEEPY transition markers,
with no stack overflow, panic, reset or LVGL decoder error. Existing startup
callbacks can briefly request NEUTRAL during activation; the demo timer then
continues its five-second cycle. The final worktree configuration is demo OFF.
Visual spacing and brow/eyelid tuning still require the user's hardware review
and are marked `VISUAL_TUNING_REQUIRED`.

## WAKE_ATTENTION reaction

`FaceReaction::WakeAttention` is one non-blocking, timestamp-driven reaction
shared by the AnimeFace hit area and `Application::HandleWakeWordDetectedEvent()`.
The tap path starts it and calls `ToggleChatState(true)` immediately; the main
task then queues the same `OGG_POPUP` cue used by wake-word activation before
the listening path. WakeNet uses the same reaction API before the normal
conversation transition. There is no second task,
delay, allocation, or bitmap copy.

The timeline is approximately 0–140 ms gaze-to-center with fully open eyes,
140–400 ms centered attention pose with the existing SMALL mouth sprite,
400–550 ms SMALL-to-closed, and 550–750 ms brow settling before the current
application expression resumes. An in-progress blink finishes first; idle gaze
and new random blinks are suspended during the reaction. PCM speech mouth
rendering has priority over the reaction mouth. Repeated triggers restart the
same state machine.

Diagnostics are limited to `ANIME_WAKE START`, `ANIME_WAKE CENTERED`,
`ANIME_WAKE MOUTH_SMALL`, and `ANIME_WAKE END`. The post-flash capture
[`anime_face_com13_wake_attention_runtime.log`](../anime_face_com13_wake_attention_runtime.log)
shows clean AnimeFace thinking/idle, gaze, and blink paths without panic, reset,
or LVGL stack overflow. The follow-up tap validation capture confirms
`ANIME_FACE_TAP`, `ANIME_WAKE START source=TOUCH`, centered/mouth phases, and
`ANIME_WAKE END`; the state machine transitions `idle -> connecting -> listening`
and back to `idle` on a second tap.

## Winking emotion

AnimeFace now exposes `PlayWink(left_eye, duration_ms)`. It closes one eyelid
through the existing five persistent eyelid frames, holds it closed, and reopens
it while leaving the other eye at its base frame. The default right-eye wink is
500 ms; automatic symmetric blink remains unchanged. The AnimeFace semantic
emotion bridge recognizes `winking` and invokes this transient animation instead
of dropping the request with the other external emotion names.

During the wink, the closed-mouth smile follows the same easing: it shifts up to
3 px toward the closing eye and tilts up to 6°, then returns to its neutral pose
as the eyelid opens. AI PCM speech keeps priority and suppresses this mouth pose.

## Build result

Command:

```text
python scripts/build.py freenove-fnk0104s --name freenove-fnk0104s
```

Result: passed. The final demo-off reaction/wink/tap-sound build
`build/xiaozhi.bin` is `0x399630`; the smallest app partition reports 9% free
(`0x569d0` bytes). The
build used ESP-IDF 6.1.0 from the local PlatformIO installation.

The production defaults intentionally keep `CONFIG_FNK_ANIME_FACE_POC=n`; for
the experimental image the flag was enabled in `sdkconfig` before the final
`ninja -C build` rebuild.

## Earlier PoC validation

The initial experimental image was flashed app-only to COM13 and run for 45
seconds after the LVGL stack fix. That capture established the persistent-layer
compositor and clean idle blink/gaze path; the expressive validation below is
the current result.

## Functional face animation

The face observes the existing `Application::GetDeviceState()` from the
display timers, with the animation state refreshed on the 30 Hz face timer. It
does not start, stop, or otherwise control a conversation. The mapping is:

| Application state | Face behavior |
| --- | --- |
| IDLE | Neutral mouth, normal randomized gaze and blink |
| LISTENING | Neutral mouth, centered gaze, attentive brow offset |
| CONNECTING/ACTIVATING | Thinking pose, alternating side gaze, neutral mouth |
| SPEAKING | Fresh AI speech PCM envelope mapped to mouth frames, centered gaze, normal blink |

The asset set also includes persistent `mouth_sad.png`, `mouth_angry.png`,
`tears/tear_left.png`, and `tears/tear_right.png` overlays. `sad` selects the
sad mouth, `angry` selects the angry mouth, and `crying` selects the sad mouth
with both tears visible. These semantic names are now routed to AnimeFace; PCM
speech still temporarily takes priority over expression mouths.

Speaking only enables the mouth driver. `AudioService` publishes a lock-free
0–100 mean-absolute AI speech PCM level from the output task; the display reads
that value at about 30 Hz. A stale or zero level keeps the mouth closed. Integer
attack/release smoothing and 12/7 open/close hysteresis map fresh speech to
`closed`, `small`, `medium`, or `wide`; the `o` sprite is not used as a loudness
shortcut. Notification, SD music, and radio output are gated out. Brows use only
the supplied sprites and the manifest's ±1 px vertical offsets.

Diagnostic markers are emitted on state/animation changes:
`ANIME_FACE_STATE: IDLE/LISTENING/THINKING/SPEAKING` and
`ANIME_FACE_MOUTH: start/stop`; timing markers include `SPEAKING_STATE_ENTER`,
`FIRST_SPEECH_PCM`, `MOUTH_FIRST_OPEN`, `LAST_SPEECH_PCM`, and `MOUTH_CLOSED`.

## Hardware validation

- **Build:** PASS — `xiaozhi.bin` size `0x39A590`; smallest app partition has
  8% free.
- **Flash:** PASS — app-only write to COM13 at `0x20000`, esptool hash verified.
- **Boot and idle/activation animation:** PASS —
  [`anime_face_com13_expressive_runtime.log`](../anime_face_com13_expressive_runtime.log)
  shows initialization, persistent asset memory, thinking>idle state changes,
  gaze transitions and repeated blink sequences with no stack overflow, panic,
  reset, or LVGL decoder error.
- **Listening/speaking mouth animation:** PENDING physical interaction — the
  captured run did not include a tap/wake or server response, so the log has no
  fresh PCM timing markers yet.
- **Tap wake popup cue:** Implemented — `ToggleChatState(true)` queues the same
  `OGG_POPUP` sound used by wake-word activation. The boot capture
  [`anime_face_com13_tap_sound_runtime.log`](../anime_face_com13_tap_sound_runtime.log)
  is clean; the audible tap cue still requires a physical tap.

Required mouth-gating result fields:

```text
MOUTH_TRIGGER = ACTUAL_SPEECH_PCM
SPEAKING_TO_FIRST_PCM_MS = pending physical AI speech run
FIRST_PCM_TO_MOUTH_OPEN_MS = pending physical AI speech run
LAST_PCM_TO_MOUTH_CLOSE_MS = pending physical AI speech run
MUSIC_FALSE_MOUTH = YES (output source gated to AI speech)
RADIO_FALSE_MOUTH = YES (output source gated to AI speech)
PLAYBACK_TIMING_REGRESSION = NO observed in build/boot capture; physical speech playback check pending
```

## Known limitations

The mouth animation follows a cheap AI speech amplitude envelope; it is not
phoneme or lip-sync timing. The iris is clipped to the fixed eye-white
rectangle; this follows the supplied manifest and keeps the renderer
allocation-free.

## Captured hardware run

- Target: ESP32-S3, MAC `7c:e8:b1:b1:65:48`, COM13.
- Flash operation: app-only write at `0x20000`; no bootloader, partition table, or assets rewrite.
- Boot capture: [`anime_face_com13_boot_runtime.log`](../anime_face_com13_boot_runtime.log).
- Stack-fix capture: [`anime_face_com13_stackfix_poc_runtime.log`](../anime_face_com13_stackfix_poc_runtime.log).
- Final chat-bar boot capture: [`anime_face_com13_chatbar_final_runtime.log`](../anime_face_com13_chatbar_final_runtime.log).
- Expressive animation capture: [`anime_face_com13_expressive_runtime.log`](../anime_face_com13_expressive_runtime.log).
- PCM-gated current capture: [`anime_face_com13_pcm_runtime.log`](../anime_face_com13_pcm_runtime.log).
- Emotion demo capture: [`anime_face_com13_emotion_demo_runtime.log`](../anime_face_com13_emotion_demo_runtime.log).
- Wake attention capture: [`anime_face_com13_wake_attention_runtime.log`](../anime_face_com13_wake_attention_runtime.log).
- Winking build capture: [`anime_face_com13_wink_runtime.log`](../anime_face_com13_wink_runtime.log).
- Wink-mouth boot capture: [`anime_face_com13_wink_mouth_runtime.log`](../anime_face_com13_wink_mouth_runtime.log).
- Tap hit-layer validation: [`anime_face_com13_tapfix_runtime.log`](../anime_face_com13_tapfix_runtime.log).
- Sad/angry/crying asset build: [`anime_face_com13_assets_runtime.log`](../anime_face_com13_assets_runtime.log).
- Asset build `ANIME_FACE_MEM after_init`: internal free 212775, DMA free 205003.
- Asset build `ANIME_FACE_MEM first_update`: internal free 164671, DMA free 156899.
- Stack-fix `ANIME_FACE_MEM after_init`: internal free 214459, largest 94208, DMA free 206687, DMA largest 94208.
- Stack-fix `ANIME_FACE_MEM first_update`: internal free 166559, largest 94208, DMA free 158787, DMA largest 94208.
- The 45-second stack-fix capture shows repeated `ANIME_FACE_GAZE` and `ANIME_FACE_BLINK` events after boot animation, with no `stack overflow`, panic, reset, or LVGL/image-decoder error. Network timeout messages are unrelated Home Assistant connectivity failures.
- The final chat-bar capture shows the same clean initialization and animation path. `ANIME_FACE_TAP` is emitted when the user taps the avatar or subtitle strip.

The full repository unittest run completed 113 tests with one pre-existing failure in `test_configure_build_uses_all_cmake_values_in_one_run`: the test expected an older `_run_idf` argument list and did not include the repository's existing `-DIDF_CMAKE_CHECK_WARN_ONLY=ON` argument.

The earlier [`anime_face_com13_final_runtime.log`](../anime_face_com13_final_runtime.log) records the pre-stack-fix image. The stack-fix image and its clean validation are recorded in [`anime_face_com13_stackfix_poc_runtime.log`](../anime_face_com13_stackfix_poc_runtime.log).
