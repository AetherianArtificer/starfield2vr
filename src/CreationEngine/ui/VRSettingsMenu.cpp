#include "VRSettingsMenu.h"

#include <array>
#include <mutex>
#include <unordered_map>

#include "GFx.h"
#include "Framework.hpp"
#include <CreationEngine/CreationEngineEntry.h>
#include <CreationEngine/models/ModSettingsStore.h>

namespace VRSettingsMenu
{
    namespace
    {
        // Category IDs 0-6 are the game's; anything else opens the generic options page.
        constexpr std::uint32_t kCategoryId = 86;

        enum RowId : std::uint32_t
        {
            kSnapTurn = 8601,
            kSnapTurnAngle,
            kRecenterAfterLoading,
            kControllerLayout,
        };

        // SettingsOptionListEntry.SDT_*
        constexpr std::uint32_t kTypeStepper  = 1;
        constexpr std::uint32_t kTypeCheckBox = 3;

        constexpr std::array<float, 4>       kSnapAngles{ 30.0f, 45.0f, 60.0f, 90.0f };
        constexpr std::array<const char*, 4> kSnapAngleLabels{ "30\xC2\xB0", "45\xC2\xB0", "60\xC2\xB0", "90\xC2\xB0" };
        constexpr std::array<const char*, 2> kLayoutLabels{ "Matching letters", "Legacy" };

        constexpr int kTickInterval = 5;

        bool IsOurRow(std::uint32_t id) { return id >= kSnapTurn && id <= kControllerLayout; }

        std::uint32_t SnapAngleIndex(float degrees)
        {
            std::uint32_t best = 0;
            for (std::uint32_t i = 1; i < kSnapAngles.size(); ++i) {
                if (std::abs(kSnapAngles[i] - degrees) < std::abs(kSnapAngles[best] - degrees)) {
                    best = i;
                }
            }
            return best;
        }

        void ApplyChange(std::uint32_t id, double value)
        {
            auto& s = GameFlow::gStore.internalSettings;
            switch (id) {
            case kSnapTurn:
                s.turnMode = value != 0.0 ? 0 : 1;
                break;
            case kSnapTurnAngle:
                s.snapTurnDegrees = kSnapAngles[std::min<std::size_t>((std::size_t)value, kSnapAngles.size() - 1)];
                break;
            case kRecenterAfterLoading:
                s.recenterAfterLoading = value != 0.0;
                break;
            case kControllerLayout:
                s.controllerLayout = std::min<int>((int)value, (int)kLayoutLabels.size() - 1);
                break;
            default:
                return;
            }
            spdlog::info("[VRSettingsMenu] Setting {} changed to {}", id, value);
            CreationEngineEntry::Get()->sync_from_store();
            g_framework->request_save_config();
        }

        class ValueChangeHandler final : public GFx::FunctionHandler
        {
        public:
            void Call(const GFx::FunctionParams* params) override
            {
                if (!params || params->argCount < 1) {
                    return;
                }
                auto& event = params->args[0];

                GFx::Value payload, id, value;
                if (!event.GetMember("params", &payload) || !payload.GetMember("id", &id) || !payload.GetMember("value", &value)) {
                    return;
                }
                const auto setting_id = static_cast<std::uint32_t>(id.GetNumber());
                if (!IsOurRow(setting_id)) {
                    return;
                }

                // Our IDs must not reach the game's settings code.
                event.Invoke("stopPropagation", nullptr, nullptr, 0);
                ApplyChange(setting_id, value.GetNumber());
            }
        };

        ValueChangeHandler g_value_change_handler;
        void*              g_listened_list{ nullptr };
        int                g_frame{ 0 };

        std::mutex                          g_menus_mutex;
        std::unordered_map<void*, void*>    g_menu_by_movie;  // movie -> IMenu

        void SetText(GFx::Value& obj, const char* name, const char* text) { obj.SetMember(name, GFx::Value(text)); }

        void MakeRow(GFx::MovieRoot* root, GFx::Value* out, std::uint32_t id, std::uint32_t type, const char* text, const char* description)
        {
            root->CreateObject(out);
            out->SetMember("uID", GFx::Value(id));
            out->SetMember("uCategory", GFx::Value(kCategoryId));
            out->SetMember("uType", GFx::Value(type));
            SetText(*out, "sText", text);
            SetText(*out, "sDescription", description);
            SetText(*out, "sPreview", "");
            out->SetMember("bEnabled", GFx::Value(true));
            out->SetMember("bSubSetting", GFx::Value(false));

            // SettingsOptionListEntry.SetEntryText reads every block regardless of type.
            GFx::Value slider;
            root->CreateObject(&slider);
            slider.SetMember("fValue", GFx::Value(0.0));
            SetText(slider, "sDisplayValue", "");
            out->SetMember("sliderData", slider);

            GFx::Value stepper, options;
            root->CreateObject(&stepper);
            root->CreateArray(&options);
            stepper.SetMember("aStepperOptions", options);
            stepper.SetMember("uIndex", GFx::Value(0u));
            out->SetMember("stepperData", stepper);

            GFx::Value checkbox;
            root->CreateObject(&checkbox);
            checkbox.SetMember("bChecked", GFx::Value(false));
            out->SetMember("checkBoxData", checkbox);
        }

        template <std::size_t N>
        void SetStepperOptions(GFx::MovieRoot* root, GFx::Value& row, const std::array<const char*, N>& labels)
        {
            GFx::Value stepper, options;
            if (!row.GetMember("stepperData", &stepper)) {
                return;
            }
            root->CreateArray(&options);
            for (auto label : labels) {
                options.PushBack(GFx::Value(label));
            }
            stepper.SetMember("aStepperOptions", options);
        }

        void RefreshRowValue(GFx::Value& row, std::uint32_t id)
        {
            const auto& s = GameFlow::gStore.internalSettings;
            GFx::Value block;
            switch (id) {
            case kSnapTurn:
                if (row.GetMember("checkBoxData", &block)) block.SetMember("bChecked", GFx::Value(s.turnMode == 0));
                break;
            case kRecenterAfterLoading:
                if (row.GetMember("checkBoxData", &block)) block.SetMember("bChecked", GFx::Value(s.recenterAfterLoading));
                break;
            case kSnapTurnAngle:
                if (row.GetMember("stepperData", &block)) block.SetMember("uIndex", GFx::Value(SnapAngleIndex(s.snapTurnDegrees)));
                break;
            case kControllerLayout:
                if (row.GetMember("stepperData", &block)) block.SetMember("uIndex", GFx::Value((std::uint32_t)s.controllerLayout));
                break;
            default:
                break;
            }
        }

        bool ArrayHasId(GFx::Value& array, std::uint32_t wanted, bool refreshOurs)
        {
            bool found = false;
            const auto size = array.GetArraySize();
            for (std::uint32_t i = 0; i < size; ++i) {
                GFx::Value element, uid;
                if (!array.GetElement(i, &element) || !element.GetMember("uID", &uid)) {
                    continue;
                }
                const auto id = static_cast<std::uint32_t>(uid.GetNumber());
                if (id == wanted) {
                    found = true;
                }
                if (refreshOurs && IsOurRow(id)) {
                    RefreshRowValue(element, id);
                }
            }
            return found;
        }

        bool GetProvider(GFx::Value& dataManager, const char* name, GFx::Value* provider, GFx::Value* data)
        {
            // Don't create the provider before the game does.
            GFx::Value args[] = { GFx::Value(name), GFx::Value(false) };
            if (!dataManager.Invoke("GetDataFromClient", provider, args, 2) || !provider->IsObjectLike()) {
                return false;
            }
            return provider->GetMember("data", data) && data->IsObjectLike();
        }

        void EnsureCategory(GFx::MovieRoot* root, GFx::Value& dataManager)
        {
            GFx::Value provider, data, headers;
            if (!GetProvider(dataManager, "SettingsCategoriesData", &provider, &data) || !data.GetMember("aCategoryHeaders", &headers)) {
                return;
            }
            if (headers.GetArraySize() == 0 || ArrayHasId(headers, kCategoryId, false)) {
                return;
            }

            GFx::Value header;
            root->CreateObject(&header);
            header.SetMember("uID", GFx::Value(kCategoryId));
            SetText(header, "sText", "VR");
            header.SetMember("bDisabled", GFx::Value(false));
            headers.PushBack(header);

            provider.Invoke("DispatchChange", nullptr, nullptr, 0);
            spdlog::info("[VRSettingsMenu] Added VR category");
        }

        void EnsureRows(GFx::MovieRoot* root, GFx::Value& dataManager)
        {
            GFx::Value provider, data, rows;
            if (!GetProvider(dataManager, "SettingsData", &provider, &data) || !data.GetMember("aGeneralSettingsList", &rows)) {
                return;
            }
            if (ArrayHasId(rows, kSnapTurn, true)) {
                return;
            }

            {
                GFx::Value row;
                MakeRow(root, &row, kSnapTurn, kTypeCheckBox, "Snap Turn",
                    "Turn in fixed steps with the right stick. Turn it off to turn smoothly instead.");
                RefreshRowValue(row, kSnapTurn);
                rows.PushBack(row);
            }
            {
                GFx::Value row;
                MakeRow(root, &row, kSnapTurnAngle, kTypeStepper, "Snap Turn Angle", "How far each snap turn rotates you.");
                SetStepperOptions(root, row, kSnapAngleLabels);
                RefreshRowValue(row, kSnapTurnAngle);
                rows.PushBack(row);
            }
            {
                GFx::Value row;
                MakeRow(root, &row, kRecenterAfterLoading, kTypeCheckBox, "Recenter After Loading Screens",
                    "Reset your view height and direction when a loading screen ends.");
                RefreshRowValue(row, kRecenterAfterLoading);
                rows.PushBack(row);
            }
            {
                GFx::Value row;
                MakeRow(root, &row, kControllerLayout, kTypeStepper, "Controller Layout",
                    "Matching letters: each controller button sends the gamepad button with the same label, so on-screen prompts match.");
                SetStepperOptions(root, row, kLayoutLabels);
                RefreshRowValue(row, kControllerLayout);
                rows.PushBack(row);
            }

            provider.Invoke("DispatchChange", nullptr, nullptr, 0);
            spdlog::info("[VRSettingsMenu] Added VR rows");
        }

        void EnsureListener(GFx::MovieRoot* root, GFx::Value& panel)
        {
            GFx::Value options_panel, list;
            if (!panel.GetMember("OptionsPanel_mc", &options_panel) || !options_panel.GetMember("OptionsList_mc", &list)) {
                return;
            }
            if (list.GetData() == g_listened_list) {
                return;
            }

            GFx::Value args[] = { GFx::Value("SettingsOptionEntry_ValueChanged"), GFx::Value() };
            root->CreateFunction(&args[1], &g_value_change_handler);
            if (list.Invoke("addEventListener", nullptr, args, 2)) {
                g_listened_list = list.GetData();
                spdlog::info("[VRSettingsMenu] Listening for option changes");
            }
        }
    }

    void OnMenuMovieCreated(void* menu)
    {
        if (!menu) {
            return;
        }
        // IMenu: GetName() is vfunc 3, the movie is at +0x88 and the root object (menuObj) at +0x58.
        auto       vtable = *reinterpret_cast<void***>(menu);
        const auto name   = reinterpret_cast<const char* (*)(void*)>(vtable[3])(menu);
        auto       movie  = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(menu) + 0x88);
        if (!name || !movie || (std::string_view{ name } != "PauseMenu" && std::string_view{ name } != "MainMenu")) {
            return;
        }
        std::scoped_lock _{ g_menus_mutex };
        g_menu_by_movie[movie] = menu;
        spdlog::info("[VRSettingsMenu] Tracking {} movie {:p}", name, movie);
    }

    void OnMovieFrame(void* movieImpl, std::string_view fileUrl)
    {
        if (fileUrl != "Interface/PauseMenu.swf" && fileUrl != "Interface/PauseMenu_LRG.swf" &&
            fileUrl != "Interface/MainMenu.swf" && fileUrl != "Interface/MainMenu_LRG.swf") {
            return;
        }
        if (++g_frame % kTickInterval != 0) {
            return;
        }

        auto root = GFx::MovieRoot::FromMovie(movieImpl);
        if (!root) {
            return;
        }

        const GFx::Value* stage_root = nullptr;
        {
            std::scoped_lock _{ g_menus_mutex };
            if (auto it = g_menu_by_movie.find(movieImpl); it != g_menu_by_movie.end()) {
                stage_root = reinterpret_cast<const GFx::Value*>(static_cast<std::uint8_t*>(it->second) + 0x58);
            }
        }
        GFx::Value variable_root;
        if (!stage_root || !stage_root->IsObjectLike()) {
            if (root->GetVariable(&variable_root, "root")) {
                stage_root = &variable_root;
            }
        }

        GFx::Value panel, visible;
        if (!stage_root || !stage_root->GetMember("SettingsPanel_mc", &panel)) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                spdlog::warn("[VRSettingsMenu] SettingsPanel_mc not found in {} (menu root {}, type {})", fileUrl,
                    stage_root == &variable_root ? "from GetVariable" : (stage_root ? "from IMenu" : "missing"),
                    stage_root ? (int)stage_root->GetType() : -1);
            }
            return;
        }
        if (!panel.GetMember("visible", &visible) || !visible.GetBool()) {
            return;
        }

        GFx::Value loader_info, domain, data_manager;
        GFx::Value class_name[] = { GFx::Value("Shared.AS3.Data.BSUIDataManager") };
        if (!stage_root->GetMember("loaderInfo", &loader_info) || !loader_info.GetMember("applicationDomain", &domain) ||
            !domain.Invoke("getDefinition", &data_manager, class_name, 1) || !data_manager.IsObjectLike()) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                spdlog::warn("[VRSettingsMenu] BSUIDataManager class not reachable");
            }
            return;
        }

        EnsureListener(root, panel);
        EnsureCategory(root, data_manager);
        EnsureRows(root, data_manager);
    }
}
