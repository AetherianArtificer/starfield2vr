#include "BodyIK.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <RE/N/NiCamera.h>
#include <RE/P/PlayerCamera.h>
#include <safetyhook/easy.hpp>
#include <mods/VR.hpp>

#include <CreationEngine/CreationEngineSingletonManager.h>
#include <ModSettings.h>
#include <CreationEngine/memory/ScanHelper.h>
#include <CreationEngine/models/GameFlow.h>
#include <CreationEngine/models/ModSettingsStore.h>
#include <CreationEngine/vr/TrackingSpace.h>
#include <CreationEngine/input/InputRequests.h>
#include <CreationEngine/StereoViewModule.h>

#include "BodyMath.h"
#include "SceneGraph.h"

// Third-person body shown in first person, as the game does for furniture that uses the 3P rig in first
// person; arms are posed with IK on the skeleton's pose buffer.
namespace body
{
    namespace
    {
        constexpr std::size_t kLoaded3DOffset   = 0xB8;   // TESObjectREFR loaded data; 3P root at +0x8
        constexpr std::size_t kFirstPerson3D    = 0xDE8;  // PlayerCharacter 1P root
        constexpr std::size_t kFlagsOffset      = 0x118;  // NiAVObject flags; bit 0 = app culled
        constexpr std::size_t kModelNodeOffset  = 0x180;  // BGSModelNode of a 3D root
        constexpr int         kSetAppCulledSlot = 61;

        struct State
        {
            bool            active{ false };
            RE::NiAVObject* root{ nullptr };
            int             frame{ -1 };
        };
        State g_state;

        // The first-person pass stamps the frame it last ran (diagnostics).
        std::atomic<int> g_fp_last_frame{ -1000 };
        int              g_frame_counter{ 0 };

        bool FirstPersonArmsLive()
        {
            return GameFlow::gStore.internalSettings.firstPersonArms;
        }
        std::atomic<RE::NiAVObject*> g_body_muzzle{ nullptr };

        RE::NiAVObject* ThirdPersonRoot(RE::PlayerCharacter* player)
        {
            auto loaded = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(player) + kLoaded3DOffset);
            return loaded ? *reinterpret_cast<RE::NiAVObject**>(loaded + 0x8) : nullptr;
        }

        RE::NiAVObject* FirstPersonRoot(RE::PlayerCharacter* player)
        {
            return *reinterpret_cast<RE::NiAVObject**>(reinterpret_cast<std::uint8_t*>(player) + kFirstPerson3D);
        }

        bool IsAppCulled(const RE::NiAVObject* node)
        {
            return (*reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(node) + kFlagsOffset) & 1) != 0;
        }

        void SetAppCulled(RE::NiAVObject* node, bool culled)
        {
            if (!node || IsAppCulled(node) == culled) {
                return;
            }
            using fn_t = void (*)(RE::NiAVObject*, bool);
            (*reinterpret_cast<fn_t**>(node))[kSetAppCulledSlot](node, culled);
        }

        RE::NiAVObject* FaceNode(RE::PlayerCharacter* player)
        {
            using fn_t     = RE::NiAVObject* (*)(RE::PlayerCharacter*);
            static auto fn = reinterpret_cast<fn_t>(MemoryScan::FuncRelocation("48 89 5C 24 18 57 48 83 EC 20 33 DB 48 89", 0x195d930, 0));
            return fn ? fn(player) : nullptr;
        }

        bool Wanted()
        {
            static auto vr = VR::get();
            return GameFlow::gStore.internalSettings.weaponFollowsHand && vr->is_hmd_active() && !ModSettings::showFlatScreenDisplay() &&
                   vr->is_using_controllers() && GameFlow::isInFirstPerson() && !GameFlow::isImmovable() && !GameFlow::isControlledByAI();
        }

        // "Third-person rig in first person" (the pilot seat's mode): the third-person body and its animation graph
        // are the live ones while the camera stays first-person. Only the active graph receives weapon events, so
        // this is what makes the body draw, hold, fire and reload with its own authored animations. Entered through
        // the game's SetFirstPerson; Use3PRig is detoured so the camera is not moved onto the third-person skeleton.
        namespace third_person_mode
        {
            constexpr std::size_t kLiveBodyFlags = 0x112A;  // bit 3: third-person body live
            constexpr std::size_t kRigModeFlags  = 0x112F;  // bit 3: third-person rig in first person

            std::atomic<bool> g_wanted{ false };
            safetyhook::InlineHook g_use_3p_rig_hook;
            void*                  g_save_original{ nullptr };

            std::uint8_t& Flags(RE::PlayerCharacter* player, std::size_t offset)
            {
                return *(reinterpret_cast<std::uint8_t*>(player) + offset);
            }

            bool Active(RE::PlayerCharacter* player)
            {
                return player && (Flags(player, kRigModeFlags) & 8) != 0;
            }

            // Game 1.16.244 addresses without a unique signature.
            std::uintptr_t GameAddress(std::uintptr_t offset)
            {
                return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + offset;
            }

            template <class Fn>
            Fn Resolve(const char* pattern, std::uintptr_t offset)
            {
                return reinterpret_cast<Fn>(MemoryScan::FuncRelocation(pattern, offset, 0));
            }

            // Enable path of Use3PRig without the camera retarget (0x1e8ba70).
            std::uintptr_t Use3PRig(RE::PlayerCharacter* player, bool enable)
            {
                auto self = CreationEngineSingletonManager::GetPlayerRef();
                if (!enable || !g_wanted || player != self) {
                    return g_use_3p_rig_hook.call<std::uintptr_t>(player, enable);
                }
                if (Active(player)) {
                    return 0;
                }
                using hide_slots_t  = void (*)(void*, std::uint32_t, bool);
                using hide_helmet_t = void (*)(RE::PlayerCharacter*, bool, std::uint32_t);
                using refresh_t     = void (*)(RE::PlayerCharacter*);
                using blend_t       = void (*)(RE::PlayerCharacter*, float);
                static auto hide_slots  = Resolve<hide_slots_t>("48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 45 0F B6 F8 44", 0x50c140);
                static auto hide_helmet = Resolve<hide_helmet_t>("48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 41 8B F8 0F", 0xb71430);
                static auto refresh     = Resolve<refresh_t>("48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC A0 00 00 00 48 8B F1", 0x1a34a50);
                static auto blend       = reinterpret_cast<blend_t>(GameAddress(0x1a3ce80));
                static auto mask_addr   = GameAddress(0x5F46750);
                const auto  mask        = *reinterpret_cast<std::uint32_t*>(mask_addr);

                Flags(player, kLiveBodyFlags) |= 8;
                hide_slots(*reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(player) + 0xC8), mask, true);
                hide_helmet(player, true, mask);
                SetAppCulled(ThirdPersonRoot(player), false);
                SetAppCulled(FirstPersonRoot(player), true);
                SetAppCulled(FaceNode(player), true);
                Flags(player, kRigModeFlags) |= 8;
                refresh(player);
                blend(player, 0.01f);
                spdlog::info("[Body] third-person rig in first person: on");
                return 0;
            }

            void SetFirstPerson(RE::PlayerCharacter* player)
            {
                using fn_t     = void (*)(RE::PlayerCharacter*, bool);
                static auto fn = Resolve<fn_t>("88 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 B8", 0x1a3f340);
                fn(player, true);
            }

            // Saves never record the mode (the game would restore a third-person camera from it).
            void SaveWithoutMode(RE::PlayerCharacter* player, void* a2, void* a3, void* a4)
            {
                const auto live = Flags(player, kLiveBodyFlags);
                const auto rig  = Flags(player, kRigModeFlags);
                if (rig & 8) {
                    Flags(player, kLiveBodyFlags) &= ~8;
                    Flags(player, kRigModeFlags) &= ~8;
                }
                reinterpret_cast<void (*)(RE::PlayerCharacter*, void*, void*, void*)>(g_save_original)(player, a2, a3, a4);
                Flags(player, kLiveBodyFlags) = live;
                Flags(player, kRigModeFlags)  = rig;
            }

            void Install()
            {
                const auto address = MemoryScan::FuncRelocation("48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 60 0F", 0x1a3ef40, 0);
                g_use_3p_rig_hook  = safetyhook::create_inline(reinterpret_cast<void*>(address), reinterpret_cast<void*>(&Use3PRig));
                spdlog::info("[Body] Use3PRig hook: {}", static_cast<bool>(g_use_3p_rig_hook));
            }

            void HookSave(RE::PlayerCharacter* player)
            {
                if (g_save_original || !player) {
                    return;
                }
                auto vtable = *reinterpret_cast<void***>(player);
                DWORD old{};
                if (VirtualProtect(&vtable[0xD0 / 8], sizeof(void*), PAGE_READWRITE, &old)) {
                    g_save_original      = vtable[0xD0 / 8];
                    vtable[0xD0 / 8]     = reinterpret_cast<void*>(&SaveWithoutMode);
                    VirtualProtect(&vtable[0xD0 / 8], sizeof(void*), old, &old);
                    spdlog::info("[Body] save hook installed");
                }
            }

            // Main-thread, once per frame: keep the mode in step with the VR body and first-person camera.
            void Update()
            {
                auto player = CreationEngineSingletonManager::GetPlayerRef();
                auto camera = CreationEngineSingletonManager::GetPlayerCameraSingleton();
                if (!player || !camera) {
                    return;
                }
                HookSave(player);
                const bool want = g_state.active && GameFlow::gStore.internalSettings.nativeWeapons;
                const bool fps  = camera->IsInFirstPerson();
                if (want != g_wanted.load()) {
                    g_wanted = want;
                    if (fps) {
                        SetFirstPerson(player);  // enters the mode, or (wanted off) runs the game's own exit
                    }
                    spdlog::info("[Body] third-person rig in first person wanted: {}", want);
                } else if (want && fps && !Active(player)) {
                    SetFirstPerson(player);  // the game left the mode (furniture exit, camera change)
                }

                // One pair of eye screenshots a few seconds into each of the first draws, for checking the grip.
                static std::chrono::steady_clock::time_point drawn_since{};
                static bool taken_this_draw{ false };
                static int  taken{ 0 };
                const auto  now = std::chrono::steady_clock::now();
                if (!player->IsWeaponDrawn()) {
                    drawn_since     = now;
                    taken_this_draw = false;
                } else if (!taken_this_draw && taken < 3 && now - drawn_since > std::chrono::seconds(4)) {
                    taken_this_draw = true;
                    ++taken;
                    StereoViewModule::Get()->RequestEyeScreenshots();
                    spdlog::info("[Body] eye screenshots requested with the weapon drawn");
                }
            }
        }

        // Called when the 3P root updates, before its skeleton.
        void UpdateVisibility(RE::PlayerCharacter* player, RE::NiAVObject* root)
        {
            auto first_person = FirstPersonRoot(player);
            if (Wanted()) {
                if (g_state.root != root) {
                    g_state.root = root;
                    spdlog::info("[Body] 3P root '{}'", root->name.c_str());
                }
                SetAppCulled(root, false);
                SetAppCulled(first_person, !FirstPersonArmsLive());
                SetAppCulled(FaceNode(player), true);
                g_state.active = true;
            } else if (g_state.active) {
                g_body_muzzle = nullptr;
                if (GameFlow::isInFirstPerson()) {
                    SetAppCulled(root, true);
                    SetAppCulled(first_person, false);
                }
                SetAppCulled(FaceNode(player), false);
                g_state.active = false;
                g_state.root   = nullptr;
            }
        }

        // A tracked hand in game world space.
        struct HandTarget
        {
            glm::vec3 position;
            glm::vec3 forward;
            glm::vec3 up;
        };

        std::optional<HandTarget> HandWorld(const glm::mat4& pose)
        {
            auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
            const auto room   = tracking::RoomRotation();
            if (!world_camera || !world_camera->parent || !room) {
                return std::nullopt;
            }
            const auto& anchor = world_camera->parent->world;
            const auto  offset = *room * tracking::ToHavokVector((glm::vec3{ pose[3] } - tracking::StandingOriginPosition()) * tracking::TrackingScale()) * anchor.scale;
            const auto  rot    = glm::mat3{ pose };
            return HandTarget{ glm::vec3{ anchor.translate.x, anchor.translate.y, anchor.translate.z } + offset,
                glm::normalize(*room * tracking::ToHavokVector(rot * glm::vec3{ 0.0f, 0.0f, -1.0f })),
                glm::normalize(*room * tracking::ToHavokVector(rot * glm::vec3{ 0.0f, 1.0f, 0.0f })) };
        }

        // Headset body and hand tracking for this frame, mapped into the game through the same tracking space as
        // the head and controllers.
        VR::BodyTrackingState g_tracked_body;
        VR::HandTrackingState g_tracked_hands[2];

        namespace joint
        {
            constexpr int kHips = 1, kNeck = 6;
            constexpr int kArmUpper[2] = { 10, 15 }, kArmLower[2] = { 11, 16 }, kWrist[2] = { 19, 45 };
            constexpr int kUpperLeg[2] = { 70, 77 }, kLowerLeg[2] = { 71, 78 }, kAnkle[2] = { 73, 80 };
        }

        struct TrackingDiag
        {
            bool supported{ false };
            bool active{ false };
            int  valid{ 0 };
            bool torso{ false };
            bool elbow[2]{};
            bool legs{ false };
            bool  fingers[2]{};
            int   hand_source[2]{};
            float index_bend[2]{ -1.0f, -1.0f };
        };
        TrackingDiag g_track_diag;

        void SnapshotTracking()
        {
            static auto vr = VR::get();
            g_tracked_body = {};
            vr->get_body_tracking(g_tracked_body);
            for (int side = 0; side < 2; ++side) {
                g_tracked_hands[side] = {};
                vr->get_hand_tracking(side == 0, g_tracked_hands[side]);
            }
            g_track_diag           = {};
            g_track_diag.supported = g_tracked_body.supported;
            g_track_diag.active    = g_tracked_body.active;
            for (std::uint32_t i = 0; i < g_tracked_body.joint_count && i < g_tracked_body.joints.size(); ++i) {
                g_track_diag.valid += g_tracked_body.joints[i].position_valid ? 1 : 0;
            }
            for (int side = 0; side < 2; ++side) {
                g_track_diag.hand_source[side] = g_tracked_hands[side].active ? g_tracked_hands[side].data_source : -1;
            }
        }

        bool BodyTrackingOn()
        {
            return GameFlow::gStore.internalSettings.bodyTracking && g_tracked_body.active;
        }

        std::optional<glm::vec3> TrackedJoint(int index)
        {
            if (!BodyTrackingOn() || index < 0 || index >= static_cast<int>(g_tracked_body.joint_count) ||
                !g_tracked_body.joints[index].position_valid) {
                return std::nullopt;
            }
            return glm::vec3{ g_tracked_body.joints[index].position };
        }

        // A stage-space direction in game world space.
        std::optional<glm::vec3> StageDirection(const glm::vec3& d)
        {
            const auto room = tracking::RoomRotation();
            if (!room || glm::length(d) < 1e-5f) {
                return std::nullopt;
            }
            return glm::normalize(*room * tracking::ToHavokVector(d));
        }

        // Direction between two tracked joints, in game world space.
        std::optional<glm::vec3> TrackedDirection(int from, int to)
        {
            const auto a = TrackedJoint(from);
            const auto b = TrackedJoint(to);
            if (!a || !b) {
                return std::nullopt;
            }
            return StageDirection(*b - *a);
        }

        // Side the limb bends toward at its middle joint (elbow or knee), in game world space.
        std::optional<glm::vec3> TrackedBend(int root, int middle, int end)
        {
            const auto a = TrackedJoint(root);
            const auto b = TrackedJoint(middle);
            const auto c = TrackedJoint(end);
            if (!a || !b || !c) {
                return std::nullopt;
            }
            const auto axis = *c - *a;
            if (glm::length(axis) < 1e-3f) {
                return std::nullopt;
            }
            const auto n    = glm::normalize(axis);
            auto       bend = (*b - *a) - glm::dot(*b - *a, n) * n;
            if (glm::length(bend) < 5e-3f) {
                return std::nullopt;  // limb nearly straight; the bend side is not observable
            }
            return StageDirection(bend);
        }

        bool PlayerSeated()
        {
            static auto vr     = VR::get();
            const float height = vr->get_floor_eye_height();
            return height >= 0.0f && height < 1.3f;
        }

        // BSModelNode pose storage (see local RE notes): locals feed the batched world computation.
        // Animation does not rewrite every local each frame; bones we wrote are restored to their animated
        // value first so the IK never builds on its own output.
        struct WrittenBone
        {
            RE::NiTransform animated{};
            RE::NiTransform written{};
        };
        // Per skeleton (body, first-person rig): bones written this frame and their animated values.
        std::unordered_map<int, WrittenBone>  g_written_body;
        std::unordered_map<int, WrittenBone>  g_written_first;
        std::unordered_map<int, WrittenBone>* g_written_active{ &g_written_body };

        struct Pose
        {
            std::uint8_t*    model{ nullptr };
            RE::NiTransform* local{ nullptr };
            RE::NiTransform* world{ nullptr };
            std::uint16_t*   parent{ nullptr };
            std::uint16_t    count{ 0 };
            std::uint16_t    top{ 0 };
            Xf               root{};      // the root transform top-level entries are relative to
            Xf               to_world{};  // pose space -> game world

            Xf World(int i, int depth = 0) const
            {
                if (i < top || depth > 64) {
                    return Compose(root, FromNi(local[i]));
                }
                return Compose(World(parent[i], depth + 1), FromNi(local[i]));
            }

            Xf GameWorld(int i) const { return Compose(to_world, World(i)); }

            void SetLocalRotation(int i, const RE::NiMatrix3& rotation) const
            {
                auto [entry, first] = (*g_written_active).try_emplace(i);
                if (first) {
                    entry->second.animated = local[i];
                }
                local[i].rotate       = rotation;
                entry->second.written = local[i];
            }

            void ScaleTranslation(int i, float k) const
            {
                auto [entry, first] = (*g_written_active).try_emplace(i);
                if (first) {
                    entry->second.animated = local[i];
                }
                local[i].translate.x *= k;
                local[i].translate.y *= k;
                local[i].translate.z *= k;
                entry->second.written = local[i];
            }

            Xf ParentWorld(int i) const { return i < top ? root : World(parent[i]); }

            // Sets bone i so its game-world transform becomes `desired`; descendants follow.
            void SetGameWorld(int i, const Xf& desired) const
            {
                const auto pose_space = Compose(Inverse(to_world), desired);
                const auto result     = Compose(Inverse(ParentWorld(i)), pose_space);
                bool       finite     = std::isfinite(result.s);
                for (int a = 0; a < 3 && finite; ++a) {
                    finite = std::isfinite(result.t[a]) && std::isfinite(result.r[a][0]) && std::isfinite(result.r[a][1]) && std::isfinite(result.r[a][2]);
                }
                if (!finite) {
                    spdlog::error("[BodyIK] non-finite transform computed for pose bone {}", i);
                    return;
                }
                {
                    auto [entry, first] = (*g_written_active).try_emplace(i);
                    if (first) {
                        entry->second.animated = local[i];
                    }
                    ToNi(result, local[i]);
                    entry->second.written = local[i];
                }
            }
        };

        // A hand's frame measured from its own bones, in the wrist bone's space: forward toward the finger
        // bases, up toward the thumb base, and the palm centre as a distance along forward.
        struct HandShape
        {
            bool      valid{ false };
            glm::vec3 forward{};
            glm::vec3 up{};
            float     palm{ 0.0f };
        };

        struct Arm
        {
            int              clavicle{ -1 }, biceps{ -1 }, forearm{ -1 }, wrist{ -1 }, thumb{ -1 };
            float            twist_prev{ 0.0f };
            // Side of the upper arm the forearm bends toward (the elbow hinge), in the upper arm's space.
            bool             has_bend{ false };
            glm::vec3        bend_side{};
            std::vector<int> fingers;
            HandShape        shape;
        };

        struct Bones
        {
            const void* storage{ nullptr };
            Arm         right, left;
            int         head{ -1 }, neck{ -1 }, weapon{ -1 }, chest{ -1 };
            struct Leg
            {
                int thigh{ -1 }, calf{ -1 }, foot{ -1 };
            } left_leg, right_leg;
            int pelvis{ -1 };  // lowest bone above both a thigh and the neck
            std::vector<int> spine;  // bones from above the pelvis up to the neck, base first
            // Finger chains (base, middle, tip) per hand: thumb, index, middle, ring, pinky.
            std::array<std::array<int, 3>, 5> fingers[2]{};
            std::unordered_map<std::string, int> by_name;
            RE::NiAVObject* weapon_node{ nullptr };
            RE::NiAVObject* r_wrist_node{ nullptr };
            // Legacy accessors used by diagnostics.
            int r_biceps{ -1 }, r_forearm{ -1 }, r_wrist{ -1 }, l_wrist{ -1 };
        };
        // The skeleton being posed: the body or the first-person rig.
        Bones  g_body_bones;
        Bones  g_first_bones;
        Bones* g_active_bones{ &g_body_bones };

        // Bones by name. Pose-buffer skeletons list their nodes in the model's sync list; a flattened node tree
        // passes its nodes directly.
        void FindBones(const Pose& pose, const void* storage, const std::vector<std::pair<int, RE::NiAVObject*>>* nodes = nullptr)
        {
            (*g_active_bones)         = {};
            (*g_active_bones).storage = storage;
            std::vector<std::pair<int, std::string>> named;
            std::vector<std::pair<int, RE::NiAVObject*>> listed;
            if (nodes) {
                listed = *nodes;
            } else {
                const auto entries = *reinterpret_cast<std::uint8_t**>(pose.model + 0x20);
                const auto count   = *reinterpret_cast<std::uint32_t*>(pose.model + 0x18);
                for (std::uint32_t e = 0; entries && e < count; ++e) {
                    listed.emplace_back(*reinterpret_cast<std::uint16_t*>(entries + e * 16), *reinterpret_cast<RE::NiAVObject**>(entries + e * 16 + 8));
                }
            }
            for (const auto& [idx, node] : listed) {
                if (!node || idx < 0 || idx >= pose.count) {
                    continue;
                }
                const std::string_view name{ node->name.c_str() };
                named.emplace_back(idx, std::string{ name });
                if (name == "R_Biceps") (*g_active_bones).right.biceps = idx;
                else if (name == "R_Forearm") (*g_active_bones).right.forearm = idx;
                else if (name == "R_Wrist") {
                    (*g_active_bones).right.wrist  = idx;
                    (*g_active_bones).r_wrist_node = node;
                }
                else if (name == "L_Biceps") (*g_active_bones).left.biceps = idx;
                else if (name == "L_Forearm") (*g_active_bones).left.forearm = idx;
                else if (name == "L_Wrist") (*g_active_bones).left.wrist = idx;
                else if (name == "C_Head") (*g_active_bones).head = idx;
                else if (name == "C_Neck") (*g_active_bones).neck = idx;
                else if (name == "L_Thigh") (*g_active_bones).left_leg.thigh = idx;
                else if (name == "L_Calf") (*g_active_bones).left_leg.calf = idx;
                else if (name == "L_Foot") (*g_active_bones).left_leg.foot = idx;
                else if (name == "R_Thigh") (*g_active_bones).right_leg.thigh = idx;
                else if (name == "R_Calf") (*g_active_bones).right_leg.calf = idx;
                else if (name == "R_Foot") (*g_active_bones).right_leg.foot = idx;
                else if (name == "C_Chest") (*g_active_bones).chest = idx;
                else if (name == "Weapon") {
                    (*g_active_bones).weapon      = idx;
                    (*g_active_bones).weapon_node = node;
                }
            }
            // Finger bases are the wrist's direct children.
            for (auto* arm : { &(*g_active_bones).right, &(*g_active_bones).left }) {
                std::string names;
                for (const auto& [idx, name] : named) {
                    if (arm->wrist < 0 || idx < pose.top || pose.parent[idx] != arm->wrist) {
                        continue;
                    }
                    names += " " + name;
                    if (Contains(name, "Thumb")) {
                        arm->thumb = idx;
                    } else if (Contains(name, "Index") || Contains(name, "Middle") || Contains(name, "Ring") || Contains(name, "Pinky") ||
                               Contains(name, "Little")) {
                        arm->fingers.push_back(idx);
                    }
                }
                // Unrecognised names: every child is a finger base, and the one farthest off their mean axis is the thumb.
                if (arm->thumb < 0 || arm->fingers.empty()) {
                    std::vector<int> children;
                    for (const auto& [idx, name] : named) {
                        if (arm->wrist >= 0 && idx >= pose.top && pose.parent[idx] == arm->wrist) {
                            children.push_back(idx);
                        }
                    }
                    if (children.size() >= 2) {
                        const auto wrist = pose.World(arm->wrist).t;
                        glm::vec3  mean{ 0.0f };
                        for (const int c : children) {
                            mean += pose.World(c).t - wrist;
                        }
                        const auto axis = glm::length(mean) > 1e-4f ? glm::normalize(mean) : glm::vec3{ 1.0f, 0.0f, 0.0f };
                        float      best = -1.0f;
                        for (const int c : children) {
                            const auto d   = pose.World(c).t - wrist;
                            const float off = glm::length(d - glm::dot(d, axis) * axis);
                            if (off > best) {
                                best       = off;
                                arm->thumb = c;
                            }
                        }
                        arm->fingers.clear();
                        for (const int c : children) {
                            if (c != arm->thumb) {
                                arm->fingers.push_back(c);
                            }
                        }
                        names += " (thumb by geometry)";
                    }
                }
                spdlog::info("[BodyIK] {} wrist children:{}", arm == &(*g_active_bones).right ? "right" : "left", names);
            }
            for (auto* arm : { &(*g_active_bones).right, &(*g_active_bones).left }) {
                if (arm->biceps >= pose.top) {
                    arm->clavicle = pose.parent[arm->biceps];
                }
            }
            for (const auto& [idx, name] : named) {
                (*g_active_bones).by_name[name] = idx;
            }
            {
                static constexpr const char* kFingerNames[5][3] = { { "thumb", "Thumb1", "Thumb2" }, { "Index", "Index1", "Index2" },
                    { "Middle", "Middle1", "Middle2" }, { "Ring", "Ring1", "Ring2" }, { "Pinky", "Pinky1", "Pinky2" } };
                for (int side = 0; side < 2; ++side) {
                    const std::string prefix = side == 0 ? "L_" : "R_";
                    for (int f = 0; f < 5; ++f) {
                        for (int j = 0; j < 3; ++j) {
                            const auto it                = (*g_active_bones).by_name.find(prefix + kFingerNames[f][j]);
                            (*g_active_bones).fingers[side][f][j] = it != (*g_active_bones).by_name.end() ? it->second : -1;
                        }
                    }
                }
            }

            // Leg bones by name fragment when the exact names differ; twist helpers excluded.
            auto find_leg = [&](int& slot, std::string_view side, std::string_view part) {
                if (slot >= 0) {
                    return;
                }
                for (const auto& [idx, name] : named) {
                    if (name.rfind(side, 0) == 0 && Contains(name, part) && !Contains(name, "Twist")) {
                        slot = idx;
                        return;
                    }
                }
            };
            find_leg((*g_active_bones).left_leg.thigh, "L_", "Thigh");
            find_leg((*g_active_bones).left_leg.calf, "L_", "Calf");
            find_leg((*g_active_bones).left_leg.foot, "L_", "Foot");
            find_leg((*g_active_bones).right_leg.thigh, "R_", "Thigh");
            find_leg((*g_active_bones).right_leg.calf, "R_", "Calf");
            find_leg((*g_active_bones).right_leg.foot, "R_", "Foot");

            // Pelvis: the lowest common ancestor of a thigh and the neck.
            if ((*g_active_bones).left_leg.thigh >= 0 && (*g_active_bones).neck >= 0) {
                std::vector<int> neck_chain;
                for (int i = (*g_active_bones).neck, guard = 0; i >= 0 && guard < 64; ++guard) {
                    neck_chain.push_back(i);
                    i = i < pose.top ? -1 : pose.parent[i];
                }
                for (int i = (*g_active_bones).left_leg.thigh, guard = 0; i >= 0 && guard < 64; ++guard) {
                    if (std::find(neck_chain.begin(), neck_chain.end(), i) != neck_chain.end()) {
                        (*g_active_bones).pelvis = i;
                        break;
                    }
                    i = i < pose.top ? -1 : pose.parent[i];
                }
            }
            // Spine: from the neck down to (not including) the pelvis, then reversed.
            if ((*g_active_bones).neck >= 0 && (*g_active_bones).pelvis >= 0) {
                for (int i = (*g_active_bones).neck >= pose.top ? pose.parent[(*g_active_bones).neck] : -1, guard = 0; i >= 0 && i != (*g_active_bones).pelvis && guard < 16; ++guard) {
                    (*g_active_bones).spine.push_back(i);
                    i = i < pose.top ? -1 : pose.parent[i];
                }
                std::reverse((*g_active_bones).spine.begin(), (*g_active_bones).spine.end());
            }
            {
                std::string all;
                for (const auto& [idx, name] : named) {
                    all += " " + std::to_string(idx) + ":" + name;
                }
                spdlog::info("[BodyIK] bones:{}", all);
                const auto pelvis_name = std::find_if(named.begin(), named.end(), [](const auto& n) { return n.first == (*g_active_bones).pelvis; });
                spdlog::info("[BodyIK] legs L {} {} {} R {} {} {} | pelvis {} '{}'", (*g_active_bones).left_leg.thigh, (*g_active_bones).left_leg.calf, (*g_active_bones).left_leg.foot,
                    (*g_active_bones).right_leg.thigh, (*g_active_bones).right_leg.calf, (*g_active_bones).right_leg.foot, (*g_active_bones).pelvis,
                    pelvis_name != named.end() ? pelvis_name->second : "");
            }
            (*g_active_bones).r_biceps  = (*g_active_bones).right.biceps;
            (*g_active_bones).r_forearm = (*g_active_bones).right.forearm;
            (*g_active_bones).r_wrist   = (*g_active_bones).right.wrist;
            (*g_active_bones).l_wrist   = (*g_active_bones).left.wrist;
            spdlog::info("[BodyIK] {} pose bones (top {}): R {} {} {} thumb {} fingers {} | L {} {} {} thumb {} fingers {} | head {} neck {} weapon {}",
                pose.count, pose.top, (*g_active_bones).right.biceps, (*g_active_bones).right.forearm, (*g_active_bones).right.wrist, (*g_active_bones).right.thumb,
                (*g_active_bones).right.fingers.size(), (*g_active_bones).left.biceps, (*g_active_bones).left.forearm, (*g_active_bones).left.wrist, (*g_active_bones).left.thumb,
                (*g_active_bones).left.fingers.size(), (*g_active_bones).head, (*g_active_bones).neck, (*g_active_bones).weapon);
        }

        // Port of FRIK's solveArmToHandWorldTarget (Fallout 4 VR Body, GPL-3.0): shoulder reach, elbow flexion floor
        // and ceiling, wrist-driven elbow direction, twist split, exact hand. FO4 distances are in units of 1/70 m.
        constexpr float kUnit = 1.0f / 70.0f;

        float SolveArm(const Pose& pose, Arm& arm, bool is_left, const Xf& hand_target, const std::optional<glm::vec3>& tracked_bend = std::nullopt)
        {
            const float neg_left = is_left ? -1.0f : 1.0f;
            const auto  up_axis  = glm::vec3{ 0.0f, 0.0f, 1.0f };

            auto forward = pose.root.r[1];
            forward.z    = 0.0f;
            if (glm::length(forward) < 1e-3f) {
                return 0.0f;
            }
            forward              = glm::normalize(forward);
            const auto sideways_r = glm::vec3{ forward.y, -forward.x, 0.0f };
            const auto sideways   = sideways_r * neg_left;

            const auto hand_pos = hand_target.t;
            const auto hand_rot = hand_target.r;

            const float orig_upper   = glm::length(pose.GameWorld(arm.forearm).t - pose.GameWorld(arm.biceps).t);
            const float orig_forearm = glm::length(pose.GameWorld(arm.wrist).t - pose.GameWorld(arm.forearm).t);
            if (orig_upper < 1e-3f || orig_forearm < 1e-3f) {
                return 0.0f;
            }
            float       upper_len   = orig_upper;
            float       forearm_len = orig_forearm;
            const float arm_length  = orig_upper + orig_forearm;

            const auto shoulder_to_hand = hand_pos - pose.GameWorld(arm.biceps).t;
            if (glm::length(shoulder_to_hand) > arm_length * 2.25f) {
                return 0.0f;
            }

            // Shoulder reach.
            if (arm.clavicle >= 0) {
                const float adjust = std::clamp(glm::length(shoulder_to_hand) - arm_length * 0.5f, 0.0f, arm_length * 0.85f) / (arm_length * 0.85f);
                auto        offset = glm::normalize(shoulder_to_hand) * (adjust * arm_length * 0.15f);
                if (offset.z < 0.0f) {
                    offset.z *= 0.4f;
                }
                const auto clavicle  = pose.GameWorld(arm.clavicle);
                const auto shoulder  = pose.GameWorld(arm.biceps).t;
                auto       clav_new  = clavicle;
                clav_new.r           = RotationBetween(shoulder - clavicle.t, shoulder + offset - clavicle.t) * clavicle.r;
                pose.SetGameWorld(arm.clavicle, clav_new);
            }

            const auto  upper_world = pose.GameWorld(arm.biceps);
            const auto  hand_to_shoulder = upper_world.t - hand_pos;
            const float hs_len      = std::max(glm::length(hand_to_shoulder), 0.1f * kUnit);

            // Flexion floor (10 degrees): stretch both bones rather than straighten the arm.
            const float min_flex = glm::radians(10.0f);
            const float reach_min = std::sqrt(upper_len * upper_len + forearm_len * forearm_len + 2.0f * upper_len * forearm_len * std::cos(min_flex));
            if (hs_len > reach_min) {
                const float stretch = hs_len / reach_min;
                upper_len *= stretch;
                forearm_len *= stretch;
            }
            // Flexion ceiling (145 degrees).
            const float max_flex      = glm::radians(145.0f);
            const float reach_max     = std::sqrt(std::max(upper_len * upper_len + forearm_len * forearm_len + 2.0f * upper_len * forearm_len * std::cos(max_flex), 0.0f));
            const float triangle_len  = std::max(hs_len, reach_max);

            // Wrist-driven elbow twist.
            const auto hand_back  = hand_rot * glm::vec3{ -1.0f, 0.0f, 0.0f };
            const auto hand_side  = hand_rot * glm::vec3{ 0.0f, -1.0f, 0.0f };
            float      twist      = std::asin(std::clamp(hand_back.z, -0.999f, 0.999f));
            const float twist2    = -std::asin(std::clamp(hand_side.z, -0.599f, 0.999f));
            const float interp    = std::clamp((hand_back.z + 0.866f) * 1.155f, 0.45f, 0.8f);
            twist                 = twist + interp * (twist2 - twist);
            twist                 = arm.twist_prev + (twist - arm.twist_prev) * 0.25f;
            arm.twist_prev        = twist;

            const auto shoulder_bone = arm.clavicle >= 0 ? pose.GameWorld(arm.clavicle).t : upper_world.t;
            const float behind_d      = -(forward.x * shoulder_bone.x + forward.y * shoulder_bone.y) - 10.0f * kUnit;
            const float hand_behind   = -(hand_pos.x * forward.x + hand_pos.y * forward.y + behind_d);
            const float behind_amount = std::clamp(hand_behind / (40.0f * kUnit), 0.0f, 1.0f);

            const float plane_angle = neg_left * glm::radians(135.0f);
            const auto  plane_dir   = glm::vec3{ forward.x * std::cos(plane_angle) - forward.y * std::sin(plane_angle),
                forward.x * std::sin(plane_angle) + forward.y * std::cos(plane_angle), 0.0f };
            const float plane_d     = -(plane_dir.x * shoulder_bone.x + plane_dir.y * shoulder_bone.y) + 16.0f * kUnit;
            const float cross_amount = std::clamp((hand_pos.x * plane_dir.x + hand_pos.y * plane_dir.y + plane_d) / (20.0f * kUnit), 0.0f, 1.0f);

            const float chest_z     = (*g_active_bones).chest >= 0 ? pose.GameWorld((*g_active_bones).chest).t.z : shoulder_bone.z;
            const float lift_thresh = 60.0f * kUnit;
            const float lift_limit  = std::clamp((chest_z + lift_thresh - hand_pos.z) / lift_thresh, 0.0f, 1.0f);
            const float up_limit    = std::clamp((1.0f - lift_limit) * 1.4f, 0.0f, 1.0f);

            const float adjust_min = std::max(behind_amount, std::min(cross_amount, lift_limit));
            const float twist_min  = glm::radians(-85.0f) + glm::radians(50.0f) * adjust_min;
            float       twist_max  = glm::radians(55.0f) - std::max(glm::radians(90.0f) * cross_amount, glm::radians(70.0f) * up_limit);
            twist_max              = std::max(twist_max, twist_min + glm::radians(15.0f));
            const float twist_limit = twist_min + (twist + glm::half_pi<float>()) / glm::pi<float>() * (twist_max - twist_min);

            // Forward pitched about the right axis by the limit, then yawed outward and back.
            const auto bend_down = glm::mat3_cast(glm::angleAxis(twist_limit, sideways_r)) * forward;

            const float side_d       = -(sideways.x * shoulder_bone.x + sideways.y * shoulder_bone.y) - 8.0f * kUnit;
            float       across       = -(hand_pos.x * sideways.x + hand_pos.y * sideways.y + side_d) / (16.0f * kUnit);
            const float side_outward = glm::dot(hand_side, glm::normalize(sideways + forward * 0.5f));
            const float arm_twist    = std::clamp(side_outward - std::max(0.0f, across + 0.25f), 0.0f, 1.0f);
            if (across < 0.0f) {
                across *= 0.2f;
            }
            const float behind_head  = std::clamp(hand_behind / (15.0f * kUnit), 0.0f, 1.0f) * std::clamp(up_limit * 1.2f, 0.0f, 1.0f);
            const float twist_fwd    = std::max(across * glm::radians(90.0f), behind_head * glm::radians(120.0f));
            const float yaw          = -neg_left * (glm::radians(150.0f) - arm_twist * glm::radians(25.0f) - twist_fwd);
            const auto  elbow_dir    = tracked_bend ? *tracked_bend
                                                    : glm::vec3{ bend_down.x * std::cos(yaw) - bend_down.y * std::sin(yaw),
                                                          bend_down.x * std::sin(yaw) + bend_down.y * std::cos(yaw), bend_down.z };

            const auto x_dir = glm::normalize(hand_to_shoulder);
            auto       y_dir = elbow_dir - x_dir * glm::dot(elbow_dir, x_dir);
            if (glm::length(y_dir) < 1e-4f) {
                return 0.0f;
            }
            y_dir = glm::normalize(y_dir);

            float wrist_angle = std::acos(std::clamp((forearm_len * forearm_len + triangle_len * triangle_len - upper_len * upper_len) /
                                                         (2.0f * forearm_len * triangle_len), -1.0f, 1.0f));
            const auto elbow = hand_pos + x_dir * (std::cos(wrist_angle) * forearm_len) + y_dir * (std::sin(wrist_angle) * forearm_len);

            // Upper arm toward the elbow, then rolled about its length so the elbow hinge faces the forearm.
            auto upper_new = upper_world;
            upper_new.r    = RotationBetween(pose.GameWorld(arm.forearm).t - upper_world.t, elbow - upper_world.t) * upper_world.r;
            if (arm.has_bend) {
                const auto axis    = glm::normalize(elbow - upper_world.t);
                auto       current = upper_new.r * arm.bend_side;
                auto       wanted  = hand_pos - elbow;
                current -= glm::dot(current, axis) * axis;
                wanted -= glm::dot(wanted, axis) * axis;
                if (glm::length(current) > 1e-4f && glm::length(wanted) > 1e-4f) {
                    current          = glm::normalize(current);
                    wanted           = glm::normalize(wanted);
                    const float roll = std::atan2(glm::dot(glm::cross(current, wanted), axis), glm::dot(current, wanted));
                    upper_new.r      = glm::mat3_cast(glm::angleAxis(roll, axis)) * upper_new.r;
                }
            }
            pose.SetGameWorld(arm.biceps, upper_new);
            const float upper_scale = glm::length(elbow - upper_world.t) / orig_upper;
            if (std::abs(upper_scale - 1.0f) > 1e-3f) {
                pose.ScaleTranslation(arm.forearm, upper_scale);
            }

            // Forearm toward the hand.
            const auto forearm_now = pose.GameWorld(arm.forearm);
            auto       forearm_new = forearm_now;
            forearm_new.r          = RotationBetween(pose.GameWorld(arm.wrist).t - forearm_now.t, hand_pos - forearm_now.t) * forearm_now.r;
            pose.SetGameWorld(arm.forearm, forearm_new);
            const float forearm_scale = glm::length(hand_pos - forearm_now.t) / orig_forearm;
            if (std::abs(forearm_scale - 1.0f) > 1e-3f) {
                pose.ScaleTranslation(arm.wrist, forearm_scale);
            }

            // Forearm takes half of the wrist's added twist; the hand is then set exactly.
            {
                const auto forearm  = pose.GameWorld(arm.forearm);
                const auto axis_w   = glm::normalize(pose.GameWorld(arm.wrist).t - forearm.t);
                const auto relative = glm::transpose(forearm.r) * hand_rot * glm::transpose(FromNi(pose.local[arm.wrist]).r);
                const auto axis_l   = glm::transpose(forearm.r) * axis_w;
                const auto q        = glm::quat_cast(relative);
                const auto proj     = glm::dot(glm::vec3{ q.x, q.y, q.z }, axis_l) * axis_l;
                auto       twist_q  = glm::quat{ q.w, proj.x, proj.y, proj.z };
                if (glm::length(twist_q) > 1e-4f) {
                    twist_q          = glm::normalize(twist_q);
                    auto forearm_rot = forearm;
                    forearm_rot.r    = forearm.r * glm::mat3_cast(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, twist_q, 0.5f));
                    pose.SetGameWorld(arm.forearm, forearm_rot);
                }
            }
            auto hand_new = pose.GameWorld(arm.wrist);
            hand_new.r    = hand_rot;
            pose.SetGameWorld(arm.wrist, hand_new);
            return hs_len - reach_min;
        }

        // Two-bone leg IK as in FRIK's setSingleLeg: the foot is kept at its target, the knee bends toward `knee_dir`.
        void SolveLeg(const Pose& pose, const Bones::Leg& leg, const Xf& foot_target, glm::vec3 knee_dir)
        {
            const auto hip   = pose.GameWorld(leg.thigh);
            const auto knee  = pose.GameWorld(leg.calf);
            const auto foot  = pose.GameWorld(leg.foot);
            const float a    = glm::length(knee.t - hip.t);
            const float b    = glm::length(foot.t - knee.t);
            if (a < 1e-3f || b < 1e-3f) {
                return;
            }
            const auto  to_foot = foot_target.t - hip.t;
            const float c       = std::clamp(glm::length(to_foot), std::abs(a - b) + 1e-3f, a + b - 1e-3f);
            const auto  x_dir   = glm::normalize(to_foot);
            auto        y_dir   = knee_dir - glm::dot(knee_dir, x_dir) * x_dir;
            if (glm::length(y_dir) < 1e-4f) {
                return;
            }
            y_dir                = glm::normalize(y_dir);
            const float cos_a    = std::clamp((a * a + c * c - b * b) / (2.0f * a * c), -1.0f, 1.0f);
            const auto  knee_pos = hip.t + x_dir * (a * cos_a) + y_dir * (a * std::sqrt(1.0f - cos_a * cos_a));

            auto hip_new = hip;
            hip_new.r    = RotationBetween(knee.t - hip.t, knee_pos - hip.t) * hip.r;
            pose.SetGameWorld(leg.thigh, hip_new);

            const auto knee_now = pose.GameWorld(leg.calf);
            auto       knee_new = knee_now;
            knee_new.r          = RotationBetween(pose.GameWorld(leg.foot).t - knee_now.t, foot_target.t - knee_now.t) * knee_now.r;
            pose.SetGameWorld(leg.calf, knee_new);

            auto foot_new = pose.GameWorld(leg.foot);
            foot_new.r    = foot_target.r;
            pose.SetGameWorld(leg.foot, foot_new);
        }

        // Real crouch: when the head is below the game's eye height the pelvis lowers by the same amount and the
        // legs bend to keep the feet where the animation planted them.
        void ApplyCrouch(const Pose& pose)
        {
            auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
            const auto& l = (*g_active_bones).left_leg;
            const auto& r = (*g_active_bones).right_leg;
            if (!world_camera || !world_camera->parent || (*g_active_bones).pelvis < 0 || l.thigh < 0 || l.calf < 0 || l.foot < 0 || r.thigh < 0 ||
                r.calf < 0 || r.foot < 0) {
                return;
            }
            // The neck sits a fixed distance below the eyes, measured from the game's standing pose (camera highest
            // above the feet). Real and button crouches then share one rule.
            static float standing_eye{ 0.0f };
            static float neck_below_eye{ -1.0f };
            const float  game_eye   = world_camera->parent->world.translate.z - pose.root.t.z;
            const float  neck_z     = pose.GameWorld((*g_active_bones).neck).t.z;
            if (game_eye > standing_eye) {
                standing_eye = game_eye;
            }
            if (game_eye >= standing_eye - 0.02f) {
                const float measured = world_camera->parent->world.translate.z - neck_z;
                neck_below_eye       = neck_below_eye < 0.0f ? measured : neck_below_eye + (measured - neck_below_eye) * 0.05f;
            }
            if (neck_below_eye < 0.0f) {
                return;
            }
            const float drop = std::clamp(world_camera->world.translate.z - neck_below_eye - neck_z, -1.2f, 0.0f);
            if (drop > -0.005f) {
                return;
            }
            auto pelvis = pose.GameWorld((*g_active_bones).pelvis);
            pelvis.t.z += drop;
            pose.SetGameWorld((*g_active_bones).pelvis, pelvis);
        }

        // Procedural walking, after FRIK's walk(): when the body moves, the feet step in turn, each lifting along an
        // arc toward a target ahead of it while the other stays planted. FO4 units converted to metres (1/70).
        struct Walk
        {
            int       state{ 0 };     // 0 standing, 1 walking, 2 stopping, 3 re-target after deceleration
            int       stepping{ 0 };  // 1 right foot, 2 left foot
            glm::vec3 l_start{}, l_target{}, l_pos{}, r_start{}, r_target{}, r_pos{}, step_dir{};
            float     step_time_in_step{ 0.0f }, current_step_time{ 0.0f }, prev_speed{ 0.0f }, speed{ 0.0f };
            int       delay_frame{ 0 };
            bool      has_last{ false };
            glm::vec3 last_pos{};
            std::chrono::steady_clock::time_point last_time{};
        };
        Walk g_walk;

        void UpdateWalk(const glm::vec3& body_pos, const glm::vec3& l_anim, const glm::vec3& r_anim)
        {
            auto& w = g_walk;
            const auto now = std::chrono::steady_clock::now();
            const float dt = w.has_last ? std::chrono::duration<float>(now - w.last_time).count() : 0.0f;
            glm::vec3 cur{ body_pos.x, body_pos.y, 0.0f };
            glm::vec3 dir = w.has_last ? cur - glm::vec3{ w.last_pos.x, w.last_pos.y, 0.0f } : glm::vec3{ 0.0f };
            w.last_pos  = cur;
            w.last_time = now;
            w.has_last  = true;
            if (dt <= 1e-4f || dt > 0.25f) {
                w.state = 0;
                w.l_pos = l_anim;
                w.r_pos = r_anim;
                return;
            }

            float speed = std::clamp(glm::length(dir) / dt, 0.0f, 5.0f);
            if (w.prev_speed > 20.0f * kUnit) {
                speed = (speed + w.prev_speed) / 2.0f;
            }
            const float step_time = std::clamp(std::cos(speed / (140.0f * kUnit)), 0.28f, 0.50f);
            dir = glm::length(dir) > 1e-6f ? glm::normalize(dir) : glm::vec3{ 0.0f };
            if (speed - w.prev_speed < -20.0f * kUnit) {
                w.state = 3;
            }
            w.prev_speed = speed;
            w.speed      = speed;

            switch (w.state) {
            case 0:
                if (speed >= 35.0f * kUnit) {
                    w.state             = 1;
                    w.stepping          = (std::rand() % 2) + 1;
                    w.step_dir          = dir;
                    w.step_time_in_step = step_time;
                    w.delay_frame       = 2;
                    w.l_start = w.l_target = w.l_pos = l_anim;
                    w.r_start = w.r_target = w.r_pos = r_anim;
                    if (w.stepping == 1) {
                        w.r_target = r_anim + w.step_dir * (speed * step_time * 1.5f);
                    } else {
                        w.l_target = l_anim + w.step_dir * (speed * step_time * 1.5f);
                    }
                    w.current_step_time = step_time / 2.0f;
                    break;
                }
                w.current_step_time = 0.0f;
                w.stepping          = 0;
                break;
            case 1:
                if (speed < 20.0f * kUnit) {
                    w.state             = 2;
                    w.current_step_time = 0.0f;
                }
                break;
            case 2:
                if (speed >= 20.0f * kUnit) {
                    w.state             = 1;
                    w.current_step_time = 0.0f;
                }
                break;
            case 3:
                w.step_dir = dir;
                if (w.stepping == 1) {
                    w.r_target = r_anim + w.step_dir * (speed * step_time * 0.1f);
                } else {
                    w.l_target = l_anim + w.step_dir * (speed * step_time * 0.1f);
                }
                w.state = 1;
                break;
            default:
                w.state = 0;
                break;
            }

            if (w.state == 0 || w.state == 2) {
                w.l_pos = l_anim;
                w.r_pos = r_anim;
                w.state = 0;
                return;
            }

            const float dot   = glm::dot(dir, w.step_dir);
            const float scale = std::min(speed * step_time * 1.5f, 140.0f * kUnit);
            const auto  dir_offset = (dir - w.step_dir) * scale;
            w.current_step_time += dt;
            const float interp = std::clamp(w.current_step_time / std::max(w.step_time_in_step, 1e-3f), 0.0f, 1.0f);

            auto step = [&](glm::vec3& target, glm::vec3& start, glm::vec3& pos, float ground) {
                if (dot < 0.9f) {
                    if (!w.delay_frame) {
                        target += dir_offset;
                        w.step_dir    = dir;
                        w.delay_frame = 2;
                    } else {
                        --w.delay_frame;
                    }
                } else {
                    w.delay_frame = w.delay_frame == 2 ? w.delay_frame : w.delay_frame + 1;
                }
                target.z = ground;
                start.z  = ground;
                pos      = start + (target - start) * interp;
                const float amount = std::clamp(glm::length(target - start) / (150.0f * kUnit), 0.0f, 1.0f);
                const float height = std::max(amount * 9.0f, 1.0f) * kUnit;
                pos.z += std::sin(interp * glm::pi<float>()) * height;
            };
            if (w.stepping == 1) {
                step(w.r_target, w.r_start, w.r_pos, r_anim.z);
                w.l_pos.z = l_anim.z;
            } else {
                step(w.l_target, w.l_start, w.l_pos, l_anim.z);
                w.r_pos.z = r_anim.z;
            }

            if (w.current_step_time > step_time) {
                w.current_step_time = 0.0f;
                w.step_dir          = dir;
                w.step_time_in_step = step_time;
                if (w.stepping == 1) {
                    w.stepping = 2;
                    w.l_target = l_anim + w.step_dir * scale;
                    w.l_start  = w.l_pos;
                } else {
                    w.stepping = 1;
                    w.r_target = r_anim + w.step_dir * scale;
                    w.r_start  = w.r_pos;
                }
            }
        }

        // Legs every frame: feet at their animated place when standing, stepping when the body moves.
        void ApplyLegs(const Pose& pose, const Xf& left_anim, const Xf& right_anim)
        {
            const auto& l = (*g_active_bones).left_leg;
            const auto& r = (*g_active_bones).right_leg;
            if (l.thigh < 0 || l.calf < 0 || l.foot < 0 || r.thigh < 0 || r.calf < 0 || r.foot < 0) {
                return;
            }
            UpdateWalk(pose.root.t, left_anim.t, right_anim.t);
            auto left  = left_anim;
            auto right = right_anim;
            auto forward = pose.root.r[1];
            forward.z    = 0.0f;
            forward      = glm::length(forward) > 1e-3f ? glm::normalize(forward) : glm::vec3{ 0.0f, 1.0f, 0.0f };
            glm::vec3 knee_dir[2] = { forward, forward };
            if (GameFlow::gStore.internalSettings.walkingLegs && g_walk.state == 1) {
                left.t  = g_walk.l_pos;
                right.t = g_walk.r_pos;
            } else if (g_tracked_body.full_body && !PlayerSeated() && (*g_active_bones).pelvis >= 0) {
                // Standing in place: feet placed where the tracked ankles are relative to the tracked hips, on the
                // animated ground height; knees bend toward the tracked knees.
                const auto hips = TrackedJoint(joint::kHips);
                const auto pelvis = pose.GameWorld((*g_active_bones).pelvis).t;
                Xf* feet[2] = { &left, &right };
                bool placed = false;
                for (int side = 0; side < 2; ++side) {
                    const auto ankle = TrackedJoint(joint::kAnkle[side]);
                    if (!hips || !ankle) {
                        continue;
                    }
                    if (const auto offset = StageDirection(*ankle - *hips)) {
                        const float distance = glm::length(*ankle - *hips) * tracking::TrackingScale();
                        auto        target   = pelvis + *offset * distance;
                        target.z             = feet[side]->t.z;
                        feet[side]->t        = target;
                        placed               = true;
                    }
                    if (const auto bend = TrackedBend(joint::kUpperLeg[side], joint::kLowerLeg[side], joint::kAnkle[side])) {
                        knee_dir[side] = *bend;
                    }
                }
                g_track_diag.legs = placed;
            }
            SolveLeg(pose, l, left, knee_dir[0]);
            SolveLeg(pose, r, right, knee_dir[1]);
        }

        // Body yaw toward the hands, after FRIK's getNeckYaw: the torso turns 0.7 of the angle between the head's
        // facing and the direction between the hands (clamped to 50 degrees), less when a hand is above the head.
        float g_facing_yaw{ 0.0f };

        float FacingYaw(const Pose& pose, const glm::vec3& eye)
        {
            const auto left  = HandWorld(tracking::GripPose(true));
            const auto right = HandWorld(tracking::GripPose(false));
            auto       forward = pose.root.r[1];
            forward.z          = 0.0f;
            float      target  = 0.0f;
            if (left && right && glm::length(forward) > 1e-3f) {
                forward             = glm::normalize(forward);
                const auto to_left  = left->position - eye;
                const auto to_right = right->position - eye;
                if (glm::length(to_left) > 0.14f && glm::length(to_right) > 0.14f) {
                    float weight = 1.0f;
                    if (to_left.z > 0.0f) weight = std::max(weight - 3.5f * to_left.z, 0.0f);
                    if (to_right.z > 0.0f) weight = std::max(weight - 3.5f * to_right.z, 0.0f);
                    auto sum = to_left + to_right;
                    sum.z    = 0.0f;
                    if (glm::length(sum) > 1e-3f) {
                        sum               = glm::normalize(sum);
                        const float angle = std::atan2(forward.x * sum.y - forward.y * sum.x, glm::dot(forward, sum));
                        target            = std::clamp(angle * weight, glm::radians(-50.0f), glm::radians(50.0f)) * 0.7f;
                    }
                }
            }
            g_facing_yaw += (target - g_facing_yaw) * 0.25f;
            return g_facing_yaw;
        }

        struct NativeDiag
        {
            bool      native{ false };
            glm::vec3 hand_in_weapon{};
        };
        NativeDiag g_native_diag;

        // Where the drawn hand and weapon ended up against where they were placed (read from the scene next frame).
        struct HandCheck
        {
            bool      placed{ false };
            Xf        wrist_target{};
            Xf        weapon_target{};
            float     wrist_error{ 0.0f };
            float     wrist_angle{ 0.0f };
            float     weapon_error{ 0.0f };
            glm::vec3 drawn_hand_in_weapon{};
            glm::vec3 model_offset{};
            std::string model_name;
        };
        HandCheck g_hand_check;

        // Support hand on the held weapon this frame.
        struct Support
        {
            bool held{ false };
            Xf   target{};
        };
        Support g_support;

        // Body yaw from the tracked hips: their forward (from the hip line, or the shoulder line without legs).
        std::optional<float> TrackedFacingYaw(const Pose& pose)
        {
            auto side = g_tracked_body.full_body ? TrackedDirection(joint::kUpperLeg[0], joint::kUpperLeg[1]) : std::nullopt;
            if (!side) {
                side = TrackedDirection(joint::kArmUpper[0], joint::kArmUpper[1]);
            }
            auto forward = pose.root.r[1];
            forward.z    = 0.0f;
            if (!side || glm::length(forward) < 1e-3f) {
                return std::nullopt;
            }
            auto tracked = glm::cross(glm::vec3{ 0.0f, 0.0f, 1.0f }, *side);
            tracked.z    = 0.0f;
            if (glm::length(tracked) < 1e-3f) {
                return std::nullopt;
            }
            forward             = glm::normalize(forward);
            tracked             = glm::normalize(tracked);
            const float target  = std::atan2(forward.x * tracked.y - forward.y * tracked.x, glm::dot(forward, tracked));
            g_facing_yaw += (target - g_facing_yaw) * 0.25f;
            return g_facing_yaw;
        }

        // Horizontal offset of the head from the neck that the spine absorbs this frame (the rest moves the body).
        glm::vec3 g_lean{ 0.0f };
        float     g_lean_applied{ 0.0f };

        // Spine lean: the bones between pelvis and neck share the rotation that brings the neck to its target, so
        // the hips stay planted (FRIK's posture bends the spine toward the head the same way).
        void ApplyLean(const Pose& pose)
        {
            g_lean_applied = 0.0f;
            if (!GameFlow::gStore.internalSettings.bodyLean || (*g_active_bones).spine.empty() || (*g_active_bones).neck < 0 || glm::length(g_lean) < 0.005f) {
                return;
            }
            const auto target = pose.GameWorld((*g_active_bones).neck).t + g_lean;
            const auto count  = static_cast<int>((*g_active_bones).spine.size());
            for (int k = 0; k < count; ++k) {
                const int  bone   = (*g_active_bones).spine[k];
                const auto world  = pose.GameWorld(bone);
                const auto neck   = pose.GameWorld((*g_active_bones).neck).t;
                const auto turn   = glm::quat_cast(RotationBetween(neck - world.t, target - world.t));
                const auto share  = glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, turn, 1.0f / static_cast<float>(count - k));
                auto       bent   = world;
                bent.r            = glm::mat3_cast(share) * world.r;
                pose.SetGameWorld(bone, bent);
            }
            g_lean_applied = glm::length(g_lean);
        }

        // Torso from body tracking: the spine bones share the rotation that turns the body's hips-to-neck axis and
        // shoulder line onto the tracked ones.
        bool ApplyTrackedTorso(const Pose& pose)
        {
            if ((*g_active_bones).spine.empty() || (*g_active_bones).neck < 0 || (*g_active_bones).pelvis < 0 || (*g_active_bones).right.biceps < 0 || (*g_active_bones).left.biceps < 0) {
                return false;
            }
            const auto up_t   = TrackedDirection(joint::kHips, joint::kNeck);
            const auto side_t = TrackedDirection(joint::kArmUpper[0], joint::kArmUpper[1]);
            if (!up_t || !side_t) {
                return false;
            }
            const auto up_g   = glm::normalize(pose.GameWorld((*g_active_bones).neck).t - pose.GameWorld((*g_active_bones).pelvis).t);
            const auto side_g = glm::normalize(pose.GameWorld((*g_active_bones).right.biceps).t - pose.GameWorld((*g_active_bones).left.biceps).t);
            auto frame = [](const glm::vec3& up, const glm::vec3& side) { return FrameOf(glm::cross(up, side), up); };
            const auto turn  = glm::quat_cast(frame(*up_t, *side_t) * glm::transpose(frame(up_g, side_g)));
            const auto count = static_cast<int>((*g_active_bones).spine.size());
            const auto share = glm::mat3_cast(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, turn, 1.0f / static_cast<float>(count)));
            for (const int bone : (*g_active_bones).spine) {
                auto bent = pose.GameWorld(bone);
                bent.r    = share * bent.r;
                pose.SetGameWorld(bone, bent);
            }
            g_track_diag.torso = true;
            return true;
        }

        // Finger poses. The side the fingers curl toward (the palm) is measured from the animated hand.
        struct FingerPalm
        {
            bool      valid{ false };
            glm::vec3 palm{};  // wrist space
        };
        FingerPalm  g_finger_palm_body[2];
        FingerPalm  g_finger_palm_first[2];
        FingerPalm* g_finger_palm{ g_finger_palm_body };

        void MeasurePalmSide(const Pose& pose, int side, const HandShape& shape, int wrist)
        {
            auto& out = g_finger_palm[side];
            if (out.valid || !shape.valid || wrist < 0) {
                return;
            }
            const auto w      = pose.GameWorld(wrist);
            const auto normal = glm::normalize(glm::cross(shape.forward, shape.up));  // wrist space, one of the palm/back sides
            float      score  = 0.0f;
            for (int f = 1; f < 5; ++f) {
                const auto& chain = (*g_active_bones).fingers[side][f];
                if (chain[0] < 0 || chain[2] < 0) {
                    continue;
                }
                const auto bend = glm::transpose(w.r) * (pose.GameWorld(chain[2]).t - pose.GameWorld(chain[0]).t) / w.s;
                score += glm::dot(bend, normal);
            }
            if (std::abs(score) < 0.005f) {
                return;  // fingers straight in this pose; measure later
            }
            out.palm  = score > 0.0f ? normal : -normal;
            out.valid = true;
            spdlog::info("[BodyIK] {} palm side ({:.2f},{:.2f},{:.2f}) in wrist space", side == 0 ? "left" : "right", out.palm.x, out.palm.y, out.palm.z);
        }

        // Bends a finger chain toward the palm: base and middle joints by the given angles.
        void CurlFinger(const Pose& pose, const std::array<int, 3>& chain, int wrist, const glm::vec3& palm_l, float base_deg, float mid_deg)
        {
            if (chain[0] < 0 || chain[1] < 0 || chain[2] < 0) {
                return;
            }
            const auto wrist_w = pose.GameWorld(wrist);
            const auto palm    = glm::normalize(wrist_w.r * palm_l);
            glm::vec3  parent_dir{ 0.0f };
            {
                const auto base = pose.GameWorld(chain[0]);
                parent_dir      = glm::normalize(base.t - wrist_w.t);
            }
            const float angles[2] = { glm::radians(base_deg), glm::radians(mid_deg) };
            for (int j = 0; j < 2; ++j) {
                const auto joint = pose.GameWorld(chain[j]);
                const auto child = pose.GameWorld(chain[j + 1]).t;
                const auto dir   = child - joint.t;
                if (glm::length(dir) < 1e-4f) {
                    return;
                }
                auto axis = glm::cross(parent_dir, palm);
                if (glm::length(axis) < 1e-4f) {
                    return;
                }
                axis              = glm::normalize(axis);
                const auto wanted = glm::mat3_cast(glm::angleAxis(angles[j], axis)) * parent_dir;
                auto       posed  = joint;
                posed.r           = RotationBetween(dir, wanted) * joint.r;
                pose.SetGameWorld(chain[j], posed);
                parent_dir = glm::normalize(wanted);
            }
        }

        struct FingerDiag
        {
            float trigger{ 0.0f };
            float grip{ 0.0f };
            bool  thumb{ false };
            int   copied{ 0 };
        };
        FingerDiag g_finger_diag[2];

        struct FingerSprings
        {
            float                                 index{ 0.0f };
            float                                 grip{ 0.0f };
            float                                 thumb{ 0.0f };
            bool                                  has_time{ false };
            std::chrono::steady_clock::time_point time{};
        };
        FingerSprings g_finger_springs[2];


        // Measures the hand frame from the animated pose (before any write this frame).
        void MeasureElbow(const Pose& pose, Arm& arm, const char* label)
        {
            if (arm.has_bend || arm.biceps < 0 || arm.forearm < 0 || arm.wrist < 0) {
                return;
            }
            const auto upper   = pose.GameWorld(arm.biceps);
            const auto forearm = pose.GameWorld(arm.forearm).t;
            const auto wrist   = pose.GameWorld(arm.wrist).t;
            const auto axis    = glm::normalize(forearm - upper.t);
            auto       side    = wrist - forearm;
            side -= glm::dot(side, axis) * axis;
            if (glm::length(side) < 0.03f) {
                return;  // elbow too straight in this pose to tell the hinge
            }
            arm.bend_side = glm::normalize(glm::transpose(upper.r) * glm::normalize(side));
            arm.has_bend  = true;
            spdlog::info("[BodyIK] {} elbow bends toward ({:.2f},{:.2f},{:.2f}) in upper-arm space", label, arm.bend_side.x, arm.bend_side.y,
                arm.bend_side.z);
        }

        void MeasureHand(const Pose& pose, Arm& arm, const char* label)
        {
            if (arm.shape.valid || arm.wrist < 0 || arm.thumb < 0 || arm.fingers.empty()) {
                return;
            }
            const auto wrist = pose.GameWorld(arm.wrist);
            glm::vec3  bases{ 0.0f };
            for (const int finger : arm.fingers) {
                bases += pose.GameWorld(finger).t;
            }
            bases /= static_cast<float>(arm.fingers.size());
            const auto to_bases = bases - wrist.t;
            const auto to_thumb = pose.GameWorld(arm.thumb).t - wrist.t;
            if (glm::length(to_bases) < 0.02f) {
                return;
            }
            const auto forward = glm::normalize(to_bases);
            auto       up      = to_thumb - glm::dot(to_thumb, forward) * forward;
            if (glm::length(up) < 0.005f) {
                return;
            }
            up               = glm::normalize(up);
            const auto to_l  = glm::transpose(wrist.r);
            arm.shape.forward = glm::normalize(to_l * forward);
            arm.shape.up      = glm::normalize(to_l * up);
            arm.shape.palm    = 0.5f * glm::length(to_bases);
            arm.shape.valid   = true;
            spdlog::info("[BodyIK] {} hand measured: forward ({:.2f},{:.2f},{:.2f}) up ({:.2f},{:.2f},{:.2f}) in wrist space, palm {:.3f} m", label,
                arm.shape.forward.x, arm.shape.forward.y, arm.shape.forward.z, arm.shape.up.x, arm.shape.up.y, arm.shape.up.z, arm.shape.palm);
        }

        // Wrist world transform that puts the hand's palm centre on the controller's grip point, fingers along
        // the grip's -Z (index finger direction) and the thumb side along +Y.
        Xf WristTarget(const Pose& pose, const Arm& arm, const HandTarget& aim, const HandTarget& grip)
        {
            Xf target = pose.GameWorld(arm.wrist);
            target.r  = FrameOf(aim.forward, aim.up) * glm::transpose(FrameOf(arm.shape.forward, arm.shape.up));
            target.t  = grip.position - aim.forward * arm.shape.palm;
            return target;
        }

        struct Diagnostics
        {
            float right_gap{ 0.0f };
            float left_gap{ 0.0f };
            bool  weapon_aligned{ false };
            float support_distance{ -1.0f };
            bool  support_held{ false };
        };
        Diagnostics g_diag;

        // Shots leave along the muzzle's local +Y (ComputeLaunchOrigin 0x1b52e8a). The muzzle's transform within the
        // weapon is read from the engine's final worlds after each update and used to aim the weapon exactly.
        struct Barrel
        {
            RE::NiAVObject* weapon{ nullptr };
            RE::NiAVObject* muzzle{ nullptr };
            bool            has_relation{ false };
            glm::vec3       shot_in_weapon{ 0.0f, 1.0f, 0.0f };
            bool            placed{ false };
            Xf              placed_weapon{};
            glm::vec3       aim_forward{ 0.0f, 1.0f, 0.0f };
            float           error_deg{ 0.0f };
            float           placement_error{ 0.0f };
        };
        Barrel g_barrel;

        // How the game holds the weapon in first person: its barrel at the crosshair and level with the view. The
        // view's forward and up in the weapon's space give the weapon's basis (ROCK derives its controller-to-weapon
        // basis from the game's own hold the same way). Sampled when the player fires; a slow estimate before that.
        // The camera parent frame is havok-space: +Y forward, +Z up.
        struct WeaponBasis
        {
            RE::NiAVObject* weapon{ nullptr };
            bool            has{ false };
            bool            fired{ false };
            glm::vec3       forward{ 0.0f, 1.0f, 0.0f };
            glm::vec3       up{ 0.0f, 0.0f, 1.0f };
        };
        WeaponBasis       g_basis;
        std::atomic<bool> g_fire_sample{ false };

        // The first-person hand's anatomical frame (fingers forward, thumb up, palm centre) in its wrist's space, so a
        // grip held by that hand can be transferred onto the body's hand whatever the two skeletons' bone axes.
        struct FirstPersonHand
        {
            RE::NiAVObject* wrist{ nullptr };
            HandShape       shape;
        };
        FirstPersonHand g_first_hands[2];  // left, right

        void MeasureFirstPersonHand(RE::NiAVObject* wrist, bool left)
        {
            auto& g_first_hand = g_first_hands[left ? 0 : 1];
            if (!wrist || (g_first_hand.wrist == wrist && g_first_hand.shape.valid)) {
                return;
            }
            g_first_hand = {};
            g_first_hand.wrist = wrist;
            RE::NiAVObject* children[32]{};
            const auto      count = ReadChildren(wrist, children, 32);
            const auto      w     = FromNi(wrist->world);
            glm::vec3       bases{ 0.0f };
            int             fingers = 0;
            std::optional<glm::vec3> thumb;
            for (std::uint16_t i = 0; i < count; ++i) {
                if (!children[i]) {
                    continue;
                }
                const std::string_view name{ children[i]->name.c_str() };
                const auto pos = ToVec(children[i]->world.translate);
                if (Contains(name, "Thumb")) {
                    thumb = pos;
                } else if (Contains(name, "Index") || Contains(name, "Middle") || Contains(name, "Ring") || Contains(name, "Pinky") || Contains(name, "Little")) {
                    bases += pos;
                    ++fingers;
                }
            }
            if (!thumb || fingers == 0) {
                spdlog::info("[BodyIK] 1P hand: fingers {} thumb {}", fingers, thumb.has_value());
                return;
            }
            bases /= static_cast<float>(fingers);
            const auto to_bases = bases - w.t;
            const auto to_thumb = *thumb - w.t;
            if (glm::length(to_bases) < 0.02f * w.s) {
                return;
            }
            const auto forward = glm::normalize(to_bases);
            auto       up      = to_thumb - glm::dot(to_thumb, forward) * forward;
            if (glm::length(up) < 1e-4f) {
                return;
            }
            up = glm::normalize(up);
            const auto to_l             = glm::transpose(w.r);
            g_first_hand.shape.forward  = glm::normalize(to_l * forward);
            g_first_hand.shape.up       = glm::normalize(to_l * up);
            g_first_hand.shape.palm     = 0.5f * glm::length(to_bases) / w.s;
            g_first_hand.shape.valid    = true;
            spdlog::info("[BodyIK] 1P {} hand measured: forward ({:.2f},{:.2f},{:.2f}) up ({:.2f},{:.2f},{:.2f}) palm {:.3f}", left ? "left" : "right", g_first_hand.shape.forward.x,
                g_first_hand.shape.forward.y, g_first_hand.shape.forward.z, g_first_hand.shape.up.x, g_first_hand.shape.up.y, g_first_hand.shape.up.z,
                g_first_hand.shape.palm);
        }

        // The body's wrist transform equivalent to the first-person wrist: same anatomical frame, same palm centre.
        Xf BodyWristFromFirstPerson(const Xf& first_wrist, const HandShape& body, bool left)
        {
            const auto& f    = g_first_hands[left ? 0 : 1].shape;
            Xf          out  = first_wrist;
            out.r            = first_wrist.r * FrameOf(f.forward, f.up) * glm::transpose(FrameOf(body.forward, body.up));
            const auto palm  = first_wrist.t + first_wrist.r * (f.forward * f.palm) * first_wrist.s;
            out.t            = palm - out.r * (body.forward * body.palm) * out.s;
            return out;
        }

        void SampleWeaponBasis(RE::NiAVObject* first_weapon)
        {
            auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
            if (!first_weapon || !world_camera || !world_camera->parent) {
                return;
            }
            if (g_basis.weapon != (*g_active_bones).weapon_node) {
                g_basis = {};
                g_basis.weapon = (*g_active_bones).weapon_node;
            }
            const auto to_weapon = glm::transpose(FromNi(first_weapon->world).r);
            const auto view_r    = FromNi(world_camera->parent->world).r;
            const auto forward   = glm::normalize(to_weapon * view_r[1]);
            const auto up        = glm::normalize(to_weapon * view_r[2]);
            const bool fired     = g_fire_sample.exchange(false);
            if (fired || !g_basis.has) {
                g_basis.forward = forward;
                g_basis.up      = up;
            } else if (!g_basis.fired) {
                g_basis.forward = glm::normalize(glm::mix(g_basis.forward, forward, 0.02f));
                g_basis.up      = glm::normalize(glm::mix(g_basis.up, up, 0.02f));
            }
            if (fired && !g_basis.fired) {
                spdlog::info("[BodyIK] firing grip sampled: barrel ({:.2f},{:.2f},{:.2f}) up ({:.2f},{:.2f},{:.2f}) in weapon space", forward.x,
                    forward.y, forward.z, up.x, up.y, up.z);
            }
            g_basis.fired = g_basis.fired || fired;
            g_basis.has   = true;

            // Check of the view-axis assumption: with hand aiming the view forward is the aim ray.
            static std::chrono::steady_clock::time_point last_log{};
            if (const auto now = std::chrono::steady_clock::now(); now - last_log > std::chrono::seconds(2)) {
                last_log = now;
                spdlog::info("[BodyIK] view forward vs aim ray: {:.1f} deg | basis from {}", glm::degrees(std::acos(std::clamp(
                    glm::dot(glm::normalize(view_r[1]), g_barrel.aim_forward), -1.0f, 1.0f))), g_basis.fired ? "firing grip" : "estimate");
            }
        }

        void MeasureBarrel()
        {
            auto muzzle = g_body_muzzle.load();
            auto weapon = (*g_active_bones).weapon_node;
            if (!muzzle || !weapon) {
                g_barrel.has_relation = false;
                g_barrel.placed       = false;
                return;
            }
            const auto weapon_world = FromNi(weapon->world);
            const auto shot_world   = glm::normalize(FromNi(muzzle->world).r[1]);
            if (g_hand_check.placed && (*g_active_bones).r_wrist_node) {
                const auto wrist_world            = FromNi((*g_active_bones).r_wrist_node->world);
                g_hand_check.wrist_error          = glm::length(wrist_world.t - g_hand_check.wrist_target.t);
                const auto relative               = glm::transpose(g_hand_check.wrist_target.r) * wrist_world.r;
                g_hand_check.wrist_angle          = glm::degrees(std::acos(std::clamp((relative[0][0] + relative[1][1] + relative[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)));
                g_hand_check.weapon_error         = glm::length(weapon_world.t - g_hand_check.weapon_target.t);
                g_hand_check.drawn_hand_in_weapon = Compose(Inverse(weapon_world), wrist_world).t;
                // The weapon model hangs under the weapon bone; its own offset there moves the grip.
                RE::NiAVObject* children[8]{};
                const auto      count = ReadChildren(weapon, children, 8);
                for (std::uint16_t i = 0; i < count; ++i) {
                    if (children[i] && children[i] != muzzle && std::strcmp(children[i]->name.c_str(), "R_HandIk") && std::strcmp(children[i]->name.c_str(), "L_HandIk")) {
                        g_hand_check.model_offset = Compose(Inverse(weapon_world), FromNi(children[i]->world)).t;
                        g_hand_check.model_name   = children[i]->name.c_str();
                        break;
                    }
                }
                g_hand_check.placed = false;
            }
            if (g_barrel.placed) {
                g_barrel.error_deg       = glm::degrees(std::acos(std::clamp(glm::dot(shot_world, g_barrel.aim_forward), -1.0f, 1.0f)));
                g_barrel.placement_error = glm::length(weapon_world.t - g_barrel.placed_weapon.t);
            }
            g_barrel.weapon         = weapon;
            g_barrel.muzzle         = muzzle;
            g_barrel.shot_in_weapon = glm::normalize(glm::transpose(weapon_world.r) * shot_world);
            g_barrel.has_relation   = true;
            g_barrel.placed         = false;
        }

        // Fingers from the controller: index from the trigger (extended off the trigger), the other three from the
        // grip, the thumb from touch. A hand holding the weapon keeps the first-person grip's finger pose and only
        // the index finger follows the trigger.
        bool ApplyTrackedFingers(const Pose& pose, int side, int wrist);

        void ApplyFingers(const Pose& pose, int side, int wrist, RE::NiAVObject* first_wrist)
        {
            if (!GameFlow::gStore.internalSettings.fingerPoses || wrist < 0) {
                return;
            }
            static auto vr    = VR::get();
            const auto  input = vr->get_finger_state(side == 0);
            auto&       diag  = g_finger_diag[side];
            diag              = { input.trigger, input.grip, input.thumb_touch, 0 };

            if (ApplyTrackedFingers(pose, side, wrist)) {
                return;
            }

            // Each finger follows its input through a fast spring and an ease-in/out curve.
            auto&       state = g_finger_springs[side];
            const auto  now   = std::chrono::steady_clock::now();
            const float dt    = state.has_time ? std::clamp(std::chrono::duration<float>(now - state.time).count(), 0.0f, 0.1f) : 0.0f;
            state.time        = now;
            state.has_time    = true;
            const float k     = 1.0f - std::exp(-dt / 0.06f);
            auto spring = [&](float& value, float target) {
                value += (target - value) * k;
                return value * value * (3.0f - 2.0f * value);
            };
            const float index = spring(state.index, input.trigger_touch ? 0.3f + 0.7f * input.trigger : 0.0f);
            const float grip  = spring(state.grip, input.grip);
            const float thumb = spring(state.thumb, input.thumb_touch ? 1.0f : 0.0f);

            const auto& chains = (*g_active_bones).fingers[side];
            if (first_wrist) {
                // A hand holding the weapon takes the first-person grip's finger directions, transferred through the
                // hands' anatomical frames so the rigs' bone axes do not matter.
                const auto& first_shape = g_first_hands[side].shape;
                const auto& body_shape  = side == 0 ? (*g_active_bones).left.shape : (*g_active_bones).right.shape;
                if (first_shape.valid && body_shape.valid) {
                    static constexpr const char* kFingerNames[5][3] = { { "thumb", "Thumb1", "Thumb2" }, { "Index", "Index1", "Index2" },
                        { "Middle", "Middle1", "Middle2" }, { "Ring", "Ring1", "Ring2" }, { "Pinky", "Pinky1", "Pinky2" } };
                    const auto first_frame = FromNi(first_wrist->world).r * FrameOf(first_shape.forward, first_shape.up);
                    const auto body_frame  = pose.GameWorld(wrist).r * FrameOf(body_shape.forward, body_shape.up);
                    const auto to_body     = body_frame * glm::transpose(first_frame);
                    const std::string prefix = side == 0 ? "L_" : "R_";
                    for (int f = 0; f < 5; ++f) {
                        if (f == 1) {
                            continue;  // the index finger follows the trigger
                        }
                        for (int j = 0; j < 2; ++j) {
                            const int bone  = chains[f][j];
                            const int child = chains[f][j + 1];
                            if (bone < 0 || child < 0) {
                                continue;
                            }
                            auto first_joint = FindDescendant(first_wrist, prefix + kFingerNames[f][j]);
                            auto first_child = first_joint ? FindDescendant(first_joint, prefix + kFingerNames[f][j + 1]) : nullptr;
                            if (!first_child) {
                                continue;
                            }
                            const auto wanted = to_body * (ToVec(first_child->world.translate) - ToVec(first_joint->world.translate));
                            const auto joint  = pose.GameWorld(bone);
                            const auto dir    = pose.GameWorld(child).t - joint.t;
                            if (glm::length(wanted) < 1e-5f || glm::length(dir) < 1e-5f) {
                                continue;
                            }
                            auto posed = joint;
                            posed.r    = RotationBetween(dir, wanted) * joint.r;
                            pose.SetGameWorld(bone, posed);
                            ++diag.copied;
                        }
                    }
                }
            }

            if (!g_finger_palm[side].valid) {
                return;
            }
            const auto& palm = g_finger_palm[side].palm;
            CurlFinger(pose, chains[1], wrist, palm, 10.0f + 50.0f * index, 10.0f + 70.0f * index);
            if (!first_wrist) {
                CurlFinger(pose, chains[2], wrist, palm, 10.0f + 65.0f * grip, 10.0f + 85.0f * grip);
                CurlFinger(pose, chains[3], wrist, palm, 10.0f + 65.0f * grip, 10.0f + 85.0f * grip);
                CurlFinger(pose, chains[4], wrist, palm, 10.0f + 65.0f * grip, 10.0f + 85.0f * grip);
                CurlFinger(pose, chains[0], wrist, palm, 5.0f + 20.0f * thumb, 5.0f + 25.0f * thumb);
            }
        }

        // Fingers from hand tracking: each finger joint points where the tracked joint does, through the hands'
        // anatomical frames (tracked: wrist to middle knuckle forward, thumb side up).
        bool ApplyTrackedFingers(const Pose& pose, int side, int wrist)
        {
            const auto& hand = g_tracked_hands[side];
            const auto& body = side == 0 ? (*g_active_bones).left.shape : (*g_active_bones).right.shape;
            auto at = [&](int i) { return glm::vec3{ hand.joints[i].position }; };
            // Index bend of the tracked hand (intermediate joint), for comparing a controller-derived hand with the trigger.
            g_track_diag.index_bend[side] = -1.0f;
            if (hand.active && hand.joints[7].position_valid && hand.joints[8].position_valid && hand.joints[9].position_valid) {
                const auto a = glm::normalize(at(8) - at(7));
                const auto b = glm::normalize(at(9) - at(8));
                g_track_diag.index_bend[side] = glm::degrees(std::acos(std::clamp(glm::dot(a, b), -1.0f, 1.0f)));
            }
            // Only an optically tracked hand adds information; with a controller held, the controller's own touch and
            // analog inputs drive the fingers and a held weapon keeps its grip.
            if (!GameFlow::gStore.internalSettings.handTracking || !hand.active || hand.data_source != 1 || !body.valid) {
                return false;
            }
            for (const int i : { 1, 3, 12 }) {
                if (!hand.joints[i].position_valid) {
                    return false;
                }
            }
            const auto fwd = at(12) - at(1);
            auto       up  = at(3) - at(1);
            if (glm::length(fwd) < 1e-3f) {
                return false;
            }
            up -= glm::dot(up, glm::normalize(fwd)) * glm::normalize(fwd);
            if (glm::length(up) < 1e-4f) {
                return false;
            }
            const auto room = tracking::RoomRotation();
            if (!room) {
                return false;
            }
            // Tracked frame in game axes, then onto the body's hand frame.
            const auto tracked_frame = FrameOf(*room * tracking::ToHavokVector(fwd), *room * tracking::ToHavokVector(up));
            const auto body_frame    = pose.GameWorld(wrist).r * FrameOf(body.forward, body.up);
            const auto to_body       = body_frame * glm::transpose(tracked_frame);

            // XrHandJointEXT: thumb metacarpal 2..tip 5; fingers proximal..tip at 7, 12, 17, 22 (+0..3).
            static constexpr int kTracked[5][3] = { { 2, 3, 4 }, { 7, 8, 9 }, { 12, 13, 14 }, { 17, 18, 19 }, { 22, 23, 24 } };
            const auto& chains = (*g_active_bones).fingers[side];
            for (int f = 0; f < 5; ++f) {
                for (int j = 0; j < 2; ++j) {
                    const int bone  = chains[f][j];
                    const int child = chains[f][j + 1];
                    const int a = kTracked[f][j], b = kTracked[f][j + 1];
                    if (bone < 0 || child < 0 || !hand.joints[a].position_valid || !hand.joints[b].position_valid) {
                        continue;
                    }
                    const auto wanted = to_body * (*room * tracking::ToHavokVector(at(b) - at(a)));
                    const auto joint  = pose.GameWorld(bone);
                    const auto dir    = pose.GameWorld(child).t - joint.t;
                    if (glm::length(wanted) < 1e-5f || glm::length(dir) < 1e-5f) {
                        continue;
                    }
                    auto posed = joint;
                    posed.r    = RotationBetween(dir, wanted) * joint.r;
                    pose.SetGameWorld(bone, posed);
                }
            }
            g_track_diag.fingers[side] = true;
            return true;
        }

        void RestoreAnimated(const Pose& pose)
        {
            for (auto it = (*g_written_active).begin(); it != (*g_written_active).end();) {
                if (it->first < pose.count && std::memcmp(&pose.local[it->first], &it->second.written, sizeof(RE::NiTransform)) == 0) {
                    pose.local[it->first] = it->second.animated;
                }
                it = (*g_written_active).erase(it);
            }
        }

        void UpdateWeaponActions(const Pose& pose, const std::optional<Xf>& weapon, bool drawn);

        void ApplyIK(const Pose& pose)
        {
            if ((*g_active_bones).head >= 0) {
                pose.local[(*g_active_bones).head].scale = 0.0f;
            }
            MeasureHand(pose, (*g_active_bones).right, "right");
            MeasureHand(pose, (*g_active_bones).left, "left");
            MeasureElbow(pose, (*g_active_bones).right, "right");
            MeasureElbow(pose, (*g_active_bones).left, "left");
            // Animated feet, captured before the pelvis moves (the feet hang below it).
            std::optional<std::pair<Xf, Xf>> feet;
            if ((*g_active_bones).left_leg.foot >= 0 && (*g_active_bones).right_leg.foot >= 0) {
                feet = std::make_pair(pose.GameWorld((*g_active_bones).left_leg.foot), pose.GameWorld((*g_active_bones).right_leg.foot));
            }
            ApplyCrouch(pose);
            if (!ApplyTrackedTorso(pose)) {
                ApplyLean(pose);
            }
            if (feet) {
                ApplyLegs(pose, feet->first, feet->second);
            }
            MeasurePalmSide(pose, 0, (*g_active_bones).left.shape, (*g_active_bones).left.wrist);
            MeasurePalmSide(pose, 1, (*g_active_bones).right.shape, (*g_active_bones).right.wrist);

            const auto room = tracking::RoomRotation();
            if (!room) {
                return;
            }
            ++g_frame_counter;
            if (FirstPersonArmsLive()) {
                // The first-person rig supplies the arms and weapon; this body's are collapsed.
                for (const int bone : { (*g_active_bones).right.biceps, (*g_active_bones).left.biceps, (*g_active_bones).weapon,
                         (*g_active_bones).by_name.contains("WeaponLeft") ? (*g_active_bones).by_name["WeaponLeft"] : -1 }) {
                    if (bone >= 0) {
                        auto [entry, first] = (*g_written_active).try_emplace(bone);
                        if (first) {
                            entry->second.animated = pose.local[bone];
                        }
                        pose.local[bone].scale = 0.0f;
                        entry->second.written  = pose.local[bone];
                    }
                }
                return;
            }
            std::optional<Xf>   held_weapon;
            RE::NiAVObject*     first_weapon_node{ nullptr };
            RE::NiAVObject*     first_right_wrist{ nullptr };
            RE::NiAVObject*     first_left_wrist{ nullptr };

            g_support      = {};
            g_native_diag  = {};
            auto& r = (*g_active_bones).right;
            if (r.biceps >= 0 && r.forearm >= 0 && r.wrist >= 0 && r.shape.valid) {
                const auto grip = HandWorld(tracking::GripPose(false));
                const auto aim  = HandWorld(tracking::AimPose(false));
                if (grip && aim) {
                    auto wrist_target = WristTarget(pose, r, *aim, *grip);

                    g_diag.weapon_aligned = false;
                    auto player           = CreationEngineSingletonManager::GetPlayerRef();
                    if (player && player->IsWeaponDrawn() && (*g_active_bones).weapon >= 0) {
                        static RE::NiAVObject* first_root{ nullptr };
                        static RE::NiAVObject* first_wrist{ nullptr };
                        static RE::NiAVObject* first_left{ nullptr };
                        static RE::NiAVObject* first_weapon{ nullptr };
                        if (auto root = FirstPersonRoot(player); root != first_root) {
                            first_root   = root;
                            first_wrist  = root ? FindDescendant(root, "R_Wrist") : nullptr;
                            first_left   = root ? FindDescendant(root, "L_Wrist") : nullptr;
                            first_weapon = root ? FindDescendant(root, "Weapon") : nullptr;
                            spdlog::info("[BodyIK] 1P rig: R_Wrist {} L_Wrist {} Weapon {}", first_wrist != nullptr, first_left != nullptr,
                                first_weapon != nullptr);
                        }
                        first_weapon_node = first_weapon;
                        first_right_wrist = first_wrist;
                        first_left_wrist  = first_left;
                        auto muzzle   = (*g_active_bones).weapon_node ? FindDescendant((*g_active_bones).weapon_node, "ProjectileNode") : nullptr;
                        g_body_muzzle = muzzle;
                        auto player_ref = CreationEngineSingletonManager::GetPlayerRef();
                        if (third_person_mode::Active(player_ref) && r.wrist >= 0) {
                            // The live third-person graph holds the weapon: its own IK has put the hands on R_HandIk and
                            // L_HandIk, so the hands' places on the weapon are read from this same skeleton.
                            const auto weapon_anim    = pose.GameWorld((*g_active_bones).weapon);
                            const auto hand_in_weapon = Compose(Inverse(weapon_anim), pose.GameWorld(r.wrist));
                            const auto& l_arm         = (*g_active_bones).left;
                            const auto support_in_weapon = l_arm.wrist >= 0 ? std::optional<Xf>{ Compose(Inverse(weapon_anim), pose.GameWorld(l_arm.wrist)) }
                                                                            : std::nullopt;
                            g_native_diag.hand_in_weapon = hand_in_weapon.t;

                            // Weapon bone axes: barrel +Y, up +Z.
                            g_basis.weapon  = (*g_active_bones).weapon_node;
                            g_basis.forward = glm::vec3{ 0.0f, 1.0f, 0.0f };
                            g_basis.up      = glm::vec3{ 0.0f, 0.0f, 1.0f };
                            g_basis.has     = true;

                            Xf weapon_xf = weapon_anim;
                            weapon_xf.r  = FrameOf(aim->forward, aim->up) * glm::transpose(FrameOf(g_basis.forward, g_basis.up));
                            wrist_target = Compose(weapon_xf, hand_in_weapon);
                            const auto palm_world = wrist_target.t + wrist_target.r * (r.shape.forward * r.shape.palm) * wrist_target.s;
                            const auto seat       = grip->position - palm_world;
                            weapon_xf.t += seat;
                            wrist_target.t += seat;
                            g_barrel.aim_forward = aim->forward;

                            g_support = {};
                            const auto left_grip = HandWorld(tracking::GripPose(true));
                            if (GameFlow::gStore.internalSettings.supportHand && support_in_weapon && l_arm.shape.valid && left_grip) {
                                auto support = Compose(weapon_xf, *support_in_weapon);
                                auto palm    = support.t + support.r * (l_arm.shape.forward * l_arm.shape.palm) * support.s;
                                const float distance    = glm::length(palm - left_grip->position);
                                static bool held        = false;
                                held                    = distance < (held ? 0.25f : 0.12f);
                                g_diag.support_distance = distance;
                                if (held) {
                                    const auto pivot = grip->position;
                                    const auto turn  = RotationBetween(palm - pivot, left_grip->position - pivot);
                                    weapon_xf.r      = turn * weapon_xf.r;
                                    weapon_xf.t      = pivot + turn * (weapon_xf.t - pivot);
                                    wrist_target.r   = turn * wrist_target.r;
                                    wrist_target.t   = pivot + turn * (wrist_target.t - pivot);
                                    g_support.held   = true;
                                    g_support.target = Compose(weapon_xf, *support_in_weapon);
                                    g_barrel.aim_forward = glm::normalize(weapon_xf.r * g_basis.forward);
                                }
                            }
                            g_barrel.placed        = true;
                            g_barrel.placed_weapon = weapon_xf;
                            pose.SetGameWorld((*g_active_bones).weapon, weapon_xf);
                            g_diag.weapon_aligned = true;
                            held_weapon           = weapon_xf;
                            g_native_diag.native  = true;
                            g_hand_check.placed        = true;
                            g_hand_check.wrist_target  = wrist_target;
                            g_hand_check.weapon_target = weapon_xf;
                        } else {
                        SampleWeaponBasis(first_weapon);
                        if (first_wrist && first_weapon) {
                            // The animation's grip: the hand in the weapon's space, transferred onto the body's hand by anatomy.
                            MeasureFirstPersonHand(first_wrist, false);
                            Xf first_hand_world = FromNi(first_wrist->world);
                            if (g_first_hands[1].shape.valid) {
                                first_hand_world = BodyWristFromFirstPerson(first_hand_world, r.shape, false);
                            }
                            auto hand_in_weapon = Compose(Inverse(FromNi(first_weapon->world)), first_hand_world);

                            // Weapon models carry an authored right-hand IK target (R_HandIk). The hand's offset from it is
                            // taken from the first-person rig and applied to the third-person model's own target.
                            auto first_ik = FindDescendant(first_weapon, "R_HandIk");
                            auto body_ik  = (*g_active_bones).weapon_node ? FindDescendant((*g_active_bones).weapon_node, "R_HandIk") : nullptr;
                            static RE::NiAVObject* logged_ik{ nullptr };
                            if (logged_ik != (*g_active_bones).weapon_node) {
                                logged_ik = (*g_active_bones).weapon_node;
                                spdlog::info("[BodyIK] R_HandIk: 1P {} 3P {}", first_ik != nullptr, body_ik != nullptr);
                            }
                            if (first_ik && body_ik) {
                                const auto hand_from_ik = Compose(Inverse(FromNi(first_ik->world)), first_hand_world);
                                const auto ik_in_weapon = Compose(Inverse(FromNi((*g_active_bones).weapon_node->world)), FromNi(body_ik->world));
                                hand_in_weapon          = Compose(ik_in_weapon, hand_from_ik);
                            }
                            Xf         weapon_xf      = Compose(wrist_target, Inverse(hand_in_weapon));
                            if (g_basis.has && g_basis.weapon == (*g_active_bones).weapon_node) {
                                // Weapon from the aim ray: barrel forward, weapon up along the controller's up.
                                weapon_xf.r = FrameOf(aim->forward, aim->up) * glm::transpose(FrameOf(g_basis.forward, g_basis.up));
                                // Hand from the weapon, then both seated so the palm centre is on the grip point.
                                wrist_target           = Compose(weapon_xf, hand_in_weapon);
                                const auto palm_world  = wrist_target.t + wrist_target.r * (r.shape.forward * r.shape.palm) * wrist_target.s;
                                const auto seat        = grip->position - palm_world;
                                weapon_xf.t += seat;
                                wrist_target.t += seat;
                            }
                            g_barrel.aim_forward = aim->forward;

                            // Two-handed: the support grip is taken when the left hand comes near it; the weapon then
                            // turns about the right grip so the support point lies toward the left hand (ROCK).
                            g_support = {};
                            const auto& l_arm = (*g_active_bones).left;
                            const auto  left_grip = HandWorld(tracking::GripPose(true));
                            if (GameFlow::gStore.internalSettings.supportHand && first_left && l_arm.shape.valid && left_grip) {
                                MeasureFirstPersonHand(first_left, true);
                                if (g_first_hands[0].shape.valid) {
                                    const auto support_in_weapon = Compose(Inverse(FromNi(first_weapon->world)),
                                        BodyWristFromFirstPerson(FromNi(first_left->world), l_arm.shape, true));
                                    auto support = Compose(weapon_xf, support_in_weapon);
                                    auto palm    = support.t + support.r * (l_arm.shape.forward * l_arm.shape.palm) * support.s;
                                    const float distance    = glm::length(palm - left_grip->position);
                                    static bool held        = false;
                                    held                    = distance < (held ? 0.25f : 0.12f);
                                    g_diag.support_distance = distance;
                                    if (held) {
                                        const auto pivot = grip->position;
                                        const auto turn  = RotationBetween(palm - pivot, left_grip->position - pivot);
                                        weapon_xf.r      = turn * weapon_xf.r;
                                        weapon_xf.t      = pivot + turn * (weapon_xf.t - pivot);
                                        wrist_target.r   = turn * wrist_target.r;
                                        wrist_target.t   = pivot + turn * (wrist_target.t - pivot);
                                        support          = Compose(weapon_xf, support_in_weapon);
                                        g_support.held   = true;
                                        g_support.target = support;
                                        if (g_basis.has) {
                                            g_barrel.aim_forward = glm::normalize(weapon_xf.r * g_basis.forward);
                                        }
                                    }
                                }
                            }

                            g_barrel.placed        = true;
                            g_barrel.placed_weapon = weapon_xf;
                            pose.SetGameWorld((*g_active_bones).weapon, weapon_xf);
                            g_diag.weapon_aligned = true;
                            held_weapon           = weapon_xf;
                        }
                        }
                    } else {
                        g_body_muzzle = nullptr;
                    }
                    const auto bend = TrackedBend(joint::kArmUpper[1], joint::kArmLower[1], joint::kWrist[1]);
                    g_track_diag.elbow[1] = bend.has_value();
                    g_diag.right_gap      = SolveArm(pose, r, false, wrist_target, bend);
                    if (held_weapon && g_native_diag.native) {
                        static auto vr    = VR::get();
                        const auto  input = vr->get_finger_state(false);
                        if (g_finger_palm[1].valid) {
                            const float index = input.trigger_touch ? 0.3f + 0.7f * input.trigger : 0.0f;
                            CurlFinger(pose, (*g_active_bones).fingers[1][1], r.wrist, g_finger_palm[1].palm, 10.0f + 50.0f * index,
                                10.0f + 70.0f * index);
                        }
                    } else {
                        ApplyFingers(pose, 1, r.wrist, held_weapon ? first_right_wrist : nullptr);
                    }
                }
            }

            auto& l = (*g_active_bones).left;
            if (l.biceps >= 0 && l.forearm >= 0 && l.wrist >= 0 && l.shape.valid) {
                const auto grip = HandWorld(tracking::GripPose(true));
                const auto aim  = HandWorld(tracking::AimPose(true));
                if (grip && aim) {
                    auto       target  = WristTarget(pose, l, *aim, *grip);
                    const bool on_grip = held_weapon && g_support.held;
                    if (on_grip) {
                        target = g_support.target;
                    }
                    g_diag.support_held = on_grip;
                    const auto bend       = TrackedBend(joint::kArmUpper[0], joint::kArmLower[0], joint::kWrist[0]);
                    g_track_diag.elbow[0] = bend.has_value();
                    g_diag.left_gap       = SolveArm(pose, l, true, target, bend);
                    if (!(on_grip && g_native_diag.native)) {
                        ApplyFingers(pose, 0, l.wrist, on_grip ? first_left_wrist : nullptr);
                    }
                }
            }
            {
                auto player = CreationEngineSingletonManager::GetPlayerRef();
                UpdateWeaponActions(pose, held_weapon, player && player->IsWeaponDrawn());
            }
        }

        // Where the body's shoulders ended up this frame, for attaching the first-person arms.
        struct BodyShoulders
        {
            bool      valid{ false };
            glm::vec3 left{};
            glm::vec3 right{};
            glm::vec3 up{ 0.0f, 0.0f, 1.0f };
            glm::vec3 pelvis{};
            glm::vec3 chest{};
            glm::vec3 forward{ 0.0f, 1.0f, 0.0f };
        };
        BodyShoulders g_body_shoulders;

        struct FirstPersonDiag
        {
            bool      active{ false };
            glm::vec3 origin{};
            float     attach_offset{ 0.0f };
            float     attach_error{ -1.0f };
            float     attach_angle{ 0.0f };
            int       branch{ -1 };
            glm::vec3 root_translate{};
            bool      weapon{ false };
            bool      support{ false };
        };
        FirstPersonDiag g_fp_diag;

        // Weapon interactions on the body: holster slots, manual reload, raise to aim. They act through the game's own
        // inputs (draw/holster/reload on X, aim on the left trigger) so the game's weapon state stays authoritative.
        struct WeaponActions
        {
            enum class Mag { Loaded, Ejected, InHand } mag{ Mag::Loaded };
            bool        grip_was[2]{};
            float       holster_dwell{ 0.0f };
            float       cooldown{ 0.0f };
            bool        aiming{ false };
            int         slot[2]{ -1, -1 };
            const char* last{ "" };
            std::chrono::steady_clock::time_point time{};
            bool        has_time{ false };
        };
        WeaponActions g_actions;

        void Pulse(int side, float seconds, float amplitude)
        {
            static auto vr = VR::get();
            vr->trigger_haptic_vibration(0.0f, seconds, 160.0f, amplitude, side == 0 ? vr->get_left_joystick() : vr->get_right_joystick());
        }

        // Slots on the body: 0 right hip, 1 left hip, 2 chest, 3 behind the right shoulder, 4 behind the left
        // shoulder, 5 magazine pouch (left hip, front).
        int SlotAt(const glm::vec3& p)
        {
            const auto& b = g_body_shoulders;
            if (!b.valid) {
                return -1;
            }
            const auto up    = glm::vec3{ 0.0f, 0.0f, 1.0f };
            const auto right = glm::normalize(glm::cross(b.forward, up));
            const glm::vec3 slots[6] = {
                b.pelvis + right * 0.22f + b.forward * 0.02f - up * 0.05f,
                b.pelvis - right * 0.22f + b.forward * 0.02f - up * 0.05f,
                b.chest + b.forward * 0.16f,
                b.right + up * 0.10f - b.forward * 0.15f,
                b.left + up * 0.10f - b.forward * 0.15f,
                b.pelvis - right * 0.15f + b.forward * 0.14f,
            };
            int   best = -1;
            float best_d = 0.17f;
            for (int i = 0; i < 6; ++i) {
                const float d = glm::length(p - slots[i]);
                if (d < best_d) {
                    best   = i;
                    best_d = d;
                }
            }
            return best;
        }

        void UpdateWeaponActions(const Pose& pose, const std::optional<Xf>& weapon, bool drawn)
        {
            auto& a = g_actions;
            const auto& settings = GameFlow::gStore.internalSettings;
            const auto now = std::chrono::steady_clock::now();
            const float dt = a.has_time ? std::clamp(std::chrono::duration<float>(now - a.time).count(), 0.0f, 0.1f) : 0.0f;
            a.time     = now;
            a.has_time = true;
            a.cooldown = std::max(0.0f, a.cooldown - dt);

            static auto vr = VR::get();
            std::optional<HandTarget> hands[2] = { HandWorld(tracking::GripPose(true)), HandWorld(tracking::GripPose(false)) };
            bool grip[2]{}, pressed[2]{};
            for (int side = 0; side < 2; ++side) {
                grip[side]       = vr->get_finger_state(side == 0).grip > 0.6f;
                pressed[side]    = grip[side] && !a.grip_was[side];
                a.grip_was[side] = grip[side];
                a.slot[side]     = hands[side] ? SlotAt(hands[side]->position) : -1;
            }

            // Holsters.
            bool right_in_slot = false;
            if (settings.holsters) {
                if (!drawn) {
                    for (int side = 0; side < 2; ++side) {
                        const bool in_slot = a.slot[side] >= 0 && a.slot[side] <= 4;
                        right_in_slot      = right_in_slot || (side == 1 && in_slot);
                        if (in_slot && pressed[side] && a.cooldown <= 0.0f) {
                            input_requests::Press(input_requests::kX, 120);
                            Pulse(side, 0.05f, 0.6f);
                            a.cooldown = 1.0f;
                            a.last     = "draw";
                            spdlog::info("[Weapon] draw from slot {}", a.slot[side]);
                        }
                    }
                } else if (weapon) {
                    const bool in_slot = a.slot[1] >= 0 && a.slot[1] <= 4;
                    a.holster_dwell    = in_slot && !a.aiming ? a.holster_dwell + dt : 0.0f;
                    if (a.holster_dwell > 0.45f && a.cooldown <= 0.0f) {
                        input_requests::Press(input_requests::kX, 900);
                        Pulse(1, 0.08f, 0.7f);
                        a.cooldown      = 1.5f;
                        a.holster_dwell = 0.0f;
                        a.last          = "holster";
                        spdlog::info("[Weapon] holster at slot {}", a.slot[1]);
                    }
                }
            }
            input_requests::Suppress(input_requests::kRightShoulder, right_in_slot || a.slot[1] == 5);

            // Manual reload: the gun's magazine part is hidden, carried in the left hand, or seated.
            const auto& bones = *g_active_bones;
            int mag = -1;
            if (auto it = bones.by_name.find("Magazine"); it != bones.by_name.end()) {
                mag = it->second;
            } else if (auto it2 = bones.by_name.find("P-Mag"); it2 != bones.by_name.end()) {
                mag = it2->second;
            }
            input_requests::Suppress(input_requests::kX, settings.manualReload && drawn);
            if (settings.manualReload && drawn && weapon) {
                const auto seated = mag >= 0 ? std::optional<Xf>{ pose.GameWorld(mag) } : std::nullopt;
                if (input_requests::TakeReloadPress() && a.mag == WeaponActions::Mag::Loaded) {
                    a.mag  = WeaponActions::Mag::Ejected;
                    a.last = "eject";
                    Pulse(1, 0.06f, 0.8f);
                    spdlog::info("[Weapon] magazine ejected");
                }
                if (a.mag == WeaponActions::Mag::Ejected && pressed[0] && a.slot[0] == 5) {
                    a.mag  = WeaponActions::Mag::InHand;
                    a.last = "grab";
                    Pulse(0, 0.04f, 0.5f);
                    spdlog::info("[Weapon] magazine taken");
                }
                if (a.mag == WeaponActions::Mag::InHand) {
                    if (!grip[0]) {
                        a.mag  = WeaponActions::Mag::Ejected;
                        a.last = "drop";
                        spdlog::info("[Weapon] magazine dropped");
                    } else if (seated && hands[0] && glm::length(hands[0]->position - seated->t) < 0.08f) {
                        a.mag  = WeaponActions::Mag::Loaded;
                        a.last = "insert";
                        input_requests::Press(input_requests::kX, 120);
                        Pulse(0, 0.08f, 1.0f);
                        Pulse(1, 0.08f, 1.0f);
                        spdlog::info("[Weapon] magazine inserted, reloading");
                    }
                }
                if (seated) {
                    if (a.mag == WeaponActions::Mag::Ejected) {
                        auto hidden = *seated;
                        hidden.s    = 1e-4f;
                        pose.SetGameWorld(mag, hidden);
                    } else if (a.mag == WeaponActions::Mag::InHand && hands[0]) {
                        auto held = *seated;
                        held.t    = hands[0]->position;
                        pose.SetGameWorld(mag, held);
                    }
                }
            } else if (!drawn) {
                input_requests::TakeReloadPress();
            }

            // Raise to aim: the sight (above the grip along the weapon's up) on the line from the eye along the barrel.
            bool aim = false;
            if (settings.raiseToAim && drawn && weapon && g_basis.has) {
                auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
                if (world_camera) {
                    const auto eye    = ToVec(world_camera->world.translate);
                    const auto barrel = glm::normalize(weapon->r * g_basis.forward);
                    const auto up     = glm::normalize(weapon->r * g_basis.up);
                    const auto sight  = (hands[1] ? hands[1]->position : weapon->t) + up * 0.08f;
                    const auto to_eye = eye - sight;
                    const float along = glm::dot(to_eye, -barrel);
                    const float off   = glm::length(to_eye + barrel * along);
                    const float limit = a.aiming ? 0.07f : 0.045f;
                    aim               = along > 0.03f && along < 0.35f && off < limit;
                }
            }
            if (aim != a.aiming) {
                spdlog::info("[Weapon] raise to aim {}", aim ? "on" : "off");
            }
            a.aiming = aim;
            input_requests::SetHeld(input_requests::kLeftTrigger, aim);
        }

        // Moving parts of a weapon model by node name (receiver rig nodes and attach points), with a provisional
        // mechanical class. Logged when the held weapon changes.
        void LogWeaponParts(RE::NiAVObject* weapon)
        {
            static RE::NiAVObject* logged{ nullptr };
            if (!weapon || weapon == logged) {
                return;
            }
            logged = weapon;
            int magazine = 0, bolt = 0;
            bool p_mag = false, p_slide = false, p_foregrip = false, pump = false, battery = false, trigger = false, muzzle = false;
            std::string names;
            std::vector<RE::NiAVObject*> stack{ weapon };
            RE::NiAVObject* children[96]{};
            while (!stack.empty() && names.size() < 6000) {
                auto node = stack.back();
                stack.pop_back();
                const std::string_view name{ node->name.c_str() };
                if (!name.empty()) {
                    names += " ";
                    names += name;
                }
                if (Contains(name, "Magazine")) ++magazine;
                if (name.size() >= 4 && Contains(name.substr(0, 4), "Bolt")) ++bolt;
                p_mag      = p_mag || Contains(name, "P-Mag");
                p_slide    = p_slide || Contains(name, "P-Slide");
                p_foregrip = p_foregrip || Contains(name, "P-Foregrip");
                pump       = pump || Contains(name, "Pump_Grip");
                battery    = battery || Contains(name, "P-Battery");
                trigger    = trigger || Contains(name, "Trigger");
                muzzle     = muzzle || name == "ProjectileNode";
                const auto count = ReadChildren(node, children, 96);
                for (std::uint16_t i = 0; i < count; ++i) {
                    if (children[i]) {
                        stack.push_back(children[i]);
                    }
                }
            }
            const char* kind = pump ? "pump" : (!p_mag && magazine >= 5) ? "revolver/sequential" : battery ? "cell" : (p_slide || bolt > 0) && p_mag ? "magazine + slide/bolt"
                             : p_mag ? "magazine" : "unknown";
            spdlog::info("[Weapon] parts: magazine nodes {} bolt nodes {} P-Mag {} P-Slide {} P-Foregrip {} Pump_Grip {} P-Battery {} trigger {} muzzle {} -> {}",
                magazine, bolt, p_mag, p_slide, p_foregrip, pump, battery, trigger, muzzle, kind);
            spdlog::info("[Weapon] nodes:{}", names);
        }

        // Frame at the collarbones: origin between them, forward from up x (left to right), up.
        Xf ShoulderFrame(const glm::vec3& left, const glm::vec3& right, const glm::vec3& up)
        {
            Xf frame;
            const auto side    = glm::normalize(right - left);
            auto       upright = up - glm::dot(up, side) * side;
            upright            = glm::length(upright) > 1e-4f ? glm::normalize(upright) : glm::vec3{ 0.0f, 0.0f, 1.0f };
            frame.r            = FrameOf(glm::cross(upright, side), upright);
            frame.t            = (left + right) * 0.5f;
            return frame;
        }

        // First-person arms (FRIK's arrangement): the game's first-person rig, which animates its own hands on its own
        // weapon, is attached at the body's shoulders. The weapon follows the aim ray, the hands come from the
        // weapon through the rig's own animated grip (the same hands that are shown), and the arms are solved with
        // the IK. The pose buffer is origin-relative; the origin the engine adds back is in the update data.
        void ApplyFirstPerson(Pose& pose, const glm::vec3& origin, RE::NiTransform& root_out)
        {
            auto& b = *g_active_bones;
            g_fp_diag        = {};
            g_fp_diag.origin = origin;
            pose.to_world    = Xf{ glm::mat3{ 1.0f }, origin, 1.0f };
            if (b.right.clavicle < 0 || b.left.clavicle < 0 || !g_body_shoulders.valid) {
                return;
            }
            MeasureHand(pose, b.right, "first-person right");
            MeasureHand(pose, b.left, "first-person left");
            MeasureElbow(pose, b.right, "first-person right");
            MeasureElbow(pose, b.left, "first-person left");
            MeasurePalmSide(pose, 0, b.left.shape, b.left.wrist);
            MeasurePalmSide(pose, 1, b.right.shape, b.right.wrist);

            // Animated relations, read before anything moves.
            std::optional<Xf> hand_in_weapon, support_in_weapon;
            std::optional<Xf> weapon_anim;
            if (b.weapon >= 0) {
                weapon_anim = pose.GameWorld(b.weapon);
                if (b.right.wrist >= 0) {
                    hand_in_weapon = Compose(Inverse(*weapon_anim), pose.GameWorld(b.right.wrist));
                }
                if (b.left.wrist >= 0) {
                    support_in_weapon = Compose(Inverse(*weapon_anim), pose.GameWorld(b.left.wrist));
                }
            }

            // Attach: the branch carrying the arms (not the root, which also carries the camera) is moved so the rig's
            // collarbones sit on the body's.
            const auto rig  = ShoulderFrame(pose.GameWorld(b.left.clavicle).t, pose.GameWorld(b.right.clavicle).t, pose.root.r[2]);
            const auto body = ShoulderFrame(g_body_shoulders.left, g_body_shoulders.right, g_body_shoulders.up);
            g_fp_diag.attach_offset = glm::length(body.t - rig.t);
            g_fp_diag.attach_angle  = glm::degrees(glm::angle(glm::quat_cast(body.r * glm::transpose(rig.r))));
            int branch = b.right.clavicle;
            for (int guard = 0; branch >= pose.top && guard < 64; ++guard) {
                branch = pose.parent[branch];
            }
            if (branch < 0 || branch >= pose.top) {
                spdlog::error("[BodyIK] first-person rig: no top-level branch above R_Clavicle");
                return;
            }
            const auto move = Compose(body, Inverse(rig));
            pose.SetGameWorld(branch, Compose(move, pose.GameWorld(branch)));
            g_fp_diag.branch = branch;
            g_fp_diag.active = true;
            g_fp_last_frame  = g_frame_counter;

            auto player = CreationEngineSingletonManager::GetPlayerRef();
            const bool drawn = player && player->IsWeaponDrawn() && weapon_anim && hand_in_weapon;
            auto& r = b.right;
            auto& l = b.left;
            std::optional<Xf> weapon_xf;
            g_support = {};

            if (r.biceps >= 0 && r.forearm >= 0 && r.wrist >= 0 && r.shape.valid) {
                const auto grip = HandWorld(tracking::GripPose(false));
                const auto aim  = HandWorld(tracking::AimPose(false));
                if (grip && aim) {
                    auto wrist_target = WristTarget(pose, r, *aim, *grip);
                    if (drawn) {
                        LogWeaponParts(b.weapon_node);
                        auto muzzle   = b.weapon_node ? FindDescendant(b.weapon_node, "ProjectileNode") : nullptr;
                        g_body_muzzle = muzzle;

                        // How the animation holds the weapon against the view (its barrel and up), sampled when firing.
                        if (auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera; world_camera && world_camera->parent) {
                            const auto to_weapon = glm::transpose(weapon_anim->r);
                            const auto view_r    = FromNi(world_camera->parent->world).r;
                            const auto forward   = glm::normalize(to_weapon * view_r[1]);
                            const auto up        = glm::normalize(to_weapon * view_r[2]);
                            if (g_basis.weapon != b.weapon_node) {
                                g_basis        = {};
                                g_basis.weapon = b.weapon_node;
                            }
                            const bool fired = g_fire_sample.exchange(false);
                            if (fired || !g_basis.has) {
                                g_basis.forward = forward;
                                g_basis.up      = up;
                            } else if (!g_basis.fired) {
                                g_basis.forward = glm::normalize(glm::mix(g_basis.forward, forward, 0.02f));
                                g_basis.up      = glm::normalize(glm::mix(g_basis.up, up, 0.02f));
                            }
                            g_basis.fired = g_basis.fired || fired;
                            g_basis.has   = true;
                        }

                        // Weapon from the aim ray; the hand from the weapon; both seated so the palm is on the grip.
                        Xf weapon = *weapon_anim;
                        weapon.r  = FrameOf(aim->forward, aim->up) * glm::transpose(FrameOf(g_basis.forward, g_basis.up));
                        wrist_target           = Compose(weapon, *hand_in_weapon);
                        const auto palm_world  = wrist_target.t + wrist_target.r * (r.shape.forward * r.shape.palm) * wrist_target.s;
                        const auto seat        = grip->position - palm_world;
                        weapon.t += seat;
                        wrist_target.t += seat;
                        g_barrel.aim_forward = aim->forward;

                        // Two hands: the support grip is taken when the left hand comes near it; the weapon then turns
                        // about the right grip toward the left hand.
                        const auto left_grip = HandWorld(tracking::GripPose(true));
                        if (GameFlow::gStore.internalSettings.supportHand && support_in_weapon && l.shape.valid && left_grip) {
                            auto support = Compose(weapon, *support_in_weapon);
                            auto palm    = support.t + support.r * (l.shape.forward * l.shape.palm) * support.s;
                            const float distance    = glm::length(palm - left_grip->position);
                            static bool held        = false;
                            held                    = distance < (held ? 0.25f : 0.12f);
                            g_diag.support_distance = distance;
                            if (held) {
                                const auto pivot = grip->position;
                                const auto turn  = RotationBetween(palm - pivot, left_grip->position - pivot);
                                weapon.r         = turn * weapon.r;
                                weapon.t         = pivot + turn * (weapon.t - pivot);
                                wrist_target.r   = turn * wrist_target.r;
                                wrist_target.t   = pivot + turn * (wrist_target.t - pivot);
                                g_support.held   = true;
                                g_support.target = Compose(weapon, *support_in_weapon);
                                if (g_basis.has) {
                                    g_barrel.aim_forward = glm::normalize(weapon.r * g_basis.forward);
                                }
                            }
                        }
                        pose.SetGameWorld(b.weapon, weapon);
                        g_barrel.placed        = true;
                        g_barrel.placed_weapon = weapon;
                        weapon_xf              = weapon;
                        g_fp_diag.weapon       = true;
                    } else {
                        g_body_muzzle = nullptr;
                    }
                    const auto bend = TrackedBend(joint::kArmUpper[1], joint::kArmLower[1], joint::kWrist[1]);
                    g_diag.right_gap = SolveArm(pose, r, false, wrist_target, bend);
                    if (weapon_xf) {
                        // The animated grip keeps its fingers; the index finger follows the trigger.
                        static auto vr    = VR::get();
                        const auto  input = vr->get_finger_state(false);
                        if (g_finger_palm[1].valid) {
                            const float index = input.trigger_touch ? 0.3f + 0.7f * input.trigger : 0.0f;
                            CurlFinger(pose, b.fingers[1][1], r.wrist, g_finger_palm[1].palm, 10.0f + 50.0f * index, 10.0f + 70.0f * index);
                        }
                    } else {
                        ApplyFingers(pose, 1, r.wrist, nullptr);
                    }
                }
            }

            if (l.biceps >= 0 && l.forearm >= 0 && l.wrist >= 0 && l.shape.valid) {
                const auto grip = HandWorld(tracking::GripPose(true));
                const auto aim  = HandWorld(tracking::AimPose(true));
                if (grip && aim) {
                    auto       target  = WristTarget(pose, l, *aim, *grip);
                    const bool on_grip = weapon_xf && g_support.held;
                    if (on_grip) {
                        target = g_support.target;
                    }
                    g_diag.support_held = on_grip;
                    g_fp_diag.support   = on_grip;
                    const auto bend = TrackedBend(joint::kArmUpper[0], joint::kArmLower[0], joint::kWrist[0]);
                    g_diag.left_gap = SolveArm(pose, l, true, target, bend);
                    if (!on_grip) {
                        ApplyFingers(pose, 0, l.wrist, nullptr);
                    }
                }
            }
            UpdateWeaponActions(pose, weapon_xf, drawn);
        }

        // The first-person rig updates node by node (no pose buffer: only the third-person root gets a model node),
        // parents before children, and its meshes capture their bones as they update. When its root has updated,
        // its tree is flattened into the pose layout the IK uses, solved, and the changed locals are written back
        // before any child updates.
        struct FlatRig
        {
            std::vector<RE::NiAVObject*> nodes;
            std::vector<std::uint16_t>   parent;
            std::vector<RE::NiTransform> local;
            std::vector<RE::NiTransform> world;
            std::uint16_t                top{ 0 };
            std::size_t                  signature{ 0 };
        };
        FlatRig g_flat;

        void FlattenRig(RE::NiAVObject* root)
        {
            auto& f = g_flat;
            f.nodes.clear();
            f.parent.clear();
            RE::NiAVObject* children[96]{};
            auto count = ReadChildren(root, children, 96);
            for (std::uint16_t i = 0; i < count; ++i) {
                if (children[i]) {
                    f.nodes.push_back(children[i]);
                    f.parent.push_back(0);
                }
            }
            f.top = static_cast<std::uint16_t>(f.nodes.size());
            for (std::size_t i = 0; i < f.nodes.size() && f.nodes.size() < 1024; ++i) {
                count = ReadChildren(f.nodes[i], children, 96);
                for (std::uint16_t c = 0; c < count; ++c) {
                    if (children[c]) {
                        f.nodes.push_back(children[c]);
                        f.parent.push_back(static_cast<std::uint16_t>(i));
                    }
                }
            }
            std::size_t signature = f.nodes.size();
            for (auto* node : f.nodes) {
                signature = signature * 1315423911u ^ reinterpret_cast<std::uintptr_t>(node);
            }
            f.signature = signature;
        }

        bool g_first_person_handled{ false };

        std::atomic<int> g_fp_root_hits{ 0 };

        bool HandleFirstPersonRoot(RE::NiAVObject* root, RE::NiUpdateData* data)
        {
            ++g_fp_root_hits;
            g_first_person_handled = false;
            if (!g_state.active || !GameFlow::gStore.internalSettings.firstPersonArms || !root) {
                return false;
            }
            const auto previous = g_flat.signature;
            FlattenRig(root);
            auto& f = g_flat;
            if (f.nodes.empty()) {
                return false;
            }
            const bool rebuilt = previous != f.signature;
            if (rebuilt) {
                g_written_first.clear();
            }
            // Writes the animation did not replace are put back to their animated value before reading.
            for (const auto& [index, entry] : g_written_first) {
                if (index < static_cast<int>(f.nodes.size()) && std::memcmp(&f.nodes[index]->local, &entry.written, sizeof(RE::NiTransform)) == 0) {
                    f.nodes[index]->local = entry.animated;
                }
            }
            g_written_first.clear();
            f.local.resize(f.nodes.size());
            f.world.resize(f.nodes.size());
            for (std::size_t i = 0; i < f.nodes.size(); ++i) {
                f.local[i] = f.nodes[i]->local;
                f.world[i] = f.nodes[i]->local;
            }

            Pose pose;
            pose.local  = f.local.data();
            pose.world  = f.world.data();
            pose.parent = f.parent.data();
            pose.count  = static_cast<std::uint16_t>(f.nodes.size());
            pose.top    = f.top;
            pose.root   = FromNi(root->world);

            g_active_bones   = &g_first_bones;
            g_written_active = &g_written_first;
            g_finger_palm    = g_finger_palm_first;
            if (rebuilt || g_first_bones.storage == nullptr) {
                std::vector<std::pair<int, RE::NiAVObject*>> listed;
                for (std::size_t i = 0; i < f.nodes.size(); ++i) {
                    listed.emplace_back(static_cast<int>(i), f.nodes[i]);
                }
                FindBones(pose, reinterpret_cast<const void*>(f.signature), &listed);
                spdlog::info("[BodyIK] first-person rig: {} nodes under '{}' (top {})", f.nodes.size(), root->name.c_str(), f.top);
            }

            glm::vec3 origin{ 0.0f };
            if (data) {
                if (auto saved = *reinterpret_cast<RE::NiPoint3**>(reinterpret_cast<std::uint8_t*>(data) + 0x28)) {
                    origin = ToVec(*saved);
                }
            }
            RE::NiTransform root_world = root->world;
            ApplyFirstPerson(pose, origin, root_world);
            g_fp_diag.root_translate = ToVec(root->world.translate);
            if (g_fp_diag.active && g_first_bones.right.clavicle >= 0) {
                g_fp_diag.attach_error = glm::length(pose.GameWorld(g_first_bones.right.clavicle).t - g_body_shoulders.right);
            }
            if (g_fp_diag.active) {
                for (const auto& [index, entry] : g_written_first) {
                    if (index >= 0 && index < static_cast<int>(f.nodes.size())) {
                        f.nodes[index]->local = f.local[index];
                    }
                }
                g_first_person_handled = true;
            }
            g_active_bones   = &g_body_bones;
            g_written_active = &g_written_body;
            g_finger_palm    = g_finger_palm_body;
            return g_first_person_handled;
        }

        safetyhook::InlineHook g_model_update_hook;

        // The pose-buffer conventions are measured against the engine before any write.
        struct Calibration
        {
            enum class Status { Measuring, Ready, Failed } status{ Status::Ready };
            int  world_variant{ 3 };  // pose worlds are game world (synced verbatim to the nodes)
            int  good_frames{ 0 };
            int  frames{ 0 };
            bool has_measured{ false };
            Xf   measured{};  // pose -> game world measured from the engine's last update
        };
        Calibration g_calibration;
        constexpr int kWorldVariants = 5;
        constexpr const char* kWorldVariantNames[kWorldVariants] = { "root world", "root translation", "root transposed", "identity", "measured" };

        // Pose space -> game world.
        Xf ToWorldVariant(int variant)
        {
            const auto root = g_state.root ? FromNi(g_state.root->world) : Xf{};
            switch (variant) {
            case 0:
                return root;
            case 1:
                return { glm::mat3{ 1.0f }, root.t, root.s };
            case 2:
                return { glm::transpose(root.r), root.t, root.s };
            case 4:
                return g_calibration.measured;
            default:
                return Xf{};
            }
        }

        float AngleBetween(const glm::mat3& a, const glm::mat3& b)
        {
            const auto d = a * glm::transpose(b);
            return glm::degrees(std::acos(std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)));
        }

        Pose ReadPose(std::uint8_t* model, const RE::NiTransform* root_local)
        {
            Pose pose;
            const auto storage = *reinterpret_cast<std::uint8_t**>(model + 0x10);
            if (!storage) {
                return pose;
            }
            const auto locals  = *reinterpret_cast<std::uint8_t**>(storage + 0x08);
            const auto worlds  = *reinterpret_cast<std::uint8_t**>(storage + 0x10);
            pose.parent        = *reinterpret_cast<std::uint16_t**>(storage + 0x28);
            if (!locals || !worlds || !pose.parent) {
                return pose;
            }
            // Each array object holds its data pointer at +0x10 (0x2be9424..0x2be9435).
            pose.local = *reinterpret_cast<RE::NiTransform**>(locals + 0x10);
            pose.world = *reinterpret_cast<RE::NiTransform**>(worlds + 0x10);
            if (!pose.local || !pose.world) {
                pose.local = nullptr;
                return pose;
            }
            pose.model    = model;
            pose.count    = *reinterpret_cast<std::uint16_t*>(model + 0x78);
            pose.top      = static_cast<std::uint16_t>(*reinterpret_cast<std::uint16_t*>(model + 0x7c) + *reinterpret_cast<std::uint16_t*>(model + 0x7e));
            pose.root     = FromNi(*root_local);  // top-level entries: world = local * root (0x2be953d)
            pose.to_world = ToWorldVariant(g_calibration.world_variant);
            return pose;
        }

        bool RagdollActive(std::uint8_t* model)
        {
            auto component = *reinterpret_cast<void**>(model + 0x70);
            if (!component) {
                return false;
            }
            using fn_t = bool (*)(void*);
            return (*reinterpret_cast<fn_t**>(component))[4](component);
        }

        // BSModelNode::UpdateTransforms: pose locals -> worlds -> synced to nodes, skin and geometry.
        void* ModelNodeUpdateTransforms(std::uint8_t* model, const RE::NiTransform* root_local, RE::NiUpdateData* data, void* out)
        {
            const bool ours = g_state.active && g_calibration.status != Calibration::Status::Failed && g_state.root && root_local &&
                              model == *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(g_state.root) + kModelNodeOffset);
            Pose            pose;
            RE::NiTransform shifted_root{};
            bool            use_shifted = false;
            if (ours && !RagdollActive(model)) {
                pose = ReadPose(model, root_local);
                if (pose.local) {
                    const auto storage = *reinterpret_cast<void**>(model + 0x10);
                    if ((*g_active_bones).storage != storage) {
                        FindBones(pose, storage);
                        (*g_written_active).clear();
                    }
                    RestoreAnimated(pose);
                    // Both transforms are final from the previous frame here.
                    MeasureBarrel();

                    // Body placed so the neck is under and behind the eyes (horizontal only). Small offsets are taken
                    // by the spine (lean); beyond kLeanReach the body follows.
                    constexpr float kNeckBehindEyes = 0.12f;
                    constexpr float kLeanReach      = 0.20f;
                    shifted_root                    = *root_local;
                    g_lean                          = glm::vec3{ 0.0f };
                    auto world_camera               = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
                    SnapshotTracking();
                    if (world_camera && (*g_active_bones).neck >= 0) {
                        const auto eye = ToVec(world_camera->world.translate);
                        // Torso partly toward the hands (FRIK: 0.7 of the hands' yaw, within 50 degrees).
                        if (GameFlow::gStore.internalSettings.bodyFacing) {
                            const auto yaw = TrackedFacingYaw(pose).value_or(FacingYaw(pose, eye));
                            Xf         root = FromNi(shifted_root);
                            const auto turn = glm::mat3_cast(glm::angleAxis(yaw, glm::vec3{ 0.0f, 0.0f, 1.0f }));
                            const auto pivot = glm::vec3{ eye.x, eye.y, root.t.z };
                            root.r          = turn * root.r;
                            root.t          = pivot + turn * (root.t - pivot);
                            ToNi(root, shifted_root);
                            pose.root = root;
                        } else {
                            g_facing_yaw = 0.0f;
                        }
                        auto forward = pose.root.r[1];
                        forward.z    = 0.0f;
                        if (glm::length(forward) > 1e-3f) {
                            forward            = glm::normalize(forward);
                            const auto neck    = pose.GameWorld((*g_active_bones).neck).t;
                            auto       delta   = eye - forward * kNeckBehindEyes - neck;
                            delta.z            = 0.0f;
                            const float length = glm::length(delta);
                            if (length < 1.0f) {
                                auto body = delta;
                                if (GameFlow::gStore.internalSettings.bodyLean) {
                                    body   = length > kLeanReach ? delta * ((length - kLeanReach) / length) : glm::vec3{ 0.0f };
                                    g_lean = delta - body;
                                }
                                shifted_root.translate.x += body.x;
                                shifted_root.translate.y += body.y;
                            }
                        }
                    }
                    if (world_camera && world_camera->parent) {
                        tracking::UpdateTrackingScale(world_camera->parent->world.translate.z - root_local->translate.z);
                    }
                    pose.root   = FromNi(shifted_root);
                    use_shifted = true;

                    // Raw arm chain as read before any write, once per second.
                    static std::chrono::steady_clock::time_point last_dump{};
                    if (const auto now = std::chrono::steady_clock::now(); (*g_active_bones).r_wrist >= 0 && now - last_dump > std::chrono::seconds(1)) {
                        last_dump = now;
                        std::string chain;
                        int         i = (*g_active_bones).r_wrist;
                        for (int guard = 0; guard < 16 && i >= 0 && i < pose.count; ++guard) {
                            const auto& l = pose.local[i];
                            const auto& w = pose.world[i];
                            chain += std::format(" [{} p{} lt({:.3f},{:.3f},{:.3f}) ls{:.3f} lr0({:.2f},{:.2f},{:.2f}) wt({:.3f},{:.3f},{:.3f})]", i,
                                i < pose.top ? -1 : pose.parent[i], l.translate.x, l.translate.y, l.translate.z, l.scale, l.rotate.entry[0].pt[0],
                                l.rotate.entry[0].pt[1], l.rotate.entry[0].pt[2], w.translate.x, w.translate.y, w.translate.z);
                            if (i < pose.top) {
                                break;
                            }
                            i = pose.parent[i];
                        }
                        spdlog::info("[BodyIK] wrist chain:{}", chain);
                    }
                    if (g_calibration.status == Calibration::Status::Ready) {
                        ApplyIK(pose);
                    }
                }
            }
            auto result = g_model_update_hook.call<void*>(model, use_shifted ? &shifted_root : root_local, data, out);
            if (pose.local) {
                // The body's shoulders as rendered this frame (the first-person arms attach there).
                const auto& b = *g_active_bones;
                if (b.left.clavicle >= 0 && b.right.clavicle >= 0 && b.neck >= 0 && b.pelvis >= 0) {
                    g_body_shoulders.left  = FromNi(pose.world[b.left.clavicle]).t;
                    g_body_shoulders.right = FromNi(pose.world[b.right.clavicle]).t;
                    g_body_shoulders.up    = glm::normalize(FromNi(pose.world[b.neck]).t - FromNi(pose.world[b.pelvis]).t);
                    g_body_shoulders.pelvis = FromNi(pose.world[b.pelvis]).t;
                    g_body_shoulders.chest  = b.chest >= 0 ? FromNi(pose.world[b.chest]).t : (g_body_shoulders.left + g_body_shoulders.right) * 0.5f;
                    auto forward            = pose.root.r[1];
                    forward.z               = 0.0f;
                    if (glm::length(forward) > 1e-3f) {
                        g_body_shoulders.forward = glm::normalize(forward);
                    }
                    g_body_shoulders.valid = true;
                }
            }

            if (pose.local && (*g_active_bones).r_wrist >= 0 && (*g_active_bones).r_wrist_node) {
                // Exact pose -> world transform from this update: synced node world vs engine pose world.
                const auto engine_wrist = FromNi(pose.world[(*g_active_bones).r_wrist]);
                const auto node_wrist   = FromNi((*g_active_bones).r_wrist_node->world);
                const auto measured     = Compose(node_wrist, Inverse(engine_wrist));
                // A real bone transform has a proper rotation, unit-ish scale and the wrist within arm's reach of the root.
                const float det         = glm::determinant(engine_wrist.r);
                const bool  plausible   = std::abs(det - 1.0f) < 0.05f && engine_wrist.s > 0.5f && engine_wrist.s < 2.0f &&
                                       glm::length(engine_wrist.t - pose.root.t) > 0.2f && glm::length(engine_wrist.t - pose.root.t) < 3.0f &&
                                       std::isfinite(measured.t.x) &&
                                       std::isfinite(measured.t.y) && std::isfinite(measured.t.z);

                if (g_calibration.status == Calibration::Status::Measuring && !plausible) {
                    ++g_calibration.frames;
                    if (g_calibration.frames % 30 == 1) {
                        spdlog::info("[BodyIK] calibration frame {}: wrist pose data implausible (det {:.3f} scale {:.3f} t ({:.3f},{:.3f},{:.3f}))",
                            g_calibration.frames, det, engine_wrist.s, engine_wrist.t.x, engine_wrist.t.y, engine_wrist.t.z);
                    }
                    if (g_calibration.frames > 600) {
                        g_calibration.status = Calibration::Status::Failed;
                        spdlog::error("[BodyIK] calibration failed; IK stays off");
                    }
                } else if (g_calibration.status == Calibration::Status::Measuring) {
                    // Nothing was written this frame, so every variant is scored against the engine's own result.
                    float error[kWorldVariants]{};
                    for (int variant = 0; variant < kWorldVariants; ++variant) {
                        error[variant] = variant == 4 && !g_calibration.has_measured
                                             ? 1e9f
                                             : glm::length(Compose(ToWorldVariant(variant), engine_wrist).t - node_wrist.t);
                    }
                    // Prefer an exact rule; the previous-frame measurement only when no rule fits.
                    int best = 0;
                    for (int variant = 1; variant < 4; ++variant) {
                        if (error[variant] < error[best]) {
                            best = variant;
                        }
                    }
                    if (error[best] > 0.005f && error[4] < 0.05f) {
                        best = 4;
                    }
                    const bool good = error[best] < (best == 4 ? 0.05f : 0.005f);
                    g_calibration.good_frames   = good && best == g_calibration.world_variant ? g_calibration.good_frames + 1 : (good ? 1 : 0);
                    g_calibration.world_variant = best;
                    ++g_calibration.frames;
                    if (g_calibration.frames % 30 == 1 || g_calibration.good_frames >= 10) {
                        const auto root = ToWorldVariant(0);
                        spdlog::info("[BodyIK] calibration frame {}: errors {:.4f}/{:.4f}/{:.4f}/{:.4f}/{:.4f} -> {} | measured vs root: "
                                     "dt ({:.3f},{:.3f},{:.3f}) angle {:.1f} (transposed {:.1f}, identity {:.1f}) scale {:.3f}/{:.3f}",
                            g_calibration.frames, error[0], error[1], error[2], error[3], error[4], kWorldVariantNames[best], measured.t.x - root.t.x,
                            measured.t.y - root.t.y, measured.t.z - root.t.z, AngleBetween(measured.r, root.r),
                            AngleBetween(measured.r, glm::transpose(root.r)), AngleBetween(measured.r, glm::mat3{ 1.0f }), measured.s, root.s);
                    }
                    if (g_calibration.good_frames >= 10) {
                        g_calibration.status = Calibration::Status::Ready;
                        spdlog::info("[BodyIK] calibrated ({}); IK enabled", kWorldVariantNames[best]);
                    } else if (g_calibration.frames > 600) {
                        g_calibration.status = Calibration::Status::Failed;
                        spdlog::error("[BodyIK] calibration failed; IK stays off");
                    }
                }
                if (plausible) {
                    g_calibration.measured     = measured;
                    g_calibration.has_measured = true;
                }
            }

            if (pose.local && g_calibration.status == Calibration::Status::Ready && (*g_active_bones).r_wrist >= 0) {
                const auto mine   = pose.World((*g_active_bones).r_wrist).t;
                const auto engine = FromNi(pose.world[(*g_active_bones).r_wrist]).t;
                const float error = glm::length(mine - engine);


                static std::chrono::steady_clock::time_point last_log{};
                const auto                                   now = std::chrono::steady_clock::now();
                if (now - last_log > std::chrono::seconds(1)) {
                    last_log          = now;
                    auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
                    const auto eye    = world_camera ? ToVec(world_camera->world.translate) : glm::vec3{ 0.0f };
                    const auto r_sh   = (*g_active_bones).r_biceps >= 0 ? pose.GameWorld((*g_active_bones).r_biceps).t : glm::vec3{ 0.0f };
                    spdlog::info("[BodyIK] layout error {:.4f} | reach gap R {:.3f} L {:.3f} | weapon aligned {} barrel error {:.2f} deg placement error {:.3f} m | shoulder-eye ({:.2f},{:.2f},{:.2f}) | thread {}",
                        error, g_diag.right_gap, g_diag.left_gap, g_diag.weapon_aligned, g_barrel.error_deg, g_barrel.placement_error, r_sh.x - eye.x, r_sh.y - eye.y, r_sh.z - eye.z,
                        GetCurrentThreadId());
                    spdlog::info("[BodyIK] walk state {} speed {:.2f} m/s | lean {:.3f} m | facing {:.1f} deg | support {} ({:.2f} m) | fingers R trig {:.2f} grip {:.2f} thumb {} copied {} | L trig {:.2f} grip {:.2f} thumb {} copied {}",
                        g_walk.state, g_walk.speed, g_lean_applied, glm::degrees(g_facing_yaw), g_diag.support_held, g_diag.support_distance, g_finger_diag[1].trigger,
                        g_finger_diag[1].grip, g_finger_diag[1].thumb, g_finger_diag[1].copied, g_finger_diag[0].trigger, g_finger_diag[0].grip,
                        g_finger_diag[0].thumb, g_finger_diag[0].copied);
                    spdlog::info("[BodyIK] tracking: body supported {} active {} valid {} | torso {} elbows L {} R {} legs {} | fingers L {} (src {} bend {:.0f}) R {} (src {} bend {:.0f}) trigger R {:.2f}",
                        g_track_diag.supported, g_track_diag.active, g_track_diag.valid, g_track_diag.torso, g_track_diag.elbow[0], g_track_diag.elbow[1],
                        g_track_diag.legs, g_track_diag.fingers[0], g_track_diag.hand_source[0], g_track_diag.index_bend[0], g_track_diag.fingers[1],
                        g_track_diag.hand_source[1], g_track_diag.index_bend[1], g_finger_diag[1].trigger);
                    {
                        auto player_ref = CreationEngineSingletonManager::GetPlayerRef();
                        auto camera     = CreationEngineSingletonManager::GetPlayerCameraSingleton();
                        spdlog::info("[Body] native weapons: wanted {} mode {} live-body bit {} fp camera {} drawn {} | native grip {} hand in weapon ({:.3f},{:.3f},{:.3f})",
                            third_person_mode::g_wanted.load(), third_person_mode::Active(player_ref),
                            player_ref ? (third_person_mode::Flags(player_ref, third_person_mode::kLiveBodyFlags) & 8) != 0 : false,
                            camera && camera->IsInFirstPerson(), player_ref && player_ref->IsWeaponDrawn(), g_native_diag.native,
                            g_native_diag.hand_in_weapon.x, g_native_diag.hand_in_weapon.y, g_native_diag.hand_in_weapon.z);
                        spdlog::info("[Body] hand check: wrist off target {:.3f} m {:.1f} deg | weapon off target {:.3f} m | drawn hand in weapon ({:.3f},{:.3f},{:.3f}) | weapon model '{}' at ({:.3f},{:.3f},{:.3f}) in the weapon bone",
                            g_hand_check.wrist_error, g_hand_check.wrist_angle, g_hand_check.weapon_error, g_hand_check.drawn_hand_in_weapon.x,
                            g_hand_check.drawn_hand_in_weapon.y, g_hand_check.drawn_hand_in_weapon.z, g_hand_check.model_name, g_hand_check.model_offset.x,
                            g_hand_check.model_offset.y, g_hand_check.model_offset.z);
                    }
                    spdlog::info("[BodyIK] first-person arms: setting {} root hits {} body active {} shoulders {} | active {} live {} origin ({:.1f},{:.1f},{:.1f}) root ({:.2f},{:.2f},{:.2f}) shoulders apart {:.2f} m {:.0f} deg, branch {}, after {:.3f} m | weapon {} support {}",
                        GameFlow::gStore.internalSettings.firstPersonArms, g_fp_root_hits.exchange(0), g_state.active, g_body_shoulders.valid,
                        g_fp_diag.active, FirstPersonArmsLive(), g_fp_diag.origin.x, g_fp_diag.origin.y, g_fp_diag.origin.z, g_fp_diag.root_translate.x,
                        g_fp_diag.root_translate.y, g_fp_diag.root_translate.z, g_fp_diag.attach_offset, g_fp_diag.attach_angle, g_fp_diag.branch,
                        g_fp_diag.attach_error, g_fp_diag.weapon,
                        g_fp_diag.support);
                    {
                        auto player = CreationEngineSingletonManager::GetPlayerRef();
                        spdlog::info("[Weapon] drawn {} | mag {} | slots L {} R {} | aiming {} | last {}", player && player->IsWeaponDrawn(),
                            static_cast<int>(g_actions.mag), g_actions.slot[0], g_actions.slot[1], g_actions.aiming, g_actions.last);
                    }
                }
            }
            return result;
        }
    }

    void OnUpdateWorld(RE::NiAVObject* obj, int engine_frame)
    {
        if (engine_frame != g_state.frame || obj == g_state.root) {
            auto player = CreationEngineSingletonManager::GetPlayerRef();
            auto root   = player ? ThirdPersonRoot(player) : nullptr;
            if (obj == root) {
                g_state.frame = engine_frame;
                UpdateVisibility(player, root);
            }
        }
    }

    void InstallModelNodeHook()
    {
        const auto address = MemoryScan::FuncRelocation("48 8B C4 53 57 41 56 41 57 48 81 EC", 0x2be93f0, 0);
        if (!address) {
            spdlog::error("[BodyIK] BSModelNode::UpdateTransforms not found");
            return;
        }
        g_model_update_hook = safetyhook::create_inline(reinterpret_cast<void*>(address), reinterpret_cast<void*>(&ModelNodeUpdateTransforms));
        third_person_mode::Install();
        spdlog::info("[BodyIK] model node hook: {}", static_cast<bool>(g_model_update_hook));
    }

    RE::NiAVObject* GetBodyMuzzle()
    {
        return g_body_muzzle.load();
    }

    bool GetBodyAimForward(float out[3])
    {
        if (!g_body_muzzle.load()) {
            return false;
        }
        out[0] = g_barrel.aim_forward.x;
        out[1] = g_barrel.aim_forward.y;
        out[2] = g_barrel.aim_forward.z;
        return true;
    }

    void NotifyPlayerFired()
    {
        g_fire_sample = true;
    }
}

namespace body
{
    bool OnFirstPersonRootUpdated(RE::NiAVObject* root, RE::NiUpdateData* data, int engine_frame)
    {
        static int last_frame{ -1 };
        if (engine_frame == last_frame) {
            return g_first_person_handled;
        }
        last_frame = engine_frame;
        return HandleFirstPersonRoot(root, data);
    }

    bool FirstPersonArmsActive()
    {
        return g_first_person_handled && FirstPersonArmsLive();
    }
}

namespace body
{
    void OnFrameStart()
    {
        third_person_mode::Update();
    }
}
