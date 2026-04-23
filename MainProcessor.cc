#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum class ActionType {
    Forward,
    Backward,
    TurnLeft,
    TurnRight,
    Zero,
    Invalid,
};

enum class CommandType {
    Realtime,
    ConfigStart,
    ConfigStop,
    SlamImageInput,
    Invalid,
};

enum class DropPolicy {
    Reject,
    DropOldest,
    DropNewest,
};

enum class SlamStatus : std::uint8_t {
    Normal = 0,
    Overloaded = 1,
    ExecutorTimeout = 2,
    ExecutorError = 3,
};

struct ControlConfig {
    float soft_limit_hz = 50.0f;
    float max_power = 70.0f;
    float left_gain = 1.0f;
    float right_gain = 1.0f;
    float left_trim = 0.0f;
    float right_trim = 0.0f;
};

struct SlamConfig {
    int max_fps = 10;
    int exec_timeout_ms = 60;
    int max_groups = 8;
    int min_quality = 10;
    DropPolicy drop_policy = DropPolicy::DropNewest;
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
};

struct SlamOutput {
    std::uint8_t control_state = 0;
    SlamStatus slam_status = SlamStatus::Normal;
    std::vector<std::uint32_t> groups;
    int quality_score = 0;
    std::uint32_t proc_ms = 0;
    std::uint64_t source_ts = 0;
};

struct SlamFusionState {
    std::uint64_t last_input_ts_ms = 0;
    std::uint32_t last_proc_ms = 0;
    std::uint32_t dropped_frames = 0;
    int last_quality_score = 0;
    std::uint32_t output_seq = 0;
};

struct ParsedCommand {
    CommandType type = CommandType::Invalid;
    std::uint32_t seq = 0;
    std::uint64_t tx_ms = 0;
    ActionType action = ActionType::Invalid;
    ControlConfig control_cfg;
    SlamConfig slam_cfg;
    SlamImageFrame frame;
    std::string parse_error;
};

struct SessionState {
    bool active = false;
    std::uint32_t session_id = 0;
    std::uint32_t config_version = 0;
    ControlConfig control_cfg;
    SlamConfig slam_cfg;
    std::uint64_t last_rt_rx_ms = 0;
    bool has_last_rt = false;
    std::uint64_t last_sli_rx_ms = 0;
    bool has_last_sli = false;
    SlamFusionState slam_fusion;
};

struct ExecutorResult {
    bool ok = false;
    bool timeout = false;
    int quality_score = 0;
    std::uint32_t proc_ms = 0;
    std::vector<std::uint32_t> groups;
};

class ISlamExecutorClient {
public:
    virtual ~ISlamExecutorClient() = default;
    virtual bool PushConfig(std::uint32_t session_id,
                            std::uint32_t config_version,
                            const ControlConfig& control_cfg,
                            const SlamConfig& slam_cfg) = 0;
    virtual ExecutorResult ProcessFrame(std::uint32_t session_id,
                                        const SlamImageFrame& frame,
                                        std::uint32_t timeout_ms) = 0;
    virtual bool StopSession(std::uint32_t session_id) = 0;
    virtual bool GetHealth() = 0;
};

class SlamExecutorMockClient : public ISlamExecutorClient {
public:
    bool PushConfig(std::uint32_t,
                    std::uint32_t,
                    const ControlConfig&,
                    const SlamConfig&) override {
        return true;
    }

    ExecutorResult ProcessFrame(std::uint32_t,
                                const SlamImageFrame& frame,
                                std::uint32_t timeout_ms) override {
        ExecutorResult result;
        result.proc_ms = static_cast<std::uint32_t>((frame.frame_id % 7U) + 10U);
        result.timeout = result.proc_ms > timeout_ms;
        result.ok = !result.timeout;
        result.quality_score = std::clamp(frame.quality_hint, 0, 100);
        if (result.ok && result.quality_score > 0) {
            result.groups.push_back(0x010A1020u + (frame.frame_id & 0xFFu));
            result.groups.push_back(0x020A1020u + ((frame.frame_id + 1U) & 0xFFu));
        }
        return result;
    }

    bool StopSession(std::uint32_t) override {
        return true;
    }

    bool GetHealth() override {
        return true;
    }
};

class ControlDownlink {
public:
    bool sendAction(ActionType action, const ControlConfig& cfg) {
        std::cout << "DOWNLINK action=" << actionToString(action)
                  << " max_power=" << cfg.max_power
                  << " left_gain=" << cfg.left_gain
                  << " right_gain=" << cfg.right_gain
                  << " left_trim=" << cfg.left_trim
                  << " right_trim=" << cfg.right_trim << std::endl;
        return true;
    }

    bool sendZero() {
        std::cout << "DOWNLINK action=ZERO" << std::endl;
        return true;
    }

    std::string buildSlamFrame(std::uint32_t seq, const SlamOutput& output) const {
        std::ostringstream oss;
        oss << "SL " << seq
            << " ctrl=" << static_cast<int>(output.control_state)
            << " status=" << static_cast<int>(output.slam_status)
            << " quality=" << output.quality_score
            << " proc_ms=" << output.proc_ms
            << " source_ts=" << output.source_ts
            << " groups=" << output.groups.size();
        for (const std::uint32_t g : output.groups) {
            oss << " 0x" << std::hex << std::uppercase << g << std::dec;
        }
        return oss.str();
    }

private:
    static std::string actionToString(ActionType action) {
        switch (action) {
            case ActionType::Forward:
                return "FORWARD";
            case ActionType::Backward:
                return "BACKWARD";
            case ActionType::TurnLeft:
                return "LEFT";
            case ActionType::TurnRight:
                return "RIGHT";
            case ActionType::Zero:
                return "ZERO";
            default:
                return "INVALID";
        }
    }
};

std::uint64_t nowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool parseUInt32(const std::string& token, std::uint32_t* out) {
    if (out == nullptr || token.empty()) {
        return false;
    }
    std::istringstream iss(token);
    std::uint32_t v = 0;
    iss >> v;
    if (!iss || !iss.eof()) {
        return false;
    }
    *out = v;
    return true;
}

bool parseUInt64(const std::string& token, std::uint64_t* out) {
    if (out == nullptr || token.empty()) {
        return false;
    }
    std::istringstream iss(token);
    std::uint64_t v = 0;
    iss >> v;
    if (!iss || !iss.eof()) {
        return false;
    }
    *out = v;
    return true;
}

bool parseInt(const std::string& token, int* out) {
    if (out == nullptr || token.empty()) {
        return false;
    }
    std::istringstream iss(token);
    int v = 0;
    iss >> v;
    if (!iss || !iss.eof()) {
        return false;
    }
    *out = v;
    return true;
}

bool parseFloat(const std::string& token, float* out) {
    if (out == nullptr || token.empty()) {
        return false;
    }
    std::istringstream iss(token);
    float v = 0.0f;
    iss >> v;
    if (!iss || !iss.eof()) {
        return false;
    }
    *out = v;
    return true;
}

std::optional<ActionType> parseActionToken(const std::string& token) {
    if (token == "F") {
        return ActionType::Forward;
    }
    if (token == "B") {
        return ActionType::Backward;
    }
    if (token == "L") {
        return ActionType::TurnLeft;
    }
    if (token == "R") {
        return ActionType::TurnRight;
    }
    return std::nullopt;
}

std::optional<DropPolicy> parseDropPolicy(const std::string& token) {
    if (token == "reject") {
        return DropPolicy::Reject;
    }
    if (token == "oldest") {
        return DropPolicy::DropOldest;
    }
    if (token == "newest") {
        return DropPolicy::DropNewest;
    }
    return std::nullopt;
}

bool parseKVToken(const std::string& token, std::string* key, std::string* value) {
    if (key == nullptr || value == nullptr) {
        return false;
    }
    const std::size_t pos = token.find('=');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= token.size()) {
        return false;
    }
    *key = token.substr(0, pos);
    *value = token.substr(pos + 1);
    return true;
}

ParsedCommand parseCommand(const std::string& line) {
    ParsedCommand cmd;

    std::istringstream iss(line);
    std::string head;
    if (!(iss >> head)) {
        cmd.parse_error = "empty";
        return cmd;
    }

    if (head == "R") {
        std::string action_token;
        if (!(iss >> cmd.seq >> cmd.tx_ms >> action_token)) {
            cmd.parse_error = "rt_missing_fields";
            return cmd;
        }
        const auto action = parseActionToken(action_token);
        if (!action.has_value()) {
            cmd.parse_error = "rt_bad_action";
            return cmd;
        }
        cmd.type = CommandType::Realtime;
        cmd.action = *action;
        return cmd;
    }

    if (head == "SLI") {
        std::string seq_token;
        std::string tx_token;
        if (!(iss >> seq_token >> tx_token)) {
            cmd.parse_error = "sli_missing_header";
            return cmd;
        }
        if (!parseUInt32(seq_token, &cmd.seq) || !parseUInt64(tx_token, &cmd.tx_ms)) {
            cmd.parse_error = "sli_bad_header";
            return cmd;
        }

        SlamImageFrame frame;
        frame.seq = cmd.seq;
        frame.tx_ms = cmd.tx_ms;

        std::string kv;
        while (iss >> kv) {
            std::string key;
            std::string value;
            if (!parseKVToken(kv, &key, &value)) {
                cmd.parse_error = "sli_bad_kv";
                return cmd;
            }
            if (key == "frame_id") {
                if (!parseUInt32(value, &frame.frame_id)) {
                    cmd.parse_error = "sli_bad_frame_id";
                    return cmd;
                }
            } else if (key == "width") {
                if (!parseInt(value, &frame.width)) {
                    cmd.parse_error = "sli_bad_width";
                    return cmd;
                }
            } else if (key == "height") {
                if (!parseInt(value, &frame.height)) {
                    cmd.parse_error = "sli_bad_height";
                    return cmd;
                }
            } else if (key == "pixel_fmt") {
                frame.pixel_fmt = value;
            } else if (key == "keyframe") {
                int keyframe_i = 0;
                if (!parseInt(value, &keyframe_i)) {
                    cmd.parse_error = "sli_bad_keyframe";
                    return cmd;
                }
                frame.keyframe = (keyframe_i != 0);
            } else if (key == "quality_hint") {
                if (!parseInt(value, &frame.quality_hint)) {
                    cmd.parse_error = "sli_bad_quality";
                    return cmd;
                }
            } else if (key == "payload_ref") {
                frame.payload_ref = value;
            } else {
                cmd.parse_error = "sli_unknown_key";
                return cmd;
            }
        }

        if (frame.frame_id == 0 || frame.width <= 0 || frame.height <= 0 ||
            frame.pixel_fmt.empty() || frame.payload_ref.empty()) {
            cmd.parse_error = "sli_incomplete";
            return cmd;
        }
        cmd.frame = frame;
        cmd.type = CommandType::SlamImageInput;
        return cmd;
    }

    if (head == "C") {
        std::string sub;
        if (!(iss >> sub)) {
            cmd.parse_error = "cfg_missing_sub";
            return cmd;
        }

        if (sub == "STOP") {
            std::string kv;
            while (iss >> kv) {
                std::string key;
                std::string value;
                if (!parseKVToken(kv, &key, &value)) {
                    cmd.parse_error = "cfg_stop_bad_kv";
                    return cmd;
                }
                if (key == "seq") {
                    if (!parseUInt32(value, &cmd.seq)) {
                        cmd.parse_error = "cfg_stop_bad_seq";
                        return cmd;
                    }
                } else if (key == "ts") {
                    if (!parseUInt64(value, &cmd.tx_ms)) {
                        cmd.parse_error = "cfg_stop_bad_ts";
                        return cmd;
                    }
                } else {
                    cmd.parse_error = "cfg_stop_unknown_key";
                    return cmd;
                }
            }
            cmd.type = CommandType::ConfigStop;
            return cmd;
        }

        if (sub == "START") {
            std::string kv;
            while (iss >> kv) {
                std::string key;
                std::string value;
                if (!parseKVToken(kv, &key, &value)) {
                    cmd.parse_error = "cfg_start_bad_kv";
                    return cmd;
                }
                if (key == "seq") {
                    if (!parseUInt32(value, &cmd.seq)) {
                        cmd.parse_error = "cfg_start_bad_seq";
                        return cmd;
                    }
                } else if (key == "ts") {
                    if (!parseUInt64(value, &cmd.tx_ms)) {
                        cmd.parse_error = "cfg_start_bad_ts";
                        return cmd;
                    }
                } else if (key == "soft_hz") {
                    if (!parseFloat(value, &cmd.control_cfg.soft_limit_hz)) {
                        cmd.parse_error = "cfg_start_bad_soft_hz";
                        return cmd;
                    }
                } else if (key == "max_power") {
                    if (!parseFloat(value, &cmd.control_cfg.max_power)) {
                        cmd.parse_error = "cfg_start_bad_max_power";
                        return cmd;
                    }
                } else if (key == "left_gain") {
                    if (!parseFloat(value, &cmd.control_cfg.left_gain)) {
                        cmd.parse_error = "cfg_start_bad_left_gain";
                        return cmd;
                    }
                } else if (key == "right_gain") {
                    if (!parseFloat(value, &cmd.control_cfg.right_gain)) {
                        cmd.parse_error = "cfg_start_bad_right_gain";
                        return cmd;
                    }
                } else if (key == "left_trim") {
                    if (!parseFloat(value, &cmd.control_cfg.left_trim)) {
                        cmd.parse_error = "cfg_start_bad_left_trim";
                        return cmd;
                    }
                } else if (key == "right_trim") {
                    if (!parseFloat(value, &cmd.control_cfg.right_trim)) {
                        cmd.parse_error = "cfg_start_bad_right_trim";
                        return cmd;
                    }
                } else if (key == "slam_max_fps") {
                    if (!parseInt(value, &cmd.slam_cfg.max_fps)) {
                        cmd.parse_error = "cfg_start_bad_slam_max_fps";
                        return cmd;
                    }
                } else if (key == "slam_timeout_ms") {
                    if (!parseInt(value, &cmd.slam_cfg.exec_timeout_ms)) {
                        cmd.parse_error = "cfg_start_bad_slam_timeout_ms";
                        return cmd;
                    }
                } else if (key == "slam_max_groups") {
                    if (!parseInt(value, &cmd.slam_cfg.max_groups)) {
                        cmd.parse_error = "cfg_start_bad_slam_max_groups";
                        return cmd;
                    }
                } else if (key == "slam_min_quality") {
                    if (!parseInt(value, &cmd.slam_cfg.min_quality)) {
                        cmd.parse_error = "cfg_start_bad_slam_min_quality";
                        return cmd;
                    }
                } else if (key == "slam_drop_policy") {
                    const auto policy = parseDropPolicy(value);
                    if (!policy.has_value()) {
                        cmd.parse_error = "cfg_start_bad_slam_drop_policy";
                        return cmd;
                    }
                    cmd.slam_cfg.drop_policy = *policy;
                } else {
                    cmd.parse_error = "cfg_start_unknown_key";
                    return cmd;
                }
            }
            cmd.type = CommandType::ConfigStart;
            return cmd;
        }

        cmd.parse_error = "cfg_bad_sub";
        return cmd;
    }

    cmd.parse_error = "bad_head";
    return cmd;
}

class MainProcessor {
public:
    explicit MainProcessor(ControlDownlink downlink)
        : downlink_(std::move(downlink)) {}

    std::string onCommData(const std::string& payload) {
        const std::uint64_t rx_ms = nowMs();
        const ParsedCommand cmd = parseCommand(payload);
        if (cmd.type == CommandType::Invalid) {
            return buildAck(false, 0, rx_ms, 0, 0, "bad_command", cmd.parse_error);
        }

        if (cmd.type == CommandType::ConfigStart) {
            return onConfigStart(cmd, rx_ms);
        }
        if (cmd.type == CommandType::ConfigStop) {
            return onConfigStop(cmd, rx_ms);
        }
        if (cmd.type == CommandType::Realtime) {
            return onRealtime(cmd, rx_ms);
        }
        if (cmd.type == CommandType::SlamImageInput) {
            return onSlamImageInput(cmd, rx_ms);
        }

        return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, "bad_command", "unsupported");
    }

private:
    std::string onConfigStart(const ParsedCommand& cmd, std::uint64_t rx_ms) {
        if (cmd.control_cfg.soft_limit_hz <= 0.0f || cmd.control_cfg.soft_limit_hz > kHardLimitHz) {
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, "cfg_reject", "bad_soft_hz");
        }
        if (cmd.slam_cfg.max_fps <= 0 || cmd.slam_cfg.max_fps > kSlamHardMaxFps ||
            cmd.slam_cfg.exec_timeout_ms <= 0 || cmd.slam_cfg.exec_timeout_ms > kSlamHardMaxTimeoutMs ||
            cmd.slam_cfg.max_groups <= 0 || cmd.slam_cfg.max_groups > kSlamHardMaxGroups) {
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, "cfg_reject", "bad_slam_cfg");
        }

        session_.active = true;
        session_.session_id += 1;
        session_.config_version += 1;
        session_.control_cfg = cmd.control_cfg;
        session_.slam_cfg = cmd.slam_cfg;
        session_.has_last_rt = false;
        session_.has_last_sli = false;
        session_.slam_fusion = SlamFusionState{};

        const std::uint64_t t0 = nowMs();
        const bool push_ok = slam_executor_.PushConfig(session_.session_id,
                                                       session_.config_version,
                                                       session_.control_cfg,
                                                       session_.slam_cfg);
        const bool zero_ok = downlink_.sendAction(ActionType::Zero, session_.control_cfg);
        const std::uint64_t down_ms = nowMs() - t0;
        if (!push_ok) {
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, down_ms,
                            "cfg_slam_downlink_fail", "push_config_failed");
        }
        return buildAck(zero_ok, cmd.seq, rx_ms, cmd.tx_ms, down_ms,
                        zero_ok ? "cfg_start" : "downlink_fail",
                        "session_frozen");
    }

    std::string onConfigStop(const ParsedCommand& cmd, std::uint64_t rx_ms) {
        const std::uint64_t t0 = nowMs();
        const bool slam_ok = session_.active ? slam_executor_.StopSession(session_.session_id) : true;
        const bool zero_ok = downlink_.sendZero();
        const std::uint64_t down_ms = nowMs() - t0;

        session_.active = false;
        session_.has_last_rt = false;
        session_.has_last_sli = false;

        if (!slam_ok) {
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, down_ms, "cfg_stop", "stop_session_failed");
        }
        return buildAck(zero_ok, cmd.seq, rx_ms, cmd.tx_ms, down_ms,
                        zero_ok ? "cfg_stop" : "downlink_fail",
                        "session_closed");
    }

    std::string onRealtime(const ParsedCommand& cmd, std::uint64_t rx_ms) {
        if (!session_.active) {
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, "rt_reject", "no_session");
        }

        const std::string rate_err = checkRealtimeRate(rx_ms);
        if (!rate_err.empty()) {
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, "rt_reject", rate_err);
        }

        const std::uint64_t t0 = nowMs();
        const bool ok = downlink_.sendAction(cmd.action, session_.control_cfg);
        const std::uint64_t down_ms = nowMs() - t0;
        session_.last_rt_rx_ms = rx_ms;
        session_.has_last_rt = true;
        return buildAck(ok, cmd.seq, rx_ms, cmd.tx_ms, down_ms,
                        ok ? "rt_apply" : "downlink_fail",
                        ok ? "rt_ok" : "rt_downlink_error");
    }

    std::string onSlamImageInput(const ParsedCommand& cmd, std::uint64_t rx_ms) {
        if (!session_.active) {
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, "sli_reject", "no_session");
        }
        if (!isSupportedPixelFormat(cmd.frame.pixel_fmt)) {
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, "sli_reject", "bad_pixel_fmt");
        }

        const std::string gate = checkSlamIngressBudget(rx_ms);
        if (!gate.empty()) {
            session_.slam_fusion.dropped_frames += 1;
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, "sli_drop", gate);
        }

        SlamOutput output;
        const std::uint64_t t0 = nowMs();
        const ExecutorResult result = slam_executor_.ProcessFrame(
            session_.session_id,
            cmd.frame,
            static_cast<std::uint32_t>(session_.slam_cfg.exec_timeout_ms));
        const std::uint64_t down_ms = nowMs() - t0;

        output.control_state = session_.active ? 1 : 0;
        output.proc_ms = result.proc_ms;
        output.source_ts = cmd.tx_ms;
        output.quality_score = result.quality_score;

        if (result.timeout) {
            output.slam_status = SlamStatus::ExecutorTimeout;
            session_.slam_fusion.dropped_frames += 1;
            updateSlamFusionState(cmd, output);
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, down_ms,
                            "sli_exec_timeout", "executor_timeout");
        }
        if (!result.ok) {
            output.slam_status = SlamStatus::ExecutorError;
            session_.slam_fusion.dropped_frames += 1;
            updateSlamFusionState(cmd, output);
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, down_ms,
                            "sli_exec_error", "executor_failed");
        }

        output.slam_status = SlamStatus::Normal;
        output.groups = result.groups;
        if (static_cast<int>(output.groups.size()) > session_.slam_cfg.max_groups) {
            output.groups.resize(static_cast<std::size_t>(session_.slam_cfg.max_groups));
        }
        if (output.quality_score < session_.slam_cfg.min_quality) {
            output.groups.clear();
        }

        // If realtime commands are very close, keep control latency priority and only return status.
        if (session_.has_last_rt && rx_ms - session_.last_rt_rx_ms <= kRtPriorityWindowMs) {
            output.slam_status = SlamStatus::Overloaded;
            output.groups.clear();
            updateSlamFusionState(cmd, output);
            session_.last_sli_rx_ms = rx_ms;
            session_.has_last_sli = true;
            return buildAck(true, cmd.seq, rx_ms, cmd.tx_ms, down_ms,
                            "sli_defer_rt", "status_only");
        }

        const std::string slam_frame = downlink_.buildSlamFrame(cmd.seq, output);
        updateSlamFusionState(cmd, output);
        session_.last_sli_rx_ms = rx_ms;
        session_.has_last_sli = true;
        return buildAck(true, cmd.seq, rx_ms, cmd.tx_ms, down_ms,
                        "sli_fusion_ok", slam_frame);
    }

    void updateSlamFusionState(const ParsedCommand& cmd, const SlamOutput& output) {
        session_.slam_fusion.last_input_ts_ms = cmd.tx_ms;
        session_.slam_fusion.last_proc_ms = output.proc_ms;
        session_.slam_fusion.last_quality_score = output.quality_score;
        session_.slam_fusion.output_seq = cmd.seq;
    }

    bool isSupportedPixelFormat(const std::string& pixel_fmt) const {
        return pixel_fmt == "GRAY8" || pixel_fmt == "RGB24" || pixel_fmt == "NV12";
    }

    std::string checkRealtimeRate(std::uint64_t rx_ms) const {
        if (!session_.has_last_rt) {
            return "";
        }
        if (rx_ms <= session_.last_rt_rx_ms) {
            return "rate_over_hard";
        }
        const float delta_ms = static_cast<float>(rx_ms - session_.last_rt_rx_ms);
        const float hz = 1000.0f / delta_ms;
        if (hz > kHardLimitHz + 1e-3f) {
            return "rate_over_hard";
        }
        if (hz > session_.control_cfg.soft_limit_hz + 1e-3f) {
            return "rate_over_soft";
        }
        return "";
    }

    std::string checkSlamIngressBudget(std::uint64_t rx_ms) const {
        if (!session_.has_last_sli) {
            return "";
        }
        if (rx_ms <= session_.last_sli_rx_ms) {
            return "sli_non_monotonic";
        }

        const double delta_ms = static_cast<double>(rx_ms - session_.last_sli_rx_ms);
        const double hz = 1000.0 / delta_ms;
        if (hz <= static_cast<double>(session_.slam_cfg.max_fps)) {
            return "";
        }

        if (session_.slam_cfg.drop_policy == DropPolicy::Reject) {
            return "drop_reject_policy";
        }
        if (session_.slam_cfg.drop_policy == DropPolicy::DropNewest) {
            return "drop_newest_overload";
        }
        return "drop_oldest_overload";
    }

    static std::string buildAck(bool ok,
                                std::uint32_t seq,
                                std::uint64_t rx_ms,
                                std::uint64_t tx_ms,
                                std::uint64_t down_ms,
                                const std::string& tag,
                                const std::string& detail) {
        const std::uint64_t up_ms = (tx_ms <= rx_ms) ? (rx_ms - tx_ms) : 0;
        std::ostringstream oss;
        oss << "ACK " << (ok ? "OK" : "ERR")
            << " seq=" << seq
            << " up_ms=" << up_ms
            << " down_ms=" << down_ms
            << " tag=" << tag
            << " detail=" << detail;
        return oss.str();
    }

    static constexpr float kHardLimitHz = 100.0f;
    static constexpr int kSlamHardMaxFps = 30;
    static constexpr int kSlamHardMaxTimeoutMs = 200;
    static constexpr int kSlamHardMaxGroups = 64;
    static constexpr std::uint64_t kRtPriorityWindowMs = 20;

    SessionState session_;
    ControlDownlink downlink_;
    SlamExecutorMockClient slam_executor_;
};

void printUsage() {
    std::cout << "Main Processor Hub (Step2)\n"
              << "Config start:\n"
              << "  C START seq=<n> ts=<ms> soft_hz=<1..100> max_power=<v> left_gain=<v> right_gain=<v> left_trim=<v> right_trim=<v> slam_max_fps=<1..30> slam_timeout_ms=<1..200> slam_max_groups=<1..64> slam_min_quality=<0..100> slam_drop_policy=<reject|oldest|newest>\n"
              << "Realtime:\n"
              << "  R <seq> <tx_ms> <F|B|L|R>\n"
              << "SLAM image input:\n"
              << "  SLI <seq> <tx_ms> frame_id=<n> width=<w> height=<h> pixel_fmt=<GRAY8|RGB24|NV12> keyframe=<0|1> quality_hint=<0..100> payload_ref=<id>\n"
              << "Config stop:\n"
              << "  C STOP seq=<n> ts=<ms>\n"
              << "Quit:\n"
              << "  q\n";
}

}  // namespace

int main() {
    ControlDownlink downlink;
    MainProcessor processor(std::move(downlink));

    printUsage();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "q" || line == "Q") {
            break;
        }
        std::cout << processor.onCommData(line) << std::endl;
    }
    return 0;
}
