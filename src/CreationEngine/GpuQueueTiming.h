#pragma once

// Times every submission to the game's graphics and compute queues with a timestamp before and after it, to give the
// GPU's busy time per frame. Logged with the [Perf] lines.
namespace GpuQueueTiming
{
    // Once per presented frame.
    void OnPresent();
}
