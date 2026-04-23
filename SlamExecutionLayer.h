#ifndef SLAM_EXECUTION_LAYER_H_
#define SLAM_EXECUTION_LAYER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace slam_exec {

enum class DropPolicy {
    Reject,
    DropOldest,
    DropNewest,
};

enum class RowChannelMode {
    R,
    G,
    B,
    Gray,
};

enum class PackMode {
    Binary,
    DebugHex,
};

struct SlamConfig {
    int max_fps = 10;
    int exec_timeout_ms = 60;
    int max_groups = 8;
    int min_quality = 10;
    DropPolicy drop_policy = DropPolicy::DropNewest;
    float row_ratio = 0.333333f;
    RowChannelMode channel_mode = RowChannelMode::G;
    int sample_stride = 1;
    int max_rows = 1;
    PackMode pack_mode = PackMode::Binary;
};

struct SlamImageFrame {
    std::uint32_t seq = 0;
    std::uint64_t tx_ms = 0;
    std::uint32_t frame_id = 0;
    int width = 0;
    int height = 0;
    std::string pixel_fmt;
    bool keyframe = false;
    int quality_hint = 0;
    std::string payload_ref;
    bool is_row_feature = false;
    int row_index = -1;
    std::string channel_mode;
    int stride = 1;
    int sample_count = 0;
    int payload_len = 0;
    std::uint32_t payload_crc32 = 0;
};

struct ExecutorResult {
    bool ok = false;
    bool timeout = false;
    int quality_score = 0;
    std::uint32_t proc_ms = 0;
    std::vector<std::uint32_t> groups;
};

class SlamExecutionLayerClient {
public:
    SlamExecutionLayerClient();
    ~SlamExecutionLayerClient();

    SlamExecutionLayerClient(const SlamExecutionLayerClient&) = delete;
    SlamExecutionLayerClient& operator=(const SlamExecutionLayerClient&) = delete;
    SlamExecutionLayerClient(SlamExecutionLayerClient&&) noexcept;
    SlamExecutionLayerClient& operator=(SlamExecutionLayerClient&&) noexcept;

    bool PushConfig(std::uint32_t session_id,
                    std::uint32_t config_version,
                    const SlamConfig& slam_cfg);

    ExecutorResult ProcessFrame(std::uint32_t session_id,
                                const SlamImageFrame& frame,
                                std::uint32_t timeout_ms);

    bool StopSession(std::uint32_t session_id);

    bool GetHealth() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace slam_exec

#endif  // SLAM_EXECUTION_LAYER_H_
