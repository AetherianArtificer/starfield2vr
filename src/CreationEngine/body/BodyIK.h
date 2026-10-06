#pragma once

#include <optional>

namespace RE
{
    class NiAVObject;
}

// Third-person body shown in first person with arm, leg and weapon IK.
namespace body
{
    // Hooks BSModelNode::UpdateTransforms and the body's animation graph, where the body is posed.
    void InstallModelNodeHook();
    // Called for each object as it updates its world transform; toggles the body at its 3P root.
    void OnUpdateWorld(RE::NiAVObject* obj, int engine_frame);
    // Once per frame on the game's main thread, before the scene updates.
    void OnFrameStart();
    // Muzzle of the weapon held by the shown body, or null when the body is not driving the weapon.
    RE::NiAVObject* GetBodyMuzzle();
    // Aim direction (game world) the held weapon was posed along this frame, if any.
    bool GetBodyAimForward(float out[3]);

    // While the body is shown it owns the actor's heading (radians, counter-clockwise, game world).
    std::optional<float> ActorHeadingTarget();
    // The same, relative to the room: what the camera's yaw offset (the actor's yaw in the room) should be.
    std::optional<float> BodyHeadingInRoom();
    // Before the body owns the heading: the camera's yaw offset, which then follows the head, to verify it against
    // the body's head heading.
    void ObserveHeadYaw(float yaw_offset);
    // While the body owns the actor's heading: the body's heading minus the head's (radians, counter-clockwise), to
    // keep stick movement relative to the head.
    std::optional<float> LocomotionTurn();
}
