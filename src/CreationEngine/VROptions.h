#pragma once
#include <array>
#include <Mod.hpp>

// Player-facing VR options shown in the in-game Settings menu and the overlay.
class VROptions : public Mod
{
public:
    enum class Kind
    {
        Header,
        Toggle,
        Choice,
        Preset,
    };

    enum Id : std::uint32_t
    {
        kComfortPreset = 8600,
        kSnapTurn,
        kSnapTurnAngle,
        kRecenterAfterLoading,
        kControllerLayout,
        kSmoothTurnSpeed,
        kStickPitch,
        kMoveDirection,
        kSpeedLimit,
        kSmoothAcceleration,
        kVignette,

        kHeaderTurning = 8650,
        kHeaderMovement,
        kHeaderControls,
        kHeaderView,

        kFirstId = 8600,
        kLastId  = 8699,
    };

    static constexpr int kCustomPreset = 3;

    struct Option
    {
        Id                       id;
        Kind                     kind;
        const char*              label;
        const char*              description;
        std::vector<std::string> choices;
        std::array<int, 3>       preset{ -1, -1, -1 };  // Low, Medium, High; -1 = not part of the preset
        ModCombo::Ptr            value;
    };

    static std::shared_ptr<VROptions>& Get()
    {
        static std::shared_ptr<VROptions> instance{ std::make_shared<VROptions>() };
        return instance;
    }

    VROptions();

    [[nodiscard]] std::string_view get_name() const override { return "VROptions"; }

    void on_draw_ui() override;
    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;

    [[nodiscard]] const std::vector<Option>& options() const { return m_options; }
    [[nodiscard]] int                        get(std::uint32_t id) const;
    void                                     set(std::uint32_t id, int value);

private:
    [[nodiscard]] const Option* find(std::uint32_t id) const;
    [[nodiscard]] int           current_preset() const;
    void                        apply_preset(int preset);
    void                        apply_to_store() const;

    std::vector<Option> m_options;
};
