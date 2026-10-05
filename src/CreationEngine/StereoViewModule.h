#pragma once
#include <atomic>
#include <safetyhook/inline_hook.hpp>
#include <d3d12.h>
#include <wrl/client.h>
#include <map>
#include <vector>
#include <mutex>
#include <string>
#include <cstdint>
#include <memory>
#include <memory/FunctionHook.h>

namespace RE
{
    class NiCamera;
}

// Native stereo: the right eye is a second engine camera view rendered in the same frame as the left eye, which is the
// world camera. Both views share one back buffer side by side.
class StereoViewModule
{
public:
    static StereoViewModule* Get()
    {
        static auto instance(new StereoViewModule);
        return instance;
    }

    void InstallHooks();

    // Game thread, once per engine frame before the cameras are posed.
    void OnFrameStart();

    [[nodiscard]] RE::NiCamera* RightCamera() const { return m_right_camera; }
    // Both eyes render through cloned cameras; the world camera keeps serving game logic but draws no eye.
    [[nodiscard]] RE::NiCamera* LeftCamera() const { return m_left_camera; }
    // The back buffer should be two eyes wide (Double Width layout).
    [[nodiscard]] bool WantsSideBySide() const { return m_wants_side_by_side; }
    // Side by side splits one double-width frame between the eyes; otherwise each eye renders the whole frame.
    [[nodiscard]] bool SideBySide() const { return m_double_width; }
    // Eye a camera renders in native stereo: 0 left, 1 right, -1 not an eye camera.
    [[nodiscard]] int EyeOf(const RE::NiCamera* camera) const;

    // Test switch: registers a fresh camera and view for the right eye.
    void RequestRenewRightCamera() { m_renew_right_camera.store(true); }

private:
    StereoViewModule() = default;

    struct ViewIdArray;

    bool TryRegister();
    void ApplyViewports(bool side_by_side);
    bool RenewRightCamera();
    void ApplyNativeShadowSettings(bool native);
    void DisableFrameGeneration();
    void*        m_frame_generation_setting{ nullptr };
    bool         m_frame_generation_looked_up{ false };
    void UpdateMenuFallback(bool native);
    bool  m_shadow_settings_applied{ false };
    float m_saved_shadow_fade_seconds{ 0.75f };
    bool  m_saved_shadow_main_view_lod{ true };
    bool  m_saved_shadow_volume_culling{ true };
    bool  m_saved_csm_occlusion_culling{ true };
    // Fullscreen menus run the game's own single view; they are shown on the flat screen.
    std::atomic<bool> m_menu_fallback{ false };
    int               m_menu_frames{ 0 };
    int               m_menu_opens{ 0 };
    [[nodiscard]] std::string Variant() const;
    std::atomic<bool> m_renew_right_camera{ false };
    int               m_right_camera_generation{ 0 };
    float             m_eye_near{ -1.0f };
    float             m_eye_far{ -1.0f };
    // Diagnostic sequence: each configuration is captured once during first-person gameplay.
    void              AdvanceExperiment();
    int               m_experiment_stage{ 0 };
    bool              m_exp_left_first{ false };
    bool              m_exp_swap_graphs{ false };
    std::string       m_last_variant{};
    void MirrorMainView();
    struct MirroredRows;
    void MirrorInto(uint32_t main_view, uint32_t view_id, RE::NiCamera* camera, MirroredRows& mirrored);
    void UpdateRightFrustum();
    void ClearCrashGuard();
    void LogRenderSizes() const;

    static uintptr_t onSetMultiCameraViewData(void* column, uint32_t graph_index, ViewIdArray* views);
public:
    // Upscaler render passes, hooked to route DLSS per eye and to report which upscaler runs for each view.
    enum UpscalerPass
    {
        kDLSSInputs,
        kDLSSUpscale,
        kXeSSInputs,
        kXeSSUpscale,
        kFSR2Inputs,
        kFSR2Upscale,
        kFSR3Upscale,
        kUpscalerPassCount
    };
    template <int Pass>
    static uintptr_t onUpscalerPass(void* pass, void* render_graph_data, void* pass_data)
    {
        return Get()->RunUpscalerPass(Pass, pass, render_graph_data, pass_data);
    }
    uintptr_t RunUpscalerPass(int pass_kind, void* pass, void* render_graph_data, void* pass_data);
    void      InstallUpscalerHooks();
    void      ReportUpscalers();

    static uintptr_t onExecuteChildPass(void* graph, void* render_graph_data, void* container);
    static uintptr_t onScaleformComposite(void* pass, void* render_graph_data, void* pass_data);
    static uintptr_t onCopyToRenderGraphOutput(void* pass, void* render_graph_data, void* pass_data);
    static uintptr_t onHdrComposite(void* pass, void* render_graph_data, void* pass_data);
    void             CaptureEyeImage(uint32_t eye, void* render_graph_data, void* pass_data);
    static bool      onIsPrimarySceneView(void* view);
    static uintptr_t onSubmitRenderGraph(void* frame_list, void* record);
    bool             RegisterRightGraph();
    void             PrepareRightGraph();
    void             UpdateWorldFreeze();
    void             MirrorHudToRightEye(void* render_graph_data, void* pass_data);

private:

    std::unique_ptr<FunctionHook> m_set_multi_view_hook{};
    std::unique_ptr<FunctionHook> m_execute_child_hook{};
    std::unique_ptr<FunctionHook> m_scaleform_composite_hook{};
    std::unique_ptr<FunctionHook> m_copy_to_output_hook{};
    std::unique_ptr<FunctionHook> m_hdr_composite_hook{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> m_eye_capture{};
    std::array<bool, 2>                                   m_eye_capture_ready{};
    std::atomic<int>                                      m_capture_logs{ 0 };
    Microsoft::WRL::ComPtr<ID3D12Resource>                m_ui_capture{};
    void                                                  CaptureUiLayer(void* render_graph_data, void* pass_data);
    std::unique_ptr<FunctionHook> m_primary_view_hook{};
    // The right eye renders in its own render graph, so it never shares the left eye's working buffers.
    std::unique_ptr<FunctionHook> m_submit_graph_hook{};
    alignas(16) uint8_t           m_right_graph_record[0x80]{};
    uint32_t                      m_right_graph_id{ 0xFFFFFF };
    bool                          m_right_graph_ready{ false };
    uint8_t                       m_right_graph_options[0x30]{};
    uint8_t                       m_right_graph_frame_flag{ 0xFF };
    int                           m_right_graph_submits{ 0 };
    uintptr_t                     m_probe_builder{ 0 };
    // Scratch copy of the UI texture, used to repeat the left eye's HUD in the right half.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hud_scratch{};
    std::atomic<int>              m_hud_mirror_logs{ 0 };
    // Pass census: every pass each view runs over a few gameplay frames, to find effects one eye misses.
    std::atomic<int>                                        m_census_frames{ 0 };
    std::mutex                                              m_census_mutex{};
    std::map<uint32_t, std::map<std::string, int>>          m_census{};
    std::vector<std::string>                                m_census_order{};
    std::atomic<int>                                        m_composite_logs{ 0 };
    void                                                    StartCensus();
    void                                                    FinishCensus();
    [[nodiscard]] uint32_t                                  SceneOf(void* render_graph_data) const;
    std::unique_ptr<FunctionHook> m_upscaler_hooks[kUpscalerPassCount]{};
    std::atomic<int>              m_upscaler_runs[kUpscalerPassCount]{};
    std::atomic<int>              m_upscaler_right_runs[kUpscalerPassCount]{};
    int                           m_upscaler_reports{ 0 };
    bool                          m_warned_shared_upscaler{ false };

    RE::NiCamera*     m_right_camera{ nullptr };
    uint32_t          m_right_view_id{ 0xFFFFFF };
    RE::NiCamera*     m_left_camera{ nullptr };
    uint32_t          m_left_view_id{ 0xFFFFFF };
    bool              m_registered{ false };
    bool              m_failed{ false };
    bool              m_signatures_ok{ false };
    bool              m_wants_side_by_side{ false };
    bool              m_viewports_split{ false };
    bool              m_double_width{ false };
    int               m_native_frames{ 0 };
    int               m_stereo_frames{ 0 };
    int               m_register_attempt_frame{ -1000 };
    int               m_missed_appends{ 0 };
    std::atomic<bool> m_appended{ false };

    float m_saved_viewport[4]{ 0.0f, 1.0f, 1.0f, 0.0f };
    float m_saved_scissors[4]{ 0.0f, 1.0f, 1.0f, 0.0f };
    float m_saved_starfield_viewport[4]{ 0.0f, 1.0f, 1.0f, 0.0f };
    float m_saved_starfield_scissors[4]{ 0.0f, 1.0f, 1.0f, 0.0f };

    // Main view rows last copied to the right view.
    struct MirroredRows
    {
        bool     valid{ false };
        uint8_t  camera_view_data[0x1C]{};
        uint32_t image_space{ 0 };
        uint8_t  feature_setup[0x210]{};
        uint8_t  directional_shadow[0x3C]{};
        uint8_t  highlight[0x14]{};
    } m_mirrored{};
    MirroredRows m_left_mirrored{};
};
