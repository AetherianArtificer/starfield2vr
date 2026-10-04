#pragma once

namespace ModConstants
{
    inline float headTrackingMultiplier = 1.0f;

    inline constexpr float DEG_TO_RAD = std::numbers::pi / 180;
    inline float lodAdjustFov = 0.0f;
    inline float fixedCameraRoll = 0.0f;
    inline bool preventCameraRoll{false};
    inline bool cameraShake{false};

    static constexpr int NUM_STAGES = 12;
    inline std::array<bool, NUM_STAGES> enabledResourcesToCopy{false, true, true, true, false, false, false, false, false, false, false, false};
    inline std::array<int, NUM_STAGES> rgResourceCount{2,2,2,2,2,2,2,1,2,2,2,2};
    inline int dominantEye{0};
    inline int headTrackingType{0};
    inline constexpr int kAimWithRightHand = 3;
}
