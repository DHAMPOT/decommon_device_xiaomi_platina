/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.light@2.0-service.xiaomi_sdm660"

#include <log/log.h>

#include "Light.h"

#include <fstream>

#define LEDS            "/sys/class/leds/"

#define LCD_LED         LEDS "lcd-backlight/"
#define WHITE_LED       LEDS "white/"

#define BLINK           "blink"
#define BRIGHTNESS      "brightness"
#define DUTY_PCTS       "duty_pcts"
#define PAUSE_HI        "pause_hi"
#define PAUSE_LO        "pause_lo"
#define RAMP_STEP_MS    "ramp_step_ms"
#define START_IDX       "start_idx"

#define MAX_LED_BRIGHTNESS    200
#define MAX_LCD_BRIGHTNESS    4095

/*
 * 8 duty percent steps.
 */
#define RAMP_STEPS 15
/*
 * Each step will stay on for 50ms by default.
 */
#define RAMP_STEP_DURATION 150
/*
 * Each value represents a duty percent (0 - 100) for the led pwm.
 */
static int32_t BRIGHTNESS_RAMP[RAMP_STEPS] = {0, 12, 25, 37, 50, 72, 85, 100, 85, 72, 50, 37, 25, 12, 0};

namespace {
/*
 * Write value to path and close file.
 */
static void set(std::string path, std::string value) {
    std::ofstream file(path);

    if (!file.is_open()) {
        ALOGW("failed to write %s to %s", value.c_str(), path.c_str());
        return;
    }

    file << value;
}

static void set(std::string path, int value) {
    set(path, std::to_string(value));
}

static uint32_t getBrightness(const LightState& state) {
    uint32_t alpha, white, green, blue;

    /*
     * Extract brightness from AARRGGBB.
     */
    alpha = (state.color >> 24) & 0xFF;
    white = (state.color >> 16) & 0xFF;
    green = (state.color >> 8) & 0xFF;
    blue = state.color & 0xFF;

    /*
     * Scale RGB brightness if Alpha brightness is not 0xFF.
     */
    if (alpha != 0xFF) {
        white = white * alpha / 0xFF;
        green = green * alpha / 0xFF;
        blue = blue * alpha / 0xFF;
    }

    return (77 * white + 150 * green + 29 * blue) >> 8;
}

static inline uint32_t scaleBrightness(uint32_t brightness, uint32_t maxBrightness) {
    return brightness * maxBrightness / 0xFF;
}

static inline uint32_t getScaledBrightness(const LightState& state, uint32_t maxBrightness) {
    return scaleBrightness(getBrightness(state), maxBrightness);
}

static void handleBacklight(Type /* type */, const LightState& state) {
    uint32_t brightness = getScaledBrightness(state, MAX_LCD_BRIGHTNESS);
    set(LCD_LED BRIGHTNESS, brightness);
}

/*
 * Scale each value of the brightness ramp according to the
 * brightness of the color.
 */
static std::string getScaledRamp(uint32_t brightness) {
    std::string ramp, pad;

    for (auto const& step : BRIGHTNESS_RAMP) {
        ramp += pad + std::to_string(step * brightness / 0xFF);
        pad = ",";
    }

    return ramp;
}

static void setNotification(const LightState& state) {
    uint32_t whiteBrightness = getScaledBrightness(state, MAX_LED_BRIGHTNESS);

    /* Disable blinking */
    set(WHITE_LED BLINK, 0);

    if (state.flashMode == Flash::TIMED) {
        /*
         * If the flashOnMs duration is not long enough to fit ramping up
         * and down at the default step duration, step duration is modified
         * to fit.
         */
        int32_t stepDuration = RAMP_STEP_DURATION;
        int32_t pauseHi = state.flashOnMs - (stepDuration * RAMP_STEPS * 2);
        int32_t pauseLo = state.flashOffMs;

        if (pauseHi < 0) {
            //stepDuration = state.flashOnMs / (RAMP_STEPS * 2);
            pauseHi = 0;
        }

        /* white */
        set(WHITE_LED START_IDX, 0 * RAMP_STEPS);
        set(WHITE_LED DUTY_PCTS, getScaledRamp(whiteBrightness));
        set(WHITE_LED PAUSE_LO, pauseLo);
        set(WHITE_LED PAUSE_HI, pauseHi);
        set(WHITE_LED RAMP_STEP_MS, stepDuration);

        /* Enable blinking */
        set(WHITE_LED BLINK, 1);
    } else {
        set(WHITE_LED BRIGHTNESS, whiteBrightness);
    }
}

static inline bool isLit(const LightState& state) {
    return state.color & 0x00ffffff;
}

/*
 * Keep sorted in the order of importance.
 */
static const LightState offState = {};
static std::vector<std::pair<Type, LightState>> notificationStates = {
    { Type::ATTENTION, offState },
    { Type::NOTIFICATIONS, offState },
    { Type::BATTERY, offState },
};

static void handleNotification(Type type, const LightState& state) {
    bool handled = false;

    for(auto it : notificationStates) {
        if (it.first == type) {
            it.second = state;
        }

        if  (!handled && isLit(it.second)) {
            setNotification(it.second);
            handled = true;
        }
    }

    if (!handled) {
        setNotification(offState);
    }
}

static std::map<Type, std::function<void(Type type, const LightState&)>> lights = {
    { Type::ATTENTION, handleNotification },
    { Type::NOTIFICATIONS, handleNotification },
    { Type::BATTERY, handleNotification },
    { Type::BACKLIGHT, handleBacklight },
};

} // anonymous namespace

namespace android {
namespace hardware {
namespace light {
namespace V2_0 {
namespace implementation {

Return<Status> Light::setLight(Type type, const LightState& state) {
    auto it = lights.find(type);

    if (it == lights.end()) {
        return Status::LIGHT_NOT_SUPPORTED;
    }

    /*
     * Lock global mutex until light state is updated.
     */
    std::lock_guard<std::mutex> lock(globalLock);

    it->second(type, state);

    return Status::SUCCESS;
}

Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb) {
    std::vector<Type> types;

    for (auto const& light : lights) {
        types.push_back(light.first);
    }

    _hidl_cb(types);

    return Void();
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace light
}  // namespace hardware
}  // namespace android
