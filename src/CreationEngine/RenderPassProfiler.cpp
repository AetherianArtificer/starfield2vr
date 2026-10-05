#include "RenderPassProfiler.h"

#include "StereoViewModule.h"
#include <Framework.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <d3d12.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <MinHook.h>
#include <vector>
#include <wrl/client.h>

#include <mods/VR.hpp>

namespace RenderPassProfiler
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr int kMaxClasses  = 512;
        constexpr int kMaxHooks    = 400;
        constexpr int kExecuteSlot = 7;

        struct PassClass
        {
            std::string name;
        };
        std::vector<PassClass>                g_classes;
        std::unordered_map<uintptr_t, int>    g_class_of_vtable;
        std::array<uintptr_t, kMaxHooks>      g_originals{};
        int                                   g_hook_count{ 0 };
        std::atomic<bool>                     g_ready{ false };

        // Timestamps per frame slot.
        constexpr int kSlots        = 4;
        constexpr int kQueriesPerSlot = 4096;
        struct Entry
        {
            uint32_t prev, query;
            uint16_t cls;
            int8_t   eye;
        };
        ComPtr<ID3D12QueryHeap>                   g_heap;
        ComPtr<ID3D12Resource>                    g_readback;
        const uint64_t*                           g_timestamps{ nullptr };
        uint64_t                                  g_hz{ 0 };
        std::array<std::atomic<uint32_t>, kSlots> g_query_count{};
        std::array<std::atomic<uint32_t>, kSlots> g_entry_count{};
        std::array<std::vector<Entry>, kSlots>    g_entries;
        std::atomic<uint32_t>                     g_frame{ 0 };
        ID3D12Device*                             g_device{ nullptr };

        // Resolving each slot from a list of our own on the game's queue, once its frame is submitted.
        struct Resolver
        {
            ComPtr<ID3D12CommandAllocator>    allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            ComPtr<ID3D12Fence>               fence;
            uint64_t                          value{ 0 };
        };
        std::array<Resolver, kSlots> g_resolvers;

        // Per class and eye (left, right, other): GPU ms summed over the reporting window.
        std::vector<std::array<double, 3>> g_sums;
        int                                g_frames_summed{ 0 };
        auto                               g_last_report = std::chrono::steady_clock::now();

        // The command list this thread recorded the last pass into, and that pass's timestamp.
        struct ListMark
        {
            ID3D12GraphicsCommandList* list{ nullptr };
            uint32_t                   query{ 0 };
            uint32_t                   slot{ 0xFFFFFFFF };
        };
        thread_local ListMark t_mark{};

        ID3D12GraphicsCommandList* ExistingCommandList(void* render_graph_data)
        {
            __try {
                auto inner = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(render_graph_data) + 0x138);
                if (inner == nullptr) {
                    return nullptr;
                }
                auto context = *reinterpret_cast<uint8_t**>(inner + 0x120);
                return context ? *reinterpret_cast<ID3D12GraphicsCommandList**>(context + 0x60) : nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        uintptr_t VtableOf(void* object)
        {
            __try {
                return *reinterpret_cast<uintptr_t*>(object);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // The pass class of an object, or -1 when it is not a render pass: the linker folds identical functions, so a
        // hooked execute function can also be called from elsewhere with other arguments.
        int ClassOf(void* pass)
        {
            if (pass == nullptr || !g_ready.load(std::memory_order_acquire)) {
                return -1;
            }
            const auto vtable = VtableOf(pass);
            if (vtable == 0) {
                return -1;
            }
            const auto it = g_class_of_vtable.find(vtable);
            return it == g_class_of_vtable.end() ? -1 : it->second;
        }

        void Mark(int cls, void* render_graph_data)
        {
            if (cls < 0 || g_heap == nullptr || render_graph_data == nullptr) {
                return;
            }
            auto list = ExistingCommandList(render_graph_data);
            if (list == nullptr) {
                return;
            }
            const uint32_t slot  = g_frame.load(std::memory_order_relaxed) % kSlots;
            const uint32_t local = g_query_count[slot].fetch_add(1, std::memory_order_relaxed);
            if (local >= kQueriesPerSlot) {
                return;
            }
            const uint32_t query = slot * kQueriesPerSlot + local;
            list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
            if (t_mark.list == list && t_mark.slot == slot) {
                const uint32_t e = g_entry_count[slot].fetch_add(1, std::memory_order_relaxed);
                if (e < kQueriesPerSlot) {
                    g_entries[slot][e] = { t_mark.query, query, (uint16_t)cls, (int8_t)StereoViewModule::Get()->EyeOfGraphPublic(render_graph_data) };
                }
            }
            t_mark = { list, query, slot };
        }

        uintptr_t RunPass(int hook, void* pass, void* render_graph_data, void* pass_data, void* a4)
        {
            using func_t   = uintptr_t(void*, void*, void*, void*);
            const int cls  = ClassOf(pass);
            if (cls < 0) {
                return reinterpret_cast<func_t*>(g_originals[hook])(pass, render_graph_data, pass_data, a4);
            }
            static auto vr = VR::get();
            const auto result = reinterpret_cast<func_t*>(g_originals[hook])(pass, render_graph_data, pass_data, a4);
            Mark(cls, render_graph_data);
            return result;
        }

        template <int Index>
        uintptr_t Detour(void* pass, void* render_graph_data, void* pass_data, void* a4)
        {
            return RunPass(Index, pass, render_graph_data, pass_data, a4);
        }

        template <int... Index>
        constexpr std::array<uintptr_t, sizeof...(Index)> Detours(std::integer_sequence<int, Index...>)
        {
            return { reinterpret_cast<uintptr_t>(&Detour<Index>)... };
        }

        // Every render pass class's vtable, from its RTTI in the game's module.
        std::vector<std::pair<std::string, uintptr_t>> FindPassVtables()
        {
            std::vector<std::pair<std::string, uintptr_t>> found;
            const auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            const auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            const auto nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            auto       sec  = IMAGE_FIRST_SECTION(nt);
            std::vector<std::pair<uint8_t*, size_t>> data_sections, rdata_sections;
            for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                const auto name = std::string(reinterpret_cast<const char*>(sec->Name), strnlen(reinterpret_cast<const char*>(sec->Name), 8));
                const auto span = std::make_pair(base + sec->VirtualAddress, (size_t)sec->Misc.VirtualSize);
                if (name == ".data") {
                    data_sections.push_back(span);
                } else if (name == ".rdata") {
                    rdata_sections.push_back(span);
                }
            }
            // Type descriptors: the mangled name follows the vtable pointer and a spare pointer.
            constexpr char kPrefix[] = ".?AV";
            constexpr char kSuffix[] = "RenderPass@CreationRendererPrivate@@";
            std::unordered_map<uint32_t, std::string> descriptors;
            for (auto [start, size] : data_sections) {
                for (size_t i = 0x10; i + 64 < size; ++i) {
                    if (memcmp(start + i, kPrefix, 4) != 0) {
                        continue;
                    }
                    const char* name = reinterpret_cast<const char*>(start + i);
                    const auto  len  = strnlen(name, 200);
                    const auto  full = std::string_view(name, len);
                    if (full.size() > sizeof(kSuffix) && full.ends_with(kSuffix)) {
                        descriptors[(uint32_t)(start + i - 0x10 - base)] = std::string(full.substr(4, full.size() - 4 - (sizeof(kSuffix) - 1))) + "RenderPass";
                    }
                    i += len;
                }
            }
            // Complete object locators (signature 1, offset 0) naming those descriptors, then the vtables after them.
            std::unordered_map<uintptr_t, const std::string*> locators;
            for (auto [start, size] : rdata_sections) {
                for (size_t i = 0; i + 24 <= size; i += 4) {
                    const auto words = reinterpret_cast<const uint32_t*>(start + i);
                    if (words[0] == 1 && words[1] == 0) {
                        if (auto it = descriptors.find(words[3]); it != descriptors.end()) {
                            locators[reinterpret_cast<uintptr_t>(start + i)] = &it->second;
                        }
                    }
                }
            }
            for (auto [start, size] : rdata_sections) {
                for (size_t i = 0; i + 16 <= size; i += 8) {
                    if (auto it = locators.find(*reinterpret_cast<const uintptr_t*>(start + i)); it != locators.end()) {
                        found.emplace_back(*it->second, reinterpret_cast<uintptr_t>(start + i + 8));
                    }
                }
            }
            return found;
        }

        void Collect(uint32_t slot)
        {
            const uint32_t entries = std::min<uint32_t>(g_entry_count[slot].load(), kQueriesPerSlot);
            const double   ms      = 1000.0 / (double)g_hz;
            for (uint32_t i = 0; i < entries; ++i) {
                const auto& e = g_entries[slot][i];
                const auto  a = g_timestamps[e.prev];
                const auto  b = g_timestamps[e.query];
                if (b > a && (b - a) * ms < 50.0) {
                    g_sums[e.cls][e.eye == 0 ? 0 : e.eye == 1 ? 1 : 2] += (b - a) * ms;
                }
            }
            ++g_frames_summed;
        }

        void Report()
        {
            if (g_frames_summed == 0) {
                return;
            }
            const double n = g_frames_summed;
            std::vector<int> order(g_classes.size());
            for (size_t i = 0; i < order.size(); ++i) {
                order[i] = (int)i;
            }
            auto total = [](const std::array<double, 3>& s) { return s[0] + s[1] + s[2]; };
            std::sort(order.begin(), order.end(), [&](int a, int b) { return total(g_sums[a]) > total(g_sums[b]); });
            std::array<double, 3> all{};
            for (const auto& s : g_sums) {
                for (int i = 0; i < 3; ++i) {
                    all[i] += s[i];
                }
            }
            spdlog::info("[Passes] GPU per frame in timed passes: left eye {:.2f} ms, right eye {:.2f} ms, other {:.2f} ms", all[0] / n, all[1] / n, all[2] / n);
            for (int i = 0; i < 30 && i < (int)order.size(); ++i) {
                const auto& s = g_sums[order[i]];
                if (total(s) / n < 0.05) {
                    break;
                }
                spdlog::info("[Passes] {:5.2f} ms  left {:5.2f}  right {:5.2f}  other {:5.2f}  {}", total(s) / n, s[0] / n, s[1] / n, s[2] / n, g_classes[order[i]].name);
            }
            for (auto& s : g_sums) {
                s = {};
            }
            g_frames_summed = 0;
        }
    }

    void Install()
    {
        auto& hook  = g_framework->get_d3d12_hook();
        auto  queue = hook ? hook->get_command_queue() : nullptr;
        g_device    = hook ? hook->get_device() : nullptr;
        if (g_device == nullptr || queue == nullptr || FAILED(queue->GetTimestampFrequency(&g_hz))) {
            spdlog::warn("[Passes] No device yet; pass profiling off");
            return;
        }
        D3D12_QUERY_HEAP_DESC heap_desc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * kQueriesPerSlot, 0 };
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC   desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = kSlots * kQueriesPerSlot * sizeof(uint64_t);
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&g_heap))) ||
            FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback))) ||
            FAILED(g_readback->Map(0, nullptr, (void**)&g_timestamps))) {
            g_heap.Reset();
            spdlog::warn("[Passes] Timestamp resources unavailable; pass profiling off");
            return;
        }
        for (auto& r : g_resolvers) {
            if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r.allocator))) ||
                FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.allocator.Get(), nullptr, IID_PPV_ARGS(&r.list))) ||
                FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence)))) {
                g_heap.Reset();
                return;
            }
            r.list->Close();
        }
        for (auto& e : g_entries) {
            e.resize(kQueriesPerSlot);
        }

        // The class table is complete before any hook goes live, as the hooks read it from the render threads.
        const auto vtables = FindPassVtables();
        for (const auto& [name, vtable] : vtables) {
            if ((int)g_classes.size() >= kMaxClasses) {
                break;
            }
            PassClass info{ name };
            g_class_of_vtable[vtable] = (int)g_classes.size();
            g_classes.push_back(info);
        }
        g_sums.assign(g_classes.size(), {});
        g_ready.store(true, std::memory_order_release);

        // Execute functions the game also calls or jumps to directly are shared code, left alone.
        std::unordered_set<uintptr_t> executes, called;
        for (const auto& [vtable, cls] : g_class_of_vtable) {
            executes.insert(reinterpret_cast<const uintptr_t*>(vtable)[kExecuteSlot]);
        }
        {
            const auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            const auto nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
            auto       sec  = IMAGE_FIRST_SECTION(nt);
            for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
                    continue;
                }
                const auto start = base + sec->VirtualAddress;
                for (size_t o = 0; o + 5 <= sec->Misc.VirtualSize; ++o) {
                    if (start[o] != 0xE8 && start[o] != 0xE9) {
                        continue;
                    }
                    const auto target = reinterpret_cast<uintptr_t>(start + o + 5) + *reinterpret_cast<const int32_t*>(start + o + 1);
                    if (executes.contains(target)) {
                        called.insert(target);
                    }
                }
            }
        }

        // All hooks are created first, with their originals stored, then enabled together.
        static const auto detours = Detours(std::make_integer_sequence<int, kMaxHooks>{});
        std::unordered_map<uintptr_t, int> hook_of_function;
        std::vector<uintptr_t>             targets;
        int                                skipped_functions = 0;
        for (const auto& [vtable, cls] : g_class_of_vtable) {
            const auto execute = reinterpret_cast<const uintptr_t*>(vtable)[kExecuteSlot];
            if (hook_of_function.contains(execute)) {
                continue;
            }
            // Import thunks and functions another hook already patched are left alone.
            const auto first = *reinterpret_cast<const uint8_t*>(execute);
            void*      original = nullptr;
            if (first == 0xE9 || first == 0xFF || called.contains(execute) || g_hook_count >= kMaxHooks ||
                MH_CreateHook(reinterpret_cast<void*>(execute), reinterpret_cast<void*>(detours[g_hook_count]), &original) != MH_OK) {
                hook_of_function[execute] = -1;
                ++skipped_functions;
                continue;
            }
            g_originals[g_hook_count] = reinterpret_cast<uintptr_t>(original);
            hook_of_function[execute] = g_hook_count++;
            targets.push_back(execute);
        }
        for (auto target : targets) {
            MH_QueueEnableHook(reinterpret_cast<void*>(target));
        }
        const auto applied = MH_ApplyQueued();
        spdlog::info("[Passes] {} render pass classes, {} execute functions hooked ({}), {} left alone", g_classes.size(), g_hook_count,
                     MH_StatusToString(applied), skipped_functions);
    }

    void MarkPass(void* pass, void* render_graph_data)
    {
        Mark(ClassOf(pass), render_graph_data);
    }

    void OnPresent()
    {
        if (g_heap == nullptr) {
            return;
        }
        auto queue = g_framework->get_d3d12_hook()->get_command_queue();
        // The frame just submitted: resolve its timestamps after its work on the game's queue.
        const uint32_t frame = g_frame.load();
        const uint32_t slot  = frame % kSlots;
        auto&          r     = g_resolvers[slot];
        if (queue != nullptr && r.fence->GetCompletedValue() >= r.value) {
            const uint32_t used = std::min<uint32_t>(g_query_count[slot].load(), kQueriesPerSlot);
            r.allocator->Reset();
            r.list->Reset(r.allocator.Get(), nullptr);
            if (used > 0) {
                r.list->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kQueriesPerSlot, used, g_readback.Get(),
                                         (uint64_t)slot * kQueriesPerSlot * sizeof(uint64_t));
            }
            r.list->Close();
            ID3D12CommandList* lists[]{ r.list.Get() };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(r.fence.Get(), ++r.value);
        }
        // The oldest slot is collected once its resolve finished, then reused.
        const uint32_t next = (frame + 1) % kSlots;
        auto&          old  = g_resolvers[next];
        if (frame + 1 >= kSlots) {
            if (old.fence->GetCompletedValue() >= old.value && old.value > 0) {
                Collect(next);
            }
        }
        g_query_count[next] = 0;
        g_entry_count[next] = 0;
        g_frame.fetch_add(1);

        const auto now = std::chrono::steady_clock::now();
        if (now - g_last_report >= std::chrono::seconds(10)) {
            Report();
            g_last_report = now;
        }
    }
}
