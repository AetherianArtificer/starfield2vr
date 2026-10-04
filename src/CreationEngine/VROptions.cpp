#include "VROptions.h"

#include "Framework.hpp"
#include <CreationEngine/models/ModSettingsStore.h>

namespace
{
    constexpr std::array<float, 4> kSnapAngles{ 30.0f, 45.0f, 60.0f, 90.0f };
    constexpr std::array<float, 3> kSmoothTurnSpeeds{ 0.4f, 0.7f, 1.0f };
    constexpr std::array<float, 3> kSpeedLimits{ 1.0f, 0.75f, 0.45f };
    constexpr std::array<float, 4> kVignetteStrengths{ 0.0f, 0.35f, 0.6f, 0.85f };
}

VROptions::VROptions()
{
    auto add = [this](Id id, Kind kind, const char* key, const char* label, const char* description, std::vector<std::string> choices,
                   int default_value, std::array<int, 3> preset = { -1, -1, -1 }) {
        Option option{ id, kind, label, description, choices, preset, nullptr };
        if (kind == Kind::Toggle || kind == Kind::Choice) {
            option.value = ModCombo::create(generate_name(key), std::move(choices), default_value);
        }
        m_options.push_back(std::move(option));
    };
    const std::vector<std::string> on_off{ "Off", "On" };

    add(kComfortPreset, Kind::Preset, "", "Comfort Preset",
        "Sets the comfort options below. Low suits players used to VR; High is gentlest for those prone to motion sickness.",
        { "Low", "Medium", "High", "Custom" }, 0);

    add(kHeaderTurning, Kind::Header, "", "Turning", "", {}, 0);
    add(kSnapTurn, Kind::Toggle, "SnapTurn", "Snap Turn", "Turn in fixed steps with the right stick instead of smoothly.", on_off, 1, { 0, 1, 1 });
    add(kSnapTurnAngle, Kind::Choice, "SnapTurnAngle", "Snap Turn Angle", "How far each snap turn rotates you.",
        { "30\xC2\xB0", "45\xC2\xB0", "60\xC2\xB0", "90\xC2\xB0" }, 1);
    add(kSmoothTurnSpeed, Kind::Choice, "SmoothTurnSpeed", "Smooth Turn Speed", "How fast the right stick turns you when Snap Turn is off.",
        { "Slow", "Medium", "Fast" }, 1);
    add(kStickPitch, Kind::Toggle, "StickPitch", "Stick Looks Up and Down",
        "Let the right stick tilt your view up and down. When off, only your head does.", on_off, 0, { 1, 0, 0 });

    add(kHeaderMovement, Kind::Header, "", "Movement", "", {}, 0);
    add(kMoveDirection, Kind::Choice, "MoveDirection", "Movement Direction", "Move toward where your head or your left hand points.",
        { "Head", "Left Hand" }, 1);
    add(kSpeedLimit, Kind::Choice, "SpeedLimit", "Speed Limit", "Cap how fast the left stick moves you. Sprinting is off below Full.",
        { "Full", "Jog", "Walk" }, 0, { 0, 0, 1 });
    add(kSmoothAcceleration, Kind::Toggle, "SmoothAcceleration", "Smooth Acceleration",
        "Ease into and out of movement instead of starting and stopping instantly.", on_off, 1, { 0, 1, 1 });
    add(kVignette, Kind::Choice, "Vignette", "Vignette", "Darken the edges of your view while you move or turn smoothly.",
        { "Off", "Light", "Medium", "Strong" }, 2, { 0, 2, 3 });

    add(kHeaderControls, Kind::Header, "", "Controls", "", {}, 0);
    add(kControllerLayout, Kind::Choice, "ControllerLayout", "Controller Layout",
        "Matching letters: each controller button sends the gamepad button with the same label, so on-screen prompts match.",
        { "Matching letters", "Legacy" }, 0);

    add(kHeaderView, Kind::Header, "", "View", "", {}, 0);
    add(kRecenterAfterLoading, Kind::Toggle, "RecenterAfterLoading", "Recenter After Loading Screens",
        "Reset your view height and direction when a loading screen ends.", on_off, 1);
}

const VROptions::Option* VROptions::find(std::uint32_t id) const
{
    for (auto& option : m_options) {
        if (option.id == id) {
            return &option;
        }
    }
    return nullptr;
}

int VROptions::get(std::uint32_t id) const
{
    if (id == kComfortPreset) {
        return current_preset();
    }
    auto option = find(id);
    return option && option->value ? option->value->value() : 0;
}

int VROptions::current_preset() const
{
    for (int preset = 0; preset < kCustomPreset; ++preset) {
        bool matches = true;
        for (auto& option : m_options) {
            if (option.value && option.preset[preset] >= 0 && option.value->value() != option.preset[preset]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return preset;
        }
    }
    return kCustomPreset;
}

void VROptions::apply_preset(int preset)
{
    for (auto& option : m_options) {
        if (option.value && option.preset[preset] >= 0) {
            option.value->value() = option.preset[preset];
        }
    }
}

void VROptions::set(std::uint32_t id, int value)
{
    if (id == kComfortPreset) {
        if (value >= 0 && value < kCustomPreset) {
            apply_preset(value);
        }
    } else if (auto option = find(id); option && option->value) {
        option->value->value() = std::clamp<int>(value, 0, (int)option->choices.size() - 1);
    } else {
        return;
    }
    apply_to_store();
    g_framework->request_save_config();
}

void VROptions::apply_to_store() const
{
    auto& s                = GameFlow::gStore.internalSettings;
    s.turnMode             = get(kSnapTurn) ? 0 : 1;
    s.snapTurnDegrees      = kSnapAngles[std::clamp(get(kSnapTurnAngle), 0, (int)kSnapAngles.size() - 1)];
    s.smoothTurnSpeed      = kSmoothTurnSpeeds[std::clamp(get(kSmoothTurnSpeed), 0, (int)kSmoothTurnSpeeds.size() - 1)];
    s.stickPitch           = get(kStickPitch) != 0;
    s.moveDirection        = get(kMoveDirection);
    s.speedLimit           = kSpeedLimits[std::clamp(get(kSpeedLimit), 0, (int)kSpeedLimits.size() - 1)];
    s.smoothAcceleration   = get(kSmoothAcceleration) != 0;
    s.vignetteStrength     = kVignetteStrengths[std::clamp(get(kVignette), 0, (int)kVignetteStrengths.size() - 1)];
    s.controllerLayout     = get(kControllerLayout);
    s.recenterAfterLoading = get(kRecenterAfterLoading) != 0;
}

void VROptions::on_draw_ui()
{
    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }
    for (auto& option : m_options) {
        switch (option.kind) {
        case Kind::Header:
            ImGui::Separator();
            ImGui::TextUnformatted(option.label);
            break;
        case Kind::Preset: {
            int         preset = current_preset();
            const char* names[]{ "Low", "Medium", "High", "Custom" };
            if (ImGui::Combo(option.label, &preset, names, 4)) {
                set(option.id, preset);
            }
            break;
        }
        default:
            if (option.value->draw(option.label)) {
                set(option.id, option.value->value());
            }
            break;
        }
    }
}

void VROptions::on_config_load(const utility::Config& cfg, bool set_defaults)
{
    for (auto& option : m_options) {
        if (option.value) {
            option.value->config_load(cfg, set_defaults);
        }
    }
    apply_to_store();
}

void VROptions::on_config_save(utility::Config& cfg)
{
    for (auto& option : m_options) {
        if (option.value) {
            option.value->config_save(cfg);
        }
    }
}
