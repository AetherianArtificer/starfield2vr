#include "StereoViewModule.h"

#include "CreationEngineRendererModule.h"
#include "CreationEngineSettings.h"
#include <_deps/directxtk12-src/Src/d3dx12.h>
#include <intrin.h>
#include <safetyhook/easy.hpp>
#include "CreationEngineSingletonManager.h"
#include "VROptions.h"
#include <CreationEngine/memory/offsets.h>
#include <CreationEngine/memory/stereo_offsets.h>
#include <CreationEngine/models/GameFlow.h>
#include <RE/C/CreationRendererPrivate.h>
#include <set>
#include <CreationEngine/models/ModSettingsStore.h>
#include <Framework.hpp>
#include <RE/M/Main.h>
#include <RE/N/NiCamera.h>
#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <fstream>
#include <mods/VR.hpp>
#include <nvidia/UpscalerAfrNvidiaModule.h>

#include "ModSettings.h"

namespace
{
    namespace offsets = Steam::MemoryOffsets::Stereo;

    constexpr uint32_t kInvalidId = 0xFFFFFF;

    // StorageTable columns of the camera view table, as offsets into the table singleton.
    constexpr size_t kDirectionalShadowColumn = 0xF0;
    constexpr size_t kHighlightColumn         = 0x100;
    constexpr size_t kFeatureSetupColumn      = 0x108;
    constexpr size_t kImageSpaceColumn        = 0x110;
    constexpr size_t kCameraViewDataColumn    = 0x120;
    // Render graph table column holding each graph's MultiCameraViewData.
    constexpr size_t kMultiCameraViewColumn = 0x120;
    constexpr size_t kGraphOptionsColumn    = 0xE8;
    constexpr size_t kGraphFrameFlagColumn  = 0x108;
    constexpr size_t kGraphKeyColumn        = 0x128;

    constexpr size_t kCameraViewDataSize     = 0x1C;
    constexpr size_t kFeatureSetupSize       = 0x210;
    constexpr size_t kDirectionalShadowSize  = 0x3C;
    constexpr size_t kHighlightSize          = 0x14;
    constexpr size_t kCameraViewIdOffset     = 0x4;  // CameraViewData::cameraHandleId
    constexpr size_t kAttachChildVtableIndex = 0x2A0 / 8;

    constexpr size_t kRootWorldCameraRoot = 0x78;
    constexpr size_t kRootStarfieldCamera = 0xA8;
    constexpr size_t kRootMainView        = 0x3D0;
    constexpr size_t kRootMainRenderGraph = 0x3D8;
    constexpr size_t kRootMainGraphRecord = 0x3E0;

    constexpr size_t kCameraClipspaceType = 0x1D8;
    constexpr size_t kCameraMinNear       = 0x1DC;
    constexpr size_t kCameraAspect        = 0x1E0;
    constexpr size_t kCameraViewport      = 0x1E4;
    constexpr size_t kCameraScissors      = 0x1F4;
    constexpr size_t kPassCameraView      = 0x24;

    constexpr int kFramesUntilStable   = 600;
    constexpr int kFramesUntilSnapshot = 400;

    template <class T>
    T& At(void* base, size_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
    }

    template <class F>
    F Fn(uintptr_t address)
    {
        return reinterpret_cast<F>(address);
    }

    uint8_t* StorageColumn(uintptr_t storage_global, size_t column)
    {
        auto storage = *reinterpret_cast<uint8_t**>(storage_global);
        return storage ? *reinterpret_cast<uint8_t**>(storage + column) : nullptr;
    }

    const uint8_t* StorageRow(uint8_t* column, uint32_t id, size_t row_size)
    {
        if (column == nullptr) {
            return nullptr;
        }
        auto indexes = *reinterpret_cast<uint32_t**>(column + 0x2C8);
        auto data    = *reinterpret_cast<uint8_t**>(column + 0x3C8);
        if (indexes == nullptr || data == nullptr) {
            return nullptr;
        }
        return data + (size_t)indexes[id & 0xFFFFFF] * row_size;
    }

    void CommitColumn(uint8_t* column)
    {
        const auto slot   = Fn<uint32_t (*)()>(offsets::StorageThreadSlot())();
        const auto writer = Fn<void* (*)(void*, uint32_t)>(offsets::StorageColumnWriter())(column + 0xC0, slot);
        Fn<void (*)(void*, void*)>(offsets::StorageColumnCommit())(column, writer);
    }

    void CameraIncRef(void* camera)
    {
        _InterlockedIncrement(&At<volatile long>(camera, 0x8));
    }

    void SetCameraRect(RE::NiCamera* camera, uintptr_t setter, const float rect[4])
    {
        float copy[4]{ rect[0], rect[1], rect[2], rect[3] };
        Fn<void (*)(RE::NiCamera*, float*)>(setter)(camera, copy);
    }

    bool SameRect(RE::NiCamera* camera, size_t offset, const float rect[4])
    {
        return std::memcmp(&At<float>(camera, offset), rect, sizeof(float) * 4) == 0;
    }

    struct RegisterContext
    {
        RE::Main::SceneGraphRoot* root;
        RE::NiCamera*             world_camera;
        RE::NiCamera*             camera;
        uint32_t                  view_id;
        uint32_t                  step;
        const char*               name;
    };

    // Mirrors the engine's own setup of the world camera and its view (scene root initialisation).
    void RegisterEngineObjects(RegisterContext* ctx)
    {
        ctx->step   = 1;
        auto camera = Fn<void* (*)(size_t)>(offsets::GameAllocate())(0x220);
        if (camera == nullptr) {
            return;
        }
        Fn<void* (*)(void*)>(offsets::NiCameraConstruct())(camera);
        CameraIncRef(camera);
        ctx->camera = static_cast<RE::NiCamera*>(camera);

        ctx->step         = 2;
        uintptr_t name    = 0;
        Fn<void (*)(uintptr_t*, const char*, uint8_t)>(offsets::FixedStringCreate())(&name, ctx->name, 0);
        std::swap(name, At<uintptr_t>(camera, 0x10));
        Fn<void (*)(uintptr_t*)>(offsets::FixedStringRelease())(&name);

        ctx->step = 3;
        Fn<void (*)(void*)>(offsets::NiCameraRegisterAsRenderCamera())(camera);
        At<float>(camera, kCameraMinNear) = At<float>(ctx->world_camera, kCameraMinNear);
        At<float>(camera, kCameraAspect)  = At<float>(ctx->world_camera, kCameraAspect);
        Fn<void (*)(void*, float, float)>(offsets::NiCameraSetNearFar())(camera, ctx->world_camera->viewFrustum._near, ctx->world_camera->viewFrustum._far);
        Fn<void (*)(void*, uint8_t)>(offsets::NiCameraSetClipspaceType())(camera, At<uint8_t>(ctx->world_camera, kCameraClipspaceType));
        ctx->camera->local = ctx->world_camera->local;

        ctx->step  = 4;
        auto owner = At<void*>(ctx->root, kRootWorldCameraRoot);
        auto vtbl  = *reinterpret_cast<uintptr_t**>(owner);
        Fn<void* (*)(void*, void*, bool)>(vtbl[kAttachChildVtableIndex])(owner, camera, true);

        ctx->step     = 5;
        ctx->view_id  = kInvalidId;
        Fn<void (*)(uint32_t*)>(offsets::CameraViewRegister())(&ctx->view_id);
        if (ctx->view_id == kInvalidId) {
            return;
        }

        // Exposure readback futures, laid out like the engine's own control blocks.
        ctx->step       = 6;
        const auto vtable = offsets::AverageLuminanceReadbackVtable();
        uintptr_t  readback_row[4]{};
        for (int i = 0; i < 2; ++i) {
            auto block = static_cast<uint8_t*>(Fn<void* (*)(size_t)>(offsets::SmallAllocate())(0x18));
            if (block == nullptr) {
                return;
            }
            std::memset(block, 0, 0x18);
            At<uintptr_t>(block, 0x0) = vtable;
            At<uint32_t>(block, 0x8)  = 1;
            At<uint32_t>(block, 0xC)  = 1;
            readback_row[i * 2]       = reinterpret_cast<uintptr_t>(block + 0x10);
            readback_row[i * 2 + 1]   = reinterpret_cast<uintptr_t>(block);
        }
        Fn<void (*)(uint32_t*, uintptr_t*)>(offsets::WriteAverageLuminanceReadback())(&ctx->view_id, readback_row);
        ctx->step = 7;
    }

    bool RegisterEngineObjectsGuarded(RegisterContext* ctx)
    {
        __try {
            RegisterEngineObjects(ctx);
            return ctx->step == 7;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    struct GraphContext
    {
        uint8_t*  record;
        uint8_t*  main_record;
        uint32_t* graph_id;
        uint8_t   main_key;
        uintptr_t main_name;
        uint32_t  step;
    };

    // Mirrors the scene root's setup of the main render graph (submission record, sort key, name, options).
    void RegisterGraphObjects(GraphContext* ctx)
    {
        ctx->step = 1;
        Fn<void (*)(uint32_t*)>(offsets::RenderGraphRegister())(ctx->graph_id);
        if (*ctx->graph_id == kInvalidId) {
            return;
        }

        // The engine builds a graph by its key and name, so the right eye's graph copies the main scene graph's
        // ("Frame"); the name string is shared, with its reference count raised for the copy.
        ctx->step = 2;
        struct
        {
            uint8_t   key;
            uint8_t   pad[7];
            uintptr_t name;
        } key{ ctx->main_key, {}, ctx->main_name };
        if (key.name) {
            _InterlockedIncrement(&At<volatile long>(reinterpret_cast<void*>(key.name), 0x10));
        }
        Fn<void (*)(uint32_t*, void*)>(offsets::WriteRenderGraphKey())(ctx->graph_id, &key);
        if (key.name) {
            Fn<void (*)(uintptr_t*)>(offsets::FixedStringRelease())(&key.name);
        }

        // An empty inline job list, then the main graph's jobs (context and feature flags) copied in.
        ctx->step = 3;
        std::memset(ctx->record, 0, 0x80);
        At<uint32_t>(ctx->record, 0x8)  = 0x80000000;
        At<uint32_t>(ctx->record, 0x78) = *ctx->graph_id;
        const auto count  = At<uint32_t>(ctx->main_record, 0x0);
        const auto local  = At<int32_t>(ctx->main_record, 0x8) < 0;
        auto       jobs   = local ? ctx->main_record + 0x10 : At<uint8_t*>(ctx->main_record, 0x10);
        for (uint32_t i = 0; jobs && i < count && i < 4; ++i) {
            auto job = jobs + 0x18 * (size_t)i;
            Fn<void (*)(void*, uint32_t, void*)>(offsets::AddRenderGraphJob())(ctx->record, At<uint32_t>(job, 0x10), At<void*>(job, 0x0));
        }
        if (count == 0) {
            return;
        }
        ctx->step = 4;
    }

    bool RegisterGraphObjectsGuarded(GraphContext* ctx)
    {
        __try {
            RegisterGraphObjects(ctx);
            return ctx->step == 4;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::filesystem::path CrashGuardPath()
    {
        return Framework::get_persistent_dir("vr_native_stereo.guard");
    }
} // namespace

struct StereoViewModule::ViewIdArray
{
    uint32_t size;
    uint32_t pad0;
    int32_t  capacity;  // sign bit set: the ids are stored inline
    uint32_t pad1;
    union
    {
        uint32_t  local[4];
        uint32_t* heap;
    };

    uint32_t* data() { return capacity < 0 ? local : heap; }
};
static_assert(sizeof(uintptr_t) == 8);

void StereoViewModule::InstallHooks()
{
    const std::pair<const char*, uintptr_t> required[]{
        { "NiCameraConstruct", offsets::NiCameraConstruct() },
        { "NiCameraRegisterAsRenderCamera", offsets::NiCameraRegisterAsRenderCamera() },
        { "NiCameraSetNearFar", offsets::NiCameraSetNearFar() },
        { "NiCameraSetViewport", offsets::NiCameraSetViewport() },
        { "NiCameraSetScissors", offsets::NiCameraSetScissors() },
        { "NiCameraSetClipspaceType", offsets::NiCameraSetClipspaceType() },
        { "CameraViewRegister", offsets::CameraViewRegister() },
        { "WriteCameraViewData", offsets::WriteCameraViewData() },
        { "WriteAverageLuminanceReadback", offsets::WriteAverageLuminanceReadback() },
        { "WriteImageSpaceData", offsets::WriteImageSpaceData() },
        { "WriteDirectionalShadowData", offsets::WriteDirectionalShadowData() },
        { "WriteHighlightSettings", offsets::WriteHighlightSettings() },
        { "BeginWriteFeatureSetup", offsets::BeginWriteFeatureSetup() },
        { "StorageThreadSlot", offsets::StorageThreadSlot() },
        { "StorageColumnWriter", offsets::StorageColumnWriter() },
        { "StorageColumnCommit", offsets::StorageColumnCommit() },
        { "SetMultiCameraViewData", offsets::SetMultiCameraViewData() },
        { "GameAllocate", offsets::GameAllocate() },
        { "SmallAllocate", offsets::SmallAllocate() },
        { "FixedStringCreate", offsets::FixedStringCreate() },
        { "FixedStringRelease", offsets::FixedStringRelease() },
        { "CameraViewStorage", offsets::CameraViewStorage() },
        { "RenderGraphStorage", offsets::RenderGraphStorage() },
        { "AverageLuminanceReadbackVtable", offsets::AverageLuminanceReadbackVtable() },
        { "RenderGraphRegister", offsets::RenderGraphRegister() },
        { "AddRenderGraphJob", offsets::AddRenderGraphJob() },
        { "SubmitRenderGraph", offsets::SubmitRenderGraph() },
        { "WriteRenderGraphOptions", offsets::WriteRenderGraphOptions() },
        { "WriteRenderGraphFrameFlag", offsets::WriteRenderGraphFrameFlag() },
        { "WriteRenderGraphKey", offsets::WriteRenderGraphKey() },
        { "NiCameraSetFrustum", GameStore::MemoryOffsets::NiCamera::SetFrustumVfunc() },
    };
    m_signatures_ok = true;
    for (auto& [name, address] : required) {
        if (address == 0) {
            spdlog::error("[Stereo] Signature not found: {}; native stereo is unavailable", name);
            m_signatures_ok = false;
        }
    }
    if (!m_signatures_ok) {
        return;
    }

    m_set_multi_view_hook = std::make_unique<FunctionHook>(offsets::SetMultiCameraViewData(), reinterpret_cast<uintptr_t>(&onSetMultiCameraViewData));
    m_set_multi_view_hook->create();
    m_submit_graph_hook = std::make_unique<FunctionHook>(offsets::SubmitRenderGraph(), reinterpret_cast<uintptr_t>(&onSubmitRenderGraph));
    m_submit_graph_hook->create();
    InstallUpscalerHooks();

    m_probe_builder = offsets::BuildReflectionProbeSubGraph();
    if (auto primary = offsets::IsPrimarySceneView(); primary && m_probe_builder) {
        m_primary_view_hook = std::make_unique<FunctionHook>(primary, reinterpret_cast<uintptr_t>(&onIsPrimarySceneView));
        m_primary_view_hook->create();
    }

    if (auto child = offsets::RenderGraphExecuteChild()) {
        m_execute_child_hook = std::make_unique<FunctionHook>(child, reinterpret_cast<uintptr_t>(&onExecuteChildPass));
        m_execute_child_hook->create();
    }
    if (auto vtable = reinterpret_cast<uintptr_t*>(
            MemoryScan::VTable("ScaleformCompositeRenderPass", ".?AVScaleformCompositeRenderPass@CreationRendererPrivate@@", 0))) {
        m_scaleform_composite_hook = std::make_unique<FunctionHook>(vtable[7], reinterpret_cast<uintptr_t>(&onScaleformComposite));
        m_scaleform_composite_hook->create();
    }
    if (auto vtable = reinterpret_cast<uintptr_t*>(
            MemoryScan::VTable("CopyToRenderGraphOutputRenderPass", ".?AVCopyToRenderGraphOutputRenderPass@CreationRendererPrivate@@", 0))) {
        m_copy_to_output_hook = std::make_unique<FunctionHook>(vtable[7], reinterpret_cast<uintptr_t>(&onCopyToRenderGraphOutput));
        m_copy_to_output_hook->create();
    } else {
        spdlog::error("[Stereo] CopyToRenderGraphOutputRenderPass not found; eye images cannot be placed");
    }
    spdlog::info("[Stereo] Native stereo hooks installed (stereo module built " __DATE__ " " __TIME__ ")");
}

int StereoViewModule::EyeOf(const RE::NiCamera* camera) const
{
    if (camera == nullptr) {
        return -1;
    }
    if (camera == m_right_camera) {
        return 1;
    }
    if (camera == m_left_camera) {
        return 0;
    }
    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    if (root && (camera == root->worldCamera || camera == root->starfieldScene.pStarFieldCamera || camera == root->starfieldScene.pGalaxyCamera)) {
        return 0;
    }
    return -1;
}

bool StereoViewModule::TryRegister()
{
    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    if (root == nullptr || root->worldCamera == nullptr || root->worldCamera->cameraHandleID == kInvalidId || At<uint32_t>(root, kRootMainView) == kInvalidId ||
        At<void*>(root, kRootWorldCameraRoot) == nullptr) {
        return false;
    }

    {
        std::ofstream guard{ CrashGuardPath() };
        guard << "native stereo starting\n";
    }

    RegisterContext ctx{ root, root->worldCamera, nullptr, kInvalidId, 0, "VR Right Eye Camera" };
    RegisterContext left_ctx{ root, root->worldCamera, nullptr, kInvalidId, 0, "VR Left Eye Camera" };
    if (!RegisterEngineObjectsGuarded(&ctx) || !RegisterEngineObjectsGuarded(&left_ctx)) {
        spdlog::error("[Stereo] Registering the eye views failed at step {}/{}; using alternate eye rendering", ctx.step, left_ctx.step);
        m_failed = true;
        ClearCrashGuard();
        return false;
    }
    m_left_camera   = left_ctx.camera;
    m_left_view_id  = left_ctx.view_id;
    m_left_mirrored = {};

    m_right_camera  = ctx.camera;
    m_right_view_id = ctx.view_id;
    m_registered    = true;
    m_mirrored      = {};
    if (!RegisterRightGraph()) {
        spdlog::error("[Stereo] Registering the right eye's render graph failed; using alternate eye rendering");
        m_failed     = true;
        m_registered = false;
        ClearCrashGuard();
        return false;
    }
    spdlog::info("[Stereo] Left eye view {:x} (camera {:x}) registered", m_left_view_id, m_left_camera->cameraHandleID);
    spdlog::info("[Stereo] Right eye view registered: camera handle {:x}, view {:x}; main view {:x}, render graph {:x}", m_right_camera->cameraHandleID, m_right_view_id,
                 At<uint32_t>(root, kRootMainView), At<uint32_t>(root, kRootMainRenderGraph));
    return true;
}

void StereoViewModule::ClearCrashGuard()
{
    std::error_code ec;
    std::filesystem::remove(CrashGuardPath(), ec);
}

void StereoViewModule::OnFrameStart()
{
    static auto vr = VR::get();

    // Runs after the config has loaded, so the fallback choice is saved.
    static bool guard_checked{ false };
    if (!guard_checked && m_signatures_ok) {
        guard_checked = true;
        std::error_code ec;
        if (std::filesystem::exists(CrashGuardPath(), ec)) {
            spdlog::error("[Stereo] The previous session ended while native stereo was starting; switching to alternate eye rendering");
            std::filesystem::remove(CrashGuardPath(), ec);
            VROptions::Get()->set(VROptions::kStereoRendering, 1);
        }
    }

    UpdateWorldFreeze();

    const bool requested = m_signatures_ok && !m_failed && vr->is_hmd_active() && vr->get_runtime()->is_openxr() &&
                           GameFlow::gStore.internalSettings.nativeStereo;

    if (requested && !m_registered && vr->m_engine_frame_count - m_register_attempt_frame > 60) {
        m_register_attempt_frame = vr->m_engine_frame_count;
        TryRegister();
    }

    const bool double_width = GameFlow::gStore.internalSettings.stereoDoubleWidth;
    if (double_width != m_double_width) {
        m_double_width   = double_width;
        m_native_frames  = 0;
        m_stereo_frames  = 0;
    }
    m_wants_side_by_side = requested && m_registered && double_width;

    // A double-width layout starts only once the back buffer holds both eyes.
    const auto backbuffer = vr->get_backbuffer_size();
    const auto eye_width  = (uint32_t)vr->get_hmd_width();
    const bool buffer_ready = double_width ? eye_width > 0 && backbuffer[0] + 8 >= eye_width * 2 : backbuffer[0] > 0;
    vr->request_native_stereo(requested && m_registered && buffer_ready);

    const bool native = vr->is_native_stereo();
    UpdateMenuFallback(native);
    ApplyViewports(native);
    ApplyNativeShadowSettings(native);
    if (!native) {
        m_missed_appends = 0;
        vr->set_native_mono_frame(false);
        return;
    }

    MirrorMainView();
    UpdateRightFrustum();
    ReportUpscalers();
    if (m_census_frames.load() > 0 && m_census_frames.fetch_sub(1) == 1) {
        FinishCensus();
    }

    m_missed_appends = m_appended.exchange(false) ? 0 : m_missed_appends + 1;
    vr->set_native_mono_frame(m_menu_fallback.load() || m_missed_appends > 1);

    if (++m_native_frames == kFramesUntilStable) {
        ClearCrashGuard();
        spdlog::info("[Stereo] Native stereo stable for {} frames", kFramesUntilStable);
    }
    if (m_renew_right_camera.exchange(false)) {
        RenewRightCamera();
    }

    // Snapshot once both views have been rendering in first-person gameplay for a while, again after any test switch.
    const auto variant = Variant();
    if (variant != m_last_variant) {
        m_last_variant  = variant;
        m_stereo_frames = 0;
        spdlog::info("[Stereo] Test variant: {}", variant);
    }
    const bool gameplay = m_missed_appends == 0 && !GameFlow::isShowingMenu() && GameFlow::isInFirstPerson();
    if (gameplay && ++m_stereo_frames == kFramesUntilSnapshot) {
        LogRenderSizes();
        StartCensus();
        vr->request_backbuffer_dump(Framework::get_persistent_dir(std::format("vr_native_stereo_{}.png", variant)).wstring());
    }
}

void StereoViewModule::LogRenderSizes() const
{
    static auto vr       = VR::get();
    auto        settings = CreationEngineRendererModule::Get()->GetCreationEngineSettings();
    const auto  backbuffer = vr->get_backbuffer_size();
    if (settings == nullptr) {
        return;
    }
    const auto& display = settings->displayGameSettings;
    spdlog::info("[Stereo] Sizes: back buffer {}x{}, eye {}x{}, window {}x{}, texture {}x{}, window rect ({}, {}, {}, {}), display rect ({}, {}, {}, {}), flags {:x}",
                 backbuffer[0], backbuffer[1], vr->get_hmd_width(), vr->get_hmd_height(), settings->windowWidth, settings->windowHeight, settings->textureWidth,
                 settings->textureHeight, display.windowRect.x, display.windowRect.y, display.windowRect.cx, display.windowRect.cy, display.displayRect.x,
                 display.displayRect.y, display.displayRect.cx, display.displayRect.cy, display.flags);
}

void StereoViewModule::ApplyViewports(bool side_by_side)
{
    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    if (root == nullptr || root->worldCamera == nullptr) {
        return;
    }
    auto world_camera     = root->worldCamera;
    auto starfield_camera = At<RE::NiCamera*>(root, kRootStarfieldCamera);

    if (side_by_side) {
        if (!m_viewports_split) {
            std::memcpy(m_saved_viewport, &At<float>(world_camera, kCameraViewport), sizeof(m_saved_viewport));
            std::memcpy(m_saved_scissors, &At<float>(world_camera, kCameraScissors), sizeof(m_saved_scissors));
            if (starfield_camera) {
                std::memcpy(m_saved_starfield_viewport, &At<float>(starfield_camera, kCameraViewport), sizeof(m_saved_starfield_viewport));
                std::memcpy(m_saved_starfield_scissors, &At<float>(starfield_camera, kCameraScissors), sizeof(m_saved_starfield_scissors));
            }
            const float full[4]{ 0.0f, 1.0f, 1.0f, 0.0f };
            if (std::abs(m_saved_viewport[1] - m_saved_viewport[0]) < 0.01f) {
                std::memcpy(m_saved_viewport, full, sizeof(full));
            }
            if (std::abs(m_saved_scissors[1] - m_saved_scissors[0]) < 0.99f) {
                std::memcpy(m_saved_scissors, full, sizeof(full));
            }
            if (std::abs(m_saved_starfield_scissors[1] - m_saved_starfield_scissors[0]) < 0.99f) {
                std::memcpy(m_saved_starfield_scissors, full, sizeof(full));
            }
            spdlog::info("[Stereo] Splitting views; world camera viewport was ({}, {}, {}, {}) scissors ({}, {}, {}, {})", m_saved_viewport[0], m_saved_viewport[1],
                         m_saved_viewport[2], m_saved_viewport[3], m_saved_scissors[0], m_saved_scissors[1], m_saved_scissors[2], m_saved_scissors[3]);
            m_viewports_split = true;
        }
        // Halves of the camera's own full-frame rect (left, right, top, bottom).
        const float middle = (m_saved_viewport[0] + m_saved_viewport[1]) * 0.5f;
        const float left_half[4]{ m_saved_viewport[0], middle, m_saved_viewport[2], m_saved_viewport[3] };
        const float right_half[4]{ middle, m_saved_viewport[1], m_saved_viewport[2], m_saved_viewport[3] };

        // The engine may reset a camera's viewport, so it is reapplied whenever it differs. Scissors stay full: culling
        // tests each eye's own screen space against them, so a half-width scissor culls half of every eye's view.
        const bool world_draws = m_left_camera == nullptr || m_menu_fallback.load();
        for (auto camera : { m_left_camera, world_draws ? world_camera : nullptr, starfield_camera }) {
            if (camera && !SameRect(camera, kCameraViewport, left_half)) {
                SetCameraRect(camera, offsets::NiCameraSetViewport(), left_half);
            }
            const float* full_scissors = camera == starfield_camera ? m_saved_starfield_scissors : m_saved_scissors;
            if (camera && !SameRect(camera, kCameraScissors, full_scissors)) {
                SetCameraRect(camera, offsets::NiCameraSetScissors(), full_scissors);
            }
        }
        if (!world_draws && !SameRect(world_camera, kCameraViewport, m_saved_viewport)) {
            SetCameraRect(world_camera, offsets::NiCameraSetViewport(), m_saved_viewport);
            SetCameraRect(world_camera, offsets::NiCameraSetScissors(), m_saved_scissors);
        }
        if (m_right_camera && !SameRect(m_right_camera, kCameraViewport, right_half)) {
            SetCameraRect(m_right_camera, offsets::NiCameraSetViewport(), right_half);
        }
        if (m_right_camera && !SameRect(m_right_camera, kCameraScissors, m_saved_scissors)) {
            SetCameraRect(m_right_camera, offsets::NiCameraSetScissors(), m_saved_scissors);
        }
    } else if (m_viewports_split) {
        SetCameraRect(world_camera, offsets::NiCameraSetViewport(), m_saved_viewport);
        SetCameraRect(world_camera, offsets::NiCameraSetScissors(), m_saved_scissors);
        if (starfield_camera) {
            SetCameraRect(starfield_camera, offsets::NiCameraSetViewport(), m_saved_starfield_viewport);
            SetCameraRect(starfield_camera, offsets::NiCameraSetScissors(), m_saved_starfield_scissors);
        }
        m_viewports_split = false;
        m_native_frames   = 0;
        spdlog::info("[Stereo] Views restored to full width");
    }
}

void StereoViewModule::MirrorMainView()
{
    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    if (!m_registered || root == nullptr) {
        return;
    }
    const auto main_view = At<uint32_t>(root, kRootMainView);
    if (main_view == kInvalidId) {
        return;
    }
    MirrorInto(main_view, m_right_view_id, m_right_camera, m_mirrored);
    if (m_left_camera) {
        MirrorInto(main_view, m_left_view_id, m_left_camera, m_left_mirrored);
    }
}

void StereoViewModule::MirrorInto(uint32_t main_view, uint32_t view_id, RE::NiCamera* camera, MirroredRows& mirrored)
{
    const auto storage = offsets::CameraViewStorage();

    MirroredRows current{};
    auto         read = [&](size_t column, size_t size, void* out) {
        auto row = StorageRow(StorageColumn(storage, column), main_view, size);
        if (row) {
            std::memcpy(out, row, size);
        }
        return row != nullptr;
    };
    if (!read(kCameraViewDataColumn, kCameraViewDataSize, current.camera_view_data) || !read(kImageSpaceColumn, sizeof(uint32_t), &current.image_space) ||
        !read(kFeatureSetupColumn, kFeatureSetupSize, current.feature_setup) || !read(kDirectionalShadowColumn, kDirectionalShadowSize, current.directional_shadow) ||
        !read(kHighlightColumn, kHighlightSize, current.highlight)) {
        return;
    }
    At<uint32_t>(current.camera_view_data, kCameraViewIdOffset) = camera->cameraHandleID;

    const bool first   = !mirrored.valid;
    auto       changed = [&](const void* a, const void* b, size_t size) { return first || std::memcmp(a, b, size) != 0; };
    auto       id      = view_id;

    if (changed(current.camera_view_data, mirrored.camera_view_data, kCameraViewDataSize)) {
        Fn<void (*)(uint32_t*, void*)>(offsets::WriteCameraViewData())(&id, current.camera_view_data);
    }
    if (changed(&current.image_space, &mirrored.image_space, sizeof(uint32_t))) {
        Fn<void (*)(uint32_t*, uint32_t*)>(offsets::WriteImageSpaceData())(&id, &current.image_space);
    }
    if (changed(current.directional_shadow, mirrored.directional_shadow, kDirectionalShadowSize)) {
        Fn<void (*)(uint32_t*, void*)>(offsets::WriteDirectionalShadowData())(&id, current.directional_shadow);
    }
    if (changed(current.highlight, mirrored.highlight, kHighlightSize)) {
        Fn<void (*)(uint32_t*, void*)>(offsets::WriteHighlightSettings())(&id, current.highlight);
    }
    if (changed(current.feature_setup, mirrored.feature_setup, kFeatureSetupSize)) {
        auto row = Fn<uint8_t* (*)(uint32_t)>(offsets::BeginWriteFeatureSetup())(id);
        if (row) {
            std::memcpy(row, current.feature_setup, kFeatureSetupSize);
            CommitColumn(StorageColumn(storage, kFeatureSetupColumn));
        }
    }
    if (first) {
        spdlog::info("[Stereo] Eye view {:x} mirrors main view {:x}", view_id, main_view);
    }
    current.valid = true;
    mirrored      = current;
}

void StereoViewModule::UpdateRightFrustum()
{
    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    if (!m_right_camera || root == nullptr || root->worldCamera == nullptr) {
        return;
    }
    auto world_camera = root->worldCamera;
    At<float>(m_right_camera, kCameraMinNear) = At<float>(world_camera, kCameraMinNear);
    At<float>(m_right_camera, kCameraAspect)  = At<float>(world_camera, kCameraAspect);
    if (At<uint8_t>(m_right_camera, kCameraClipspaceType) != At<uint8_t>(world_camera, kCameraClipspaceType)) {
        Fn<void (*)(void*, uint8_t)>(offsets::NiCameraSetClipspaceType())(m_right_camera, At<uint8_t>(world_camera, kCameraClipspaceType));
    }

    // Through the hooked setter, which fills in each camera's eye frustum.
    using set_frustum_t = void (*)(RE::NiCamera*, RE::NiFrustum*);
    static auto set_frustum = Fn<set_frustum_t>(GameStore::MemoryOffsets::NiCamera::SetFrustumVfunc());
    auto        left        = world_camera->viewFrustum;
    auto        right       = world_camera->viewFrustum;
    set_frustum(world_camera, &left);
    set_frustum(m_right_camera, &right);
    if (m_left_camera) {
        At<float>(m_left_camera, kCameraMinNear) = At<float>(world_camera, kCameraMinNear);
        At<float>(m_left_camera, kCameraAspect)  = At<float>(world_camera, kCameraAspect);
        if (At<uint8_t>(m_left_camera, kCameraClipspaceType) != At<uint8_t>(world_camera, kCameraClipspaceType)) {
            Fn<void (*)(void*, uint8_t)>(offsets::NiCameraSetClipspaceType())(m_left_camera, At<uint8_t>(world_camera, kCameraClipspaceType));
        }
        auto left_clone = world_camera->viewFrustum;
        set_frustum(m_left_camera, &left_clone);
    }
}

uintptr_t StereoViewModule::onSetMultiCameraViewData(void* column, uint32_t graph_index, ViewIdArray* views)
{
    static auto instance = Get();
    using func_t         = decltype(onSetMultiCameraViewData);
    static auto original = instance->m_set_multi_view_hook->get_original<func_t>();
    static auto vr       = VR::get();

    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    if (instance->m_right_graph_ready && instance->m_left_camera && vr->is_native_stereo() && !instance->m_menu_fallback.load() && views && root && views->size == 1 &&
        column == StorageColumn(offsets::RenderGraphStorage(), kMultiCameraViewColumn) && graph_index == (At<uint32_t>(root, kRootMainRenderGraph) & 0xFFFFFF) &&
        (views->data()[0] & 0xFFFFFF) == (At<uint32_t>(root, kRootMainView) & 0xFFFFFF)) {
        ViewIdArray left{};
        left.size     = 1;
        left.capacity = static_cast<int32_t>(0x80000004u);
        left.local[0] = instance->m_exp_swap_graphs ? instance->m_right_view_id : instance->m_left_view_id;
        static bool logged{ false };
        if (!logged) {
            logged = true;
            spdlog::info("[Stereo] Main render graph renders the left eye view {:x} instead of the world camera's view", instance->m_left_view_id);
        }
        return original(column, graph_index, &left);
    }
    if (instance->m_right_graph_ready || !vr->is_native_stereo() || !instance->m_registered || views == nullptr || root == nullptr || views->size != 1 ||
        column != StorageColumn(offsets::RenderGraphStorage(), kMultiCameraViewColumn) || graph_index != (At<uint32_t>(root, kRootMainRenderGraph) & 0xFFFFFF)) {
        return original(column, graph_index, views);
    }

    const auto first = views->data()[0];
    if ((first & 0xFFFFFF) != (At<uint32_t>(root, kRootMainView) & 0xFFFFFF)) {
        static uint32_t logged_view{ kInvalidId };
        if (logged_view != first) {
            logged_view = first;
            spdlog::info("[Stereo] Main render graph shows view {:x}, which has no right eye; showing it in both eyes", first);
        }
        return original(column, graph_index, views);
    }

    // A copy with both eyes; the caller's array is left untouched.
    ViewIdArray both{};
    both.size     = 2;
    both.capacity = static_cast<int32_t>(0x80000004u);
    both.local[0] = first;
    both.local[1] = instance->m_right_view_id;
    instance->m_appended.store(true);

    static bool logged{ false };
    if (!logged) {
        logged = true;
        spdlog::info("[Stereo] Main render graph {:x} now renders views {:x} and {:x}", graph_index, first, instance->m_right_view_id);
    }
    return original(column, graph_index, &both);
}

namespace
{
    struct UpscalerPassInfo
    {
        const char* label;
        const char* rtti;
        bool        dlss;
        bool        shares_history;
    };

    constexpr UpscalerPassInfo kUpscalerPasses[]{
        { "DLSS inputs", ".?AVDLSSProduceInputsRenderPass@CreationRendererPrivate@@", true, false },
        { "DLSS", ".?AVDLSSUpscaleRenderPass@CreationRendererPrivate@@", true, false },
        { "XeSS inputs", ".?AVXeSSProduceInputsRenderPass@CreationRendererPrivate@@", false, true },
        { "XeSS", ".?AVXeSSUpscaleRenderPass@CreationRendererPrivate@@", false, true },
        { "FSR2 inputs", ".?AVFSR2ProduceInputsRenderPass@CreationRendererPrivate@@", false, true },
        { "FSR2", ".?AVFSR2UpscaleRenderPass@CreationRendererPrivate@@", false, true },
        { "FSR3", ".?AVFSR3UpscaleRenderPass@CreationRendererPrivate@@", false, true },
    };

    template <int... Pass>
    constexpr std::array<uintptr_t, sizeof...(Pass)> UpscalerDetours(std::integer_sequence<int, Pass...>)
    {
        return { reinterpret_cast<uintptr_t>(&StereoViewModule::onUpscalerPass<Pass>)... };
    }
} // namespace

void StereoViewModule::InstallUpscalerHooks()
{
    static_assert(std::size(kUpscalerPasses) == kUpscalerPassCount);
    static const auto detours = UpscalerDetours(std::make_integer_sequence<int, kUpscalerPassCount>{});
    for (int i = 0; i < kUpscalerPassCount; ++i) {
        auto vtable = reinterpret_cast<uintptr_t*>(MemoryScan::VTable(kUpscalerPasses[i].label, kUpscalerPasses[i].rtti, 0));
        if (vtable == nullptr) {
            spdlog::warn("[Stereo] {} pass not found", kUpscalerPasses[i].label);
            continue;
        }
        m_upscaler_hooks[i] = std::make_unique<FunctionHook>(vtable[7], detours[i]);
        m_upscaler_hooks[i]->create();
    }
}

uintptr_t StereoViewModule::RunUpscalerPass(int pass_kind, void* pass, void* render_graph_data, void* pass_data)
{
    using func_t  = uintptr_t(void*, void*, void*);
    auto original = m_upscaler_hooks[pass_kind]->get_original<func_t>();
    static auto vr = VR::get();

    if (!vr->is_native_stereo() || pass == nullptr) {
        return original(pass, render_graph_data, pass_data);
    }

    const auto view        = At<uint32_t>(pass, kPassCameraView);
    const auto viewport_id = render_graph_data ? At<uint32_t>(render_graph_data, 0x140) : 0u;
    uint32_t   scene_id    = kInvalidId;
    if (render_graph_data && viewport_id < 8) {
        auto base = At<uint8_t*>(render_graph_data, 0x108 + 8 * (size_t)viewport_id);
        if (base) {
            scene_id = At<uint32_t>(base, 0x24);
        }
    }
    const auto right_view   = m_right_view_id & 0xFFFFFF;
    const auto right_camera = m_right_camera ? m_right_camera->cameraHandleID & 0xFFFFFF : kInvalidId;
    const bool right        = (view & 0xFFFFFF) == right_view || (scene_id & 0xFFFFFF) == right_view || (scene_id & 0xFFFFFF) == right_camera;

    static std::atomic<int> logged{ 0 };
    if (logged.fetch_add(1) < 16) {
        spdlog::info("[Stereo] {} pass: pass view {:x}, graph viewport {}, scene {:x} -> right eye {} (right view {:x}, right camera {:x})", kUpscalerPasses[pass_kind].label,
                     view, viewport_id, scene_id, right, m_right_view_id, right_camera);
    }
    m_upscaler_runs[pass_kind].fetch_add(1);
    if (right) {
        m_upscaler_right_runs[pass_kind].fetch_add(1);
    }

    if (!kUpscalerPasses[pass_kind].dlss) {
        return original(pass, render_graph_data, pass_data);
    }
    UpscalerAfrNvidiaModule::set_secondary_view(right);
    auto result = original(pass, render_graph_data, pass_data);
    UpscalerAfrNvidiaModule::set_secondary_view(false);
    return result;
}

void StereoViewModule::ReportUpscalers()
{
    constexpr int kReportInterval = 300;
    if (m_native_frames % kReportInterval != 0 || m_native_frames == 0) {
        return;
    }

    std::string summary;
    bool        shared_history = false;
    for (int i = 0; i < kUpscalerPassCount; ++i) {
        const auto runs  = m_upscaler_runs[i].exchange(0);
        const auto right = m_upscaler_right_runs[i].exchange(0);
        if (runs == 0) {
            continue;
        }
        summary += std::format("{}{} {} ({} right eye)", summary.empty() ? "" : ", ", kUpscalerPasses[i].label, runs, right);
        shared_history = shared_history || kUpscalerPasses[i].shares_history;
    }
    if (m_upscaler_reports < 20) {
        ++m_upscaler_reports;
        spdlog::info("[Stereo] Upscaler passes over the last {} frames: {}", kReportInterval, summary.empty() ? std::string{ "none (native TAA)" } : summary);
    }
    if (shared_history && !m_warned_shared_upscaler) {
        m_warned_shared_upscaler = true;
        spdlog::warn("[Stereo] XeSS and FSR keep one history for both eyes, which smears each eye with the other; use DLSS or TAA with Native stereo");
    }
}

uint32_t StereoViewModule::SceneOf(void* render_graph_data) const
{
    if (render_graph_data == nullptr) {
        return kInvalidId;
    }
    const auto viewport = At<uint32_t>(render_graph_data, 0x140);
    if (viewport >= 8) {
        return kInvalidId;
    }
    auto base = At<uint8_t*>(render_graph_data, 0x108 + 8 * (size_t)viewport);
    return base ? At<uint32_t>(base, 0x24) : kInvalidId;
}

void StereoViewModule::StartCensus()
{
    std::scoped_lock _{ m_census_mutex };
    m_census.clear();
    m_census_order.clear();
    m_census_frames = 3;
}

void StereoViewModule::FinishCensus()
{
    std::scoped_lock _{ m_census_mutex };
    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    const auto left  = m_left_camera ? m_left_view_id & 0xFFFFFF : root ? At<uint32_t>(root, kRootMainView) & 0xFFFFFF : kInvalidId;
    const auto right = m_right_view_id & 0xFFFFFF;

    for (auto& [scene, passes] : m_census) {
        spdlog::info("[Stereo] Census: scene {:x} ran {} distinct passes", scene, passes.size());
    }
    auto find = [&](uint32_t id) -> const std::map<std::string, int>* {
        for (auto& [scene, passes] : m_census) {
            if ((scene & 0xFFFFFF) == id) {
                return &passes;
            }
        }
        return nullptr;
    };
    auto l = find(left);
    auto r = find(right);
    if (l == nullptr || r == nullptr) {
        spdlog::warn("[Stereo] Census: no passes recorded for the {} eye", l ? "right" : "left");
        return;
    }
    for (auto& [name, count] : *l) {
        auto it = r->find(name);
        if (it == r->end()) {
            spdlog::info("[Stereo] Census: left eye only: {} x{}", name, count);
        } else if (it->second != count) {
            spdlog::info("[Stereo] Census: {} runs {}x left, {}x right", name, count, it->second);
        }
    }
    for (auto& [name, count] : *r) {
        if (l->find(name) == l->end()) {
            spdlog::info("[Stereo] Census: right eye only: {} x{}", name, count);
        }
    }
    std::string shared;
    for (auto& [scene, passes] : m_census) {
        if ((scene & 0xFFFFFF) == left || (scene & 0xFFFFFF) == right) {
            continue;
        }
        for (auto& [name, count] : passes) {
            shared += std::format("{}{} x{}", shared.empty() ? "" : ", ", name, count);
        }
        spdlog::info("[Stereo] Census: passes outside both eye views (scene {:x}): {}", scene, shared);
        shared.clear();
    }
    // Execution order of the effect, translucency, sky and scene setup passes, tagged by eye.
    std::string order;
    for (auto& entry : m_census_order) {
        order += entry;
        if (order.size() > 1500) {
            spdlog::info("[Stereo] Order: {}", order);
            order.clear();
        }
    }
    if (!order.empty()) {
        spdlog::info("[Stereo] Order: {}", order);
    }
}

uintptr_t StereoViewModule::onExecuteChildPass(void* graph, void* render_graph_data, void* container)
{
    static auto instance = Get();
    using func_t         = uintptr_t(void*, void*, void*);
    static auto original = instance->m_execute_child_hook->get_original<func_t>();

    if (instance->m_census_frames.load() > 0 && container != nullptr) {
        auto pass = At<void*>(container, 0x20);
        auto name = pass ? At<const char*>(pass, 0x8) : nullptr;
        if (name != nullptr) {
            const auto scene = instance->SceneOf(render_graph_data);
            std::scoped_lock _{ instance->m_census_mutex };
            ++instance->m_census[scene][name];
            const std::string_view n{ name };
            const bool tracked = n.find("Particle") != n.npos || n.find("Forward") != n.npos || n.find("OIT") != n.npos || n.find("Sky") != n.npos ||
                                 n.find("SceneSetup") != n.npos || n.find("FrameSetup") != n.npos || n.find("Cloud") != n.npos || n.find("Effect") != n.npos ||
                                 n.find("Emissive") != n.npos || n.find("Translucen") != n.npos;
            if (tracked && instance->m_census_order.size() < 600) {
                const auto s   = scene & 0xFFFFFF;
                const char* eye = s == (instance->m_left_view_id & 0xFFFFFF) ? "L" : s == (instance->m_right_view_id & 0xFFFFFF) ? "R" : "-";
                instance->m_census_order.push_back(std::format("{}:{} ", eye, n));
            }
        }
    }
    return original(graph, render_graph_data, container);
}

uintptr_t StereoViewModule::onScaleformComposite(void* pass, void* render_graph_data, void* pass_data)
{
    static auto instance = Get();
    using func_t         = uintptr_t(void*, void*, void*);
    static auto original = instance->m_scaleform_composite_hook->get_original<func_t>();
    static auto vr       = VR::get();

    if (vr->is_native_stereo() && pass_data != nullptr && GameFlow::gStore.internalSettings.stereoHudBothEyes) {
        instance->MirrorHudToRightEye(render_graph_data, pass_data);
    }
    if (vr->is_native_stereo() && pass_data != nullptr && instance->m_composite_logs.fetch_add(1) < 4) {
        auto data  = static_cast<RE::CreationRendererPrivate::RenderPassData*>(pass_data);
        auto items = data->renderPassItems;
        const uint32_t count = items ? (uint32_t)items->_size : 0;
        spdlog::info("[Stereo] HUD composite for scene {:x} with {} resources", instance->SceneOf(render_graph_data), count);
        for (uint32_t i = 0; i < count && i < 8; ++i) {
            auto resource = data->getNativeResourceByIndex(i);
            if (resource) {
                const auto desc = resource->GetDesc();
                spdlog::info("[Stereo]   resource {}: {:p} {}x{} format {}", i, (void*)resource, desc.Width, desc.Height, (uint32_t)desc.Format);
            } else {
                spdlog::info("[Stereo]   resource {}: none", i);
            }
        }
    }
    return original(pass, render_graph_data, pass_data);
}

bool StereoViewModule::onIsPrimarySceneView(void* view)
{
    static auto instance = Get();
    using func_t         = bool(void*);
    static auto original = instance->m_primary_view_hook->get_original<func_t>();
    static auto vr       = VR::get();

    const bool primary = original(view);
    if (primary || view == nullptr || !vr->is_native_stereo() || !instance->m_registered) {
        return primary;
    }
    if ((At<uint32_t>(view, 0x24) & 0xFFFFFF) != (instance->m_right_view_id & 0xFFFFFF)) {
        return primary;
    }

    // The right eye builds its own reflection probe blend; the scene-level probe and GI updates stay shared.
    constexpr uintptr_t kProbeBuilderSize = 0x2760;
    const auto          caller            = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (caller < instance->m_probe_builder || caller >= instance->m_probe_builder + kProbeBuilderSize) {
        return primary;
    }
    static bool logged{ false };
    if (!logged) {
        logged = true;
        spdlog::info("[Stereo] Right eye view gets its own reflection probe blend");
    }
    return true;
}

void StereoViewModule::MirrorHudToRightEye(void* render_graph_data, void* pass_data)
{
    auto data  = static_cast<RE::CreationRendererPrivate::RenderPassData*>(pass_data);
    auto graph = static_cast<RE::CreationRendererPrivate::RenderGraphData*>(render_graph_data);
    if (data == nullptr || graph == nullptr || data->renderPassItems == nullptr) {
        return;
    }
    auto context = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(graph->getCommandList());
    if (context == nullptr || context->pID3D12CommandList == nullptr) {
        return;
    }
    auto command_list = context->pID3D12CommandList;
    auto device       = g_framework->get_d3d12_hook()->get_device();

    // The UI movies sit in the left half (the left eye); copy that half over the right half before compositing.
    const uint32_t count = (uint32_t)data->renderPassItems->_size;
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        auto item     = data->getRenderPassItemByIndex(i);
        auto resource = data->getNativeResourceByIndex(i);
        if (item == nullptr || resource == nullptr) {
            continue;
        }
        const auto desc  = resource->GetDesc();
        const auto state = (D3D12_RESOURCE_STATES)RE::CreationRendererPrivate::RenderPassItem::getDXGIState(item->stateOrFlags);
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.Width < 64) {
            continue;
        }
        if (m_hud_scratch == nullptr || m_hud_scratch->GetDesc().Width != desc.Width || m_hud_scratch->GetDesc().Height != desc.Height ||
            m_hud_scratch->GetDesc().Format != desc.Format) {
            m_hud_scratch.Reset();
            auto scratch_desc  = desc;
            scratch_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            const CD3DX12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &scratch_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_hud_scratch)))) {
                spdlog::error("[Stereo] Failed to create the HUD scratch texture");
                return;
            }
        }

        const UINT half = (UINT)(desc.Width / 2);
        D3D12_BOX  left_half{ 0, 0, 0, half, desc.Height, 1 };

        D3D12_RESOURCE_BARRIER to_copy[]{
            CD3DX12_RESOURCE_BARRIER::Transition(resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_hud_scratch.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        command_list->ResourceBarrier(2, to_copy);
        CD3DX12_TEXTURE_COPY_LOCATION scratch_location{ m_hud_scratch.Get(), 0 };
        CD3DX12_TEXTURE_COPY_LOCATION ui_location{ resource, 0 };
        command_list->CopyTextureRegion(&scratch_location, 0, 0, 0, &ui_location, &left_half);

        D3D12_RESOURCE_BARRIER to_paste[]{
            CD3DX12_RESOURCE_BARRIER::Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(m_hud_scratch.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        command_list->ResourceBarrier(2, to_paste);
        command_list->CopyTextureRegion(&ui_location, half, 0, 0, &scratch_location, &left_half);

        D3D12_RESOURCE_BARRIER restore[]{
            CD3DX12_RESOURCE_BARRIER::Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, state),
            CD3DX12_RESOURCE_BARRIER::Transition(m_hud_scratch.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        };
        command_list->ResourceBarrier(2, restore);

        if (m_hud_mirror_logs.fetch_add(1) < 2) {
            spdlog::info("[Stereo] HUD mirrored to the right eye: resource {} {}x{} format {} state {:x}", i, desc.Width, desc.Height, (uint32_t)desc.Format, (uint32_t)state);
        }
        // The first texture is the UI layer.
        return;
    }
}

void StereoViewModule::UpdateWorldFreeze()
{
    static auto vr = VR::get();
    // Alternate eye resubmits the older eye every frame, as before native stereo existed.
    if (GameFlow::gStore.internalSettings.stereoMode == 2 && !vr->is_using_async_aer()) {
        vr->set_async_aer(true);
    }
}

bool StereoViewModule::RegisterRightGraph()
{
    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    if (root == nullptr || At<uint32_t>(root, kRootMainRenderGraph) == kInvalidId) {
        return false;
    }
    auto main_record = reinterpret_cast<uint8_t*>(root) + kRootMainGraphRecord;
    auto key_row     = StorageRow(StorageColumn(offsets::RenderGraphStorage(), kGraphKeyColumn), At<uint32_t>(root, kRootMainRenderGraph), 0x10);
    if (key_row == nullptr) {
        return false;
    }
    GraphContext ctx{ m_right_graph_record, main_record, &m_right_graph_id, key_row[0], *reinterpret_cast<const uintptr_t*>(key_row + 8), 0 };
    spdlog::info("[Stereo] Main render graph key {:x}, name {:x}", ctx.main_key, ctx.main_name);
    if (!RegisterGraphObjectsGuarded(&ctx)) {
        spdlog::error("[Stereo] Right eye render graph setup stopped at step {}", ctx.step);
        return false;
    }
    m_right_graph_ready = true;
    std::memset(m_right_graph_options, 0, sizeof(m_right_graph_options));
    m_right_graph_frame_flag = 0xFF;
    spdlog::info("[Stereo] Right eye render graph {:x} registered with {} job(s); main graph {:x}", m_right_graph_id, At<uint32_t>(main_record, 0x0),
                 At<uint32_t>(root, kRootMainRenderGraph));
    return true;
}

void StereoViewModule::PrepareRightGraph()
{
    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    if (root == nullptr) {
        return;
    }
    const auto main_graph = At<uint32_t>(root, kRootMainRenderGraph);
    const auto storage    = offsets::RenderGraphStorage();
    auto       id         = m_right_graph_id;

    // The main graph's options and per-frame flag, copied when they change.
    if (auto row = StorageRow(StorageColumn(storage, kGraphOptionsColumn), main_graph, sizeof(m_right_graph_options))) {
        if (std::memcmp(row, m_right_graph_options, sizeof(m_right_graph_options)) != 0) {
            std::memcpy(m_right_graph_options, row, sizeof(m_right_graph_options));
            uint8_t options[sizeof(m_right_graph_options)];
            std::memcpy(options, row, sizeof(options));
            Fn<void (*)(uint32_t*, void*)>(offsets::WriteRenderGraphOptions())(&id, options);
        }
    }
    if (auto row = StorageRow(StorageColumn(storage, kGraphFrameFlagColumn), main_graph, 1)) {
        if (*row != m_right_graph_frame_flag) {
            m_right_graph_frame_flag = *row;
            uint8_t flag             = *row;
            Fn<void (*)(uint32_t*, uint8_t*)>(offsets::WriteRenderGraphFrameFlag())(&id, &flag);
        }
    }

    // The right graph renders only the right eye's view.
    ViewIdArray views{};
    views.size     = 1;
    views.capacity = static_cast<int32_t>(0x80000004u);
    views.local[0] = m_exp_swap_graphs ? m_left_view_id : m_right_view_id;
    m_set_multi_view_hook->get_original<decltype(onSetMultiCameraViewData)>()(StorageColumn(storage, kMultiCameraViewColumn), id & 0xFFFFFF, &views);
    m_appended.store(true);
}

uintptr_t StereoViewModule::onSubmitRenderGraph(void* frame_list, void* record)
{
    static auto instance = Get();
    using func_t         = uintptr_t(void*, void*);
    static auto original = instance->m_submit_graph_hook->get_original<func_t>();
    static auto vr       = VR::get();

    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    if (instance->m_right_graph_ready && vr->is_native_stereo() && !instance->m_menu_fallback.load() && root &&
        record == reinterpret_cast<uint8_t*>(root) + kRootMainGraphRecord &&
        At<uint32_t>(root, kRootMainView) != kInvalidId) {
        instance->PrepareRightGraph();
        if (GameFlow::gStore.internalSettings.nativeLeftGraphFirst || instance->m_exp_left_first) {
            const auto result = original(frame_list, record);
            original(frame_list, instance->m_right_graph_record);
            return result;
        }
        original(frame_list, instance->m_right_graph_record);
        if (instance->m_right_graph_submits++ == 0) {
            spdlog::info("[Stereo] Right eye render graph submitted ahead of the main graph");
        }
    }
    return original(frame_list, record);
}


std::string StereoViewModule::Variant() const
{
    const bool left_first = GameFlow::gStore.internalSettings.nativeLeftGraphFirst || m_exp_left_first;
    return std::format("{}_{}_{}_camera{}", m_double_width ? "double" : "window", left_first ? "leftfirst" : "rightfirst",
                       m_exp_swap_graphs ? "swapped" : "maingraphleft", m_right_camera_generation);
}

bool StereoViewModule::RenewRightCamera()
{
    auto root = CreationEngineSingletonManager::GetSceneGraphRoot();
    if (!m_registered || root == nullptr || root->worldCamera == nullptr) {
        return false;
    }
    RegisterContext ctx{ root, root->worldCamera, nullptr, kInvalidId, 0, "VR Right Eye Camera" };
    if (!RegisterEngineObjectsGuarded(&ctx)) {
        spdlog::error("[Stereo] Renewing the right eye camera failed at step {}", ctx.step);
        return false;
    }
    m_right_camera  = ctx.camera;
    m_right_view_id = ctx.view_id;
    m_mirrored      = {};
    ++m_right_camera_generation;
    spdlog::info("[Stereo] Right eye now uses camera {:x}, view {:x} (generation {})", m_right_camera->cameraHandleID, m_right_view_id, m_right_camera_generation);
    return true;
}

void StereoViewModule::UpdateMenuFallback(bool native)
{
    static auto vr       = VR::get();
    const bool  fallback = native && ModSettings::showFlatScreenDisplay();
    if (fallback != m_menu_fallback.load()) {
        m_menu_fallback.store(fallback);
        m_menu_frames = 0;
        if (fallback) {
            ++m_menu_opens;
        }
        spdlog::info("[Stereo] {}", fallback ? "Fullscreen menu: the main graph renders the game's own view in the left half, shown on the flat screen"
                                              : "Fullscreen menu closed: both eye views render again");
    }
    // The first few menus of a session are captured shortly after opening and once settled, with the passes they run.
    if (!fallback || m_menu_opens > 6) {
        return;
    }
    ++m_menu_frames;
    if (m_menu_frames == 60) {
        StartCensus();
    }
    if (m_menu_frames == 20 || m_menu_frames == 120 || m_menu_frames == 400) {
        spdlog::info("[Stereo] Menu {} frame {}: {}", m_menu_opens, m_menu_frames, GameFlow::isShowingMenu() ? "menu showing" : "no menu");
        vr->request_backbuffer_dump(Framework::get_persistent_dir(std::format("vr_native_stereo_menu{}_{}.png", m_menu_opens, m_menu_frames)).wstring());
    }
}

void StereoViewModule::ApplyNativeShadowSettings(bool native)
{
    // Every eye view runs the dynamic shadow selection, but the per-light fade state is shared: each eye resets the fades of
    // lights only the other eye sees, so those shadows keep restarting their tiled fade. The main view whose LOD the
    // dynamic shadow maps borrow is not rendered in native stereo, so each shadow map picks its own. Shadow caster occlusion
    // culling tests against a depth pyramid that only one eye builds, so the other eye loses casters; culling is turned off.
    if (native == m_shadow_settings_applied) {
        return;
    }
    m_shadow_settings_applied = native;
    auto settings = CreationEngineSettings::Get();
    using Type    = CreationEngineSettings::SettingType;
    constexpr auto kFade = "fDynamicShadowFadeSeconds:Shadows";
    constexpr auto kLod  = "bDynamicShadowmapsUseMainViewLOD:Shadows";
    constexpr auto kVolumeCulling = "bEnableShadowVolumeCulling:Shadows";
    constexpr auto kCsmCulling    = "bCSMEnableOcclusionCulling:Shadows";
    if (native) {
        m_saved_shadow_fade_seconds  = settings->get_setting(kFade, Type::kINISetting, 0.75f);
        m_saved_shadow_main_view_lod = settings->get_setting(kLod, Type::kINISetting, true);
        m_saved_shadow_volume_culling = settings->get_setting(kVolumeCulling, Type::kINISetting, true);
        m_saved_csm_occlusion_culling = settings->get_setting(kCsmCulling, Type::kINISetting, true);
    }
    const bool volume = settings->set_setting(kVolumeCulling, Type::kINISetting, native ? false : m_saved_shadow_volume_culling);
    const bool csm    = settings->set_setting(kCsmCulling, Type::kINISetting, native ? false : m_saved_csm_occlusion_culling);
    spdlog::info("[Stereo] Shadow caster occlusion culling {}: volume ({}), cascades ({})", native ? "off" : "restored", volume ? "set" : "not found",
                 csm ? "set" : "not found");
    const bool fade = settings->set_setting(kFade, Type::kINISetting, native ? 0.0f : m_saved_shadow_fade_seconds);
    const bool lod  = settings->set_setting(kLod, Type::kINISetting, native ? false : m_saved_shadow_main_view_lod);
    spdlog::info("[Stereo] Shadow settings {}: dynamic shadow fade {}s ({}), main view LOD {} ({})", native ? "for native stereo" : "restored",
                 settings->get_setting(kFade, Type::kINISetting, -1.0f), fade ? "set" : "not found", settings->get_setting(kLod, Type::kINISetting, false),
                 lod ? "set" : "not found");
}

void StereoViewModule::AdvanceExperiment()
{
    // Which eye looks wrong in each capture tells apart graph order, graph identity and camera registration.
    switch (m_experiment_stage++) {
    case 0:
        m_exp_left_first = true;
        break;
    case 1:
        m_exp_left_first  = false;
        m_exp_swap_graphs = true;
        break;
    case 2:
        m_exp_swap_graphs = false;
        RenewRightCamera();
        break;
    default:
        return;
    }
    spdlog::info("[Stereo] Diagnostic stage {}: left graph first {}, graphs swapped {}, right camera generation {}", m_experiment_stage, m_exp_left_first,
                 m_exp_swap_graphs, m_right_camera_generation);
}

uintptr_t StereoViewModule::onCopyToRenderGraphOutput(void* pass, void* render_graph_data, void* pass_data)
{
    static auto instance = Get();
    using func_t         = uintptr_t(void*, void*, void*);
    static auto original = instance->m_copy_to_output_hook->get_original<func_t>();
    static auto vr       = VR::get();

    // The output copy writes into the view's scissor rect. Eye scissors stay full for culling, so for this pass the
    // scissor is narrowed to the eye's viewport and each eye lands in its own half.
    const auto viewport_id = render_graph_data ? At<uint32_t>(render_graph_data, 0x140) : 8u;
    if (!vr->is_native_stereo() || viewport_id >= 8) {
        return original(pass, render_graph_data, pass_data);
    }
    auto rect = At<float*>(render_graph_data, 0x108 + 8 * (size_t)viewport_id);
    if (rect == nullptr || std::abs(rect[1] - rect[0]) > 0.99f) {
        return original(pass, render_graph_data, pass_data);
    }
    float scissors[4];
    std::memcpy(scissors, rect + 4, sizeof(scissors));
    std::memcpy(rect + 4, rect, sizeof(scissors));
    static std::atomic<int> logged{ 0 };
    if (logged.fetch_add(1) < 4) {
        spdlog::info("[Stereo] Output copy for view {:x}: viewport ({}, {}, {}, {}), scissors ({}, {}, {}, {}) narrowed to the viewport", At<uint32_t>(rect, 0x24), rect[0],
                     rect[1], rect[2], rect[3], scissors[0], scissors[1], scissors[2], scissors[3]);
    }
    const auto result = original(pass, render_graph_data, pass_data);
    std::memcpy(rect + 4, scissors, sizeof(scissors));
    return result;
}
