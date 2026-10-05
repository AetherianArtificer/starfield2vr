#include "FoveatedRendering.h"

#include <Framework.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <d3d12.h>
#include <memory>
#include <utility/PointerHook.hpp>
#include <vector>
#include <wrl/client.h>

#include "CreationEngineSettings.h"
#include <mods/VR.hpp>

namespace FoveatedRendering
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        // ID3D12GraphicsCommandList5 vtable slots.
        constexpr int kSetViewportsSlot        = 21;
        constexpr int kSetShadingRateSlot      = 77;
        constexpr int kSetShadingRateImageSlot = 78;

        using SetViewportsFn        = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList5*, UINT, const D3D12_VIEWPORT*);
        using SetShadingRateFn      = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList5*, D3D12_SHADING_RATE, const D3D12_SHADING_RATE_COMBINER*);
        using SetShadingRateImageFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList5*, ID3D12Resource*);

        std::unique_ptr<PointerHook> g_viewports_hook, g_rate_hook, g_image_hook;
        SetViewportsFn               g_set_viewports{ nullptr };
        SetShadingRateFn             g_set_rate{ nullptr };
        SetShadingRateImageFn        g_set_image{ nullptr };
        bool                         g_install_tried{ false };
        UINT                         g_tile{ 0 };
        bool                         g_rate_4x4{ false };

        // Read by the command list hooks on the render threads.
        std::atomic<bool>     g_active{ false };
        std::atomic<uint32_t> g_output_w{ 0 }, g_output_h{ 0 };
        std::atomic<uint64_t> g_wanted_size{ 0 };  // scene viewport size with no image yet, width << 32 | height

        // Images per scene viewport size, published by the game thread; retired ones stay alive.
        struct Image
        {
            uint32_t        w{ 0 }, h{ 0 };
            int             level{ 0 };
            ID3D12Resource* resource{ nullptr };
        };
        constexpr int                       kMaxImages = 8;
        std::array<Image, kMaxImages>       g_images{};
        std::atomic<int>                    g_image_count{ 0 };
        std::vector<ComPtr<ID3D12Resource>> g_owned;
        std::atomic<int>                    g_level{ 0 };

        std::atomic<uint32_t> g_scene_binds{ 0 }, g_game_images{ 0 };

        // The command list being recorded on this thread, and whether our image is bound on it.
        struct ListState
        {
            ID3D12GraphicsCommandList5* list{ nullptr };
            bool                        scene{ false };
            bool                        ours{ false };
            D3D12_SHADING_RATE          base{ D3D12_SHADING_RATE_1X1 };
            D3D12_SHADING_RATE_COMBINER first{ D3D12_SHADING_RATE_COMBINER_PASSTHROUGH };
        };
        thread_local ListState t_state{};

        ListState& StateFor(ID3D12GraphicsCommandList5* list)
        {
            if (t_state.list != list) {
                t_state      = {};
                t_state.list = list;
            }
            return t_state;
        }

        ID3D12Resource* ImageFor(uint32_t w, uint32_t h)
        {
            const int count = g_image_count.load(std::memory_order_acquire);
            const int level = g_level.load(std::memory_order_relaxed);
            for (int i = 0; i < count; ++i) {
                if (g_images[i].w == w && g_images[i].h == h && g_images[i].level == level) {
                    return g_images[i].resource;
                }
            }
            return nullptr;
        }

        // The eyes' scene passes render below the output resolution, with the eyes' aspect ratio. Shadow maps, probes,
        // reduced-resolution effects and everything after DLSS fall outside this.
        bool IsScenePass(const D3D12_VIEWPORT& vp)
        {
            const float ow = (float)g_output_w.load(std::memory_order_relaxed);
            const float oh = (float)g_output_h.load(std::memory_order_relaxed);
            if (ow <= 0.0f || oh <= 0.0f || vp.Height <= 0.0f || vp.TopLeftX != 0.0f || vp.TopLeftY != 0.0f) {
                return false;
            }
            const float aspect = ow / oh;
            return std::fabs(vp.Width / vp.Height - aspect) < aspect * 0.01f && vp.Width < ow * 0.98f && vp.Width >= ow * 0.33f;
        }

        void BindOurs(ID3D12GraphicsCommandList5* list, ListState& state, ID3D12Resource* image)
        {
            const D3D12_SHADING_RATE_COMBINER combiners[2]{ state.first, D3D12_SHADING_RATE_COMBINER_MAX };
            g_set_rate(list, state.base, combiners);
            g_set_image(list, image);
            state.ours = true;
        }

        void Unbind(ID3D12GraphicsCommandList5* list, ListState& state)
        {
            const D3D12_SHADING_RATE_COMBINER combiners[2]{ state.first, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH };
            g_set_rate(list, state.base, combiners);
            g_set_image(list, nullptr);
            state.ours = false;
        }

        void STDMETHODCALLTYPE OnSetViewports(ID3D12GraphicsCommandList5* list, UINT count, const D3D12_VIEWPORT* viewports)
        {
            g_set_viewports(list, count, viewports);
            if (!g_active.load(std::memory_order_relaxed)) {
                if (t_state.list == list && t_state.ours) {
                    Unbind(list, t_state);
                }
                return;
            }
            auto& state = StateFor(list);
            state.scene = count > 0 && viewports != nullptr && IsScenePass(viewports[0]);
            if (state.scene) {
                const auto w     = (uint32_t)viewports[0].Width;
                const auto h     = (uint32_t)viewports[0].Height;
                auto       image = ImageFor(w, h);
                if (image != nullptr) {
                    BindOurs(list, state, image);
                    g_scene_binds.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                g_wanted_size.store((uint64_t)w << 32 | h, std::memory_order_relaxed);
            }
            if (state.ours) {
                Unbind(list, state);
            }
        }

        void STDMETHODCALLTYPE OnSetShadingRate(ID3D12GraphicsCommandList5* list, D3D12_SHADING_RATE base, const D3D12_SHADING_RATE_COMBINER* combiners)
        {
            auto& state = StateFor(list);
            state.base  = base;
            state.first = combiners ? combiners[0] : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
            if (!state.ours) {
                g_set_rate(list, base, combiners);
                return;
            }
            const D3D12_SHADING_RATE_COMBINER ours[2]{ state.first, D3D12_SHADING_RATE_COMBINER_MAX };
            g_set_rate(list, base, ours);
        }

        void STDMETHODCALLTYPE OnSetShadingRateImage(ID3D12GraphicsCommandList5* list, ID3D12Resource* image)
        {
            auto& state = StateFor(list);
            if (image != nullptr) {
                g_game_images.fetch_add(1, std::memory_order_relaxed);
            }
            // On the scene passes our image replaces the game's own.
            if (state.ours && state.scene) {
                return;
            }
            g_set_image(list, image);
        }

        void Install()
        {
            g_install_tried = true;
            auto& hook      = g_framework->get_d3d12_hook();
            auto  device    = hook ? hook->get_device() : nullptr;
            if (device == nullptr) {
                g_install_tried = false;
                return;
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options, sizeof(options))) ||
                options.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2 || options.ShadingRateImageTileSize == 0) {
                spdlog::info("[Foveated] Variable rate shading tier 2 not supported; foveated rendering unavailable");
                return;
            }
            g_tile     = options.ShadingRateImageTileSize;
            g_rate_4x4 = options.AdditionalShadingRatesSupported != FALSE;

            ComPtr<ID3D12CommandAllocator>     allocator;
            ComPtr<ID3D12GraphicsCommandList5> list;
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
                spdlog::error("[Foveated] Could not create a command list to hook");
                return;
            }
            list->Close();
            auto vtable       = *reinterpret_cast<void***>(list.Get());
            g_viewports_hook  = std::make_unique<PointerHook>(&vtable[kSetViewportsSlot], (void*)&OnSetViewports);
            g_rate_hook       = std::make_unique<PointerHook>(&vtable[kSetShadingRateSlot], (void*)&OnSetShadingRate);
            g_image_hook      = std::make_unique<PointerHook>(&vtable[kSetShadingRateImageSlot], (void*)&OnSetShadingRateImage);
            g_set_viewports   = g_viewports_hook->get_original<SetViewportsFn>();
            g_set_rate        = g_rate_hook->get_original<SetShadingRateFn>();
            g_set_image       = g_image_hook->get_original<SetShadingRateImageFn>();
            spdlog::info("[Foveated] Installed: {} px tiles, 4x4 rate {}", g_tile, g_rate_4x4 ? "supported" : "not supported");

            auto       settings = CreationEngineSettings::Get();
            using Type          = CreationEngineSettings::SettingType;
            constexpr auto kVrs = "bEnableVariableRateShading:Display";
            spdlog::info("[Foveated] The game's own variable rate shading: preference {}, default {}", settings->get_setting(kVrs, Type::kINIPrefSetting, false),
                         settings->get_setting(kVrs, Type::kINISetting, false));
        }

        // Full rate in the middle, half rate in a ring, quarter rate (or half) beyond, by distance from the view centre
        // as a fraction of the half-size.
        std::vector<uint8_t> Pattern(uint32_t tiles_x, uint32_t tiles_y, uint32_t w, uint32_t h, int level)
        {
            constexpr std::array<std::array<float, 2>, 4> kRadii{ { { 9.0f, 9.0f }, { 0.65f, 0.95f }, { 0.50f, 0.78f }, { 0.40f, 0.65f } } };
            const auto  radii = kRadii[std::clamp(level, 0, 3)];
            const auto  outer = g_rate_4x4 ? D3D12_SHADING_RATE_4X4 : D3D12_SHADING_RATE_2X2;
            std::vector<uint8_t> data(tiles_x * tiles_y);
            for (uint32_t y = 0; y < tiles_y; ++y) {
                for (uint32_t x = 0; x < tiles_x; ++x) {
                    const float px = ((float)x + 0.5f) * g_tile / (float)w * 2.0f - 1.0f;
                    const float py = ((float)y + 0.5f) * g_tile / (float)h * 2.0f - 1.0f;
                    const float r  = std::sqrt(px * px + py * py);
                    data[y * tiles_x + x] = (uint8_t)(r < radii[0] ? D3D12_SHADING_RATE_1X1 : r < radii[1] ? D3D12_SHADING_RATE_2X2 : outer);
                }
            }
            return data;
        }

        ComPtr<ID3D12Resource> BuildImage(uint32_t w, uint32_t h, int level)
        {
            auto device = g_framework->get_d3d12_hook()->get_device();
            if (device == nullptr) {
                return nullptr;
            }
            const uint32_t tiles_x = (w + g_tile - 1) / g_tile;
            const uint32_t tiles_y = (h + g_tile - 1) / g_tile;

            D3D12_HEAP_PROPERTIES gpu_heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC   desc{};
            desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width            = tiles_x;
            desc.Height           = tiles_y;
            desc.DepthOrArraySize = 1;
            desc.MipLevels        = 1;
            desc.Format           = DXGI_FORMAT_R8_UINT;
            desc.SampleDesc.Count = 1;
            ComPtr<ID3D12Resource> image;
            if (FAILED(device->CreateCommittedResource(&gpu_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image)))) {
                return nullptr;
            }

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64                             upload_size = 0;
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &upload_size);
            D3D12_HEAP_PROPERTIES upload_heap{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC   buffer{};
            buffer.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width            = upload_size;
            buffer.Height           = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels        = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> upload;
            if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) {
                return nullptr;
            }
            const auto pattern = Pattern(tiles_x, tiles_y, w, h, level);
            uint8_t*   mapped  = nullptr;
            if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
                return nullptr;
            }
            for (uint32_t y = 0; y < tiles_y; ++y) {
                memcpy(mapped + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch, pattern.data() + (size_t)y * tiles_x, tiles_x);
            }
            upload->Unmap(0, nullptr);

            D3D12_COMMAND_QUEUE_DESC           queue_desc{ D3D12_COMMAND_LIST_TYPE_DIRECT };
            ComPtr<ID3D12CommandQueue>         queue;
            ComPtr<ID3D12CommandAllocator>     allocator;
            ComPtr<ID3D12GraphicsCommandList>  list;
            ComPtr<ID3D12Fence>                fence;
            if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
                FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
                FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                return nullptr;
            }
            D3D12_TEXTURE_COPY_LOCATION dst{ image.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint = footprint;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = image.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
            list->ResourceBarrier(1, &barrier);
            list->Close();
            ID3D12CommandList* lists[]{ list.Get() };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fence.Get(), 1);
            auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(1, event);
            WaitForSingleObject(event, 2000);
            CloseHandle(event);
            if (fence->GetCompletedValue() < 1) {
                spdlog::error("[Foveated] Shading rate image upload timed out");
                return nullptr;
            }
            return image;
        }
    }

    void OnFrameStart(bool stereo, int level)
    {
        if (!g_install_tried) {
            Install();
        }
        if (g_set_image == nullptr) {
            return;
        }

        // Images are only ever added, as frames in flight may still use the old ones.
        g_level.store(level, std::memory_order_relaxed);
        const auto size = g_framework->get_d3d12_rt_size();
        g_output_w.store((uint32_t)size.x, std::memory_order_relaxed);
        g_output_h.store((uint32_t)size.y, std::memory_order_relaxed);
        g_active.store(stereo && level > 0, std::memory_order_relaxed);

        const auto wanted = g_wanted_size.exchange(0, std::memory_order_relaxed);
        const int  count  = g_image_count.load(std::memory_order_relaxed);
        if (wanted != 0 && level > 0 && count < kMaxImages) {
            const auto w = (uint32_t)(wanted >> 32), h = (uint32_t)wanted;
            if (ImageFor(w, h) == nullptr) {
                if (auto image = BuildImage(w, h, level)) {
                    g_images[count] = { w, h, level, image.Get() };
                    g_owned.push_back(image);
                    g_image_count.store(count + 1, std::memory_order_release);
                    spdlog::info("[Foveated] Shading rate image for {}x{} scene passes, level {}", w, h, level);
                }
            }
        }

        static auto last_report = std::chrono::steady_clock::now();
        static int  frames      = 0;
        ++frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(10)) {
            spdlog::info("[Foveated] Level {}, {}: {:.0f} scene pass binds per frame, {:.0f} game shading rate images per frame", level,
                         g_active.load() ? "active" : "inactive", (double)g_scene_binds.exchange(0) / frames, (double)g_game_images.exchange(0) / frames);
            last_report = now;
            frames      = 0;
        }
    }
}
