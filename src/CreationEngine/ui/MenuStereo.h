#pragma once
#include <d3d12.h>

// Fullscreen menus in depth. Clips listed in menu_depth.txt are moved toward or away from the player in the menu's own
// 3D space, keeping their place and size on the menu panel, and every menu movie is drawn once per eye so the depth
// shows. Clips left out stay on the panel.
namespace MenuStereo
{
    void InstallHooks();
    // SFSE: a menu's movie was created.
    void OnMenuMovieCreated(void* menu);
    // Once per movie per frame, where the movie is advanced.
    void OnMovieFrame(void* movie);
    // At the Scaleform composite, which reads the UI layer: on frames drawn per eye it holds the right eye's UI.
    void OnComposite(ID3D12GraphicsCommandList* command_list, ID3D12Resource* layer);
    // After the Scaleform composite; the next Scaleform pass starts a new frame.
    void EndFrame();
    // A menu showing a 3D scene of the game's (the data menu, the inventory) advanced in the last few frames.
    bool SceneMenuShowing();
}
