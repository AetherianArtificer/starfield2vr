#include "GpuQueueTiming.h"

#include <Framework.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <d3d12.h>
#include <memory>
#include <mutex>
#include <utility/PointerHook.hpp>
#include <vector>
#include <wrl/client.h>

namespace GpuQueueTiming
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr int kExecuteSlot = 10;  // ID3D12CommandQueue::ExecuteCommandLists
        using ExecuteFn            = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

        // Pre-recorded lists that each write one timestamp, per queue type (0 direct, 1 compute).
        constexpr int kMarkers = 1024;
        struct Marker
        {
            ComPtr<ID3D12CommandAllocator>    allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            ID3D12Fence*                      fence{ nullptr };
            uint64_t                          value{ 0 };  // in use until the fence reaches it
        };
        std::array<std::array<Marker, kMarkers>, 2> g_markers{};
        std::array<int, 2>                           g_next{};

        struct QueueFence
        {
            ID3D12CommandQueue* queue{ nullptr };
            ComPtr<ID3D12Fence> fence;
            uint64_t            value{ 0 };
            int                 type{ 0 };
        };
        std::vector<QueueFence> g_queues;

        struct Pending
        {
            uint32_t     frame;
            int          type;
            int          begin, end;
            ID3D12Fence* fence;
            uint64_t     value;
        };
        std::vector<Pending> g_pending;

        std::mutex                     g_mutex;
        std::unique_ptr<PointerHook>   g_hook;
        ExecuteFn                      g_execute{ nullptr };
        ComPtr<ID3D12QueryHeap>        g_heap;
        ComPtr<ID3D12Resource>         g_readback;
        const uint64_t*                g_timestamps{ nullptr };
        uint64_t                       g_hz{ 0 };
        bool                           g_tried{ false };
        std::atomic<uint32_t>          g_frame{ 0 };

        // Per frame: busy time per queue type, and the graphics queue's first start and last end.
        struct FrameTimes
        {
            uint32_t frame{ 0xFFFFFFFF };
            uint64_t busy[2]{};
            uint64_t first{ UINT64_MAX }, last{ 0 };
            int      submits[2]{};
        };
        constexpr int                    kFrames = 8;
        std::array<FrameTimes, kFrames>  g_frames{};
        double                           g_sum_busy[2]{}, g_sum_span{ 0 }, g_sum_submits[2]{};
        int                              g_counted{ 0 };
        std::chrono::steady_clock::time_point g_last_report{};

        int TypeIndex(D3D12_COMMAND_LIST_TYPE type)
        {
            return type == D3D12_COMMAND_LIST_TYPE_DIRECT ? 0 : type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1 : -1;
        }

        QueueFence* FenceFor(ID3D12CommandQueue* queue)
        {
            for (auto& q : g_queues) {
                if (q.queue == queue) {
                    return &q;
                }
            }
            const int type = TypeIndex(queue->GetDesc().Type);
            if (type < 0 || g_queues.size() >= 16) {
                return nullptr;
            }
            ComPtr<ID3D12Device> device;
            QueueFence           entry{ queue, nullptr, 0, type };
            if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&entry.fence)))) {
                return nullptr;
            }
            g_queues.push_back(entry);
            return &g_queues.back();
        }

        // A marker whose last use has finished, or -1.
        int TakeMarker(int type)
        {
            const int index  = g_next[type];
            auto&     marker = g_markers[type][index];
            if (marker.fence != nullptr && marker.fence->GetCompletedValue() < marker.value) {
                return -1;
            }
            g_next[type] = (index + 1) % kMarkers;
            return index;
        }

        void STDMETHODCALLTYPE OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
        {
            std::unique_lock lock{ g_mutex };
            auto             fence = FenceFor(queue);
            const int        type  = fence ? fence->type : -1;
            const int        begin = type >= 0 ? TakeMarker(type) : -1;
            const int        end   = begin >= 0 ? TakeMarker(type) : -1;
            if (end < 0) {
                lock.unlock();
                g_execute(queue, count, lists);
                return;
            }
            std::vector<ID3D12CommandList*> all;
            all.reserve(count + 2);
            all.push_back(g_markers[type][begin].list.Get());
            all.insert(all.end(), lists, lists + count);
            all.push_back(g_markers[type][end].list.Get());
            g_execute(queue, (UINT)all.size(), all.data());

            const uint64_t value = ++fence->value;
            queue->Signal(fence->fence.Get(), value);
            for (int index : { begin, end }) {
                g_markers[type][index].fence = fence->fence.Get();
                g_markers[type][index].value = value;
            }
            if (g_pending.size() < 4096) {
                g_pending.push_back({ g_frame.load(std::memory_order_relaxed), type, type * kMarkers + begin, type * kMarkers + end, fence->fence.Get(), value });
            }
        }

        bool Install()
        {
            g_tried     = true;
            auto& hook  = g_framework->get_d3d12_hook();
            auto  device = hook ? hook->get_device() : nullptr;
            auto  game_queue = hook ? hook->get_command_queue() : nullptr;
            if (device == nullptr || game_queue == nullptr) {
                g_tried = false;
                return false;
            }
            if (FAILED(game_queue->GetTimestampFrequency(&g_hz))) {
                return false;
            }
            D3D12_QUERY_HEAP_DESC heap_desc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kMarkers * 2, 0 };
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC   desc{};
            desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width            = kMarkers * 2 * sizeof(uint64_t);
            desc.Height           = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels        = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&g_heap))) ||
                FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback))) ||
                FAILED(g_readback->Map(0, nullptr, (void**)&g_timestamps))) {
                spdlog::warn("[Perf] GPU queue timing unavailable");
                return false;
            }
            for (int type = 0; type < 2; ++type) {
                const auto list_type = type == 0 ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE;
                for (int i = 0; i < kMarkers; ++i) {
                    auto&     marker = g_markers[type][i];
                    const int query  = type * kMarkers + i;
                    if (FAILED(device->CreateCommandAllocator(list_type, IID_PPV_ARGS(&marker.allocator))) ||
                        FAILED(device->CreateCommandList(0, list_type, marker.allocator.Get(), nullptr, IID_PPV_ARGS(&marker.list)))) {
                        spdlog::warn("[Perf] GPU queue timing unavailable: no command lists");
                        return false;
                    }
                    marker.list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
                    marker.list->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 1, g_readback.Get(), query * sizeof(uint64_t));
                    marker.list->Close();
                }
            }
            auto vtable = *reinterpret_cast<void***>(game_queue);
            g_hook      = std::make_unique<PointerHook>(&vtable[kExecuteSlot], (void*)&OnExecute);
            g_execute   = g_hook->get_original<ExecuteFn>();
            spdlog::info("[Perf] GPU queue timing installed");
            return true;
        }

        void Collect()
        {
            std::scoped_lock _{ g_mutex };
            const uint32_t   current = g_frame.load();
            auto             it      = g_pending.begin();
            while (it != g_pending.end()) {
                if (it->fence->GetCompletedValue() < it->value) {
                    ++it;
                    continue;
                }
                const uint64_t start = g_timestamps[it->begin];
                const uint64_t stop  = g_timestamps[it->end];
                auto&          f     = g_frames[it->frame % kFrames];
                if (f.frame != it->frame) {
                    f       = {};
                    f.frame = it->frame;
                }
                if (stop > start) {
                    f.busy[it->type] += stop - start;
                    if (it->type == 0) {
                        f.first = std::min(f.first, start);
                        f.last  = std::max(f.last, stop);
                    }
                }
                ++f.submits[it->type];
                it = g_pending.erase(it);
            }
            // Frames four behind have all their submissions back.
            auto& done = g_frames[(current - 4) % kFrames];
            if (current >= 4 && done.frame == current - 4) {
                const double ms = 1000.0 / (double)g_hz;
                for (int type = 0; type < 2; ++type) {
                    g_sum_busy[type] += done.busy[type] * ms;
                    g_sum_submits[type] += done.submits[type];
                }
                if (done.last > done.first) {
                    g_sum_span += (done.last - done.first) * ms;
                }
                ++g_counted;
                done.frame = 0xFFFFFFFF;
            }
        }
    }

    void OnPresent()
    {
        if (!g_tried) {
            Install();
        }
        if (g_execute == nullptr) {
            return;
        }
        Collect();
        g_frame.fetch_add(1);

        const auto now = std::chrono::steady_clock::now();
        if (now - g_last_report >= std::chrono::seconds(5)) {
            if (g_counted > 0) {
                const double n = g_counted;
                spdlog::info("[Perf] GPU per frame: graphics queue busy {:.1f} ms over {:.1f} ms from first to last submission ({:.0f} submissions), compute queue busy {:.1f} ms "
                             "({:.0f} submissions)",
                             g_sum_busy[0] / n, g_sum_span / n, g_sum_submits[0] / n, g_sum_busy[1] / n, g_sum_submits[1] / n);
            }
            g_sum_busy[0] = g_sum_busy[1] = g_sum_span = g_sum_submits[0] = g_sum_submits[1] = 0;
            g_counted     = 0;
            g_last_report = now;
        }
    }
}
