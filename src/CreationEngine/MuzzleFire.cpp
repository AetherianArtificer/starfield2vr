#include "MuzzleFire.h"

#include <safetyhook/easy.hpp>

#include "CreationEngineConstants.h"
#include "CreationEngineSingletonManager.h"
#include <CreationEngine/memory/ScanHelper.h>
#include <mods/VR.hpp>

namespace MuzzleFire
{
    namespace
    {
        // ProjectileLaunchData offsets (1.16.244).
        constexpr std::size_t kShooterOffset       = 0x38;
        constexpr std::size_t kAimFromMuzzleOffset = 0xF2;

        safetyhook::InlineHook g_compute_launch_origin_hook{};

        // Player shots take the projectile-node path when this flag is set; NPCs never read it.
        bool ComputeLaunchOrigin(std::uint8_t* launch_data)
        {
            static auto vr = VR::get();
            if (launch_data && ModConstants::headTrackingType == ModConstants::kAimWithRightHand && vr->is_using_controllers()) {
                const auto player  = CreationEngineSingletonManager::GetPlayerRef();
                const auto shooter = *reinterpret_cast<void**>(launch_data + kShooterOffset);
                if (player && shooter == player) {
                    launch_data[kAimFromMuzzleOffset] = 1;
                }
            }
            return g_compute_launch_origin_hook.call<bool>(launch_data);
        }
    }

    void Install()
    {
        const auto address = MemoryScan::FuncRelocation(
            "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 B8 FA FF FF 48 81 EC 08 06 00 00", 0x1b52510, 9101);
        if (!address) {
            spdlog::error("[MuzzleFire] ComputeLaunchOrigin not found");
            return;
        }
        g_compute_launch_origin_hook = safetyhook::create_inline(reinterpret_cast<void*>(address), reinterpret_cast<void*>(&ComputeLaunchOrigin));
        if (!g_compute_launch_origin_hook) {
            spdlog::error("[MuzzleFire] Failed to hook ComputeLaunchOrigin");
        }
    }
}
