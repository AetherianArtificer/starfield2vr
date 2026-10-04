//
// Created by sergp on 6/23/2024.
//

#include "CreationEngineCameraManager.h"
#include "CreationEngineConstants.h"
#include "CreationEngineRendererModule.h"
#include "CreationEngineSingletonManager.h"
#include "RE/P/PlayerCamera.h"
#include "RE/S/ScaleformGFxMovie.h"
#include <CreationEngine/memory/offsets.h>
#include <CreationEngine/memory/ScanHelper.h>
#include <safetyhook/easy.hpp>
#include <CreationEngine/models/GameFlow.h>
#include <CreationEngine/models/ModSettingsStore.h>
#include <CreationEngine/ui/VRSettingsMenu.h>
#include <REL/Relocation.h>
#include <cstdlib>
#include <format>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <optional>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/vector_angle.hpp>
#include <mods/VR.hpp>

#include "ModSettings.h"

namespace {
    const  glm::mat4 permutation_pre = {
        1, 0, 0, 0,
        0, 0, 1, 0,
        0, -1, 0, 0,
        0, 0,  0, 1
    };
    const glm::mat4 permutation_post = glm::transpose(permutation_pre);

    glm::mat4 to_havok_space(const glm::mat4& mat) {
        return permutation_pre * mat * permutation_post;
    }

    glm::mat4 from_havok_space(const glm::mat4& mat) {
        return permutation_post * mat * permutation_pre;
    }
    float yaw_offset{0.0f};


}


void onSetNimFrustumDetour(RE::NiCamera *camera, RE::NiFrustum *frustum) {
    CreationEngineCameraManager::Get()->onSetNimFrustum(camera, frustum);
}

void onCalcNimFrustumDetour(RE::NiCamera *camera, float fov, float aspectRatio, float nearDist, float farDist,
                            char lodAdjust) {
    CreationEngineCameraManager::Get()->onCalcNiFrustum(camera, fov, aspectRatio, nearDist, farDist, lodAdjust);
}

void onScaleformSetViewPortDetour(uintptr_t *thisMovie, Scaleform::Gfx::Viewport *viewport) {
    CreationEngineCameraManager::Get()->onScaleformSetViewPort(thisMovie, viewport);
}

/*

void onScaleformMovieSetProjectionMatrix3DDetour(uintptr_t* thisMovie, Matrix4x4f& matrix)
{
    CreationEngineCameraManager::Get()->onScaleformMovieSetProjectionMatrix3D(thisMovie, matrix);
}
*/

bool isValidCamera(RE::NiCamera *pCamera) {
    static auto sceneGraphRoot = CreationEngineSingletonManager::GetSceneGraphRoot();
    return pCamera == sceneGraphRoot->worldCamera || pCamera == sceneGraphRoot->starfieldScene.pStarFieldCamera ||
           sceneGraphRoot->starfieldScene.pGalaxyCamera == pCamera;
}

namespace
{
    namespace body
    {
        void InstallModelNodeHook();
    }
}

void CreationEngineCameraManager::InstallHooks() {
    body::InstallModelNodeHook();
    REL::Relocation<uintptr_t> onNiAVObjectUpdateWorldAddr{GameStore::MemoryOffsets::BSFadeNode::vtable_UpdateWorld()};
    m_onNiAVObjectUpdateWorldHook = std::make_unique<FunctionHook>(onNiAVObjectUpdateWorldAddr.address(),
                                                                   reinterpret_cast<uintptr_t>(&onNiAVObjectUpdateWorld));
    m_onNiAVObjectUpdateWorldHook->create();

    REL::Relocation<uintptr_t> onGetCameraRotationAddr{ GameStore::MemoryOffsets::FirstPersonState::GetRotationQuatV() };
    m_onGetCameraRotationHook = std::make_unique<FunctionHook>(onGetCameraRotationAddr.address(), reinterpret_cast<uintptr_t>(&onFPSGetCameraRotation));
    m_onGetCameraRotationHook->create();


    REL::Relocation<uintptr_t> setNimFrustumVFuncAddr{GameStore::MemoryOffsets::NiCamera::SetFrustumVfunc()};
    m_onSetNimFrustumHook = std::make_unique<FunctionHook>(setNimFrustumVFuncAddr.address(),
                                                           reinterpret_cast<uintptr_t>(&onSetNimFrustumDetour));
    m_onSetNimFrustumHook->create();

    REL::Relocation<uintptr_t> calcNimFrustumVFuncAddr{GameStore::MemoryOffsets::NiCamera::CalcFrustumVfunc()};
    m_onCalcNimFrustumHook = std::make_unique<FunctionHook>(calcNimFrustumVFuncAddr.address(),
                                                            reinterpret_cast<uintptr_t>(&onCalcNimFrustumDetour));
    m_onCalcNimFrustumHook->create();

    REL::Relocation<uintptr_t> scaleformSetViewPortAddr{GameStore::MemoryOffsets::Scaleform::Movie::SetViewportVFunc()};
    m_onScaleformSetViewPortHook = std::make_unique<FunctionHook>(scaleformSetViewPortAddr.address(),
                                                                  reinterpret_cast<uintptr_t>(&onScaleformSetViewPortDetour));
    m_onScaleformSetViewPortHook->create();
}


namespace
{
    // Aim rotation (stage space) sampled once per engine frame, shared by the view, the aim and the meshes.
    std::mutex g_aim_mutex;
    glm::mat4  g_aim_rotation{ 1.0f };
    // The rotation last written into the game camera, and the engine frame it was written on.
    glm::mat4  g_applied_aim_rotation{ 1.0f };
    int        g_applied_aim_frame{ -1000 };
    // Right controller pose (stage space) sampled together with the aim rotation.
    glm::mat4  g_right_hand_pose{ 1.0f };

    // Grip poses (palm) of both controllers, sampled with the aim rotation.
    glm::mat4 g_grip_pose[2]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };
    glm::mat4 g_left_aim_pose{ 1.0f };

    glm::mat4 AimPose(bool left);

    glm::mat4 GripPose(bool left)
    {
        std::scoped_lock _{ g_aim_mutex };
        return g_grip_pose[left ? 0 : 1];
    }

    glm::mat4 RightHandPose()
    {
        std::scoped_lock _{ g_aim_mutex };
        return g_right_hand_pose;
    }

    glm::mat4 AimPose(bool left)
    {
        std::scoped_lock _{ g_aim_mutex };
        return left ? g_left_aim_pose : g_right_hand_pose;
    }

    // Tracking space: room orientation (game world, column-vector) maintained by the aim hook. It stays fixed
    // while only the head turns and changes with snap or game turning.
    std::mutex g_room_mutex;
    glm::mat3  g_room_rotation{ 1.0f };
    bool       g_has_room{ false };

    void SetRoomRotation(const RE::NiQuaternion& q)
    {
        std::scoped_lock _{ g_room_mutex };
        g_room_rotation = glm::mat3_cast(glm::normalize(glm::quat{ q.w, q.x, q.y, q.z }));
        g_has_room      = true;
    }

    // Camera-parent translation as seen when the view was last set, for timing diagnostics.
    glm::vec3 g_view_anchor{ 0.0f };
    int       g_view_anchor_frame{ -1 };

    // Eye position (stage space, relative to the standing origin) the view was set with this frame. The
    // first-person rig is drawn from the camera root without it, so the weapon is offset by it.
    std::mutex g_view_eye_mutex;
    glm::vec3  g_view_eye{ 0.0f };

    glm::vec3 ViewEyeOffset()
    {
        std::scoped_lock _{ g_view_eye_mutex };
        return g_view_eye;
    }

    std::optional<glm::mat3> RoomRotation()
    {
        std::scoped_lock _{ g_room_mutex };
        return g_has_room ? std::optional<glm::mat3>{ g_room_rotation } : std::nullopt;
    }

    // Stage-space position recentering was done at. The stored transform offset is the inverse of the recenter
    // pose, so its translation column is not this point.
    glm::vec3 StandingOriginPosition()
    {
        static auto vr = VR::get();
        return glm::vec3{ glm::inverse(vr->get_transform_offset())[3] };
    }

    // Scale applied to tracked head and hand movement so the player's eye height maps onto the character's.
    std::atomic<float> g_tracking_scale{ 1.0f };

    float TrackingScale() { return GameFlow::gStore.internalSettings.matchBodyHeight ? g_tracking_scale.load() : 1.0f; }

    // Re-measured after each recenter from the eye height above the floor (OpenXR stage space).
    void UpdateTrackingScale(float character_eye_height)
    {
        static glm::vec3 measured_origin{ -1000.0f };
        const auto       origin = StandingOriginPosition();
        if (glm::length(origin - measured_origin) < 1e-4f || character_eye_height < 0.5f || character_eye_height > 3.0f) {
            return;
        }
        static auto vr   = VR::get();
        const float real = vr->get_floor_eye_height();
        if (real < 0.0f) {
            return;  // floor height not known yet
        }
        measured_origin = origin;
        const float scale = real >= 1.3f && real <= 2.2f ? std::clamp(character_eye_height / real, 0.7f, 1.4f) : 1.0f;
        g_tracking_scale  = scale;
        spdlog::info("[Body] eye height: real {:.3f} m, character {:.3f} m -> tracking scale {:.3f}{}", real, character_eye_height, scale,
            real < 1.3f ? " (seated, not scaled)" : "");
    }

    // A stage-space vector in the game's havok axes.
    glm::vec3 ToHavokVector(const glm::vec3& v)
    {
        return glm::vec3{ to_havok_space(glm::translate(glm::mat4{ 1.0f }, v))[3] };
    }

    // The aim rotation currently baked into the game camera's parent transform.
    glm::mat4 AppliedAimRotation()
    {
        std::scoped_lock _{ g_aim_mutex };
        return g_applied_aim_rotation;
    }

    glm::mat4 AimRotation()
    {
        std::scoped_lock _{ g_aim_mutex };
        return g_aim_rotation;
    }

    void RecordAppliedAim(const glm::mat4& rotation, int frame)
    {
        std::scoped_lock _{ g_aim_mutex };
        g_applied_aim_rotation = rotation;
        g_applied_aim_frame    = frame;
    }

    // The rotation the camera's parent will carry this frame. If the game stopped updating its camera
    // (paused by a menu or popup), it still carries the last applied one.
    glm::mat4 CameraParentAimRotation(int frame)
    {
        std::scoped_lock _{ g_aim_mutex };
        return frame - g_applied_aim_frame <= 2 ? g_aim_rotation : g_applied_aim_rotation;
    }
}

void CreationEngineCameraManager::SnapshotAimPose() {
    static auto vr = VR::get();
    const bool hand = ModConstants::headTrackingType == ModConstants::kAimWithRightHand && vr->is_using_controllers();
    const auto rotation   = vr->get_rotation(hand ? vr->get_right_controller_index() : 0);
    const auto right_hand = vr->get_transform(vr->get_right_controller_index());
    const auto left_grip  = vr->get_grip_transform(vr->get_left_controller_index());
    const auto left_aim   = vr->get_transform(vr->get_left_controller_index());
    const auto right_grip = vr->get_grip_transform(vr->get_right_controller_index());
    std::scoped_lock _{ g_aim_mutex };
    g_grip_pose[0]    = left_grip;
    g_left_aim_pose   = left_aim;
    g_grip_pose[1]    = right_grip;
    g_aim_rotation    = rotation;
    g_right_hand_pose = right_hand;
}

RE::NiAVObject *getCameraRootNode() {
    auto playerCamera = CreationEngineSingletonManager::GetPlayerCameraSingleton();

    if (!playerCamera || !playerCamera->IsInFirstPerson()) {
        return nullptr;
    }
    RE::NiAVObject *a_camera = playerCamera->pFirstPersonModeState->cameraRoot;
    while (a_camera) {
        if (a_camera->name == "Root") {
            return a_camera;
        }
        a_camera = a_camera->parent;
    }
    return nullptr;
}

void UpdateMesh(RE::NiAVObject* camera) {
    if (GameFlow::isImmovable() || GameFlow::isControlledByAI()) {
        return;
    }

    if(GameFlow::gStore.internalSettings.decoupledPitch && !(ModConstants::headTrackingType == 0 && GameFlow::isAimingDownSights())) {
        return;
    }
    static auto vr = VR::get();
    auto camera_quat = RE::NiQuaternion(camera->world.rotate);
    auto glm_camera_quat = glm::normalize(glm::quat(camera_quat.w, camera_quat.x, camera_quat.y, camera_quat.z));

    auto current_hmd_rotation = AimRotation();
    auto hmd_rotation_quat = glm::normalize(
            glm::quat_cast(current_hmd_rotation));
    hmd_rotation_quat = {hmd_rotation_quat.w, hmd_rotation_quat.x, -hmd_rotation_quat.z, hmd_rotation_quat.y};
    auto quat_out = glm_camera_quat;
    {
        if (GameFlow::gStore.internalSettings.pawnControl) {
            quat_out = glm::rotate(quat_out, -yaw_offset, glm::vec3{0.0f, 0.0f, 1.0f});
        }
        quat_out = quat_out * hmd_rotation_quat;
        camera_quat = RE::NiQuaternion(quat_out.w, quat_out.x, quat_out.y, quat_out.z);
        camera_quat.ToMatrix(camera->world.rotate);
    }
}

namespace
{
    bool IsDescendant(const RE::NiAVObject* node, const RE::NiAVObject* ancestor)
    {
        for (auto n = node; n; n = n->parent) {
            if (n == ancestor) {
                return true;
            }
        }
        return false;
    }

    // NiTransform rotations are stored for row vectors: the engine composes world = local * parent
    // (NiAVObject::UpdateWorldData -> 0x316A50(&parent.world, &out, &local)), so the column-vector matrix is
    // the transpose of the stored one.
    bool RowVectorConvention() { return true; }

    // Rotation of an engine transform as a column-vector matrix (R * v).
    RE::NiMatrix3 RotationOf(const RE::NiTransform& t) { return RowVectorConvention() ? t.rotate.Transpose() : t.rotate; }

    // Column-vector rotation converted to the engine's storage.
    RE::NiMatrix3 ToEngineRotation(const RE::NiMatrix3& r) { return RowVectorConvention() ? r.Transpose() : r; }

    // Right controller position in world space, placed relative to the camera's parent like the eye is.
    // Right controller in world space: eye anchor + room rotation * (hand relative to the standing origin).
    // Uses the same anchor, room rotation and pose snapshot as the view, so head motion cannot move it.
    std::optional<RE::NiPoint3> RightHandWorldPosition()
    {
        auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
        const auto room   = RoomRotation();
        if (!world_camera || !world_camera->parent || !room) {
            return std::nullopt;
        }
        static auto vr     = VR::get();
        const auto  hand   = RightHandPose();
        const auto  offset = *room * ToHavokVector((glm::vec3{ hand[3] } - StandingOriginPosition()) * TrackingScale());
        const auto& anchor = world_camera->parent->world;
        return RE::NiPoint3{ anchor.translate.x + offset.x * anchor.scale, anchor.translate.y + offset.y * anchor.scale,
            anchor.translate.z + offset.z * anchor.scale };
    }

    glm::vec3 ToVec(const RE::NiPoint3& p) { return { p.x, p.y, p.z }; }


    // A direction given in the right controller's own space (OpenXR: -Z forward, +Y up), in world space.
    // A direction given in the right controller's own space (OpenXR: -Z forward, +Y up), in world space.
    std::optional<glm::vec3> HandDirectionWorld(const glm::vec3& local_dir)
    {
        const auto room = RoomRotation();
        if (!room) {
            return std::nullopt;
        }
        const auto hand = RightHandPose();
        return glm::normalize(*room * ToHavokVector(glm::mat3{ hand } * local_dir));
    }

    RE::NiPoint3 ToPoint(const glm::vec3& v) { return { v.x, v.y, v.z }; }

    RE::NiMatrix3 FromColumns(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
    {
        RE::NiMatrix3 m{};
        for (int r = 0; r < 3; ++r) {
            m.entry[r].pt[0] = a[r];
            m.entry[r].pt[1] = b[r];
            m.entry[r].pt[2] = c[r];
            m.entry[r].pt[3] = 0.0f;
        }
        return m;
    }

    // Right-handed frame (right, forward, up) from a forward direction and an approximate up.
    RE::NiMatrix3 Frame(glm::vec3 forward, glm::vec3 up)
    {
        forward = glm::normalize(forward);
        glm::vec3 right = glm::cross(forward, up);
        if (glm::length(right) < 1e-4f) {
            right = glm::cross(forward, glm::vec3{ 0.0f, 0.0f, 1.0f });
        }
        right = glm::normalize(right);
        up    = glm::cross(right, forward);
        return FromColumns(right, forward, up);
    }

    // Leaf objects have no child array; read it under SEH so a non-node never crashes the walk.
    std::uint16_t ReadChildren(const RE::NiAVObject* node, RE::NiAVObject** out, std::uint16_t capacity)
    {
        std::uint16_t count = 0;
        __try {
            auto       as_node = reinterpret_cast<const RE::NiNode*>(node);
            const auto size    = as_node->children.m_size;
            if (as_node->children.entries && size <= as_node->children.m_arrayBufLen && size <= capacity) {
                for (std::uint16_t i = 0; i < size; ++i) {
                    out[count++] = as_node->children.entries[i];
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            count = 0;
        }
        return count;
    }

    // The later bone pass reads local transforms, so arms are hidden by collapsing the clavicles' local scale.
    void SetArmsHidden(RE::NiAVObject* node, bool hidden, int depth)
    {
        if (!node || depth > 8) {
            return;
        }
        const std::string_view name{ node->name.c_str() };
        if (name == "L_Clavicle" || name == "R_Clavicle") {
            node->local.scale = hidden ? 0.0f : 1.0f;
            return;
        }
        RE::NiAVObject* children[64]{};
        const auto      count = ReadChildren(node, children, 64);
        for (std::uint16_t i = 0; i < count; ++i) {
            SetArmsHidden(children[i], hidden, depth + 1);
        }
    }

    RE::NiAVObject* FindChild(RE::NiAVObject* node, std::string_view name)
    {
        RE::NiAVObject* children[64]{};
        const auto      count = ReadChildren(node, children, 64);
        for (std::uint16_t i = 0; i < count; ++i) {
            if (children[i] && name == children[i]->name.c_str()) {
                return children[i];
            }
        }
        return nullptr;
    }

    // Transform of `node` relative to `ancestor`, built from this frame's local transforms.
    RE::NiTransform RelativeTo(const RE::NiAVObject* node, const RE::NiAVObject* ancestor)
    {
        RE::NiTransform result{};
        result.rotate = RE::NiMatrix3{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 4; ++j) {
                result.rotate.entry[i].pt[j] = (i == j) ? 1.0f : 0.0f;
            }
        }
        result.translate = RE::NiPoint3{};
        result.scale     = 1.0f;
        for (auto n = node; n && n != ancestor; n = n->parent) {
            RE::NiTransform local = n->local;
            local.rotate          = RotationOf(n->local);
            result                = local * result;
        }
        return result;
    }

    // The first-person rig is updated with the player translation removed (bMove1stPersonToOrigin): each node's
    // world = parent world * local, render data is captured as each node updates, and the translation is added
    // back afterwards. The hook runs for the rig root (COM) before its descendants, so setting COM's
    // origin-relative world here places the whole rig, weapon and attached nodes included, for this frame.
    RE::NiAVObject* FindDescendant(RE::NiAVObject* node, std::string_view name, int depth = 0)
    {
        if (!node || depth > 40) {
            return nullptr;
        }
        RE::NiAVObject* children[128]{};
        const auto      count = ReadChildren(node, children, 128);
        for (std::uint16_t i = 0; i < count; ++i) {
            if (children[i] && name == children[i]->name.c_str()) {
                return children[i];
            }
        }
        for (std::uint16_t i = 0; i < count; ++i) {
            if (auto found = FindDescendant(children[i], name, depth + 1)) {
                return found;
            }
        }
        return nullptr;
    }

    // Third-person body shown in first person, as the game does for furniture that uses the 3P rig in first
    // person; arms are posed with IK on the skeleton's pose buffer.
    namespace body
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
                SetAppCulled(first_person, true);
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

        // Column-vector rigid transform with uniform scale: x -> r * (s * x) + t.
        struct Xf
        {
            glm::mat3 r{ 1.0f };
            glm::vec3 t{ 0.0f };
            float     s{ 1.0f };
        };

        // Stored rotations are row-vector matrices; glm column a equals stored row a.
        Xf FromNi(const RE::NiTransform& n)
        {
            Xf x;
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    x.r[a][b] = n.rotate.entry[a].pt[b];
                }
            }
            x.t = { n.translate.x, n.translate.y, n.translate.z };
            x.s = n.scale;
            return x;
        }

        void ToNi(const Xf& in, RE::NiTransform& n)
        {
            Xf x = in;
            x.r[0] = glm::normalize(x.r[0]);
            x.r[1] = glm::normalize(x.r[1] - glm::dot(x.r[1], x.r[0]) * x.r[0]);
            x.r[2] = glm::cross(x.r[0], x.r[1]);
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    n.rotate.entry[a].pt[b] = x.r[a][b];
                }
                n.rotate.entry[a].pt[3] = 0.0f;
            }
            n.translate = { x.t.x, x.t.y, x.t.z };
            n.scale     = x.s;
        }

        Xf Compose(const Xf& parent, const Xf& local)
        {
            return { parent.r * local.r, parent.t + parent.s * (parent.r * local.t), parent.s * local.s };
        }

        Xf Inverse(const Xf& x)
        {
            const auto rt = glm::transpose(x.r);
            const auto s  = x.s != 0.0f ? 1.0f / x.s : 1.0f;
            return { rt, -(rt * x.t) * s, s };
        }

        glm::mat3 RotationBetween(glm::vec3 from, glm::vec3 to)
        {
            from = glm::normalize(from);
            to   = glm::normalize(to);
            return glm::mat3_cast(glm::rotation(from, to));
        }

        glm::mat3 FrameOf(glm::vec3 forward, glm::vec3 up)
        {
            forward         = glm::normalize(forward);
            glm::vec3 right = glm::cross(forward, up);
            if (glm::length(right) < 1e-4f) {
                right = glm::cross(forward, glm::vec3{ 0.0f, 0.0f, 1.0f });
            }
            right = glm::normalize(right);
            return glm::mat3{ right, forward, glm::cross(right, forward) };
        }

        // Rotation from RelativeTo (column-vector convention) as glm.
        glm::mat3 ToGlm(const RE::NiMatrix3& m)
        {
            glm::mat3 r{};
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    r[col][row] = m.entry[row].pt[col];
                }
            }
            return r;
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
            const auto room   = RoomRotation();
            if (!world_camera || !world_camera->parent || !room) {
                return std::nullopt;
            }
            const auto& anchor = world_camera->parent->world;
            const auto  offset = *room * ToHavokVector((glm::vec3{ pose[3] } - StandingOriginPosition()) * TrackingScale()) * anchor.scale;
            const auto  rot    = glm::mat3{ pose };
            return HandTarget{ glm::vec3{ anchor.translate.x, anchor.translate.y, anchor.translate.z } + offset,
                glm::normalize(*room * ToHavokVector(rot * glm::vec3{ 0.0f, 0.0f, -1.0f })),
                glm::normalize(*room * ToHavokVector(rot * glm::vec3{ 0.0f, 1.0f, 0.0f })) };
        }

        // BSModelNode pose storage (see local RE notes): locals feed the batched world computation.
        // Animation does not rewrite every local each frame; bones we wrote are restored to their animated
        // value first so the IK never builds on its own output.
        struct WrittenBone
        {
            RE::NiTransform animated{};
            RE::NiTransform written{};
        };
        std::unordered_map<int, WrittenBone> g_written;

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

            void ScaleTranslation(int i, float k) const
            {
                auto [entry, first] = g_written.try_emplace(i);
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
                bool       finite     = std::isfinite(result.s) && result.s > 0.0f && result.s < 10.0f && glm::length(result.t) < 10.0f;
                for (int a = 0; a < 3 && finite; ++a) {
                    finite = std::isfinite(result.t[a]) && std::isfinite(result.r[a][0]) && std::isfinite(result.r[a][1]) && std::isfinite(result.r[a][2]);
                }
                if (finite) {
                    auto [entry, first] = g_written.try_emplace(i);
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
            RE::NiAVObject* weapon_node{ nullptr };
            RE::NiAVObject* r_wrist_node{ nullptr };
            // Legacy accessors used by diagnostics.
            int r_biceps{ -1 }, r_forearm{ -1 }, r_wrist{ -1 }, l_wrist{ -1 };
        };
        Bones g_bones;

        bool Contains(std::string_view name, std::string_view part)
        {
            const auto it = std::search(name.begin(), name.end(), part.begin(), part.end(),
                [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
            return it != name.end();
        }

        void FindBones(const Pose& pose, const void* storage)
        {
            g_bones         = {};
            g_bones.storage = storage;
            std::vector<std::pair<int, std::string>> named;
            const auto entries = *reinterpret_cast<std::uint8_t**>(pose.model + 0x20);
            const auto count   = *reinterpret_cast<std::uint32_t*>(pose.model + 0x18);
            for (std::uint32_t e = 0; entries && e < count; ++e) {
                const auto idx  = *reinterpret_cast<std::uint16_t*>(entries + e * 16);
                const auto node = *reinterpret_cast<RE::NiAVObject**>(entries + e * 16 + 8);
                if (!node || idx >= pose.count) {
                    continue;
                }
                const std::string_view name{ node->name.c_str() };
                named.emplace_back(idx, std::string{ name });
                if (name == "R_Biceps") g_bones.right.biceps = idx;
                else if (name == "R_Forearm") g_bones.right.forearm = idx;
                else if (name == "R_Wrist") {
                    g_bones.right.wrist  = idx;
                    g_bones.r_wrist_node = node;
                }
                else if (name == "L_Biceps") g_bones.left.biceps = idx;
                else if (name == "L_Forearm") g_bones.left.forearm = idx;
                else if (name == "L_Wrist") g_bones.left.wrist = idx;
                else if (name == "C_Head") g_bones.head = idx;
                else if (name == "C_Neck") g_bones.neck = idx;
                else if (name == "L_Thigh") g_bones.left_leg.thigh = idx;
                else if (name == "L_Calf") g_bones.left_leg.calf = idx;
                else if (name == "L_Foot") g_bones.left_leg.foot = idx;
                else if (name == "R_Thigh") g_bones.right_leg.thigh = idx;
                else if (name == "R_Calf") g_bones.right_leg.calf = idx;
                else if (name == "R_Foot") g_bones.right_leg.foot = idx;
                else if (name == "C_Chest") g_bones.chest = idx;
                else if (name == "Weapon") {
                    g_bones.weapon      = idx;
                    g_bones.weapon_node = node;
                }
            }
            // Finger bases are the wrist's direct children.
            for (auto* arm : { &g_bones.right, &g_bones.left }) {
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
                spdlog::info("[BodyIK] {} wrist children:{}", arm == &g_bones.right ? "right" : "left", names);
            }
            for (auto* arm : { &g_bones.right, &g_bones.left }) {
                if (arm->biceps >= pose.top) {
                    arm->clavicle = pose.parent[arm->biceps];
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
            find_leg(g_bones.left_leg.thigh, "L_", "Thigh");
            find_leg(g_bones.left_leg.calf, "L_", "Calf");
            find_leg(g_bones.left_leg.foot, "L_", "Foot");
            find_leg(g_bones.right_leg.thigh, "R_", "Thigh");
            find_leg(g_bones.right_leg.calf, "R_", "Calf");
            find_leg(g_bones.right_leg.foot, "R_", "Foot");

            // Pelvis: the lowest common ancestor of a thigh and the neck.
            if (g_bones.left_leg.thigh >= 0 && g_bones.neck >= 0) {
                std::vector<int> neck_chain;
                for (int i = g_bones.neck, guard = 0; i >= 0 && guard < 64; ++guard) {
                    neck_chain.push_back(i);
                    i = i < pose.top ? -1 : pose.parent[i];
                }
                for (int i = g_bones.left_leg.thigh, guard = 0; i >= 0 && guard < 64; ++guard) {
                    if (std::find(neck_chain.begin(), neck_chain.end(), i) != neck_chain.end()) {
                        g_bones.pelvis = i;
                        break;
                    }
                    i = i < pose.top ? -1 : pose.parent[i];
                }
            }
            {
                std::string all;
                for (const auto& [idx, name] : named) {
                    all += " " + std::to_string(idx) + ":" + name;
                }
                spdlog::info("[BodyIK] bones:{}", all);
                const auto pelvis_name = std::find_if(named.begin(), named.end(), [](const auto& n) { return n.first == g_bones.pelvis; });
                spdlog::info("[BodyIK] legs L {} {} {} R {} {} {} | pelvis {} '{}'", g_bones.left_leg.thigh, g_bones.left_leg.calf, g_bones.left_leg.foot,
                    g_bones.right_leg.thigh, g_bones.right_leg.calf, g_bones.right_leg.foot, g_bones.pelvis,
                    pelvis_name != named.end() ? pelvis_name->second : "");
            }
            g_bones.r_biceps  = g_bones.right.biceps;
            g_bones.r_forearm = g_bones.right.forearm;
            g_bones.r_wrist   = g_bones.right.wrist;
            g_bones.l_wrist   = g_bones.left.wrist;
            spdlog::info("[BodyIK] {} pose bones (top {}): R {} {} {} thumb {} fingers {} | L {} {} {} thumb {} fingers {} | head {} neck {} weapon {}",
                pose.count, pose.top, g_bones.right.biceps, g_bones.right.forearm, g_bones.right.wrist, g_bones.right.thumb,
                g_bones.right.fingers.size(), g_bones.left.biceps, g_bones.left.forearm, g_bones.left.wrist, g_bones.left.thumb,
                g_bones.left.fingers.size(), g_bones.head, g_bones.neck, g_bones.weapon);
        }

        // Port of FRIK's solveArmToHandWorldTarget (Fallout 4 VR Body, GPL-3.0): shoulder reach, elbow flexion floor
        // and ceiling, wrist-driven elbow direction, twist split, exact hand. FO4 distances are in units of 1/70 m.
        constexpr float kUnit = 1.0f / 70.0f;

        float SolveArm(const Pose& pose, Arm& arm, bool is_left, const Xf& hand_target)
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

            const float chest_z     = g_bones.chest >= 0 ? pose.GameWorld(g_bones.chest).t.z : shoulder_bone.z;
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
            const auto  elbow_dir    = glm::vec3{ bend_down.x * std::cos(yaw) - bend_down.y * std::sin(yaw),
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
            const auto& l = g_bones.left_leg;
            const auto& r = g_bones.right_leg;
            if (!world_camera || !world_camera->parent || g_bones.pelvis < 0 || l.thigh < 0 || l.calf < 0 || l.foot < 0 || r.thigh < 0 ||
                r.calf < 0 || r.foot < 0) {
                return;
            }
            // The neck sits a fixed distance below the eyes, measured from the game's standing pose (camera highest
            // above the feet). Real and button crouches then share one rule.
            static float standing_eye{ 0.0f };
            static float neck_below_eye{ -1.0f };
            const float  game_eye   = world_camera->parent->world.translate.z - pose.root.t.z;
            const float  neck_z     = pose.GameWorld(g_bones.neck).t.z;
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
            const auto left_foot  = pose.GameWorld(l.foot);
            const auto right_foot = pose.GameWorld(r.foot);
            auto       pelvis     = pose.GameWorld(g_bones.pelvis);
            pelvis.t.z += drop;
            pose.SetGameWorld(g_bones.pelvis, pelvis);

            auto forward = pose.root.r[1];
            forward.z    = 0.0f;
            forward      = glm::length(forward) > 1e-3f ? glm::normalize(forward) : glm::vec3{ 0.0f, 1.0f, 0.0f };
            SolveLeg(pose, l, left_foot, forward);
            SolveLeg(pose, r, right_foot, forward);
        }

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
        FirstPersonHand g_first_hand;

        void MeasureFirstPersonHand(RE::NiAVObject* wrist)
        {
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
            spdlog::info("[BodyIK] 1P hand measured: forward ({:.2f},{:.2f},{:.2f}) up ({:.2f},{:.2f},{:.2f}) palm {:.3f}", g_first_hand.shape.forward.x,
                g_first_hand.shape.forward.y, g_first_hand.shape.forward.z, g_first_hand.shape.up.x, g_first_hand.shape.up.y, g_first_hand.shape.up.z,
                g_first_hand.shape.palm);
        }

        // The body's wrist transform equivalent to the first-person wrist: same anatomical frame, same palm centre.
        Xf BodyWristFromFirstPerson(const Xf& first_wrist, const HandShape& body)
        {
            const auto& f    = g_first_hand.shape;
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
            if (g_basis.weapon != g_bones.weapon_node) {
                g_basis = {};
                g_basis.weapon = g_bones.weapon_node;
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
            auto weapon = g_bones.weapon_node;
            if (!muzzle || !weapon) {
                g_barrel.has_relation = false;
                g_barrel.placed       = false;
                return;
            }
            const auto weapon_world = FromNi(weapon->world);
            const auto shot_world   = glm::normalize(FromNi(muzzle->world).r[1]);
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

        void RestoreAnimated(const Pose& pose)
        {
            for (auto it = g_written.begin(); it != g_written.end();) {
                if (it->first < pose.count && std::memcmp(&pose.local[it->first], &it->second.written, sizeof(RE::NiTransform)) == 0) {
                    pose.local[it->first] = it->second.animated;
                }
                it = g_written.erase(it);
            }
        }

        void ApplyIK(const Pose& pose)
        {
            if (g_bones.head >= 0) {
                pose.local[g_bones.head].scale = 0.0f;
            }
            MeasureHand(pose, g_bones.right, "right");
            MeasureHand(pose, g_bones.left, "left");
            MeasureElbow(pose, g_bones.right, "right");
            MeasureElbow(pose, g_bones.left, "left");
            ApplyCrouch(pose);

            const auto room = RoomRotation();
            if (!room) {
                return;
            }
            auto& r = g_bones.right;
            if (r.biceps >= 0 && r.forearm >= 0 && r.wrist >= 0 && r.shape.valid) {
                const auto grip = HandWorld(GripPose(false));
                const auto aim  = HandWorld(AimPose(false));
                if (grip && aim) {
                    auto wrist_target = WristTarget(pose, r, *aim, *grip);

                    g_diag.weapon_aligned = false;
                    auto player           = CreationEngineSingletonManager::GetPlayerRef();
                    if (player && player->IsWeaponDrawn() && g_bones.weapon >= 0) {
                        static RE::NiAVObject* first_root{ nullptr };
                        static RE::NiAVObject* first_wrist{ nullptr };
                        static RE::NiAVObject* first_weapon{ nullptr };
                        if (auto root = FirstPersonRoot(player); root != first_root) {
                            first_root   = root;
                            first_wrist  = root ? FindDescendant(root, "R_Wrist") : nullptr;
                            first_weapon = root ? FindDescendant(root, "Weapon") : nullptr;
                            spdlog::info("[BodyIK] 1P rig: R_Wrist {} Weapon {}", first_wrist != nullptr, first_weapon != nullptr);
                        }
                        auto muzzle   = g_bones.weapon_node ? FindDescendant(g_bones.weapon_node, "ProjectileNode") : nullptr;
                        g_body_muzzle = muzzle;
                        SampleWeaponBasis(first_weapon);
                        if (first_wrist && first_weapon) {
                            // The animation's grip: the hand in the weapon's space, transferred onto the body's hand by anatomy.
                            MeasureFirstPersonHand(first_wrist);
                            Xf first_hand_world = FromNi(first_wrist->world);
                            if (g_first_hand.shape.valid) {
                                first_hand_world = BodyWristFromFirstPerson(first_hand_world, r.shape);
                            }
                            auto hand_in_weapon = Compose(Inverse(FromNi(first_weapon->world)), first_hand_world);

                            // Weapon models carry an authored right-hand IK target (R_HandIk). The hand's offset from it is
                            // taken from the first-person rig and applied to the third-person model's own target.
                            auto first_ik = FindDescendant(first_weapon, "R_HandIk");
                            auto body_ik  = g_bones.weapon_node ? FindDescendant(g_bones.weapon_node, "R_HandIk") : nullptr;
                            static RE::NiAVObject* logged_ik{ nullptr };
                            if (logged_ik != g_bones.weapon_node) {
                                logged_ik = g_bones.weapon_node;
                                spdlog::info("[BodyIK] R_HandIk: 1P {} 3P {}", first_ik != nullptr, body_ik != nullptr);
                            }
                            if (first_ik && body_ik) {
                                const auto hand_from_ik = Compose(Inverse(FromNi(first_ik->world)), first_hand_world);
                                const auto ik_in_weapon = Compose(Inverse(FromNi(g_bones.weapon_node->world)), FromNi(body_ik->world));
                                hand_in_weapon          = Compose(ik_in_weapon, hand_from_ik);
                            }
                            Xf         weapon_xf      = Compose(wrist_target, Inverse(hand_in_weapon));
                            if (g_basis.has && g_basis.weapon == g_bones.weapon_node) {
                                // Weapon from the aim ray: barrel forward, weapon up along the controller's up.
                                weapon_xf.r = FrameOf(aim->forward, aim->up) * glm::transpose(FrameOf(g_basis.forward, g_basis.up));
                                // Hand from the weapon, then both seated so the palm centre is on the grip point.
                                wrist_target           = Compose(weapon_xf, hand_in_weapon);
                                const auto palm_world  = wrist_target.t + wrist_target.r * (r.shape.forward * r.shape.palm) * wrist_target.s;
                                const auto seat        = grip->position - palm_world;
                                weapon_xf.t += seat;
                                wrist_target.t += seat;
                            }
                            g_barrel.placed        = true;
                            g_barrel.placed_weapon = weapon_xf;
                            g_barrel.aim_forward   = aim->forward;
                            pose.SetGameWorld(g_bones.weapon, weapon_xf);
                            g_diag.weapon_aligned = true;
                        }
                    } else {
                        g_body_muzzle = nullptr;
                    }
                    g_diag.right_gap = SolveArm(pose, r, false, wrist_target);
                }
            }

            auto& l = g_bones.left;
            if (l.biceps >= 0 && l.forearm >= 0 && l.wrist >= 0 && l.shape.valid) {
                const auto grip = HandWorld(GripPose(true));
                const auto aim  = HandWorld(AimPose(true));
                if (grip && aim) {
                    g_diag.left_gap = SolveArm(pose, l, true, WristTarget(pose, l, *aim, *grip));
                }
            }
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
                    if (g_bones.storage != storage) {
                        FindBones(pose, storage);
                        g_written.clear();
                    }
                    RestoreAnimated(pose);
                    // Both transforms are final from the previous frame here.
                    MeasureBarrel();

                    // Body placed so the neck is under and behind the eyes (horizontal only).
                    constexpr float kNeckBehindEyes = 0.12f;
                    shifted_root                    = *root_local;
                    auto world_camera               = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
                    if (world_camera && g_bones.neck >= 0) {
                        auto forward = pose.root.r[1];
                        forward.z    = 0.0f;
                        if (glm::length(forward) > 1e-3f) {
                            forward            = glm::normalize(forward);
                            const auto eye     = ToVec(world_camera->world.translate);
                            const auto neck    = pose.GameWorld(g_bones.neck).t;
                            auto       delta   = eye - forward * kNeckBehindEyes - neck;
                            delta.z            = 0.0f;
                            if (glm::length(delta) < 1.0f) {
                                shifted_root.translate.x += delta.x;
                                shifted_root.translate.y += delta.y;
                            }
                        }
                    }
                    if (world_camera && world_camera->parent) {
                        UpdateTrackingScale(world_camera->parent->world.translate.z - root_local->translate.z);
                    }
                    pose.root   = FromNi(shifted_root);
                    use_shifted = true;

                    // Raw arm chain as read before any write, once per second.
                    static std::chrono::steady_clock::time_point last_dump{};
                    if (const auto now = std::chrono::steady_clock::now(); g_bones.r_wrist >= 0 && now - last_dump > std::chrono::seconds(1)) {
                        last_dump = now;
                        std::string chain;
                        int         i = g_bones.r_wrist;
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

            if (pose.local && g_bones.r_wrist >= 0 && g_bones.r_wrist_node) {
                // Exact pose -> world transform from this update: synced node world vs engine pose world.
                const auto engine_wrist = FromNi(pose.world[g_bones.r_wrist]);
                const auto node_wrist   = FromNi(g_bones.r_wrist_node->world);
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

            if (pose.local && g_calibration.status == Calibration::Status::Ready && g_bones.r_wrist >= 0) {
                const auto mine   = pose.World(g_bones.r_wrist).t;
                const auto engine = FromNi(pose.world[g_bones.r_wrist]).t;
                const float error = glm::length(mine - engine);


                static std::chrono::steady_clock::time_point last_log{};
                const auto                                   now = std::chrono::steady_clock::now();
                if (now - last_log > std::chrono::seconds(1)) {
                    last_log          = now;
                    auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
                    const auto eye    = world_camera ? ToVec(world_camera->world.translate) : glm::vec3{ 0.0f };
                    const auto r_sh   = g_bones.r_biceps >= 0 ? pose.GameWorld(g_bones.r_biceps).t : glm::vec3{ 0.0f };
                    spdlog::info("[BodyIK] layout error {:.4f} | reach gap R {:.3f} L {:.3f} | weapon aligned {} barrel error {:.2f} deg placement error {:.3f} m | shoulder-eye ({:.2f},{:.2f},{:.2f}) | thread {}",
                        error, g_diag.right_gap, g_diag.left_gap, g_diag.weapon_aligned, g_barrel.error_deg, g_barrel.placement_error, r_sh.x - eye.x, r_sh.y - eye.y, r_sh.z - eye.z,
                        GetCurrentThreadId());
                }
            }
            return result;
        }

        void InstallModelNodeHook()
        {
            const auto address = MemoryScan::FuncRelocation("48 8B C4 53 57 41 56 41 57 48 81 EC", 0x2be93f0, 0);
            if (!address) {
                spdlog::error("[BodyIK] BSModelNode::UpdateTransforms not found");
                return;
            }
            g_model_update_hook = safetyhook::create_inline(reinterpret_cast<void*>(address), reinterpret_cast<void*>(&ModelNodeUpdateTransforms));
            spdlog::info("[BodyIK] model node hook: {}", static_cast<bool>(g_model_update_hook));
        }
    }

    struct WeaponPlacement
    {
        bool            arms_hidden{ false };
        RE::NiAVObject* arms_root{ nullptr };
        bool            has_target{ false };
        glm::vec3       last_target{};  // world-space grip target set last frame
    };
    WeaponPlacement g_placement;

    void RestoreArms()
    {
        if (g_placement.arms_hidden && g_placement.arms_root) {
            SetArmsHidden(g_placement.arms_root, false, 0);
        }
        g_placement.arms_hidden = false;
        g_placement.has_target  = false;
    }

    template <class UpdateFn>
    bool PlaceWeaponInHand(RE::NiAVObject* rig_root, RE::NiUpdateData* data, UpdateFn update)
    {
        auto player = CreationEngineSingletonManager::GetPlayerRef();
        if (!player || !player->IsWeaponDrawn()) {
            RestoreArms();
            return false;
        }
        auto weapon = player->getEquippedWeaponRootNode().first;
        auto muzzle = player->getEquippedWeaponProjectile();
        auto grip   = weapon ? FindChild(weapon, "R_HandIk") : nullptr;
        if (!weapon || !muzzle || !grip) {
            RestoreArms();
            return false;
        }
        if (!IsDescendant(weapon, rig_root) || !IsDescendant(muzzle, rig_root)) {
            return false;  // another child of the rig root
        }

        // Descendants still hold last frame's final world transforms: measure where last frame's grip landed.
        const auto landed = ToVec(grip->world.translate);

        update(rig_root, data);

        auto world_camera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;
        if (!world_camera || !world_camera->parent) {
            return true;
        }

        const auto hand = RightHandWorldPosition();
        if (!hand) {
            return true;
        }

        // Origin offset the engine adds back after this pass.
        glm::vec3 origin{ 0.0f };
        if (auto offset = *reinterpret_cast<RE::NiPoint3**>(reinterpret_cast<std::uint8_t*>(data) + 0x28)) {
            origin = ToVec(*offset);
        }

        // This frame's weapon geometry relative to the rig root.
        const auto grip_rel   = RelativeTo(grip, rig_root);
        const auto weapon_rel = RelativeTo(weapon, rig_root);
        const auto muzzle_rel = RelativeTo(muzzle, rig_root);

        const auto weapon_p  = ToVec(weapon_rel.translate);
        const auto barrel    = ToVec(muzzle_rel.translate) - weapon_p;
        if (glm::length(barrel) < 0.01f) {
            return true;
        }

        // The hand IK target is animated (idle sway); use its offset on the weapon from when the weapon was drawn.
        static const RE::NiAVObject* grip_weapon{ nullptr };
        static RE::NiPoint3          grip_offset{};  // in the weapon's own frame
        const float                  weapon_scale = weapon_rel.scale != 0.0f ? weapon_rel.scale : 1.0f;
        if (grip_weapon != weapon) {
            grip_weapon = weapon;
            const auto d = ToPoint((ToVec(grip_rel.translate) - weapon_p) / weapon_scale);
            grip_offset  = weapon_rel.rotate.Transpose() * d;
        }
        const auto grip_p = weapon_p + ToVec(weapon_rel.rotate * grip_offset) * weapon_scale;

        // Weapon frame: barrel direction, and the model's own up axis.
        const auto forward_w = glm::normalize(barrel);
        auto       up_w      = ToVec(weapon_rel.rotate * RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
        up_w                 = up_w - glm::dot(up_w, forward_w) * forward_w;
        if (glm::length(up_w) < 0.01f) {
            return true;
        }
        up_w = glm::normalize(up_w);

        // Controller frame, converted exactly like the hand position (world rotation == origin-relative rotation).
        const auto forward_h = HandDirectionWorld(glm::vec3{ 0.0f, 0.0f, -1.0f });
        const auto up_h      = HandDirectionWorld(glm::vec3{ 0.0f, 1.0f, 0.0f });
        if (!forward_h || !up_h) {
            return true;
        }
        const auto target = ToVec(*hand) - origin;

        // COM.world = M with M(grip_p) = target and the weapon frame turned onto the controller frame.
        // Keep the rig's own scale; positions relative to COM scale with it.
        const float rig_scale     = rig_root->world.scale != 0.0f ? rig_root->world.scale : 1.0f;
        const auto  rotation      = Frame(*forward_h, *up_h) * Frame(forward_w, up_w).Transpose();
        rig_root->world.rotate    = ToEngineRotation(rotation);
        rig_root->world.translate = ToPoint(target - ToVec(rotation * ToPoint(grip_p * rig_scale)));

        g_placement.arms_root   = rig_root;
        SetArmsHidden(rig_root, true, 0);
        g_placement.arms_hidden = true;

        static std::chrono::steady_clock::time_point last_log{};
        const auto                                   now = std::chrono::steady_clock::now();
        if (g_placement.has_target && now - last_log > std::chrono::seconds(1)) {
            last_log = now;
            spdlog::info("[WeaponHand] last frame's grip landed {:.3f} m from its target | origin ({:.2f},{:.2f},{:.2f}) target ({:.2f},{:.2f},{:.2f})",
                glm::length(landed - g_placement.last_target), origin.x, origin.y, origin.z, target.x, target.y, target.z);
            if (const auto room = RoomRotation()) {
                const auto camera_rotation = RotationOf(world_camera->parent->world);
                glm::mat3  camera_col{};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        camera_col[c][r] = camera_rotation.entry[r].pt[c];
                    }
                }
                const glm::mat3 implied = camera_col * glm::mat3{ to_havok_space(glm::inverse(AppliedAimRotation())) };
                const glm::mat3 delta   = *room * glm::transpose(implied);
                const float     angle   = std::acos(std::clamp((delta[0][0] + delta[1][1] + delta[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f));
                spdlog::info("[WeaponHand] room rotation vs camera-implied: {:.2f} deg", glm::degrees(angle));
            }
            spdlog::info("[WeaponHand] hand fwd ({:.2f},{:.2f},{:.2f}) up ({:.2f},{:.2f},{:.2f}) | weapon fwd ({:.2f},{:.2f},{:.2f}) up ({:.2f},{:.2f},{:.2f}) | grip_rel ({:.2f},{:.2f},{:.2f})",
                forward_h->x, forward_h->y, forward_h->z, up_h->x, up_h->y, up_h->z, forward_w.x, forward_w.y, forward_w.z, up_w.x, up_w.y, up_w.z,
                grip_p.x, grip_p.y, grip_p.z);
        }
        static std::chrono::steady_clock::time_point last_trace{};
        if (const auto room = RoomRotation(); room && now - last_trace > std::chrono::milliseconds(100)) {
            last_trace           = now;
            static auto vr       = VR::get();
            const auto  origin_s = StandingOriginPosition();
            const auto  hmd_t    = glm::vec3{ vr->get_transform(0)[3] } - origin_s;
            const auto  hand_t   = glm::vec3{ RightHandPose()[3] } - origin_s;
            const auto  anchor   = ToVec(world_camera->parent->world.translate);
            const auto  eye      = ToVec(world_camera->world.translate);
            const auto  expected = *room * ToHavokVector(hmd_t) * world_camera->parent->world.scale;
            const float yaw      = glm::degrees(std::atan2((*room)[0][1], (*room)[0][0]));
            spdlog::info("[HeadTrace] f={} vf={} hmd=({:.3f},{:.3f},{:.3f}) hand=({:.3f},{:.3f},{:.3f}) anchor-O=({:.3f},{:.3f},{:.3f}) "
                         "anchor-viewanchor={:.4f} eye-anchor=({:.3f},{:.3f},{:.3f}) expected=({:.3f},{:.3f},{:.3f}) roomyaw={:.2f} scale={:.3f} parent='{}'",
                vr->m_engine_frame_count, g_view_anchor_frame, hmd_t.x, hmd_t.y, hmd_t.z, hand_t.x, hand_t.y, hand_t.z, anchor.x - origin.x,
                anchor.y - origin.y, anchor.z - origin.z, glm::length(anchor - g_view_anchor), eye.x - anchor.x, eye.y - anchor.y, eye.z - anchor.z,
                expected.x, expected.y, expected.z, yaw, world_camera->parent->world.scale, world_camera->parent->name.c_str());
        }
        g_placement.last_target = target + origin;
        g_placement.has_target  = true;
        return true;
    }
}

void CreationEngineCameraManager::onNiAVObjectUpdateWorld(RE::NiAVObject *obj, RE::NiUpdateData *a_data) {
    static auto instance = CreationEngineCameraManager::Get();
    using func_t = decltype(onNiAVObjectUpdateWorld);
    static auto original_func = instance->m_onNiAVObjectUpdateWorldHook->get_original<func_t>();
    static auto vr = VR::get();
    if (vr->is_hmd_active() && !ModSettings::showFlatScreenDisplay()) {
        // Third-person body: visibility at its root, then the head and right wrist as they update.
        if (vr->m_engine_frame_count != body::g_state.frame || obj == body::g_state.root) {
            auto player = CreationEngineSingletonManager::GetPlayerRef();
            auto root   = player ? body::ThirdPersonRoot(player) : nullptr;
            if (obj == root) {
                body::g_state.frame = vr->m_engine_frame_count;
                body::UpdateVisibility(player, root);
            }
        }

        auto camera_root = getCameraRootNode();
        if (obj->parent && camera_root && camera_root == obj->parent) {
            const bool is_mesh = !std::strstr(obj->name.c_str(), "Camera");
            if (is_mesh) {
                RestoreArms();
            }
            original_func(obj, a_data);
            if (is_mesh) {
                UpdateMesh(obj);
            }
            return;
        }
    }
    original_func(obj, a_data);
}

void CreationEngineCameraManager::onScaleformSetViewPort(uintptr_t *thisMovie, Scaleform::Gfx::Viewport *viewport) {
    using func_t = decltype(onScaleformSetViewPortDetour);
    static auto original_func = m_onScaleformSetViewPortHook->get_original<func_t>();
    onScaleformSetViewPortInternal(thisMovie, viewport);
    original_func(thisMovie, viewport);
}

void
CreationEngineCameraManager::onScaleformSetViewPortInternal(uintptr_t *thisMovie, Scaleform::Gfx::Viewport *viewport) {

    static auto vr = VR::get();
    auto cc = reinterpret_cast<RE::Scaleform::GFx::MovieImpl *>(thisMovie);
    auto file_url = cc->GetMovieDef()->GetFileURL();
    GameFlow::renderMenu(file_url);
    VRSettingsMenu::OnMovieFrame(thisMovie, file_url);

    auto backbuffer_size = vr->get_backbuffer_size();
    auto viewport_buffer_width = viewport->bufferWidth;
    auto viewport_buffer_height = viewport->bufferHeight;

    int offset_left = 0;
    int offset_top = 0;

    auto settings = GameFlow::getMenuSettings(file_url);

    if (ModSettings::showFlatScreenDisplay() || !vr->is_hmd_active()) {
        return;
    }

    auto width_multiplier = settings.hud_scale;
    auto height_multiplier = settings.hud_scale;

    // Implement offset based on dominant eye and menu-specific offset_value
    auto current_eye = vr->get_current_render_eye();
    if (ModConstants::dominantEye == 1) {
        // Dominant eye is right
        offset_left = (current_eye == VRRuntime::Eye::RIGHT) ? -settings.perspective : 0;
    } else {
        // Dominant eye is left
        offset_left = (current_eye == VRRuntime::Eye::LEFT) ? settings.perspective : 0;
    }

    auto visible_width = std::min((int) ((float) backbuffer_size[0] * width_multiplier), viewport_buffer_width);
    auto visible_height = std::min((int) ((float) backbuffer_size[1] * height_multiplier), viewport_buffer_height);
    viewport->width = visible_width;
    viewport->height = visible_height;
    viewport->left = (int) (viewport_buffer_width - visible_width) / 2 + offset_left;
    viewport->top = (int) (viewport_buffer_height - visible_height) / 2 + offset_top;
}

void CreationEngineCameraManager::onSetNimFrustum(RE::NiCamera *pCamera, RE::NiFrustum *pFrustum) {
    using func_t = decltype(onSetNimFrustumDetour);
    static auto original_func = m_onSetNimFrustumHook->get_original<func_t>();
    onSetNiFrustumInternal(pCamera, pFrustum);
    original_func(pCamera, pFrustum);
}

void aiming_adjustments(Vector4f &frustum, float fov_adjustment) {
    frustum[0] = std::tanf(std::atanf(frustum[0]) - fov_adjustment);
    frustum[1] = std::tanf(std::atanf(frustum[1]) + fov_adjustment);
    frustum[2] = std::tanf(std::atanf(frustum[2]) + fov_adjustment);
    frustum[3] = std::tanf(std::atanf(frustum[3]) - fov_adjustment);
}

void CreationEngineCameraManager::onSetNiFrustumInternal(RE::NiCamera *pCamera, RE::NiFrustum *pFrustum) {
    if (!isValidCamera(pCamera)) {
        return;
    }
    static auto vr = VR::get();
    if (!vr->is_hmd_active()) {
        return;
    }
    auto eye = vr->get_current_render_eye() == VRRuntime::Eye::LEFT ? 0 : 1;
    auto runtime = vr->get_runtime();
    Vector4f frustum = runtime->frustums[eye];
    aiming_adjustments(frustum, get_fov_adjustment());
    pFrustum->left = frustum[0];
    pFrustum->right = frustum[1];
    pFrustum->top = frustum[2];
    pFrustum->bottom = frustum[3];
}

void CreationEngineCameraManager::onCalcNiFrustum(RE::NiCamera *pCamera, float fov, float aspectRatio, float nearz,
                                                  float farz, char lodAdjust) {
    using func_t = decltype(onCalcNimFrustumDetour);
    static auto original_func = m_onCalcNimFrustumHook->get_original<func_t>();
    auto playerCamera = CreationEngineSingletonManager::GetPlayerCameraSingleton();

    //    spdlog::info("Setting frustum for camera [{}] {} camera[{} {}] setFov[{}]", fmt::ptr(pCamera), pCamera->name, playerCamera->fov, playerCamera->notViewFov, fov);
    static auto vr = VR::get();

    if (vr->is_hmd_active() && CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera == pCamera) {
        //        fov = fov + Constants::lodAdjustFov;
        m_fov_adjust = fov - playerCamera->fov;
        vr->m_nearz = nearz;
        vr->m_farz = farz;
    }
    original_func(pCamera, fov, aspectRatio, nearz, farz, lodAdjust);
}

float CreationEngineCameraManager::get_fov_adjustment() const {
    if (GameFlow::gStore.internalSettings.preventZoom) {
        return 0.0f;
    }
    return tanf((m_fov_adjust * ModConstants::DEG_TO_RAD) / 2.0f);
}

float CreationEngineCameraManager::get_head_tracking_multiplier() const {
    static auto playerCamera = CreationEngineSingletonManager::GetPlayerCameraSingleton();
    auto multiplier = (playerCamera->fov + m_fov_adjust) / playerCamera->fov;
    return multiplier * ModConstants::headTrackingMultiplier;
}


void CreationEngineCameraManager::UpdateWorldCamera() {
    static auto vr = VR::get();
    auto worldCamera = CreationEngineSingletonManager::GetSceneGraphRoot()->worldCamera;

    if (!worldCamera) {
        return;
    }

    static auto originalRotation = worldCamera->local.rotate;
    static auto originalPosition = worldCamera->local.translate;

    if (!vr->is_hmd_active() || ModConstants::cameraShake || ModSettings::showFlatScreenDisplay()) {
        worldCamera->local.rotate = originalRotation;
        worldCamera->local.translate = originalPosition;
        return;
    }

    if(!GameFlow::isImmovable() && !GameFlow::isControlledByAI() && GameFlow::isInFirstPerson()) {
        if (worldCamera->parent) {
            const auto& t       = worldCamera->parent->world.translate;
            g_view_anchor       = glm::vec3{ t.x, t.y, t.z };
            g_view_anchor_frame = vr->m_engine_frame_count;
        }
        auto hmd_transform = vr->get_transform(0);
        hmd_transform[3] -= glm::vec4{ StandingOriginPosition(), 0.0f };
        const float tracking_scale = TrackingScale();
        hmd_transform[3]           = glm::vec4{ glm::vec3{ hmd_transform[3] } * tracking_scale, 1.0f };
        auto eye = vr->get_current_eye_transform();
        eye[3]   = glm::vec4{ glm::vec3{ eye[3] } * tracking_scale, 1.0f };
        {
            std::scoped_lock _{ g_view_eye_mutex };
            g_view_eye = glm::vec3{ (hmd_transform * eye)[3] };
        }
        // The camera's parent carries the aim rotation; keep the view on the head.
        hmd_transform      = glm::inverse(CameraParentAimRotation(vr->m_engine_frame_count)) * hmd_transform * eye;
        hmd_transform = to_havok_space(hmd_transform);
        worldCamera->local.rotate = originalRotation * *(RE::NiMatrix3*) & hmd_transform;
        worldCamera->local.translate.x = hmd_transform[3][0];
        worldCamera->local.translate.y = hmd_transform[3][1];
        worldCamera->local.translate.z = hmd_transform[3][2];
    } else {
        auto head_rotation = vr->get_transform(0);
        head_rotation[3] -= glm::vec4{ StandingOriginPosition(), 0.0f };
        auto eye = vr->get_current_eye_transform();
        head_rotation = head_rotation * eye;
        head_rotation = to_havok_space(head_rotation);
        worldCamera->local.rotate = originalRotation * *(RE::NiMatrix3 *) &head_rotation;
        worldCamera->local.translate.x = head_rotation[3][0];
        worldCamera->local.translate.y = head_rotation[3][1];
        worldCamera->local.translate.z = head_rotation[3][2];
    }
}


void CreationEngineCameraManager::onFPSGetCameraRotation(RE::FirstPersonState *fps, RE::NiQuaternion *quat_out) {
    static auto instance = CreationEngineCameraManager::Get();
    static auto original_func = instance->m_onGetCameraRotationHook->get_original<decltype(onFPSGetCameraRotation)>();
    original_func(fps, quat_out);
    static auto vr = VR::get();
    if (!vr->is_hmd_active() || ModConstants::cameraShake || ModSettings::showFlatScreenDisplay()) {
        yaw_offset = 0.0f;
        return;
    }
    if (!GameFlow::isImmovable() && !GameFlow::isControlledByAI()) {
        // order of extraction Pitch->Yaw->Roll (Havok X->Z->Y)
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();

        // Applied on the right-eye frame so the turn lands at the start of an eye pair.
        if (p_player && vr->get_current_render_eye() == VRRuntime::Eye::RIGHT) {
            if (const float snap = GameFlow::pendingSnapYaw.exchange(0.0f); snap != 0.0f) {
                const float two_pi = 2.0f * glm::pi<float>();
                p_player->data.angle.z = std::fmod(p_player->data.angle.z + snap + two_pi, two_pi);
                spdlog::info("[SnapTurn] Applied {:.0f} degrees on engine frame {}", glm::degrees(snap), vr->m_engine_frame_count);
            }
        }

        RE::NiMatrix3 havok_rotation;
        quat_out->ToMatrix(havok_rotation);
        float pitch, yaw, roll;
        havok_rotation.ToEulerAnglesXYZ(pitch, roll, yaw);

        auto current_hmd_rotation = vr->get_rotation(0);
        auto rotation_quat = glm::normalize(glm::quat_cast(to_havok_space(current_hmd_rotation)));
        auto ni_hmd_rotation = RE::NiQuaternion(rotation_quat.w, rotation_quat.x, rotation_quat.y, rotation_quat.z);
        const auto aim_rotation = AimRotation();
        const auto aim_quat = glm::normalize(glm::quat_cast(to_havok_space(aim_rotation)));
        const auto ni_aim_rotation = RE::NiQuaternion(aim_quat.w, aim_quat.x, aim_quat.y, aim_quat.z);
        {
            if (GameFlow::gStore.internalSettings.pawnControl) {
                yaw -= yaw_offset;
                havok_rotation.FromEulerAnglesXYZ(pitch, roll, yaw);

                auto new_quat = RE::NiQuaternion(havok_rotation);

                new_quat = new_quat * ni_hmd_rotation;

                auto delta = quat_out->InvertVector() * new_quat;
                delta.ToMatrix(havok_rotation);
                float delta_pitch, delta_yaw, delta_roll;
                havok_rotation.ToEulerAnglesXYZ(delta_pitch, delta_roll, delta_yaw);
                yaw_offset += delta_yaw;
                auto corrected_yaw = p_player->data.angle.z - delta_yaw + 2.0f * glm::pi<float>();
                corrected_yaw = std::fmod(corrected_yaw, 2.0f * glm::pi<float>());
                p_player->data.angle.z = corrected_yaw;
            }
            if (GameFlow::gStore.internalSettings.decoupledPitch && !((ModConstants::headTrackingType == 0 && GameFlow::isAimingDownSights()) || ModConstants::headTrackingType == 2)) {
                pitch = 0.0f;
            }
            havok_rotation.FromEulerAnglesXYZ(pitch, roll, yaw);
            const auto room_rotation = RE::NiQuaternion(havok_rotation);
            SetRoomRotation(room_rotation);
            *quat_out = room_rotation * ni_aim_rotation;
            RecordAppliedAim(aim_rotation, vr->m_engine_frame_count);
        }
    } else {
        yaw_offset = 0.0f;
        GameFlow::pendingSnapYaw.store(0.0f);
    }
}

RE::NiAVObject* CreationEngineCameraManager::GetBodyMuzzle()
{
    return body::g_body_muzzle.load();
}

bool CreationEngineCameraManager::GetBodyAimForward(float out[3])
{
    if (!body::g_body_muzzle.load()) {
        return false;
    }
    out[0] = body::g_barrel.aim_forward.x;
    out[1] = body::g_barrel.aim_forward.y;
    out[2] = body::g_barrel.aim_forward.z;
    return true;
}

void CreationEngineCameraManager::NotifyPlayerFired()
{
    body::g_fire_sample = true;
}
