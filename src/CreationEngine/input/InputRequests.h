#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// Gamepad input the mod sends to the game on the player's behalf (weapon interactions), and physical buttons it
// holds back from the game while an interaction owns them. Written by the body code, applied by the input mapper.
namespace input_requests
{
    enum Button : int
    {
        kX = 0,       // reload / draw (tap), holster (hold)
        kLeftTrigger,  // aim down sights
        kRightShoulder,
        kCount
    };

    namespace detail
    {
        inline std::atomic<std::int64_t> g_press_until[kCount]{};
        inline std::atomic<bool>         g_held[kCount]{};
        inline std::atomic<bool>         g_suppressed[kCount]{};
        inline std::atomic<int>          g_reload_presses{ 0 };

        inline std::int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    }

    // Presses `button` for `ms` milliseconds (a tap or a hold).
    inline void Press(Button button, int ms)
    {
        detail::g_press_until[button] = detail::NowMs() + ms;
    }

    // Holds `button` while `held` stays true.
    inline void SetHeld(Button button, bool held)
    {
        detail::g_held[button] = held;
    }

    // Keeps the player's own press of `button` from reaching the game.
    inline void Suppress(Button button, bool suppressed)
    {
        detail::g_suppressed[button] = suppressed;
    }

    inline bool Requested(Button button)
    {
        return detail::g_held[button] || detail::NowMs() < detail::g_press_until[button];
    }

    inline bool Suppressed(Button button)
    {
        return detail::g_suppressed[button];
    }

    // The player pressed the reload button while it was suppressed (manual reload uses it to eject).
    inline void NoteReloadPress()
    {
        ++detail::g_reload_presses;
    }

    inline bool TakeReloadPress()
    {
        int pending = detail::g_reload_presses.load();
        while (pending > 0) {
            if (detail::g_reload_presses.compare_exchange_weak(pending, pending - 1)) {
                return true;
            }
        }
        return false;
    }
}
