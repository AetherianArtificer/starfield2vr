#include "MenuStereo.h"

#include "GFx.h"
#include <CreationEngine/CreationEngineRendererModule.h>
#include <CreationEngine/StereoViewModule.h>
#include <CreationEngine/memory/ScanHelper.h>
#include <Framework.hpp>
#include <RE/C/CreationRendererPrivate.h>
#include <_deps/directxtk12-src/Src/d3dx12.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
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
        // ---- The layout: per menu, clips and regions, each at a depth tier (docs/vr-menu-rules.md).

        constexpr const char* kLayoutHeader = "# SFVR menu depth 7";
        constexpr const char* kDefaultLayout =
            "# SFVR menu depth 7\n"
            "# Fullscreen menus sit on a flat panel 3 m away. Each line puts part of a menu at a depth tier, in metres toward\n"
            "# the player (negative is behind the panel). Saved changes apply while the game runs.\n"
            "#   tier <name> <metres>               a depth every menu shares\n"
            "#   tilt <degrees>                     how far tilted parts turn toward the player\n"
            "#   <menu> <clip.path> <tier> [tilt] [inner]   a clip, by its path from the menu's root clip\n"
            "#   <menu> @x0,y0,x1,y1 <tier> [tilt] [inner]  the root clip's children placed inside that rectangle of the 1920x1080 stage\n"
            "# inner moves a clip's children instead of the clip: a clip its parent's timeline animates (a fade in) stops being\n"
            "# animated once a script moves it, and stays as it was, often invisible.\n"
            "#   <menu> <list.path> lift            the list's selected entry rises off the list by the lift tier\n"
            "# Only parts narrower than half the stage tilt. A line at the surface tier leaves its clips untouched: timeline masks\n"
            "# (clip depths) and what they mask must stay there, or everything drawn after them vanishes.\n"
            "# Logos, decoration and anything covering the stage stay on the surface; nothing goes behind it.\n"
            "tier surface 0\n"
            "tier info 0.1\n"
            "tier controls 0.2\n"
            "tier focus 0.35\n"
            "tier modal 0.5\n"
            "tier lift 0.08\n"
            "tilt 12\n"
            "\n"
            "MainMenu GameLogo_mc surface\n"
            "MainMenu AdBannerHolder_mc info\n"
            "MainMenu MOTDHolder_mc info\n"
            "MainMenu MainPanel_mc.MainList_mc focus tilt\n"
            "MainMenu MainPanel_mc.ConfirmPrompt_mc modal\n"
            "MainMenu MainPanel_mc.ButtonBar_mc controls\n"
            "MainMenu MainPanel_mc.LargeButtonBar_mc controls\n"
            "MainMenu LoadPanel_mc focus\n"
            "MainMenu MainPanel_mc.MainList_mc lift\n"
            "MainMenu LoadPanel_mc.LoadList_mc lift\n"
            "MainMenu SettingsPanel_mc controls\n"
            "MainMenu ButtonBar_mc controls\n"
            "MainMenu EngagementPrompt_mc controls\n"
            "\n"
            "PauseMenu GameLogo_mc surface\n"
            "PauseMenu MainPanel_mc.MainList_mc focus tilt\n"
            "PauseMenu MainPanel_mc.ConfirmPrompt_mc modal\n"
            "PauseMenu MainPanel_mc.ButtonBar_mc controls\n"
            "PauseMenu LoadPanel_mc focus\n"
            "PauseMenu SavePanel_mc focus\n"
            "PauseMenu MainPanel_mc.MainList_mc lift\n"
            "PauseMenu SettingsPanel_mc controls\n"
            "PauseMenu HelpPanel_mc controls\n"
            "PauseMenu InstalledContentPanel_mc controls\n"
            "PauseMenu CreationsLibraryPanel_mc controls\n"
            "\n"
            "# The hub: the player stays on the surface, the six tiles and the selection highlight come forward. The planet,\n"
            "# weapon and ship previews are cut out by timeline masks, which stay on the surface with what they mask.\n"
            "DataMenu @125,135,135,150 surface\n"
            "DataMenu InventoryPreviewClipMask_mc surface\n"
            "DataMenu InventoryWeaponPreview_mc surface\n"
            "DataMenu ShipPreviewClipMask_mc surface\n"
            "DataMenu ShipPreview_mc surface\n"
            "DataMenu @0,60,520,480 focus inner\n"
            "DataMenu Map_mc focus inner\n"
            "DataMenu @0,560,520,940 focus inner\n"
            "DataMenu Ship_mc focus inner\n"
            "DataMenu @1400,60,1920,480 focus inner\n"
            "DataMenu Skill_mc focus inner\n"
            "DataMenu @1400,560,1920,940 focus inner\n"
            "DataMenu Inventory_mc focus inner\n"
            "DataMenu @900,40,1020,120 focus inner\n"
            "DataMenu Power_mc focus inner\n"
            "DataMenu @0,880,1920,1000 focus inner\n"
            "DataMenu Mission_mc focus inner\n"
            "DataMenu MenuHighlight_mc focus inner\n"
            "\n"
            "InventoryMenu CategoryHeader_mc focus tilt inner\n"
            "InventoryMenu CategoryList_mc focus tilt inner\n"
            "InventoryMenu CategoryFooter_mc focus tilt inner\n"
            "InventoryMenu ItemHeader_mc focus tilt\n"
            "InventoryMenu ItemList_mc focus tilt\n"
            "InventoryMenu CategoryList_mc lift\n"
            "InventoryMenu ItemList_mc lift\n"
            "InventoryMenu ItemCard_mc info\n"
            "InventoryMenu EquippedItemCard_mc info\n"
            "InventoryMenu PlayerStatus_mc info\n"
            "InventoryMenu ButtonBar_mc controls\n"
            "InventoryMenu QuantityMenu_mc modal\n"
            "InventoryMenu LegendaryDetailCard_mc modal\n";

        struct ClipDepth
        {
            std::vector<std::string> path;  // empty for a region
            double                   region[4]{};
            double                   metres{ 0.0 };
            bool                     tilt{ false };
            bool                     lift{ false };  // the path is a list whose selected entry rises
            bool                     inner{ false };  // its children move, not the clip
        };

        std::mutex                                              g_layout_mutex;
        std::unordered_map<std::string, std::vector<ClipDepth>> g_layout;
        std::atomic<double>                                     g_tilt_degrees{ 0.0 };
        std::filesystem::file_time_type                         g_layout_time{};
        std::atomic<int>                                        g_layout_check{ 0 };
        std::mutex                                              g_load_mutex;

        void LoadLayout()
        {
            const auto path = Framework::get_persistent_dir("menu_depth.txt");
            std::error_code error;
            bool write = !std::filesystem::exists(path, error);
            if (!write) {
                std::string first;
                std::getline(std::ifstream{ path }, first);
                if (first.rfind(kLayoutHeader, 0) != 0) {
                    const auto old = Framework::get_persistent_dir("menu_depth.previous.txt");
                    std::filesystem::remove(old, error);
                    std::filesystem::rename(path, old, error);
                    spdlog::info("[MenuStereo] menu_depth.txt is from an older version; kept as {}", old.string());
                    write = true;
                }
            }
            if (write) {
                std::ofstream{ path } << kDefaultLayout;
                spdlog::info("[MenuStereo] Wrote the default menu depth layout to {}", path.string());
            }
            const auto time = std::filesystem::last_write_time(path, error);
            if (error || time == g_layout_time) {
                return;
            }
            g_layout_time = time;
            std::unordered_map<std::string, double>                 tiers;
            std::unordered_map<std::string, std::vector<ClipDepth>> layout;
            double tilt = 0.0;
            std::ifstream file{ path };
            std::string line;
            int clips = 0;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                std::istringstream words{ line };
                std::string first, second, third, fourth, fifth;
                words >> first >> second;
                if (first == "tilt") {
                    tilt = std::atof(second.c_str());
                    continue;
                }
                words >> third >> fourth >> fifth;
                if (first == "tier") {
                    if (third.empty()) {
                        spdlog::error("[MenuStereo] menu_depth.txt: \"{}\" has no depth", line);
                        continue;
                    }
                    tiers[second] = std::atof(third.c_str());
                    continue;
                }
                if (second.empty() || third.empty()) {
                    spdlog::error("[MenuStereo] menu_depth.txt: cannot read \"{}\"", line);
                    continue;
                }
                ClipDepth entry{};
                if (const auto tier = tiers.find(third); tier != tiers.end()) {
                    entry.metres = tier->second;
                } else {
                    char* end = nullptr;
                    entry.metres = std::strtod(third.c_str(), &end);
                    if (end == third.c_str()) {
                        spdlog::error("[MenuStereo] menu_depth.txt: \"{}\": no tier named {}", line, third);
                        continue;
                    }
                }
                entry.tilt  = fourth == "tilt" || fifth == "tilt";
                entry.inner = fourth == "inner" || fifth == "inner";
                entry.lift = third == "lift";
                if (second[0] == '@') {
                    if (std::sscanf(second.c_str() + 1, "%lf,%lf,%lf,%lf", &entry.region[0], &entry.region[1], &entry.region[2], &entry.region[3]) != 4) {
                        spdlog::error("[MenuStereo] menu_depth.txt: \"{}\": a region is @x0,y0,x1,y1", line);
                        continue;
                    }
                } else {
                    std::istringstream parts{ second };
                    for (std::string part; std::getline(parts, part, '.');) {
                        entry.path.push_back(part);
                    }
                }
                layout[first].push_back(std::move(entry));
                ++clips;
            }
            g_tilt_degrees.store(tilt);
            std::scoped_lock _{ g_layout_mutex };
            g_layout = std::move(layout);
            spdlog::info("[MenuStereo] Menu depth layout: {} entries in {} menus, {} tiers, tilt {} degrees", clips, g_layout.size(), tiers.size(), tilt);
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
        // A menu opened for the first time this session, snapshotted after this many per-eye frames.
        std::string                           g_snapshot_name;
        int                                   g_snapshot_frames{ -1 };

        // The values last written to a clip, and the clip's own values they were made from.
        struct Placed
        {
            double base[4]{};
            double wrote[4]{};
            bool   placed{ false };
            // -1 when the clip lies left of the projection centre, 1 right of it; 0 when it is too wide to tilt.
            double side{ 1.0 };
            int    rebases{ 0 };
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

        // The entry clip each list last lifted.
        std::unordered_map<void*, int> g_lifted;  // list -> clip index

        // BSScrollingContainer: selectedClipIndex and GetClipByIndex give the selected entry's clip.
        void LiftSelection(GFx::Value& list, double z)
        {
            double selected = -1.0;
            if (!Number(list, "selectedClipIndex", selected)) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    spdlog::error("[MenuStereo] A lift entry is not a scrolling list (no selectedClipIndex)");
                }
                return;
            }
            auto set = [&](int index, double value) {
                GFx::Value clip, argument{ (double)index };
                if (index >= 0 && list.Invoke("GetClipByIndex", &clip, &argument, 1) && clip.IsObjectLike()) {
                    clip.SetMember("z", GFx::Value(value));
                }
            };
            std::scoped_lock _{ g_placed_mutex };
            auto [it, added] = g_lifted.try_emplace(list.GetData(), -1);
            if (it->second != (int)selected) {
                set(it->second, 0.0);
                it->second = (int)selected;
            }
            set((int)selected, z);
        }

        // The clip a clip is masked by, or null.
        void* MaskOf(const GFx::Value& clip)
        {
            GFx::Value mask;
            return clip.GetMember("mask", &mask) && mask.IsObjectLike() ? mask.GetData() : nullptr;
        }

        void LogKept(const GFx::Value& clip, const char* why)
        {
            static std::mutex                      mutex;
            static std::unordered_map<void*, bool> logged;
            std::scoped_lock _{ mutex };
            if (logged[clip.GetData()]) {
                return;
            }
            logged[clip.GetData()] = true;
            GFx::Value name;
            const char* text = clip.GetMember("name", &name) ? name.GetString() : nullptr;
            spdlog::info("[MenuStereo] {} stays on the surface: {}", text ? text : "(unnamed)", why);
        }

        // A clip's position before it was placed.
        bool OwnPosition(const GFx::Value& clip, double& x, double& y)
        {
            {
                std::scoped_lock _{ g_placed_mutex };
                if (const auto it = g_placed.find(clip.GetData()); it != g_placed.end() && it->second.placed) {
                    x = it->second.base[0];
                    y = it->second.base[1];
                    return true;
                }
            }
            return Number(clip, "x", x) && Number(clip, "y", y);
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
        // Tilted clips turn about their vertical axis so the edge nearer the centre goes back.
        void Place(GFx::Value& clip, double z, double tilt, double focal, double cx, double cy)
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
            // Scale read back from a tilted clip comes from its 3D matrix, smaller by the cosine of the tilt, so only a
            // change well beyond that is the menu scaling the clip itself.
            int rebased = 0;
            for (int i = 0; i < 4; ++i) {
                const double tolerance = i < 2 ? 0.1 : std::max(0.002, std::abs(placed.wrote[i]) * 0.05);
                if (!placed.placed || std::abs(now[i] - placed.wrote[i]) > tolerance) {
                    placed.base[i] = now[i];
                    rebased += placed.placed ? 1 : 0;
                }
            }
            if (rebased > 0 && ++placed.rebases == 30) {
                spdlog::error("[MenuStereo] A clip at {:.1f}, {:.1f} keeps changing under its placement; it may drift", now[0], now[1]);
            }
            if (!placed.placed) {
                placed.placed = true;
                double width = 0.0;
                Number(clip, "width", width);
                placed.side = now[0] + width * 0.5 < cx ? -1.0 : 1.0;
                // Only side-anchored lists tilt; a part as wide as half the stage would swing far out of its place.
                if (tilt != 0.0 && width > 960.0) {
                    placed.side = 0.0;
                    spdlog::error("[MenuStereo] A clip {:.0f} wide is set to tilt; only parts narrower than half the stage tilt", width);
                }
                GFx::Value name;
                const char* text = clip.GetMember("name", &name) ? name.GetString() : nullptr;
                spdlog::info("[MenuStereo] {} at {:.1f}, {:.1f} scale {:.3f} width {:.1f} placed {:.1f} along the perspective", text ? text : "(unnamed)", now[0], now[1],
                             now[2], width, z);
            }
            const double k = (focal + z) / focal;
            const double out[4]{ cx + (placed.base[0] - cx) * k, cy + (placed.base[1] - cy) * k, placed.base[2] * k, placed.base[3] * k };
            clip.SetMember("z", GFx::Value(z));
            // Positive rotationY brings a clip's right edge toward the viewer.
            clip.SetMember("rotationY", GFx::Value(tilt * placed.side));
            for (int i = 0; i < 4; ++i) {
                clip.SetMember(kNames[i], GFx::Value(out[i]));
                placed.wrote[i] = out[i];
            }
        }

        // Places a clip, or its children for an inner line. (ox, oy, scale) places the clip's parent on the stage.
        void PlaceGroup(GFx::Value& clip, bool inner, double z, double tilt, double focal, double cx, double cy, double ox, double oy, double scale)
        {
            if (!inner) {
                Place(clip, z, tilt, focal, (cx - ox) / scale, (cy - oy) / scale);
                return;
            }
            double x = 0.0, y = 0.0, s = 1.0, children = 0.0;
            if (!Number(clip, "x", x) || !Number(clip, "y", y) || !Number(clip, "scaleX", s) || s == 0.0 || !Number(clip, "numChildren", children)) {
                return;
            }
            const double px = ox + x * scale;
            const double py = oy + y * scale;
            const double ps = scale * s;
            for (uint32_t i = 0; i < (uint32_t)children && i < 256; ++i) {
                GFx::Value child, index{ i };
                if (clip.Invoke("getChildAt", &child, &index, 1) && child.IsObjectLike()) {
                    Place(child, z, tilt, focal, (cx - px) / ps, (cy - py) / ps);
                }
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
        static std::unordered_map<std::string, bool> seen;
        if (!seen[name]) {
            seen[name] = true;
            spdlog::info("[MenuStereo] Menu {} opened", name);
            g_snapshot_name   = name;
            g_snapshot_frames = 90;
        }
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
        const double tilt = g_tilt_degrees.load();
        constexpr size_t kMaxDepth = 8;
        // Clips named on a line of their own; regions leave them to that line.
        std::vector<void*> named;
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
            auto& clip = chain[clip_depth.path.size() - 1];
            if (clip_depth.lift) {
                LiftSelection(clip, z / ps);
                continue;
            }
            named.push_back(clip.GetData());
            if (clip_depth.metres == 0.0 && !clip_depth.tilt) {
                continue;
            }
            if (MaskOf(clip) != nullptr) {
                LogKept(clip, "it is masked");
                continue;
            }
            // (px, py, ps) places the clip's parent on the stage.
            PlaceGroup(clip, clip_depth.inner, z, clip_depth.tilt ? tilt : 0.0, focal, cx, cy, px, py, ps);
        }

        // Regions: the root clip's children whose own position lies inside, in stage pixels.
        bool any_region = false;
        for (const auto& clip_depth : clips) {
            any_region = any_region || clip_depth.path.empty();
        }
        if (!any_region) {
            return;
        }
        auto root = const_cast<GFx::Value*>(menu_root);
        double children = 0.0;
        if (!Number(*root, "numChildren", children)) {
            return;
        }
        const uint32_t count = std::min<uint32_t>((uint32_t)children, 512);
        std::vector<void*> masks;
        for (uint32_t i = 0; i < count; ++i) {
            GFx::Value child, index{ i };
            if (root->Invoke("getChildAt", &child, &index, 1) && child.IsObjectLike()) {
                if (auto mask = MaskOf(child)) {
                    masks.push_back(mask);
                }
            }
        }
        for (uint32_t i = 0; i < count; ++i) {
            GFx::Value child, index{ i };
            double x = 0.0, y = 0.0;
            if (!root->Invoke("getChildAt", &child, &index, 1) || !child.IsObjectLike() || !OwnPosition(child, x, y)) {
                continue;
            }
            if (std::find(named.begin(), named.end(), child.GetData()) != named.end()) {
                continue;
            }
            const bool is_mask = std::find(masks.begin(), masks.end(), child.GetData()) != masks.end();
            const bool masked  = MaskOf(child) != nullptr;
            const double sx = ox + x * scale;
            const double sy = oy + y * scale;
            for (const auto& clip_depth : clips) {
                const auto& r = clip_depth.region;
                if (!clip_depth.path.empty() || sx < r[0] || sx > r[2] || sy < r[1] || sy > r[3]) {
                    continue;
                }
                if (clip_depth.metres == 0.0 && !clip_depth.tilt) {
                    break;
                }
                if (is_mask || masked) {
                    LogKept(child, is_mask ? "it masks another clip" : "it is masked");
                    break;
                }
                const double z = -clip_depth.metres * focal / VR::kMenuPanelDistance;
                PlaceGroup(child, clip_depth.inner, z, clip_depth.tilt ? tilt : 0.0, focal, cx, cy, ox, oy, scale);
                break;
            }
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

        {
            std::scoped_lock _{ g_menus_mutex };
            if (g_snapshot_frames >= 0 && g_snapshot_frames-- == 0) {
                const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                VR::get()->request_menu_dump(Framework::get_persistent_dir(std::format("vr_menu_{}_{}.png", g_snapshot_name, stamp)).wstring());
                spdlog::info("[MenuStereo] Saved a snapshot of {}", g_snapshot_name);
            }
        }
        static int frames = 0;
        if (++frames % 600 == 1) {
            spdlog::info("[MenuStereo] Per-eye menu frame: eye projections so far left {}, right {}", g_eye_projections[0].load(), g_eye_projections[1].load());
        }
    }

    void EndFrame() { g_frame_open = false; }
}
