#pragma once

// Hooks the execute function of every render pass class. Each pass's GPU time is the span between timestamps written
// after it and after the previous pass on the same command list; the most expensive passes are logged per eye.
namespace RenderPassProfiler
{
    // After the stereo module's own pass hooks, which this leaves alone.
    void Install();

    void OnPresent();

    // Called by the stereo module's own pass hooks, after the pass ran.
    void MarkPass(void* pass, void* render_graph_data);
}
