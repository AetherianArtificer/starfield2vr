#include "GameFlow.h"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include "ModSettingsStore.h"
#include <CreationEngine/memory/ScanHelper.h>
#include <RE/B/BSFixedString.h>
#include <windows.h>
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

    namespace
    {
        // The UI singleton: the global pointing at the object whose vtable is UI's.
        uint8_t** FindUiSingleton()
        {
            const auto vtable = MemoryScan::VTable("UI", ".?AVUI@@", 0);
            if (vtable == 0) {
                return nullptr;
            }
            const auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            const auto nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
            auto       sec  = IMAGE_FIRST_SECTION(nt);
            for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                if (std::strncmp(reinterpret_cast<const char*>(sec->Name), ".data", 8) != 0) {
                    continue;
                }
                auto slots = reinterpret_cast<uint8_t**>(base + sec->VirtualAddress);
                for (size_t n = 0; n < sec->Misc.VirtualSize / sizeof(void*); ++n) {
                    auto candidate = slots[n];
                    if (candidate == nullptr || (reinterpret_cast<uintptr_t>(candidate) & 7) != 0 ||
                        (candidate >= base && candidate < base + nt->OptionalHeader.SizeOfImage)) {
                        continue;
                    }
                    MEMORY_BASIC_INFORMATION info{};
                    if (VirtualQuery(candidate, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & (PAGE_READWRITE | PAGE_READONLY)) == 0) {
                        continue;
                    }
                    if (*reinterpret_cast<uintptr_t*>(candidate) == vtable) {
                        return &slots[n];
                    }
                }
            }
            return nullptr;
        }

        // The open menus' names, read under a fault guard; false when the stack does not hold menus (a layout mismatch).
        bool ReadMenuNames(uint8_t* ui, const char** names, uint32_t& count)
        {
            const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const auto end  = base + reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew)->OptionalHeader.SizeOfImage;
            __try {
                const auto size  = *reinterpret_cast<uint32_t*>(ui + 0x3F0);
                const auto menus = *reinterpret_cast<uint8_t***>(ui + 0x3F8);
                if (size > 64 || (size > 0 && menus == nullptr)) {
                    return false;
                }
                count = 0;
                for (uint32_t i = 0; i < size; ++i) {
                    const auto vtable = *reinterpret_cast<uintptr_t*>(menus[i]);
                    if (vtable < base || vtable >= end) {
                        return false;
                    }
                    names[count++] = reinterpret_cast<RE::BSFixedString*>(menus[i] + 0xB0)->c_str();
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Menus shown on the flat screen; others (the HUD, dialogue, scopes, faders) keep the 3D view.
        bool IsFullscreenMenu(std::string_view name)
        {
            static constexpr std::string_view kMenus[]{
                "MainMenu", "PauseMenu", "LoadingMenu", "MessageBoxMenu", "InventoryMenu", "DataMenu", "SkillsMenu", "StarMapMenu", "GalaxyStarMapMenu",
                "WorkshopMenu", "ResearchMenu", "WeaponsCraftingMenu", "ArmorCraftingMenu", "IndustrialCraftingMenu", "DrugsCraftingMenu",
                "SpaceshipEditorMenu", "ShipCrewMenu",
            };
            return std::find(std::begin(kMenus), std::end(kMenus), name) != std::end(kMenus);
        }
    }

    bool isFullscreenMenuOpen()
    {
        static uint8_t** singleton = [] {
            auto found = FindUiSingleton();
            if (found == nullptr) {
                spdlog::error("[GameFlow] UI singleton not found; fullscreen menus will not switch to the flat screen");
            } else {
                spdlog::info("[GameFlow] UI singleton at {:x}", reinterpret_cast<uintptr_t>(found) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
            }
            return found;
        }();
        if (singleton == nullptr || *singleton == nullptr) {
            return false;
        }
        // The menu stack: size at 0x3F0, entries at 0x3F8, each a menu with its name at 0xB0.
        static bool layout_ok = true;
        const char* raw[64]{};
        uint32_t    count = 0;
        if (!layout_ok) {
            return false;
        }
        if (!ReadMenuNames(*singleton, raw, count)) {
            layout_ok = false;
            spdlog::error("[GameFlow] The UI menu stack does not hold menus at the expected offsets; fullscreen menus will not switch to the flat screen");
            return false;
        }
        bool                     fullscreen = false;
        std::vector<std::string> names;
        for (uint32_t i = 0; i < count; ++i) {
            if (raw[i] == nullptr) {
                continue;
            }
            names.emplace_back(raw[i]);
            fullscreen |= IsFullscreenMenu(raw[i]);
        }
        static std::vector<std::string> last_names;
        if (names != last_names) {
            std::string list;
            for (const auto& n : names) {
                list += (list.empty() ? "" : ", ") + n;
            }
            spdlog::info("[GameFlow] Open menus: {}{}", list.empty() ? "none" : list, fullscreen ? " (flat screen)" : "");
            last_names = std::move(names);
        }
        return fullscreen;
    }

    void resetGameState() {
        update_loading_recenter();
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
