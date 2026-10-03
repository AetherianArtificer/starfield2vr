#pragma once
#include <Mod.hpp>

class CreationEngineEntry : public Mod
{
public:
    inline static std::shared_ptr<CreationEngineEntry>& Get()
    {
        static std::shared_ptr<CreationEngineEntry> instance{ std::make_shared<CreationEngineEntry>() };
        return instance;
    }

    [[nodiscard]] inline std::string_view get_name() const override { return "CreationEngine"; }

    std::optional<std::string> on_initialize() override;
    void                       on_draw_ui() override;
    void                       on_config_load(const utility::Config& cfg, bool ) override;
    void                       on_config_save(utility::Config& cfg) override;

private:
    inline static const std::vector<std::string> s_dominant_eye{
        "Right",
        "Left",
    };

    inline static const std::vector<std::string> s_aiming_mode{
      "HeadGun",
      "Free",
      "HeadGunAlwaysOn",
  };
    const ModCombo::Ptr m_dominant_eye{ ModCombo::create(generate_name("DominantEye"), s_dominant_eye) };
    const ModSlider::Ptr m_head_tracking_multiplier{ ModSlider::create(generate_name("HeadTrackingSensitivity"), 0.5, 2.0, 1.0) };
    const ModCombo::Ptr m_head_tracking_type{ ModCombo::create(generate_name("AimingMode"), s_aiming_mode) };
    const ModToggle::Ptr m_disable_zoom{ ModToggle::create(generate_name("DisableZoom"), false) };
    const ModToggle::Ptr m_decoupled_pitch{ ModToggle::create(generate_name("DecoupledPitch"), false) };
    const ModToggle::Ptr m_pawn_control_rotation{ ModToggle::create(generate_name("PawnControlRotation"), true) };
    const ModToggle::Ptr m_taa_anf_nvidia_fix{ ModToggle::create(generate_name("NvidiaDlssAndTaaFix"), true) };
    const ModSlider::Ptr m_hud_scale{ModSlider::create(generate_name("HUDScale"), 0.1, 1.0, 0.4) };
    const ModSlider::Ptr m_hud_perspective{ ModSlider::create(generate_name("HUDPerspective"), 0, 600, 150) };
    const ModToggle::Ptr m_alternative_joy_layout{ ModToggle::create(generate_name("JoyAlternativeLayout"), false) };

    inline static const std::vector<std::string> s_controller_layout{
        "Matching letters (Quest / Touch)",
        "Legacy",
    };
    const ModCombo::Ptr m_controller_layout{ ModCombo::create(generate_name("ControllerLayout"), s_controller_layout) };
    const ModToggle::Ptr m_recenter_after_loading{ ModToggle::create(generate_name("RecenterAfterLoading"), true) };

    ValueList m_options{*m_dominant_eye, *m_head_tracking_multiplier, *m_head_tracking_type, *m_taa_anf_nvidia_fix, *m_disable_zoom, *m_hud_scale, *m_hud_perspective, *m_alternative_joy_layout, *m_decoupled_pitch, *m_pawn_control_rotation, *m_controller_layout, *m_recenter_after_loading };
};
