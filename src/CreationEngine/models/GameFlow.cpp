#include "GameFlow.h"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include "ModSettingsStore.h"
#include "RE/P/PlayerCamera.h"
#include <CreationEngine/CreationEngineSingletonManager.h>
#include <mods/VR.hpp>

namespace GameFlow
{
    State gState{};
    auto vr = VR::get();

    const std::unordered_map<uint32_t, MenuSettings> menu_settings = {
            {         "Interface/ScopeMenu.swf"_DJB,   { 1.0f, 400 } },
            {         "Interface/ScopeMenu_LRG.swf"_DJB,   { 1.0f, 400 } },
            {       "Interface/MonocleMenu.swf"_DJB,   { 0.6f, 100 } },
            {       "Interface/MonocleMenu_LRG.swf"_DJB,   { 0.6f, 100 } }
    };

    // Recenter after loading screens; the startup recenter can fire before the headset is worn.
    namespace
    {
        constexpr int kFramesAfterLoadingToRecenter = 30;

        bool loading_seen_this_frame{false};
        bool loading_pending{false};
        int  frames_since_loading{0};
        // Where the player stood when the loading screen appeared: only a move to somewhere else recenters.
        RE::NiPoint3 position_before_loading{};
        bool         have_position_before_loading{false};
        constexpr float kLoadingMoveToRecenter = 10.0f;

        std::vector<std::string> last_menus;

        bool PlayerPosition(RE::NiPoint3& out)
        {
            auto player = CreationEngineSingletonManager::GetPlayerRef();
            if (player == nullptr) {
                return false;
            }
            out = player->data.location;
            return true;
        }

        // The ship HUD is only drawn while piloting.
        bool ship_hud_seen_this_frame{false};
        std::atomic<bool> ship_hud_last_frame{false};

        void update_loading_recenter()
        {
            if (loading_seen_this_frame) {
                if (!loading_pending) {
                    have_position_before_loading = PlayerPosition(position_before_loading);
                }
                loading_pending      = true;
                frames_since_loading = 0;
            } else if (loading_pending && ++frames_since_loading >= kFramesAfterLoadingToRecenter) {
                loading_pending = false;
                RE::NiPoint3 now{};
                const bool   known = have_position_before_loading && PlayerPosition(now);
                const float  moved = known ? std::sqrt((now.x - position_before_loading.x) * (now.x - position_before_loading.x) +
                                                       (now.y - position_before_loading.y) * (now.y - position_before_loading.y) +
                                                       (now.z - position_before_loading.z) * (now.z - position_before_loading.z))
                                           : -1.0f;
                if (known && moved < kLoadingMoveToRecenter) {
                    spdlog::info("[GameFlow] Loading screen ended where it began ({:.1f} m), view kept", moved);
                } else if (gStore.internalSettings.recenterAfterLoading) {
                    spdlog::info("[GameFlow] Loading screen ended {:.0f} m away, requesting recenter", moved);
                    vr->get_runtime()->wants_reset_origin = true;
                }
            }
            loading_seen_this_frame = false;
        }
    }

    void resetGameState() {
        update_loading_recenter();
        {
            std::vector<std::string> menus;
            for (auto part : gStore.debugData.ui_parts) {
                std::string name{ part };
                if (std::find(menus.begin(), menus.end(), name) == menus.end()) {
                    menus.push_back(name);
                }
            }
            std::sort(menus.begin(), menus.end());
            if (menus != last_menus) {
                std::string list;
                for (const auto& m : menus) {
                    list += (list.empty() ? "" : ", ") + m;
                }
                spdlog::info("[GameFlow] Menus drawn: {}", list.empty() ? "none" : list);
                last_menus = std::move(menus);
            }
        }
        ship_hud_last_frame.store(ship_hud_seen_this_frame);
        ship_hud_seen_this_frame = false;
        gStore.debugData.ui_parts.clear();
        gState.uiData.modulino++;
        gState.uiData.rendered_menus_count[gState.uiData.modulino % 2] = 0;
    }

    void renderMenu(std::string_view menuNameHash) {
        switch (djb2Hash(menuNameHash.data())) {
        case "Interface/SpaceshipHudMenu.swf"_DJB:
        case "Interface/SpaceshipHudMenu_LRG.swf"_DJB:
            ship_hud_seen_this_frame = true;
            [[fallthrough]];
        case "Interface/HUDMenu.gfx"_DJB:
        case "Interface/HUDMenu_LRG.gfx"_DJB:
            //            case "Interface/HUDMessagesMenu.gfx"_DJB:
        case "Interface/ScopeMenu.swf"_DJB:
        case "Interface/ScopeMenu_LRG.swf"_DJB:
        case "Interface/FavoritesMenu.swf"_DJB:
        case "Interface/FavoritesMenu_LRG.swf"_DJB:
        case "Interface/MonocleMenu.swf"_DJB:
        case "Interface/MonocleMenu_LRG.swf"_DJB:
        case "Interface/DialogueMenu.swf"_DJB:
        case "Interface/DialogueMenu_LRG.swf"_DJB:
            gState.uiData.rendered_menus_count[gState.uiData.modulino % 2]--;
            break;
        case "Interface/WorkshopMenu.swf"_DJB:
        case "Interface/WorkshopMenu_LRG.swf"_DJB:
        case "Interface/ResearchMenu.swf"_DJB:
        case "Interface/ResearchMenu_LRG.swf"_DJB:
        case "Interface/WeaponCraftingMenu.swf"_DJB:
        case "Interface/WeaponCraftingMenu_LRG.swf"_DJB:
        case "Interface/WeaponsCraftingMenu.swf"_DJB:
        case "Interface/WeaponsCraftingMenu_LRG.swf"_DJB:
        case "Interface/ArmorCraftingMenu.swf"_DJB:
        case "Interface/ArmorCraftingMenu_LRG.swf"_DJB:
        case "Interface/IndustrialCraftingMenu.swf"_DJB:
        case "Interface/IndustrialCraftingMenu_LRG.swf"_DJB:
        case "Interface/DrugsCraftingMenu.swf"_DJB:
        case "Interface/DrugsCraftingMenu.swf_LRG"_DJB:
        case "Interface/SpaceShipEditorMenu.swf"_DJB:
        case "Interface/SpaceShipEditorMenu_LRG.swf"_DJB:
        case "Interface/ShipCrewMenu.swf"_DJB:
        case "Interface/ShipCrewMenu_LRG.swf"_DJB:
        case "Interface/SkillsMenu.swf"_DJB:
        case "Interface/SkillsMenu_LRG.swf"_DJB:
        case "Interface/MainMenu.swf"_DJB:
        case "Interface/MainMenu_LRG.swf"_DJB:
        case "Interface/CursorMenu.swf"_DJB:
        case "Interface/CursorMenu_LRG.swf"_DJB:
        case "Interface/DataMenu.swf"_DJB:
        case "Interface/DataMenu_LRG.swf"_DJB:
        case "Interface/InventoryMenu.swf"_DJB:
        case "Interface/InventoryMenu_LRG.swf"_DJB:
            gState.uiData.rendered_menus_count[gState.uiData.modulino % 2]++;
            break;
        case "Interface/LoadingMenu.swf"_DJB:
        case "Interface/LoadingMenu_LRG.swf"_DJB:
            loading_seen_this_frame = true;
            gState.uiData.rendered_menus_count[gState.uiData.modulino % 2]++;
            break;
        case "Interface/PauseMenu.swf"_DJB:
        case "Interface/PauseMenu_LRG.swf"_DJB:
        case "Interface/GalaxyStarMapMenu.swf"_DJB:
        case "Interface/GalaxyStarMapMenu_LRG.swf"_DJB:
        case "Interface/StarMapMenu.swf"_DJB:
        case "Interface/StarMapMenu_LRG.swf"_DJB:
            gState.uiData.rendered_menus_count[gState.uiData.modulino % 2]++;
            break;
        default:
            break;

        }
        gStore.debugData.ui_parts.push_back(menuNameHash);
    }

    MenuSettings getMenuSettings(std::string_view menuUrl) {
        auto hash = djb2Hash(menuUrl.data());
        MenuSettings settings = {gStore.hudSettings.hudScale, gStore.hudSettings.perspective };
        auto         it       = menu_settings.find(hash);
        if (it != menu_settings.end()) {
            settings.hud_scale = it->second.hud_scale;
            settings.perspective += it->second.perspective;
        }
        auto isMenu = gState.uiData.rendered_menus_count[(gState.uiData.modulino + 1) % 2] >= 0;
        if(isMenu) {
            settings.perspective = 0;
        }
        return settings;
    }

    bool isShowingMenu() {
        return gState.uiData.rendered_menus_count[(gState.uiData.modulino + 1) % 2] >= 0;
    }

    bool isPilotingShip() {
        return ship_hud_last_frame.load();
    }

    bool isAimingDownSights() {
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();
        return p_player && p_player->IsInIronSights();
    }
    bool isWeaponDrawn() {
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();
        return p_player && p_player->IsWeaponDrawn();
    }
    bool isImmovable() {
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();
        if(!p_player) {
            return true;
        }
        return p_player->Immovable();
    }
    bool isControlledByAI() {
        auto p_player = CreationEngineSingletonManager::GetPlayerRef();
        if(!p_player) {
            return false;
        }
        return p_player->ControlledByAI();
    }
    bool isInFirstPerson() {
        auto p_camera = CreationEngineSingletonManager::GetPlayerCameraSingleton();
        return p_camera && p_camera->IsInFirstPerson();
    }
}
