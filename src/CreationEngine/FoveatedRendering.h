#pragma once

// Fixed foveated rendering: coarser pixel shading toward the edges of each eye's view, where the lenses blur the image
// anyway. A shading rate image is bound on the passes that render the scene at the eyes' render resolution, before DLSS.
// Compute work (most of the lighting) is not affected; the game's own VRS covers that when it is on.
namespace FoveatedRendering
{
    // 0 off, 1 light, 2 balanced, 3 strong.
    void OnFrameStart(bool stereo, int level);
}
