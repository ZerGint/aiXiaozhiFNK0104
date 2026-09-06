/*
 * FluxGarage RoboEyes adapted for LVGL 9 Canvas / XiaoZhi ESP32
 * Smoothly animated robot eyes for ESP32-S3 displays.
 * Original Copyright (C) 2024-2025 Dennis Hoelscher (www.fluxgarage.com)
 * Licensed under GPL v3.
 */

#pragma once

#include <lvgl.h>
#include <esp_log.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>

// Display colors
#define ROBOEYES_BGCOLOR 0
#define ROBOEYES_MAINCOLOR 1

// Mood types
#define ROBOEYES_DEFAULT 0
#define ROBOEYES_TIRED 1
#define ROBOEYES_ANGRY 2
#define ROBOEYES_HAPPY 3

// Positions
#define ROBOEYES_N 1
#define ROBOEYES_NE 2
#define ROBOEYES_E 3
#define ROBOEYES_SE 4
#define ROBOEYES_S 5
#define ROBOEYES_SW 6
#define ROBOEYES_W 7
#define ROBOEYES_NW 8
#define ROBOEYES_CENTER 0

class LvglCanvasDisplayAdapter {
public:
    lv_obj_t* canvas = nullptr;
    lv_layer_t layer;
    bool layer_active = false;
    lv_color_t bg_color = lv_color_hex(0x000000);
    lv_color_t main_color = lv_color_hex(0x00F0FF); // Cyan robot eyes

    void setColors(lv_color_t bg, lv_color_t main) {
        bg_color = bg;
        main_color = main;
    }

    void clearDisplay() {
        if (canvas) {
            lv_canvas_fill_bg(canvas, bg_color, LV_OPA_COVER);
            lv_canvas_init_layer(canvas, &layer);
            layer_active = true;
        }
    }

    void fillRoundRect(int x, int y, int w, int h, int r, uint8_t color_idx) {
        if (!layer_active || w <= 0 || h <= 0) return;
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = (color_idx == ROBOEYES_BGCOLOR) ? bg_color : main_color;
        dsc.bg_opa = LV_OPA_COVER;
        dsc.radius = r;
        lv_area_t coords = { (int32_t)x, (int32_t)y, (int32_t)(x + w - 1), (int32_t)(y + h - 1) };
        lv_draw_rect(&layer, &dsc, &coords);
    }

    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color_idx) {
        if (!layer_active) return;
        lv_draw_triangle_dsc_t dsc;
        lv_draw_triangle_dsc_init(&dsc);
        dsc.color = (color_idx == ROBOEYES_BGCOLOR) ? bg_color : main_color;
        dsc.opa = LV_OPA_COVER;
        dsc.p[0].x = x0; dsc.p[0].y = y0;
        dsc.p[1].x = x1; dsc.p[1].y = y1;
        dsc.p[2].x = x2; dsc.p[2].y = y2;
        lv_draw_triangle(&layer, &dsc);
    }

    void display() {
        if (layer_active && canvas) {
            lv_canvas_finish_layer(canvas, &layer);
            layer_active = false;
            lv_obj_invalidate(canvas);
        }
    }
};

class RoboEyes {
public:
    LvglCanvasDisplayAdapter adapter;

    int screenWidth = 240;
    int screenHeight = 120;
    int frameInterval = 33;
    uint32_t fpsTimer = 0;

    bool tired = false;
    bool angry = false;
    bool happy = false;
    bool curious = false;
    bool cyclops = false;
    bool eyeL_open = true;
    bool eyeR_open = true;

    // Eye Left Geometry
    int eyeLwidthDefault = 60;
    int eyeLheightDefault = 60;
    int eyeLwidthCurrent = 60;
    int eyeLheightCurrent = 60;
    int eyeLwidthNext = 60;
    int eyeLheightNext = 60;
    int eyeLheightOffset = 0;
    uint8_t eyeLborderRadiusDefault = 16;
    uint8_t eyeLborderRadiusCurrent = 16;
    uint8_t eyeLborderRadiusNext = 16;

    // Eye Right Geometry
    int eyeRwidthDefault = 60;
    int eyeRheightDefault = 60;
    int eyeRwidthCurrent = 60;
    int eyeRheightCurrent = 60;
    int eyeRwidthNext = 60;
    int eyeRheightNext = 60;
    int eyeRheightOffset = 0;
    uint8_t eyeRborderRadiusDefault = 16;
    uint8_t eyeRborderRadiusCurrent = 16;
    uint8_t eyeRborderRadiusNext = 16;

    // Coordinates
    int spaceBetweenDefault = 24;
    int spaceBetweenCurrent = 24;
    int spaceBetweenNext = 24;

    int eyeLxDefault = 48;
    int eyeLyDefault = 30;
    int eyeLx = 48;
    int eyeLy = 30;
    int eyeLxNext = 48;
    int eyeLyNext = 30;

    int eyeRxDefault = 132;
    int eyeRyDefault = 30;
    int eyeRx = 132;
    int eyeRy = 30;
    int eyeRxNext = 132;
    int eyeRyNext = 30;

    // Eyelids
    uint8_t eyelidsTiredHeight = 0;
    uint8_t eyelidsTiredHeightNext = 0;
    uint8_t eyelidsAngryHeight = 0;
    uint8_t eyelidsAngryHeightNext = 0;
    uint8_t eyelidsHappyBottomOffset = 0;
    uint8_t eyelidsHappyBottomOffsetNext = 0;

    // Animations
    bool hFlicker = false;
    bool hFlickerAlternate = false;
    uint8_t hFlickerAmplitude = 2;

    bool vFlicker = false;
    bool vFlickerAlternate = false;
    uint8_t vFlickerAmplitude = 4;

    bool autoblinker = true;
    int blinkInterval = 3;
    int blinkIntervalVariation = 3;
    uint32_t blinktimer = 0;

    bool idle = true;
    int idleInterval = 2;
    int idleIntervalVariation = 3;
    uint32_t idleAnimationTimer = 0;

    bool confused = false;
    uint32_t confusedAnimationTimer = 0;
    int confusedAnimationDuration = 500;
    bool confusedToggle = true;

    bool laugh = false;
    uint32_t laughAnimationTimer = 0;
    int laughAnimationDuration = 500;
    bool laughToggle = true;

    bool sweat = false;

    void begin(lv_obj_t* canvas_obj, int width, int height, uint8_t fps) {
        adapter.canvas = canvas_obj;
        screenWidth = width;
        screenHeight = height;
        setFramerate(fps);

        eyeLwidthDefault = width / 4;
        eyeLheightDefault = height / 2;
        eyeRwidthDefault = eyeLwidthDefault;
        eyeRheightDefault = eyeLheightDefault;
        spaceBetweenDefault = width / 10;
        eyeLborderRadiusDefault = eyeLwidthDefault / 3;
        eyeRborderRadiusDefault = eyeLborderRadiusDefault;

        eyeLwidthCurrent = eyeLwidthNext = eyeLwidthDefault;
        eyeLheightCurrent = eyeLheightNext = eyeLheightDefault;
        eyeRwidthCurrent = eyeRwidthNext = eyeRwidthDefault;
        eyeRheightCurrent = eyeRheightNext = eyeRheightDefault;

        eyeLborderRadiusCurrent = eyeLborderRadiusNext = eyeLborderRadiusDefault;
        eyeRborderRadiusCurrent = eyeRborderRadiusNext = eyeRborderRadiusDefault;

        spaceBetweenCurrent = spaceBetweenNext = spaceBetweenDefault;

        eyeLxDefault = (screenWidth - (eyeLwidthDefault + spaceBetweenDefault + eyeRwidthDefault)) / 2;
        eyeLyDefault = (screenHeight - eyeLheightDefault) / 2;
        eyeLx = eyeLxNext = eyeLxDefault;
        eyeLy = eyeLyNext = eyeLyDefault;

        eyeRxDefault = eyeLxDefault + eyeLwidthDefault + spaceBetweenDefault;
        eyeRyDefault = eyeLyDefault;
        eyeRx = eyeRxNext = eyeRxDefault;
        eyeRy = eyeRyNext = eyeRyDefault;

        eyeL_open = true;
        eyeR_open = true;
    }

    void setFramerate(uint8_t fps) {
        frameInterval = 1000 / (fps > 0 ? fps : 30);
    }

    void setWidth(int left, int right) {
        eyeLwidthNext = left; eyeRwidthNext = right;
        eyeLwidthDefault = left; eyeRwidthDefault = right;
    }

    void setHeight(int left, int right) {
        eyeLheightNext = left; eyeRheightNext = right;
        eyeLheightDefault = left; eyeRheightDefault = right;
    }

    void setBorderradius(uint8_t left, uint8_t right) {
        eyeLborderRadiusNext = left; eyeRborderRadiusNext = right;
        eyeLborderRadiusDefault = left; eyeRborderRadiusDefault = right;
    }

    void setSpacebetween(int space) {
        spaceBetweenNext = space;
        spaceBetweenDefault = space;
    }

    void setMood(uint8_t mood) {
        switch (mood) {
        case ROBOEYES_TIRED:  tired = true;  angry = false; happy = false; break;
        case ROBOEYES_ANGRY:  tired = false; angry = true;  happy = false; break;
        case ROBOEYES_HAPPY:  tired = false; angry = false; happy = true;  break;
        default:             tired = false; angry = false; happy = false; break;
        }
    }

    int getScreenConstraint_X() {
        return screenWidth - eyeLwidthCurrent - spaceBetweenCurrent - eyeRwidthCurrent;
    }

    int getScreenConstraint_Y() {
        return screenHeight - eyeLheightDefault;
    }

    void setPosition(uint8_t pos) {
        int maxX = getScreenConstraint_X();
        int maxY = getScreenConstraint_Y();
        if (maxX < 0) maxX = 0;
        if (maxY < 0) maxY = 0;

        switch (pos) {
        case ROBOEYES_N:  eyeLxNext = maxX / 2; eyeLyNext = 0; break;
        case ROBOEYES_NE: eyeLxNext = maxX;     eyeLyNext = 0; break;
        case ROBOEYES_E:  eyeLxNext = maxX;     eyeLyNext = maxY / 2; break;
        case ROBOEYES_SE: eyeLxNext = maxX;     eyeLyNext = maxY; break;
        case ROBOEYES_S:  eyeLxNext = maxX / 2; eyeLyNext = maxY; break;
        case ROBOEYES_SW: eyeLxNext = 0;        eyeLyNext = maxY; break;
        case ROBOEYES_W:  eyeLxNext = 0;        eyeLyNext = maxY / 2; break;
        case ROBOEYES_NW: eyeLxNext = 0;        eyeLyNext = 0; break;
        default:          eyeLxNext = maxX / 2; eyeLyNext = maxY / 2; break;
        }
    }

    void setAutoblinker(bool active, int interval = 3, int variation = 3) {
        autoblinker = active;
        blinkInterval = interval;
        blinkIntervalVariation = variation;
    }

    void setIdleMode(bool active, int interval = 2, int variation = 3) {
        idle = active;
        idleInterval = interval;
        idleIntervalVariation = variation;
    }

    void setCuriosity(bool curiousBit) { curious = curiousBit; }
    void setCyclops(bool cyclopsBit) { cyclops = cyclopsBit; }
    void setHFlicker(bool flickerBit, uint8_t amplitude = 2) { hFlicker = flickerBit; hFlickerAmplitude = amplitude; }
    void setVFlicker(bool flickerBit, uint8_t amplitude = 4) { vFlicker = flickerBit; vFlickerAmplitude = amplitude; }
    void setSweat(bool sweatBit) { sweat = sweatBit; }

    void close() {
        eyeLheightNext = 1; eyeRheightNext = 1;
        eyeL_open = false; eyeR_open = false;
    }

    void open() {
        eyeL_open = true; eyeR_open = true;
    }

    void blink() {
        close();
        open();
    }

    void anim_confused() { confused = true; }
    void anim_laugh() { laugh = true; }

    void update() {
        uint32_t now = lv_tick_get();
        if (now - fpsTimer < (uint32_t)frameInterval) {
            return;
        }
        fpsTimer = now;

        drawEyes(now);
    }

    void drawEyes(uint32_t now) {
        // Curiosity sizing
        if (curious) {
            if (eyeLxNext <= 10) eyeLheightOffset = 8;
            else if (eyeLxNext >= (getScreenConstraint_X() - 10) && cyclops) eyeLheightOffset = 8;
            else eyeLheightOffset = 0;

            if (eyeRxNext >= screenWidth - eyeRwidthCurrent - 10) eyeRheightOffset = 8;
            else eyeRheightOffset = 0;
        } else {
            eyeLheightOffset = 0;
            eyeRheightOffset = 0;
        }

        // Smooth tweening
        eyeLheightCurrent = (eyeLheightCurrent + eyeLheightNext + eyeLheightOffset) / 2;
        eyeLy += ((eyeLheightDefault - eyeLheightCurrent) / 2);
        eyeLy -= eyeLheightOffset / 2;

        eyeRheightCurrent = (eyeRheightCurrent + eyeRheightNext + eyeRheightOffset) / 2;
        eyeRy += ((eyeRheightDefault - eyeRheightCurrent) / 2);
        eyeRy -= eyeRheightOffset / 2;

        if (eyeL_open && eyeLheightCurrent <= 1 + eyeLheightOffset) eyeLheightNext = eyeLheightDefault;
        if (eyeR_open && eyeRheightCurrent <= 1 + eyeRheightOffset) eyeRheightNext = eyeRheightDefault;

        eyeLwidthCurrent = (eyeLwidthCurrent + eyeLwidthNext) / 2;
        eyeRwidthCurrent = (eyeRwidthCurrent + eyeRwidthNext) / 2;
        spaceBetweenCurrent = (spaceBetweenCurrent + spaceBetweenNext) / 2;

        eyeLx = (eyeLx + eyeLxNext) / 2;
        eyeLy = (eyeLy + eyeLyNext) / 2;

        eyeRxNext = eyeLxNext + eyeLwidthCurrent + spaceBetweenCurrent;
        eyeRyNext = eyeLyNext;
        eyeRx = (eyeRx + eyeRxNext) / 2;
        eyeRy = (eyeRy + eyeRyNext) / 2;

        eyeLborderRadiusCurrent = (eyeLborderRadiusCurrent + eyeLborderRadiusNext) / 2;
        eyeRborderRadiusCurrent = (eyeRborderRadiusCurrent + eyeRborderRadiusNext) / 2;

        // Auto blinker
        if (autoblinker) {
            if (now >= blinktimer) {
                blink();
                int var_sec = (blinkIntervalVariation > 0) ? (rand() % blinkIntervalVariation) : 0;
                blinktimer = now + (blinkInterval + var_sec) * 1000;
            }
        }

        // Laugh animation
        if (laugh) {
            if (laughToggle) {
                setVFlicker(true, 5);
                laughAnimationTimer = now;
                laughToggle = false;
            } else if (now >= laughAnimationTimer + laughAnimationDuration) {
                setVFlicker(false, 0);
                laughToggle = true;
                laugh = false;
            }
        }

        // Confused animation
        if (confused) {
            if (confusedToggle) {
                setHFlicker(true, 10);
                confusedAnimationTimer = now;
                confusedToggle = false;
            } else if (now >= confusedAnimationTimer + confusedAnimationDuration) {
                setHFlicker(false, 0);
                confusedToggle = true;
                confused = false;
            }
        }

        // Idle animation
        if (idle) {
            if (now >= idleAnimationTimer) {
                int max_x = getScreenConstraint_X();
                int max_y = getScreenConstraint_Y();
                if (max_x > 0) eyeLxNext = rand() % max_x;
                if (max_y > 0) eyeLyNext = rand() % max_y;
                int var_sec = (idleIntervalVariation > 0) ? (rand() % idleIntervalVariation) : 0;
                idleAnimationTimer = now + (idleInterval + var_sec) * 1000;
            }
        }

        // Flicker offsets
        if (hFlicker) {
            if (hFlickerAlternate) { eyeLx += hFlickerAmplitude; eyeRx += hFlickerAmplitude; }
            else                  { eyeLx -= hFlickerAmplitude; eyeRx -= hFlickerAmplitude; }
            hFlickerAlternate = !hFlickerAlternate;
        }

        if (vFlicker) {
            if (vFlickerAlternate) { eyeLy += vFlickerAmplitude; eyeRy += vFlickerAmplitude; }
            else                  { eyeLy -= vFlickerAmplitude; eyeRy -= vFlickerAmplitude; }
            vFlickerAlternate = !vFlickerAlternate;
        }

        if (cyclops) {
            eyeRwidthCurrent = 0;
            eyeRheightCurrent = 0;
            spaceBetweenCurrent = 0;
        }

        // Actual Canvas Drawing
        adapter.clearDisplay();

        // Draw main eye rectangles
        adapter.fillRoundRect(eyeLx, eyeLy, eyeLwidthCurrent, eyeLheightCurrent, eyeLborderRadiusCurrent, ROBOEYES_MAINCOLOR);
        if (!cyclops) {
            adapter.fillRoundRect(eyeRx, eyeRy, eyeRwidthCurrent, eyeRheightCurrent, eyeRborderRadiusCurrent, ROBOEYES_MAINCOLOR);
        }

        // Eyelid targets
        if (tired)  { eyelidsTiredHeightNext = eyeLheightCurrent / 2; eyelidsAngryHeightNext = 0; } else { eyelidsTiredHeightNext = 0; }
        if (angry)  { eyelidsAngryHeightNext = eyeLheightCurrent / 2; eyelidsTiredHeightNext = 0; } else { eyelidsAngryHeightNext = 0; }
        if (happy)  { eyelidsHappyBottomOffsetNext = eyeLheightCurrent / 2; } else { eyelidsHappyBottomOffsetNext = 0; }

        // Draw tired top eyelids
        eyelidsTiredHeight = (eyelidsTiredHeight + eyelidsTiredHeightNext) / 2;
        if (eyelidsTiredHeight > 0) {
            if (!cyclops) {
                adapter.fillTriangle(eyeLx, eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy - 1, eyeLx, eyeLy + eyelidsTiredHeight - 1, ROBOEYES_BGCOLOR);
                adapter.fillTriangle(eyeRx, eyeRy - 1, eyeRx + eyeRwidthCurrent, eyeRy - 1, eyeRx + eyeRwidthCurrent, eyeRy + eyelidsTiredHeight - 1, ROBOEYES_BGCOLOR);
            } else {
                adapter.fillTriangle(eyeLx, eyeLy - 1, eyeLx + (eyeLwidthCurrent / 2), eyeLy - 1, eyeLx, eyeLy + eyelidsTiredHeight - 1, ROBOEYES_BGCOLOR);
                adapter.fillTriangle(eyeLx + (eyeLwidthCurrent / 2), eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy + eyelidsTiredHeight - 1, ROBOEYES_BGCOLOR);
            }
        }

        // Draw angry top eyelids
        eyelidsAngryHeight = (eyelidsAngryHeight + eyelidsAngryHeightNext) / 2;
        if (eyelidsAngryHeight > 0) {
            if (!cyclops) {
                adapter.fillTriangle(eyeLx, eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy + eyelidsAngryHeight - 1, ROBOEYES_BGCOLOR);
                adapter.fillTriangle(eyeRx, eyeRy - 1, eyeRx + eyeRwidthCurrent, eyeRy - 1, eyeRx, eyeRy + eyelidsAngryHeight - 1, ROBOEYES_BGCOLOR);
            } else {
                adapter.fillTriangle(eyeLx, eyeLy - 1, eyeLx + (eyeLwidthCurrent / 2), eyeLy - 1, eyeLx + (eyeLwidthCurrent / 2), eyeLy + eyelidsAngryHeight - 1, ROBOEYES_BGCOLOR);
                adapter.fillTriangle(eyeLx + (eyeLwidthCurrent / 2), eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy - 1, eyeLx + (eyeLwidthCurrent / 2), eyeLy + eyelidsAngryHeight - 1, ROBOEYES_BGCOLOR);
            }
        }

        // Draw happy bottom eyelids
        eyelidsHappyBottomOffset = (eyelidsHappyBottomOffset + eyelidsHappyBottomOffsetNext) / 2;
        if (eyelidsHappyBottomOffset > 0) {
            adapter.fillRoundRect(eyeLx - 1, (eyeLy + eyeLheightCurrent) - eyelidsHappyBottomOffset + 1, eyeLwidthCurrent + 2, eyeLheightDefault, eyeLborderRadiusCurrent, ROBOEYES_BGCOLOR);
            if (!cyclops) {
                adapter.fillRoundRect(eyeRx - 1, (eyeRy + eyeRheightCurrent) - eyelidsHappyBottomOffset + 1, eyeRwidthCurrent + 2, eyeRheightDefault, eyeRborderRadiusCurrent, ROBOEYES_BGCOLOR);
            }
        }

        adapter.display();
    }
};
