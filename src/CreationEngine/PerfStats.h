#pragma once

// Periodic frame-time summary in vr_log.txt.
namespace PerfStats
{
    void OnPresent();
    // GPU timestamps taken as render graphs finish recording: frame start (CRBeginFrame), each eye's scene graph, frame end.
    enum class GpuPoint
    {
        kFrameStart,
        kFirstEye,
        kSecondEye,
        kFrameEnd,
    };
    void MarkGpu(void* command_list, GpuPoint point, int frame);
}
