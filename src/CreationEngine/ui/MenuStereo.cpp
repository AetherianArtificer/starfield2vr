#include "MenuStereo.h"

#include "GFx.h"
#include <CreationEngine/CreationEngineRendererModule.h>
#include <CreationEngine/StereoViewModule.h>
#include <CreationEngine/memory/ScanHelper.h>
#include <Framework.hpp>
#include <RE/C/CreationRendererPrivate.h>
#include <_deps/directxtk12-src/Src/d3dx12.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory/FunctionHook.h>
#include <mods/VR.hpp>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace MenuStereo
{
    namespace
    {
        // ---- The layout: per menu, clips and how far toward the player they stand, in metres.

        constexpr const char* kDefaultLayout =
            "# Menu depth: metres toward the player from the menu panel, which is 3 m away. Negative pushes a clip back.\n"
            "# <menu> <clip path from the menu's root clip> <metres>. Saved changes apply while the game runs.\n"
            "MainMenu GameLogo_mc -0.5\n"
            "MainMenu AdBannerHolder_mc 0.25\n"
            "MainMenu MOTDHolder_mc 0.25\n"
            "MainMenu MainPanel_mc 1.0\n"
            "MainMenu LoadPanel_mc 1.0\n"
            "MainMenu SettingsPanel_mc 0.8\n"
            "MainMenu ButtonBar_mc 0.8\n"
            "MainMenu EngagementPrompt_mc 0.6\n"
            "PauseMenu GameLogo_mc -0.5\n"
            "PauseMenu MainPanel_mc 1.0\n"
            "PauseMenu LoadPanel_mc 1.0\n"
            "PauseMenu SavePanel_mc 1.0\n"
            "PauseMenu SettingsPanel_mc 0.8\n"
            "PauseMenu HelpPanel_mc 0.8\n"
            "PauseMenu InstalledContentPanel_mc 0.8\n"
            "PauseMenu CreationsLibraryPanel_mc 0.8\n";

        struct ClipDepth
        {
            std::vector<std::string> path;
            double                   metres;
        };

        std::mutex                                              g_layout_mutex;
        std::unordered_map<std::string, std::vector<ClipDepth>> g_layout;
        std::filesystem::file_time_type                         g_layout_time{};
        std::atomic<int>                                        g_layout_check{ 0 };
        std::mutex                                              g_load_mutex;

        void LoadLayout()
        {
            const auto path = Framework::get_persistent_dir("menu_depth.txt");
            std::error_code error;
            if (!std::filesystem::exists(path, error)) {
                std::ofstream{ path } << kDefaultLayout;
                spdlog::info("[MenuStereo] Wrote the default menu depth layout to {}", path.string());
            }
            const auto time = std::filesystem::last_write_time(path, error);
            if (error || time == g_layout_time) {
                return;
            }
            g_layout_time = time;
            std::unordered_map<std::string, std::vector<ClipDepth>> layout;
            std::ifstream file{ path };
            std::string line;
            int clips = 0;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                std::istringstream words{ line };
                std::string menu, clip;
                double metres = 0.0;
                if (!(words >> menu >> clip >> metres)) {
                    spdlog::error("[MenuStereo] menu_depth.txt: cannot read \"{}\"", line);
                    continue;
                }
                ClipDepth entry{ {}, metres };
                std::istringstream parts{ clip };
                for (std::string part; std::getline(parts, part, '.');) {
                    entry.path.push_back(part);
                }
                layout[menu].push_back(std::move(entry));
                ++clips;
            }
            std::scoped_lock _{ g_layout_mutex };
            g_layout = std::move(layout);
            spdlog::info("[MenuStereo] Menu depth layout: {} clips in {} menus", clips, g_layout.size());
        }

        // ---- Menus, their movies, and the clips moved in them.

        struct MenuMovie
        {
            void*       menu;
            std::string name;
        };
        std::mutex                            g_menus_mutex;
        std::unordered_map<void*, MenuMovie>  g_menus;  // movie -> menu
        std::atomic<float>                    g_ipd{ 0.064f };

        // The values last written to a clip, and the clip's own values they were made from.
        struct Placed
        {
            double base[4]{};
            double wrote[4]{};
            bool   placed{ false };
        };
        std::mutex                        g_placed_mutex;
        std::unordered_map<void*, Placed> g_placed;  // clip -> values

        bool Number(const GFx::Value& object, const char* name, double& out)
        {
            GFx::Value value;
            if (!object.GetMember(name, &value)) {
                return false;
            }
            const auto type = value.GetType();
            if (type != GFx::Value::kInt && type != GFx::Value::kUInt && type != GFx::Value::kNumber) {
                return false;
            }
            out = value.GetNumber();
            return true;
        }

        // The focal length and projection centre of the movie's perspective, in stage pixels.
        bool Perspective(const GFx::Value& clip, double& focal, double& cx, double& cy)
        {
            GFx::Value root, transform, projection, centre;
            return clip.GetMember("root", &root) && root.GetMember("transform", &transform) && transform.GetMember("perspectiveProjection", &projection) &&
                   Number(projection, "focalLength", focal) && projection.GetMember("projectionCenter", &centre) && Number(centre, "x", cx) &&
                   Number(centre, "y", cy) && focal > 1.0;
        }

        // Moves a clip `z` along its axis in the perspective, and toward the projection centre by as much as that makes
        // it larger, so it keeps its place and size on the panel. (cx, cy) is the centre in the clip's parent's space.
        void Place(GFx::Value& clip, double z, double focal, double cx, double cy)
        {
            static const char* kNames[4]{ "x", "y", "scaleX", "scaleY" };
            double now[4]{};
            for (int i = 0; i < 4; ++i) {
                if (!Number(clip, kNames[i], now[i])) {
                    return;
                }
            }
            std::scoped_lock _{ g_placed_mutex };
            auto& placed = g_placed[clip.GetData()];
            // Positions are kept in twentieths of a pixel, so a value read back differs from the one written by up to
            // that much; a larger change is the menu moving the clip itself, and its new value is the clip's own.
            for (int i = 0; i < 4; ++i) {
                const double tolerance = i < 2 ? 0.1 : 0.002;
                if (!placed.placed || std::abs(now[i] - placed.wrote[i]) > tolerance) {
                    placed.base[i] = now[i];
                }
            }
            if (!placed.placed) {
                placed.placed = true;
                spdlog::info("[MenuStereo] Clip at {:.1f}, {:.1f} scale {:.3f} placed {:.1f} along the perspective", now[0], now[1], now[2], z);
            }
            const double k = (focal + z) / focal;
            const double out[4]{ cx + (placed.base[0] - cx) * k, cy + (placed.base[1] - cy) * k, placed.base[2] * k, placed.base[3] * k };
            clip.SetMember("z", GFx::Value(z));
            for (int i = 0; i < 4; ++i) {
                clip.SetMember(kNames[i], GFx::Value(out[i]));
                placed.wrote[i] = out[i];
            }
        }

        // ---- Per-eye drawing.

        // Scaleform's StereoParams, kept for the copy the engine makes of the stereo object.
        struct StereoParams
        {
            float display_width_cm{ 0.0f };
            float distortion{ 0.75f };
            float display_diag_inches{ 52.0f };
            float display_aspect{ 9.0f / 16.0f };
            float eye_separation_cm{ 6.4f };
        };
        StereoParams g_params{};

        // A projection for an eye `s` view units across from the centre, which leaves the stage plane (view depth `zp`)
        // where it is: the eye's shift, then the shear that brings the stage plane back.
        void EyeProjection(const float* p, float zp, float s, float* out)
        {
            float m[16];
            std::memcpy(m, p, sizeof(m));
            for (int r = 0; r < 4; ++r) {
                m[r * 4 + 3] -= s * m[r * 4 + 0];
            }
            const float w0 = p[14] * zp + p[15];
            const float k  = p[0] * s / w0;
            for (int c = 0; c < 4; ++c) {
                m[c] += k * m[12 + c];
            }
            std::memcpy(out, m, sizeof(m));
        }

        std::atomic<int> g_eye_projections[2]{};

        // StereoImplBase: [0] destructor, [1] SetParams, [2] GetParams, [3] GetStereoProj.
        void* StereoDestroy(void* self, unsigned) { return self; }
        void  StereoSetParams(void*, const void*) {}
        void* StereoGetParams(void*) { return &g_params; }
        void  StereoGetProj(void*, const float* projection, float screen_distance, float* left, float* right, float)
        {
            // The stage plane spans NDC -1..1 across the panel, so view units per metre on it are its width over the
            // panel's. An eye set the player's eye distance apart in those units sees depth that agrees with the
            // perspective's size change.
            const float zp     = -screen_distance;
            const float w0     = projection[14] * zp + projection[15];
            const float width  = 2.0f * w0 / std::abs(projection[0]);
            const float half   = 0.5f * g_ipd.load() * width / VR::menu_panel_width();
            const float s      = projection[0] < 0.0f ? -half : half;
            if (left) {
                EyeProjection(projection, zp, -s, left);
                g_eye_projections[0].fetch_add(1);
            }
            if (right) {
                EyeProjection(projection, zp, s, right);
                g_eye_projections[1].fetch_add(1);
            }
            static bool logged = false;
            if (!logged && left) {
                logged = true;
                // A point halfway to the camera must sit further right in the left eye than in the right.
                float r[16];
                EyeProjection(projection, zp, s, r);
                const float zc = zp * 0.5f;
                const float lx = (left[2] * zc + left[3]) / (left[14] * zc + left[15]);
                const float rx = (r[2] * zc + r[3]) / (r[14] * zc + r[15]);
                spdlog::info("[MenuStereo] Menu perspective: camera {:.1f} view units from the stage, stage {:.1f} wide, eye offset {:.2f}; a nearer point is at x {:.4f} "
                             "left, {:.4f} right ({})",
                             screen_distance, width, half, lx, rx, lx > rx ? "correct" : "WRONG: depth is inverted");
                spdlog::info("[MenuStereo] Projection rows: [{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}]",
                             projection[0], projection[1], projection[2], projection[3], projection[4], projection[5], projection[6], projection[7], projection[8],
                             projection[9], projection[10], projection[11], projection[12], projection[13], projection[14], projection[15]);
            }
        }

        void* g_stereo_vtable[4]{ reinterpret_cast<void*>(&StereoDestroy), reinterpret_cast<void*>(&StereoSetParams), reinterpret_cast<void*>(&StereoGetParams),
                                  reinterpret_cast<void*>(&StereoGetProj) };
        // RefCountImpl: vtable, then the count, which never reaches zero.
        struct StereoImpl
        {
            void**        vtable{ g_stereo_vtable };
            volatile long refcount{ 1 << 28 };
        } g_stereo;

        // The renderer: MatrixState at +0x270; in it the eye at +0x284 (0 none, 1 left, 2 right), the stereo object
        // at +0x288 and the dirty flag at +0x280. HAL slot 46 installs the stereo object.
        void SetEye(void* hal, int eye)
        {
            auto matrices = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(hal) + 0x270);
            if (*reinterpret_cast<void**>(matrices + 0x288) != &g_stereo) {
                auto set_impl = reinterpret_cast<void (*)(void*, void*)>((*reinterpret_cast<void***>(hal))[46]);
                set_impl(hal, &g_stereo);
            }
            *reinterpret_cast<int*>(matrices + 0x284) = eye;
            *(matrices + 0x280)                       = 1;
        }

        std::unique_ptr<FunctionHook> g_pass_hook{};
        std::unique_ptr<FunctionHook> g_display_hook{};
        uintptr_t                     g_pass_vtable{ 0 };

        // The frame's Scaleform passes: opened by the first, closed after the composite.
        bool            g_frame_open{ false };
        bool            g_frame_stereo{ false };
        bool            g_left_started{ false };
        ID3D12Resource* g_ui_layer{ nullptr };

        struct PassTarget
        {
            ID3D12GraphicsCommandList* list{ nullptr };
            ID3D12Resource*            target{ nullptr };
            D3D12_RESOURCE_STATES      state{};
        };
        thread_local PassTarget t_pass{};

        // The left eye's UI as the frame's movies are drawn, a holding copy of the right eye's, and the left eye's
        // finished UI for the headset. The first two rest in COPY_DEST, the last in PIXEL_SHADER_RESOURCE.
        Microsoft::WRL::ComPtr<ID3D12Resource> g_left{}, g_hold{}, g_left_out{};

        bool Make(Microsoft::WRL::ComPtr<ID3D12Resource>& texture, const D3D12_RESOURCE_DESC& like, D3D12_RESOURCE_STATES state, const wchar_t* name)
        {
            if (texture != nullptr) {
                const auto desc = texture->GetDesc();
                if (desc.Width == like.Width && desc.Height == like.Height && desc.Format == like.Format) {
                    return true;
                }
            }
            texture.Reset();
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(like.Format, like.Width, like.Height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            const CD3DX12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            if (FAILED(g_framework->get_d3d12_hook()->get_device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture)))) {
                spdlog::error("[MenuStereo] Failed to create {}x{} texture", like.Width, like.Height);
                return false;
            }
            texture->SetName(name);
            return true;
        }

        void Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, D3D12_RESOURCE_STATES dst_state, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state)
        {
            D3D12_RESOURCE_BARRIER before[2]{};
            D3D12_RESOURCE_BARRIER after[2]{};
            UINT count = 0;
            if (dst_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                before[count] = CD3DX12_RESOURCE_BARRIER::Transition(dst, dst_state, D3D12_RESOURCE_STATE_COPY_DEST);
                after[count++] = CD3DX12_RESOURCE_BARRIER::Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, dst_state);
            }
            if (src_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                before[count] = CD3DX12_RESOURCE_BARRIER::Transition(src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                after[count++] = CD3DX12_RESOURCE_BARRIER::Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
            }
            if (count > 0) {
                list->ResourceBarrier(count, before);
            }
            list->CopyResource(dst, src);
            if (count > 0) {
                list->ResourceBarrier(count, after);
            }
        }

        // The texture a Scaleform pass draws into: the UI layer the composite reads, or the one large render target.
        bool FindTarget(void* render_graph_data, void* pass_data, PassTarget& out)
        {
            auto data  = static_cast<RE::CreationRendererPrivate::RenderPassData*>(pass_data);
            auto graph = static_cast<RE::CreationRendererPrivate::RenderGraphData*>(render_graph_data);
            static bool logged = false;
            if (data == nullptr || data->renderPassItems == nullptr || graph == nullptr) {
                if (!logged) {
                    logged = true;
                    spdlog::error("[MenuStereo] A Scaleform pass has no pass data; menus are drawn once for both eyes");
                }
                return false;
            }
            auto context = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(graph->getCommandList());
            if (context == nullptr || context->pID3D12CommandList == nullptr) {
                return false;
            }
            const uint32_t count = (uint32_t)data->renderPassItems->_size;
            ID3D12Resource*       target{ nullptr };
            D3D12_RESOURCE_STATES target_state{};
            bool                  matched{ false };
            for (uint32_t i = 0; i < count && i < 16; ++i) {
                auto item     = data->getRenderPassItemByIndex(i);
                auto resource = data->getNativeResourceByIndex(i);
                if (item == nullptr || resource == nullptr) {
                    continue;
                }
                const auto desc  = resource->GetDesc();
                const auto state = (D3D12_RESOURCE_STATES)RE::CreationRendererPrivate::RenderPassItem::getDXGIState(item->stateOrFlags);
                if (!logged) {
                    spdlog::info("[MenuStereo] Scaleform pass texture {}: {}x{} format {} samples {} state {:x}{}", i, desc.Width, desc.Height, (uint32_t)desc.Format,
                                 desc.SampleDesc.Count, (uint32_t)state, resource == g_ui_layer ? " (the composite's UI layer)" : "");
                }
                if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.Width < 1024 ||
                    (state & D3D12_RESOURCE_STATE_RENDER_TARGET) == 0) {
                    continue;
                }
                if (resource == g_ui_layer) {
                    target       = resource;
                    target_state = state;
                    matched      = true;
                } else if (target == nullptr) {
                    target       = resource;
                    target_state = state;
                }
            }
            if (!logged) {
                logged = true;
                spdlog::info("[MenuStereo] Scaleform passes draw into {}", target == nullptr ? "no render target found: menus are drawn once for both eyes"
                                                                          : matched ? "the composite's UI layer"
                                                                                    : "a render target the composite does not read (yet)");
            }
            if (target == nullptr) {
                return false;
            }
            out = PassTarget{ context->pID3D12CommandList, target, target_state };
            return true;
        }

        uintptr_t OnPass(void* pass, void* render_graph_data, void* pass_data)
        {
            using func_t         = uintptr_t(void*, void*, void*);
            static auto original = g_pass_hook->get_original<func_t>();
            if (*reinterpret_cast<uintptr_t*>(pass) != g_pass_vtable) {
                return original(pass, render_graph_data, pass_data);
            }
            static auto vr = VR::get();
            if (!g_frame_open) {
                g_frame_open   = true;
                g_frame_stereo = vr->is_hmd_active() && vr->is_native_stereo() && StereoViewModule::Get()->IsMenuFallback();
                g_left_started = false;
            }
            PassTarget target{};
            if (!g_frame_stereo || !FindTarget(render_graph_data, pass_data, target)) {
                return original(pass, render_graph_data, pass_data);
            }
            t_pass             = target;
            const auto result  = original(pass, render_graph_data, pass_data);
            t_pass             = {};
            return result;
        }

        // Renderer2D::Display(hal, tree root). Drawn again, the movie's render cache is already up to date and is only
        // drawn; the left eye's draw goes over the left eye's UI so far, the right eye's over the right's.
        void OnDisplay(void* hal, void* root)
        {
            using func_t         = void(void*, void*);
            static auto original = g_display_hook->get_original<func_t>();
            if (t_pass.target == nullptr) {
                original(hal, root);
                return;
            }
            const auto list   = t_pass.list;
            const auto target = t_pass.target;
            const auto state  = t_pass.state;
            const auto desc   = target->GetDesc();
            if (!Make(g_left, desc, D3D12_RESOURCE_STATE_COPY_DEST, L"Menu UI, left eye") || !Make(g_hold, desc, D3D12_RESOURCE_STATE_COPY_DEST, L"Menu UI, right eye hold")) {
                original(hal, root);
                return;
            }
            if (!g_left_started) {
                Copy(list, g_left.Get(), D3D12_RESOURCE_STATE_COPY_DEST, target, state);
                g_left_started = true;
            }
            Copy(list, g_hold.Get(), D3D12_RESOURCE_STATE_COPY_DEST, target, state);
            Copy(list, target, state, g_left.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            SetEye(hal, 1);
            original(hal, root);
            Copy(list, g_left.Get(), D3D12_RESOURCE_STATE_COPY_DEST, target, state);
            Copy(list, target, state, g_hold.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            SetEye(hal, 2);
            original(hal, root);
            SetEye(hal, 0);
        }
    }

    void InstallHooks()
    {
        auto vtable = reinterpret_cast<uintptr_t*>(MemoryScan::VTable("ScaleformRenderPass", ".?AVScaleformRenderPass@CreationRendererPrivate@@", 0));
        const auto display = MemoryScan::InstructionRelocation("48 8B 1D ? ? ? ? 48 8D 4C 24 58 E8 ? ? ? ? 48 8B D0 48 8B CB E8", 24, 28, 0x320f6f0);
        if (vtable == nullptr || display == 0) {
            spdlog::error("[MenuStereo] Scaleform pass or Display not found; menus are drawn once for both eyes");
            return;
        }
        g_pass_vtable  = reinterpret_cast<uintptr_t>(vtable);
        g_pass_hook    = std::make_unique<FunctionHook>(vtable[7], reinterpret_cast<uintptr_t>(&OnPass));
        g_pass_hook->create();
        g_display_hook = std::make_unique<FunctionHook>(display, reinterpret_cast<uintptr_t>(&OnDisplay));
        g_display_hook->create();
        spdlog::info("[MenuStereo] Menus are drawn per eye on fullscreen menu frames");
    }

    void OnMenuMovieCreated(void* menu)
    {
        if (menu == nullptr) {
            return;
        }
        // IMenu: GetName() is vfunc 3, the movie is at +0x88.
        auto       vtable = *reinterpret_cast<void***>(menu);
        const auto name   = reinterpret_cast<const char* (*)(void*)>(vtable[3])(menu);
        auto       movie  = *reinterpret_cast<void**>(static_cast<uint8_t*>(menu) + 0x88);
        if (name == nullptr || movie == nullptr) {
            return;
        }
        std::scoped_lock _{ g_menus_mutex };
        g_menus[movie] = MenuMovie{ menu, name };
    }

    void OnMovieFrame(void* movie)
    {
        static auto vr = VR::get();
        if (!vr->is_hmd_active() || !vr->is_native_stereo()) {
            return;
        }
        if (g_layout_check.fetch_add(1) % 300 == 0) {
            std::scoped_lock load{ g_load_mutex };
            LoadLayout();
            const auto left  = glm::vec3{ vr->get_eye_transform(VRRuntime::Eye::LEFT)[3] };
            const auto right = glm::vec3{ vr->get_eye_transform(VRRuntime::Eye::RIGHT)[3] };
            const float ipd  = glm::length(right - left);
            if (std::abs(ipd - g_ipd.load()) > 0.0005f) {
                g_ipd.store(ipd);
                spdlog::info("[MenuStereo] Eye distance {:.1f} mm", ipd * 1000.0f);
            }
        }

        MenuMovie menu{};
        {
            std::scoped_lock _{ g_menus_mutex };
            const auto it = g_menus.find(movie);
            if (it == g_menus.end()) {
                return;
            }
            menu = it->second;
        }
        std::vector<ClipDepth> clips;
        {
            std::scoped_lock _{ g_layout_mutex };
            const auto it = g_layout.find(menu.name);
            if (it == g_layout.end()) {
                return;
            }
            clips = it->second;
        }
        auto menu_root = reinterpret_cast<const GFx::Value*>(static_cast<uint8_t*>(menu.menu) + 0x58);
        if (!menu_root->IsObjectLike()) {
            return;
        }
        double focal = 0.0, cx = 0.0, cy = 0.0;
        if (!Perspective(*menu_root, focal, cx, cy)) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                spdlog::error("[MenuStereo] {}: its perspective cannot be read; its clips stay on the panel", menu.name);
            }
            return;
        }
        static std::unordered_map<std::string, double> logged_focal;
        if (logged_focal[menu.name] != focal) {
            logged_focal[menu.name] = focal;
            spdlog::info("[MenuStereo] {}: focal length {:.1f}, projection centre {:.1f}, {:.1f}", menu.name, focal, cx, cy);
        }

        // The menu's root clip places everything below it on the stage.
        double ox = 0.0, oy = 0.0, scale = 1.0;
        if (!Number(*menu_root, "x", ox) || !Number(*menu_root, "y", oy) || !Number(*menu_root, "scaleX", scale) || scale == 0.0) {
            return;
        }

        // A metre on the panel, 3 m away, is a third of the focal length in the menu's depth.
        constexpr size_t kMaxDepth = 8;
        for (const auto& clip_depth : clips) {
            if (clip_depth.path.empty() || clip_depth.path.size() > kMaxDepth) {
                continue;
            }
            // The centre in the clip's parent's space, through the 2D placement of the clips above it.
            GFx::Value chain[kMaxDepth];
            const GFx::Value* at = menu_root;
            double px = ox, py = oy, ps = scale;
            bool found = true;
            for (size_t i = 0; i < clip_depth.path.size(); ++i) {
                if (!at->GetMember(clip_depth.path[i].c_str(), &chain[i]) || !chain[i].IsObjectLike()) {
                    found = false;
                    break;
                }
                if (i + 1 < clip_depth.path.size()) {
                    double x = 0.0, y = 0.0, s = 1.0;
                    if (!Number(chain[i], "x", x) || !Number(chain[i], "y", y) || !Number(chain[i], "scaleX", s) || s == 0.0) {
                        found = false;
                        break;
                    }
                    px += x * ps;
                    py += y * ps;
                    ps *= s;
                }
                at = &chain[i];
            }
            if (!found) {
                continue;
            }
            const double z = -clip_depth.metres * focal / VR::kMenuPanelDistance;
            Place(chain[clip_depth.path.size() - 1], z, focal, (cx - px) / ps, (cy - py) / ps);
        }
    }

    void OnComposite(ID3D12GraphicsCommandList* command_list, ID3D12Resource* layer)
    {
        g_ui_layer = layer;
        if (!g_frame_open || !g_frame_stereo || !g_left_started || g_left == nullptr) {
            return;
        }
        if (!Make(g_left_out, g_left->GetDesc(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"Menu UI, left eye for the headset")) {
            return;
        }
        Copy(command_list, g_left_out.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, g_left.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        VR::get()->set_native_ui_source_left(g_left_out.Get());

        static int frames = 0;
        if (++frames % 600 == 1) {
            spdlog::info("[MenuStereo] Per-eye menu frame: eye projections so far left {}, right {}", g_eye_projections[0].load(), g_eye_projections[1].load());
        }
    }

    void EndFrame() { g_frame_open = false; }
}
