#pragma once

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
}
