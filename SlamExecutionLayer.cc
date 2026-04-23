#include "SlamExecutionLayer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace slam_exec {
namespace {

std::uint32_t fnv1a32(const std::vector<std::uint8_t>& data) {
    std::uint32_t hash = 2166136261u;
    for (const std::uint8_t byte : data) {
        hash ^= static_cast<std::uint32_t>(byte);
        hash *= 16777619u;
    }
    return hash;
}

std::uint32_t fnv1a32String(const std::string& text) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char c : text) {
        hash ^= static_cast<std::uint32_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

int clampRowIndex(float row_ratio, int height) {
    if (height <= 0) {
        return 0;
    }
    const float raw = static_cast<float>(height - 1) * row_ratio;
    const int row_index = static_cast<int>(std::lround(raw));
    return std::clamp(row_index, 0, height - 1);
}

std::uint8_t pseudoChannelValue(std::uint32_t seed, int x, int y, int channel) {
    const std::uint32_t mixed = seed ^ (static_cast<std::uint32_t>(x) * 73856093u)
                                ^ (static_cast<std::uint32_t>(y) * 19349663u)
                                ^ (static_cast<std::uint32_t>(channel) * 83492791u);
    return static_cast<std::uint8_t>((mixed ^ (mixed >> 8) ^ (mixed >> 16)) & 0xFFu);
}

std::uint8_t grayFromRgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    // Integer approximation: Y = (77*R + 150*G + 29*B) >> 8
    const int y = (77 * static_cast<int>(r)) + (150 * static_cast<int>(g)) + (29 * static_cast<int>(b));
    return static_cast<std::uint8_t>(y >> 8);
}

std::vector<std::uint8_t> extractRowSamples(const SlamImageFrame& frame,
                                            const SlamConfig& cfg,
                                            int* out_row_index) {
    std::vector<std::uint8_t> samples;
    if (out_row_index == nullptr || frame.width <= 0 || frame.height <= 0) {
        return samples;
    }

    const int stride = std::max(1, cfg.sample_stride);
    const int row_index = clampRowIndex(cfg.row_ratio, frame.height);
    *out_row_index = row_index;

    samples.reserve(static_cast<std::size_t>((frame.width + stride - 1) / stride));
    const std::uint32_t seed = fnv1a32String(frame.payload_ref) ^ frame.frame_id;

    for (int x = 0; x < frame.width; x += stride) {
        const std::uint8_t r = pseudoChannelValue(seed, x, row_index, 0);
        const std::uint8_t g = pseudoChannelValue(seed, x, row_index, 1);
        const std::uint8_t b = pseudoChannelValue(seed, x, row_index, 2);

        std::uint8_t value = 0;
        switch (cfg.channel_mode) {
            case RowChannelMode::R:
                value = r;
                break;
            case RowChannelMode::G:
                value = g;
                break;
            case RowChannelMode::B:
                value = b;
                break;
            case RowChannelMode::Gray:
                value = grayFromRgb(r, g, b);
                break;
        }
        samples.push_back(value);
    }

    return samples;
}

std::string channelModeToString(RowChannelMode mode) {
    switch (mode) {
        case RowChannelMode::R:
            return "R";
        case RowChannelMode::G:
            return "G";
        case RowChannelMode::B:
            return "B";
        case RowChannelMode::Gray:
            return "GRAY";
    }
    return "G";
}

}  // namespace

struct SlamExecutionLayerClient::Impl {
    std::uint32_t session_id = 0;
    std::uint32_t config_version = 0;
    SlamConfig slam_cfg;
    bool active = false;
    bool transport_connected = false;

    bool ensureConnected() {
        transport_connected = true;
        return transport_connected;
    }

    bool pushConfig(std::uint32_t in_session_id,
                    std::uint32_t in_config_version,
                    const SlamConfig& cfg) {
        if (!ensureConnected()) {
            return false;
        }
        session_id = in_session_id;
        config_version = in_config_version;
        slam_cfg = cfg;
        active = true;
        return true;
    }

    ExecutorResult processFrame(std::uint32_t in_session_id,
                                const SlamImageFrame& frame,
                                std::uint32_t timeout_ms) {
        ExecutorResult result;
        if (!active || in_session_id != session_id || !ensureConnected()) {
            result.ok = false;
            result.timeout = false;
            result.quality_score = 0;
            return result;
        }

        int row_index = -1;
        const std::vector<std::uint8_t> row_samples = extractRowSamples(frame, slam_cfg, &row_index);
        if (row_samples.empty()) {
            result.ok = false;
            result.timeout = false;
            result.quality_score = 0;
            return result;
        }

        const std::uint32_t hash = fnv1a32(row_samples);
        const std::uint32_t sample_count = static_cast<std::uint32_t>(row_samples.size());

        SlamImageFrame enriched = frame;
        enriched.is_row_feature = true;
        enriched.row_index = row_index;
        enriched.channel_mode = channelModeToString(slam_cfg.channel_mode);
        enriched.stride = std::max(1, slam_cfg.sample_stride);
        enriched.sample_count = static_cast<int>(sample_count);
        enriched.payload_len = static_cast<int>(sample_count);
        enriched.payload_crc32 = hash;

        // Simulated transport + SLAM runtime RTT.
        result.proc_ms = static_cast<std::uint32_t>(8u + (frame.frame_id % 5u) + (sample_count % 7u));
        result.timeout = result.proc_ms > timeout_ms;
        if (result.timeout) {
            result.ok = false;
            result.quality_score = 0;
            return result;
        }

        result.ok = true;
        const int signal = static_cast<int>((hash ^ (hash >> 13)) & 0x7Fu);
        result.quality_score = std::clamp(frame.quality_hint + (signal / 8), 0, 100);

        if (result.quality_score >= slam_cfg.min_quality) {
            const std::uint32_t group0 = 0x01000000u
                                         | (static_cast<std::uint32_t>(row_index & 0x3FF) << 14)
                                         | (static_cast<std::uint32_t>(sample_count & 0x3FF) << 4)
                                         | static_cast<std::uint32_t>(signal & 0x0Fu);
            const std::uint32_t group1 = 0x02000000u | (hash & 0x00FFFFFFu);
            result.groups.push_back(group0);
            result.groups.push_back(group1);
        }
        if (static_cast<int>(result.groups.size()) > slam_cfg.max_groups) {
            result.groups.resize(static_cast<std::size_t>(slam_cfg.max_groups));
        }

        return result;
    }

    bool stopSession(std::uint32_t in_session_id) {
        if (!active) {
            return true;
        }
        if (in_session_id != session_id) {
            return false;
        }
        active = false;
        return true;
    }

    bool getHealth() const {
        return transport_connected;
    }
};

SlamExecutionLayerClient::SlamExecutionLayerClient()
    : impl_(std::make_unique<Impl>()) {}

SlamExecutionLayerClient::~SlamExecutionLayerClient() = default;

SlamExecutionLayerClient::SlamExecutionLayerClient(SlamExecutionLayerClient&&) noexcept = default;

SlamExecutionLayerClient& SlamExecutionLayerClient::operator=(SlamExecutionLayerClient&&) noexcept = default;

bool SlamExecutionLayerClient::PushConfig(std::uint32_t session_id,
                                          std::uint32_t config_version,
                                          const SlamConfig& slam_cfg) {
    return impl_->pushConfig(session_id, config_version, slam_cfg);
}

ExecutorResult SlamExecutionLayerClient::ProcessFrame(std::uint32_t session_id,
                                                      const SlamImageFrame& frame,
                                                      std::uint32_t timeout_ms) {
    return impl_->processFrame(session_id, frame, timeout_ms);
}

bool SlamExecutionLayerClient::StopSession(std::uint32_t session_id) {
    return impl_->stopSession(session_id);
}

bool SlamExecutionLayerClient::GetHealth() const {
    return impl_->getHealth();
}

}  // namespace slam_exec
