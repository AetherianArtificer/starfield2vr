#include "ModConfig.h"
#include "Mods.hpp"
#include <mods/VR.hpp>
#include <mods/VRConfig.hpp>
#ifdef _DEBUG
#include <nvidia/ShaderDebugOverlay.h>
#endif
#include <nvidia/UpscalerAfrNvidiaModule.h>

#include "ModSettings.h"
#include "CreationEngine/CreationEngineEntry.h"
#include "CreationEngine/GameSettingsComponent.h"
#include "CreationEngine/models/ModSettingsStore.h"
#include "CreationEngine/models/GameFlow.h"

namespace ModSettings {
    // HudScale g_hudScale;
    // CrosshairTranslate g_crosshairTranslate;
    // AW2InternalSettings g_aw2_settings;
    // AW2GameFlow g_game_state;
    // DebugAndCalibration g_debugAndCalibration;
}

Mods::Mods() {
    m_mods.emplace_back(VRConfig::get());
    m_mods.emplace_back(VR::get());
    m_mods.emplace_back(UpscalerAfrNvidiaModule::Get());
#ifdef _DEBUG
    m_mods.emplace_back(ShaderDebugOverlay::Get());
#endif
    m_mods.emplace_back(CreationEngineEntry::Get());
    m_mods.emplace_back(GameSettingsComponent::Get());
}




void VR::on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) {
//    ZoneScopedN(__FUNCTION__);
    auto pXinputGamepad = (XINPUT_GAMEPAD*)state;

    const auto now = std::chrono::steady_clock::now();

    if (now - m_last_xinput_update > std::chrono::seconds(2)) {
        m_lowest_xinput_user_index = user_index;
    }

    if (user_index < m_lowest_xinput_user_index) {
        m_lowest_xinput_user_index = user_index;
        spdlog::info("[VR] Changed lowest XInput user index to {}", user_index);
    }

    if (user_index != m_lowest_xinput_user_index) {
        if (!m_spoofed_gamepad_connection && is_using_controllers()) {
            spdlog::info("[VR] XInputGetState called, but user index is {}", user_index);
        }

        return;
    }

    if (!m_spoofed_gamepad_connection) {
        spdlog::info("[VR] Successfully spoofed gamepad connection @ {}", user_index);
    }

    m_last_xinput_update = now;
    m_spoofed_gamepad_connection = true;

    if (is_using_controllers_within(std::chrono::minutes(5)))
        if (is_using_controllers_within(std::chrono::minutes(5)))
        {
            *retval = ERROR_SUCCESS;

        }

    if (!is_using_controllers()) {
        return;
    }

//    // Clear button state for VR controllers
//    if (is_using_controllers_within(std::chrono::seconds(5))) {
//        state->Gamepad.wButtons = 0;
//        state->Gamepad.bLeftTrigger = 0;
//        state->Gamepad.bRightTrigger = 0;
//        state->Gamepad.sThumbLX = 0;
//        state->Gamepad.sThumbLY = 0;
//        state->Gamepad.sThumbRX = 0;
//        state->Gamepad.sThumbRY = 0;
//    }


    const auto wants_swap = m_swap_controllers->value();
    const auto left_joystick = !wants_swap ? get_left_joystick():get_right_joystick();
    const auto right_joystick = !wants_swap? get_right_joystick():get_left_joystick();

    const auto& a_button_left = !wants_swap ? m_action_a_button_left : m_action_a_button_right;
    const auto& a_button_right = !wants_swap ? m_action_a_button_right : m_action_a_button_left;

    const auto is_right_a_button_down = is_action_active_any_joystick(a_button_right);
    const auto is_left_a_button_down = is_action_active_any_joystick(a_button_left);

    const auto is_left_joystick_click_down = is_action_active(m_action_joystick_click, left_joystick);
    const auto is_right_joystick_click_down = is_action_active(m_action_joystick_click, right_joystick);

    static bool ui_toggle_active = false;
    bool both_clicks_down = is_left_joystick_click_down && is_right_joystick_click_down;

    if (both_clicks_down && !ui_toggle_active) {
        ui_toggle_active = true;
        g_framework->set_draw_ui(!g_framework->is_drawing_ui());
    } else if (!is_left_joystick_click_down || !is_right_joystick_click_down) {
        ui_toggle_active = false;
    }

    if (g_framework->is_drawing_ui()) {
        return;
    }

    // Snap turn (docs/motion-controls.md "C1"): a right-stick flick queues a discrete yaw step that the
    // camera hook applies once; the stick's X axis is then withheld from the game. Menus and ship
    // piloting keep the raw stick.
    const auto& internal_settings = GameFlow::gStore.internalSettings;
    const bool snap_turn_active = internal_settings.turnMode == 0 && !GameFlow::isShowingMenu() && !GameFlow::isPilotingShip();
    if (snap_turn_active) {
        constexpr float kSnapTrigger = 0.7f;
        constexpr float kSnapRearm   = 0.3f;
        constexpr auto  kSnapRepeat  = std::chrono::milliseconds(300);

        static bool                                  snap_armed{true};
        static std::chrono::steady_clock::time_point last_snap{};

        const float x = get_joystick_axis(right_joystick).x;
        if (std::abs(x) < kSnapRearm) {
            snap_armed = true;
        } else if (snap_armed && std::abs(x) >= kSnapTrigger && now - last_snap >= kSnapRepeat) {
            snap_armed = false;
            last_snap  = now;
            const float step = glm::radians(internal_settings.snapTurnDegrees) * (x > 0.0f ? 1.0f : -1.0f);
            GameFlow::pendingSnapYaw.fetch_add(step);
        }
    }

    // Matching-letters layout (default): each physical button sends the gamepad button with the
    // same label, so in-game prompts match the controller. See docs/motion-controls.md "P2b".
    if (GameFlow::gStore.internalSettings.controllerLayout == 0) {
        using clock = std::chrono::steady_clock;
        constexpr auto kTapPulse = std::chrono::milliseconds(100);
        constexpr auto kMenuHold = std::chrono::milliseconds(500);
        constexpr float kStickDeflection = 0.5f;

        auto& buttons = pXinputGamepad->wButtons;

        // Letters follow the physical labels: A/B on the right controller, X/Y on the left.
        if (is_action_active_any_joystick(m_action_a_button_right)) buttons |= XINPUT_GAMEPAD_A;
        if (is_action_active_any_joystick(m_action_b_button_right)) buttons |= XINPUT_GAMEPAD_B;
        if (is_action_active_any_joystick(m_action_a_button_left))  buttons |= XINPUT_GAMEPAD_X;
        if (is_action_active_any_joystick(m_action_b_button_left))  buttons |= XINPUT_GAMEPAD_Y;

        if (is_left_joystick_click_down)  buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (is_right_joystick_click_down) buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;

        const auto left_trigger_down  = is_action_active(m_action_trigger, left_joystick);
        const auto right_trigger_down = is_action_active(m_action_trigger, right_joystick);
        const auto left_grip_down     = is_action_active(m_action_grip, left_joystick);
        const auto right_grip_down    = is_action_active(m_action_grip, right_joystick);

        if (left_trigger_down)  pXinputGamepad->bLeftTrigger  = 255;
        if (right_trigger_down) pXinputGamepad->bRightTrigger = 255;
        if (right_grip_down)    buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;

        const auto now = clock::now();
        const auto left_axis  = get_joystick_axis(left_joystick);
        const auto right_axis = get_joystick_axis(right_joystick);

        // Left grip: held with the left stick it is a D-pad; a tap with no stick use sends LB on release.
        static bool              left_grip_was_down{false};
        static bool              left_grip_used_as_modifier{false};
        static clock::time_point lb_pulse_until{};

        if (left_grip_down) {
            if (std::abs(left_axis.x) >= kStickDeflection || std::abs(left_axis.y) >= kStickDeflection || is_left_joystick_click_down) {
                left_grip_used_as_modifier = true;
            }
            if (left_axis.y >= kStickDeflection)  buttons |= XINPUT_GAMEPAD_DPAD_UP;
            if (left_axis.y <= -kStickDeflection) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (left_axis.x >= kStickDeflection)  buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (left_axis.x <= -kStickDeflection) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;

            if (is_left_joystick_click_down) {
                // Left grip + left stick click: toggle the flat-screen view (as in the legacy layout).
                buttons &= ~XINPUT_GAMEPAD_LEFT_THUMB;
                static clock::time_point last_flat_toggle{};
                if (now - last_flat_toggle > kMenuHold) {
                    last_flat_toggle = now;
                    auto& flat_screen = ModSettings::g_internalSettings.forceFlatScreen;
                    flat_screen = !flat_screen;
                }
            }
        } else if (left_grip_was_down) {
            if (!left_grip_used_as_modifier) {
                lb_pulse_until = now + kTapPulse;
            }
            left_grip_used_as_modifier = false;
        }
        left_grip_was_down = left_grip_down;

        if (now < lb_pulse_until) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;

        // Menu button: tap = Start (pause), hold = View/Back (data menus).
        static bool              menu_was_down{false};
        static clock::time_point menu_down_since{};
        static clock::time_point start_pulse_until{};

        const auto menu_down = is_action_active_any_joystick(m_action_system_button);
        if (menu_down) {
            if (!menu_was_down) {
                menu_down_since = now;
            }
            if (now - menu_down_since >= kMenuHold) {
                buttons |= XINPUT_GAMEPAD_BACK;
            }
        } else if (menu_was_down && now - menu_down_since < kMenuHold) {
            start_pulse_until = now + kTapPulse;
        }
        menu_was_down = menu_down;

        if (now < start_pulse_until) buttons |= XINPUT_GAMEPAD_START;

        // Explicit D-pad actions (controllers that have one).
        if (is_action_active_any_joystick(m_action_dpad_up))    buttons |= XINPUT_GAMEPAD_DPAD_UP;
        if (is_action_active_any_joystick(m_action_dpad_right)) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (is_action_active_any_joystick(m_action_dpad_down))  buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (is_action_active_any_joystick(m_action_dpad_left))  buttons |= XINPUT_GAMEPAD_DPAD_LEFT;

        // Sticks. While the left grip turns the left stick into a D-pad, don't also move.
        if (!left_grip_down) {
            pXinputGamepad->sThumbLX = (int16_t)std::clamp<float>((float)pXinputGamepad->sThumbLX + left_axis.x * 32767.0f, -32767.0f, 32767.0f);
            pXinputGamepad->sThumbLY = (int16_t)std::clamp<float>((float)pXinputGamepad->sThumbLY + left_axis.y * 32767.0f, -32767.0f, 32767.0f);
        }
        if (!snap_turn_active) {
            pXinputGamepad->sThumbRX = (int16_t)std::clamp<float>((float)pXinputGamepad->sThumbRX + right_axis.x * 32767.0f, -32767.0f, 32767.0f);
        }
        pXinputGamepad->sThumbRY = (int16_t)std::clamp<float>((float)pXinputGamepad->sThumbRY + right_axis.y * 32767.0f, -32767.0f, 32767.0f);
        return;
    }

    if (is_right_a_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_A;
    }

    if (is_left_a_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_B;
    }

    const auto& b_button_left = !wants_swap ? m_action_b_button_left : m_action_b_button_right;
    const auto& b_button_right = !wants_swap ? m_action_b_button_right : m_action_b_button_left;

    const auto is_right_b_button_down = is_action_active_any_joystick(b_button_right);
    const auto is_left_b_button_down = is_action_active_any_joystick(b_button_left);

    if (is_right_b_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_Y;
    }

    if (is_left_b_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_X;
    }


    {
        if (is_left_joystick_click_down) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        }

        if (is_right_joystick_click_down) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        }
    }


    const auto is_left_trigger_down = is_action_active(m_action_trigger, left_joystick);
    const auto is_right_trigger_down = is_action_active(m_action_trigger, right_joystick);
    const auto is_left_grip_down = is_action_active(m_action_grip, left_joystick);
    const auto is_right_grip_down = is_action_active(m_action_grip, right_joystick);


    if((is_left_joystick_click_down && is_left_grip_down) || (GetAsyncKeyState(VK_F12) & 1)) {
        static std::chrono::steady_clock::time_point m_last_grip_click = std::chrono::steady_clock::now();
        if(std::chrono::steady_clock::now() - m_last_grip_click > std::chrono::milliseconds(500)) {
            m_last_grip_click                           = std::chrono::steady_clock::now();
            auto& flatScreen = ModSettings::g_internalSettings.forceFlatScreen;
            flatScreen = !flatScreen;
        }
    }

    const auto system_button_down = is_action_active_any_joystick(m_action_system_button);
    if(system_button_down) {
        if(is_left_grip_down) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_BACK;
            pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_START;
        } else {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_START;
            pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_BACK;
        }

    } else {
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_START;
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_BACK;
    }

    if (is_right_grip_down) {
        pXinputGamepad->bLeftTrigger = 255;
    }

    if (is_right_trigger_down) {
        pXinputGamepad->bRightTrigger = 255;
    }

    const auto thumbrest_touch_right_down = is_action_active_any_joystick(m_action_thumbrest_touch_right);
    const auto thumbrest_touch_left_down = is_action_active_any_joystick(m_action_thumbrest_touch_left);


    if (thumbrest_touch_right_down && !GameFlow::gStore.internalSettings.alternativeJoyLayout) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    } else if(is_left_grip_down && is_right_trigger_down && GameFlow::gStore.internalSettings.alternativeJoyLayout) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        pXinputGamepad->bRightTrigger = 0;
    }

    if (thumbrest_touch_left_down && !GameFlow::gStore.internalSettings.alternativeJoyLayout) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    } else if(is_left_grip_down && is_left_trigger_down && GameFlow::gStore.internalSettings.alternativeJoyLayout) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    }

    const auto is_dpad_up_down = is_action_active_any_joystick(m_action_dpad_up);

    if (is_dpad_up_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    }

    const auto is_dpad_right_down = is_action_active_any_joystick(m_action_dpad_right);

    if (is_dpad_right_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }

    const auto is_dpad_down_down = is_action_active_any_joystick(m_action_dpad_down);

    if (is_dpad_down_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    }

    const auto is_dpad_left_down = is_action_active_any_joystick(m_action_dpad_left);

    if (is_dpad_left_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    }

    const auto left_joystick_axis = get_joystick_axis(left_joystick);
    const auto right_joystick_axis = get_joystick_axis(right_joystick);

    const auto true_left_joystick_axis = get_joystick_axis(m_left_joystick);
    const auto true_right_joystick_axis = get_joystick_axis(m_right_joystick);

    pXinputGamepad->sThumbLX = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbLX + left_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    pXinputGamepad->sThumbLY = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbLY + left_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    if (!snap_turn_active) {
        pXinputGamepad->sThumbRX = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbRX + right_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    }
    pXinputGamepad->sThumbRY = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbRY + right_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);


    // Touching the thumbrest allows us to use the thumbstick as a dpad.  Additional options are for controllers without capacitives/games that rely solely on DPad
    if (is_left_grip_down) {

        float ty{0.0f};
        float tx{0.0f};
        //SHORT ThumbY{0};
        //SHORT ThumbX{0};
        // If someone is accidentally touching both thumbrests while also moving a joystick, this will default to left joystick.

        ty = true_left_joystick_axis.y; // ? wants_swap
        tx = true_left_joystick_axis.x;

        if (ty >= 0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        }

        if (ty <= -0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }

        if (tx >= 0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }

        if (tx <= -0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }

        if (wants_swap) {
            pXinputGamepad->sThumbRY = 0;
            pXinputGamepad->sThumbRX = 0;
        } else {
            pXinputGamepad->sThumbLY = 0;
            pXinputGamepad->sThumbLX = 0;
        }
    }

}

