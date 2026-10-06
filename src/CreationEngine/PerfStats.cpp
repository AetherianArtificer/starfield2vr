#include "PerfStats.h"
#include "GpuQueueTiming.h"
#include "RenderPassProfiler.h"
#include <CreationEngine/models/ModSettingsStore.h>

#include <Framework.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <d3d12.h>
#include <mutex>
#include <vector>
#include <wrl/client.h>

#include <mods/VR.hpp>

namespace PerfStats
{
    namespace
    {
        using clock = std::chrono::steady_clock;

        constexpr auto kReportInterval = std::chrono::seconds(5);

        clock::time_point  g_last_present{};
        clock::time_point  g_window_start{};
        std::vector<float> g_frame_ms;

        // A running average.
        struct Average
        {
            double sum{ 0.0 };
            int    count{ 0 };
            void   add(double value)
            {
                sum += value;
                ++count;
            }
            double take()
            {
                const double value = count > 0 ? sum / count : 0.0;
                sum                = 0.0;
                count              = 0;
                return value;
            }
        };

        // CPU stages from the Reflex markers, matched per engine frame.
        constexpr int kMarkerFrames = 8;
        struct MarkerFrame
        {
            uint32_t                         frame{ 0xFFFFFFFF };
            std::array<clock::time_point, 6> at{};
            uint32_t                         seen{ 0 };
        };
        std::mutex                              g_marker_mutex;
        std::array<MarkerFrame, kMarkerFrames>  g_marker_frames{};
        Average                                 g_cpu_simulation, g_cpu_submit, g_cpu_present;

        std::mutex             g_cpu_mutex;
        std::array<Average, 4> g_cpu_spans{};

        void Report(float seconds)
        {
            if (g_frame_ms.empty()) {
                return;
            }
            std::sort(g_frame_ms.begin(), g_frame_ms.end());
            const auto count = g_frame_ms.size();
            float      sum   = 0.0f;
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

            double simulation, submit, present;
            {
                std::scoped_lock _{ g_marker_mutex };
                simulation = g_cpu_simulation.take();
                submit     = g_cpu_submit.take();
                present    = g_cpu_present.take();
            }

            spdlog::info("[Perf] {:.1f} fps | frame avg {:.1f} ms, p99 {:.1f} ms, max {:.1f} ms | budget {:.1f} ms ({:.0f} Hz), {:.0f}% over", fps, sum / count, p99,
                g_frame_ms.back(), budget_ms, period > 0.0 ? 1.0 / period : 0.0, 100.0f * over / count);
            std::array<double, 4> mod{};
            {
                std::scoped_lock _{ g_cpu_mutex };
                for (size_t i = 0; i < mod.size(); ++i) {
                    mod[i] = g_cpu_spans[i].take();
                }
            }
            spdlog::info("[Perf] CPU: simulation {:.1f} ms, render submit {:.1f} ms, present {:.1f} ms | in simulation: VR wait {:.1f}, VR sync {:.1f}, mod frame {:.2f}, "
                         "body IK {:.2f} ms",
                simulation, submit, present, mod[0], mod[1], mod[2], mod[3]);

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

        if (GameFlow::gStore.internalSettings.perfLogging) {
            GpuQueueTiming::OnPresent();
            RenderPassProfiler::OnPresent();
        }
        g_frame_ms.push_back(std::chrono::duration<float, std::milli>(now - g_last_present).count());
        g_last_present = now;

        if (now - g_window_start >= kReportInterval) {
            Report(std::chrono::duration<float>(now - g_window_start).count());
            g_window_start = now;
        }
    }

    void OnReflexMarker(uint32_t marker, uint32_t frame)
    {
        if (marker > 5) {
            return;
        }
        const auto now = clock::now();
        std::scoped_lock _{ g_marker_mutex };
        auto& entry = g_marker_frames[frame % kMarkerFrames];
        if (entry.frame != frame) {
            entry       = {};
            entry.frame = frame;
        }
        entry.at[marker] = now;
        entry.seen |= 1u << marker;
        auto stage = [&](uint32_t from, uint32_t to, Average& average) {
            if (marker == to && (entry.seen & (1u << from))) {
                average.add(std::chrono::duration<double, std::milli>(entry.at[to] - entry.at[from]).count());
            }
        };
        stage(0, 1, g_cpu_simulation);
        stage(2, 3, g_cpu_submit);
        stage(4, 5, g_cpu_present);
    }

    void AddCpu(CpuSpan span, double ms)
    {
        std::scoped_lock _{ g_cpu_mutex };
        g_cpu_spans[(size_t)span].add(ms);
    }
}
