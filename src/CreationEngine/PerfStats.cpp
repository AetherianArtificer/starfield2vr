#include "PerfStats.h"

#include <algorithm>
#include <chrono>
#include <vector>

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

            spdlog::info("[Perf] {:.1f} fps | frame avg {:.1f} ms, p99 {:.1f} ms, max {:.1f} ms | budget {:.1f} ms ({:.0f} Hz), {:.0f}% over", fps, sum / count, p99,
                g_frame_ms.back(), budget_ms, period > 0.0 ? 1.0 / period : 0.0, 100.0f * over / count);

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

        g_frame_ms.push_back(std::chrono::duration<float, std::milli>(now - g_last_present).count());
        g_last_present = now;

        if (now - g_window_start >= kReportInterval) {
            Report(std::chrono::duration<float>(now - g_window_start).count());
            g_window_start = now;
        }
    }
}
