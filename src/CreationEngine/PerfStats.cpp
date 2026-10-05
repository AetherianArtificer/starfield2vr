#include "PerfStats.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include <Framework.hpp>
#include <array>
#include <atomic>
#include <d3d12.h>
#include <mutex>
#include <mods/VR.hpp>
#include <wrl/client.h>

namespace PerfStats
{
    namespace
    {
        using clock = std::chrono::steady_clock;

        constexpr auto kReportInterval = std::chrono::seconds(5);

        clock::time_point  g_last_present{};
        clock::time_point  g_window_start{};
        std::vector<float> g_frame_ms;

        // Timestamp ring: per frame slot, begin and end of the frame and of each eye's scene graph.
        constexpr int kSlots     = 4;
        constexpr int kPerSlot   = 4;
        constexpr int kQueries   = kSlots * kPerSlot;
        std::mutex                                    g_gpu_mutex;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap>       g_query_heap;
        Microsoft::WRL::ComPtr<ID3D12Resource>        g_readback;
        uint64_t                                      g_timestamp_hz{ 0 };
        std::array<std::atomic<uint32_t>, kSlots>     g_written{};
        std::array<int, kSlots>                       g_slot_frame{};
        std::array<double, 3>                         g_gpu_sum_ms{};
        std::array<int, 3>                            g_gpu_samples{};

        bool EnsureQueries()
        {
            if (g_query_heap != nullptr) {
                return true;
            }
            auto& hook = g_framework->get_d3d12_hook();
            auto device = hook ? hook->get_device() : nullptr;
            auto queue  = hook ? hook->get_command_queue() : nullptr;
            if (device == nullptr || queue == nullptr || FAILED(queue->GetTimestampFrequency(&g_timestamp_hz))) {
                return false;
            }
            D3D12_QUERY_HEAP_DESC heap_desc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kQueries, 0 };
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC   desc{};
            desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width            = kQueries * sizeof(uint64_t);
            desc.Height           = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels        = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&g_query_heap))) ||
                FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback)))) {
                g_query_heap.Reset();
                return false;
            }
            return true;
        }

        // Spans whose begin and end both landed in a slot that is old enough to have finished on the GPU.
        void CollectGpu(int frame)
        {
            std::scoped_lock _{ g_gpu_mutex };
            if (g_readback == nullptr || g_timestamp_hz == 0) {
                return;
            }
            for (int slot = 0; slot < kSlots; ++slot) {
                const auto written = g_written[slot].load();
                if (written == 0 || frame - g_slot_frame[slot] < kSlots - 1) {
                    continue;
                }
                uint64_t* data = nullptr;
                const D3D12_RANGE range{ slot * kPerSlot * sizeof(uint64_t), (slot + 1) * kPerSlot * sizeof(uint64_t) };
                if (FAILED(g_readback->Map(0, &range, reinterpret_cast<void**>(&data)))) {
                    continue;
                }
                // Spans: whole frame (start to end), first eye (start to first eye), second eye (first to second eye).
                const auto base = data + slot * kPerSlot;
                constexpr int kSpans[3][2]{ { 0, 3 }, { 0, 1 }, { 1, 2 } };
                for (int span = 0; span < 3; ++span) {
                    const auto [from, to] = kSpans[span];
                    const uint32_t both   = (1u << from) | (1u << to);
                    if ((written & both) == both && base[to] > base[from]) {
                        g_gpu_sum_ms[span] += (double)(base[to] - base[from]) * 1000.0 / (double)g_timestamp_hz;
                        ++g_gpu_samples[span];
                    }
                }
                const D3D12_RANGE nothing{ 0, 0 };
                g_readback->Unmap(0, &nothing);
                g_written[slot] = 0;
            }
        }

        void Report(float seconds)
        {
            if (g_frame_ms.empty()) {
                return;
            }
            std::sort(g_frame_ms.begin(), g_frame_ms.end());
            const auto  count = g_frame_ms.size();
            float       sum   = 0.0f;
            for (auto ms : g_frame_ms) {
                sum += ms;
            }
            const float fps = count / seconds;
            const float p99 = g_frame_ms[std::min(count - 1, count * 99 / 100)];

            // Every frame carries both eyes, so the game must deliver one frame per display refresh.
            const double period    = VR::get()->get_display_period();
            const float  budget_ms = period > 0.0 ? (float)(period * 1000.0) : 0.0f;
            std::size_t  over      = 0;
            if (budget_ms > 0.0f) {
                over = count - (std::upper_bound(g_frame_ms.begin(), g_frame_ms.end(), budget_ms * 1.1f) - g_frame_ms.begin());
            }

            // GPU time of the whole frame and of each eye's scene graph; a frame GPU time near the CPU frame time means GPU-bound.
            std::array<double, 3> gpu{};
            {
                std::scoped_lock _{ g_gpu_mutex };
                for (int span = 0; span < 3; ++span) {
                    gpu[span]           = g_gpu_samples[span] > 0 ? g_gpu_sum_ms[span] / g_gpu_samples[span] : 0.0;
                    g_gpu_sum_ms[span]  = 0.0;
                    g_gpu_samples[span] = 0;
                }
            }
            spdlog::info("[Perf] {:.1f} fps | frame avg {:.1f} ms, p99 {:.1f} ms, max {:.1f} ms | budget {:.1f} ms ({:.0f} Hz), {:.0f}% over | GPU frame {:.1f} ms, eyes {:.1f} + {:.1f} ms",
                fps, sum / count, p99, g_frame_ms.back(), budget_ms, period > 0.0 ? 1.0 / period : 0.0, 100.0f * over / count, gpu[0], gpu[1], gpu[2]);

            g_frame_ms.clear();
        }
    }

    void OnPresent()
    {
        const auto now = clock::now();
        if (g_last_present.time_since_epoch().count() == 0) {
            g_last_present = now;
            g_window_start = now;
            g_frame_ms.reserve(2048);
            return;
        }

        CollectGpu(VR::get()->m_render_frame_count);
        g_frame_ms.push_back(std::chrono::duration<float, std::milli>(now - g_last_present).count());
        g_last_present = now;

        if (now - g_window_start >= kReportInterval) {
            Report(std::chrono::duration<float>(now - g_window_start).count());
            g_window_start = now;
        }
    }

    void MarkGpu(void* command_list, GpuPoint point, int frame)
    {
        auto list = static_cast<ID3D12GraphicsCommandList*>(command_list);
        if (list == nullptr || !EnsureQueries()) {
            return;
        }
        const int slot  = ((frame % kSlots) + kSlots) % kSlots;
        const int index = slot * kPerSlot + (int)point;
        if (point == GpuPoint::kFrameStart) {
            std::scoped_lock _{ g_gpu_mutex };
            g_written[slot]    = 0;
            g_slot_frame[slot] = frame;
        }
        list->EndQuery(g_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
        list->ResolveQueryData(g_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index, 1, g_readback.Get(), index * sizeof(uint64_t));
        g_written[slot] |= 1u << (int)point;
    }
}
