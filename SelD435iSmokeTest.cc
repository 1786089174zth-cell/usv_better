#include "SlamExecutionLayer.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

using slam_exec::ExecutorResult;
using slam_exec::SlamConfig;
using slam_exec::SlamExecutionLayerClient;
using slam_exec::SlamImageFrame;

namespace {

std::uint64_t nowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string formatSampleLine(const slam_exec::D435iFrameSample& sample) {
    std::ostringstream oss;
    oss << "sample row=" << sample.row_index
        << " rgb_bytes=" << sample.rgb_row.size()
        << " depth_values=" << sample.depth_row.size()
        << " gyro=[" << std::fixed << std::setprecision(4)
        << sample.gyro_x << "," << sample.gyro_y << "," << sample.gyro_z << "]";

    oss << " rgb_row=[";
    const std::size_t rgb_triplets = sample.rgb_row.size() / 3u;
    for (std::size_t i = 0; i < rgb_triplets; ++i) {
        const std::size_t base = i * 3u;
        oss << static_cast<int>(sample.rgb_row[base]) << ","
            << static_cast<int>(sample.rgb_row[base + 1u]) << ","
            << static_cast<int>(sample.rgb_row[base + 2u]);
        if (i + 1u != rgb_triplets) {
            oss << ";";
        }
    }
    oss << "]";

    oss << " depth_row=[";
    const std::size_t depth_count = sample.depth_row.size();
    for (std::size_t i = 0; i < depth_count; ++i) {
        oss << sample.depth_row[i];
        if (i + 1u != depth_count) {
            oss << ",";
        }
    }
    oss << "]";
    return oss.str();
}

}  // namespace

int main() {
    constexpr std::uint32_t kSessionId = 1u;
    constexpr std::uint32_t kConfigVersion = 1u;
    constexpr int kRunSeconds = 10;
    const int max_fps = 10;
    const std::uint32_t exec_timeout_ms = 60u;
    const float row_ratio = 0.333333f;
    const int sample_stride = 1;

    SlamConfig cfg;
    cfg.max_fps = max_fps;
    cfg.exec_timeout_ms = static_cast<int>(exec_timeout_ms);
    cfg.row_ratio = row_ratio;
    cfg.sample_stride = sample_stride;

    SlamExecutionLayerClient client;
    std::string init_error;
    if (!client.InitializeD435i(&init_error)) {
        std::cerr << "D435i init failed: " << init_error << std::endl;
        return 1;
    }

    if (!client.PushConfig(kSessionId, kConfigVersion, cfg)) {
        std::cerr << "PushConfig failed" << std::endl;
        return 2;
    }

    const std::uint64_t start_ms = nowMs();
    const std::uint64_t end_ms = start_ms + static_cast<std::uint64_t>(kRunSeconds * 1000);
    const std::uint32_t frame_interval_ms = static_cast<std::uint32_t>(1000 / max_fps);

    std::uint32_t frame_id = 0;
    std::uint32_t sent = 0;
    std::uint32_t ok = 0;
    std::uint32_t timeout = 0;
    std::uint32_t err = 0;
    std::uint32_t groups_total = 0;
    std::uint32_t quality_total = 0;
    ExecutorResult last_result;
    std::uint32_t capture_err = 0;
    std::string capture_error;

    while (nowMs() < end_ms) {
        slam_exec::D435iFrameSample sample;
        capture_error.clear();
        const bool have_sample = client.CaptureD435iFrame(exec_timeout_ms, cfg, &sample, &capture_error);
        if (!have_sample) {
            capture_err++;
            std::cout << "capture_err=" << capture_error << std::endl;
        } else {
            std::cout << formatSampleLine(sample) << std::endl;
        }

        SlamImageFrame frame;
        frame.seq = sent;
        frame.tx_ms = nowMs();
        frame.frame_id = ++frame_id;
        frame.width = 640;
        frame.height = 480;
        frame.pixel_fmt = "BGR8";
        frame.keyframe = (frame_id % 30 == 0);
        frame.quality_hint = 60;
        frame.payload_ref = "d435i_live";

        const ExecutorResult result = client.ProcessFrame(kSessionId, frame, exec_timeout_ms);
        last_result = result;
        sent++;
        if (result.ok) {
            ok++;
        } else if (result.timeout) {
            timeout++;
        } else {
            err++;
        }
        groups_total += static_cast<std::uint32_t>(result.groups.size());
        quality_total += static_cast<std::uint32_t>(result.quality_score);

        if (sent % 10u == 0u) {
            std::cout << "tick=" << sent
                      << " ok=" << ok
                      << " timeout=" << timeout
                      << " err=" << err
                      << " last_quality=" << result.quality_score
                      << " last_groups=" << result.groups.size()
                      << " last_proc_ms=" << result.proc_ms
                      << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
    }

    const double avg_quality = sent > 0 ? static_cast<double>(quality_total) / sent : 0.0;
    const double avg_groups = sent > 0 ? static_cast<double>(groups_total) / sent : 0.0;

    std::cout << "\n=== SEL 10s D435i smoke test ===\n";
    std::cout << "sent=" << sent
              << " ok=" << ok
              << " timeout=" << timeout
              << " err=" << err
              << " capture_err=" << capture_err
              << " avg_quality=" << avg_quality
              << " avg_groups=" << avg_groups
              << " health=" << (client.GetHealth() ? "OK" : "BAD")
              << std::endl;

    std::cout << "last: ok=" << (last_result.ok ? "true" : "false")
              << " timeout=" << (last_result.timeout ? "true" : "false")
              << " quality=" << last_result.quality_score
              << " proc_ms=" << last_result.proc_ms
              << " groups=" << last_result.groups.size()
              << std::endl;

    client.StopSession(kSessionId);
    client.ShutdownD435i();
    return 0;
}
