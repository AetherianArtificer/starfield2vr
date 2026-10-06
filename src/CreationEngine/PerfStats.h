#pragma once

#include <cstdint>

// Periodic frame-time summary in vr_log.txt: present intervals and CPU stage times from the game's Reflex markers.
namespace PerfStats
{
    void OnPresent();

    // Reflex marker (0 simulation start, 1 simulation end, 2 render submit start, 3 render submit end, 4 present start,
    // 5 present end) for an engine frame.
    void OnReflexMarker(uint32_t marker, uint32_t frame);

    // CPU work the mod adds to each frame, timed where it runs.
    enum class CpuSpan
    {
        kVrWait,    // waiting for the previous frame's present
        kVrSync,    // headset frame sync and pose update
        kModFrame,  // per-frame stereo, camera and aim work
        kBodyIk,    // the player body's IK in its model update
    };
    void AddCpu(CpuSpan span, double ms);

}
