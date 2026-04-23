#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum class ActionType {
    Forward,
    Backward,
    Left,
    Right,
    Invalid,
};

enum class SlamStatus : std::uint8_t {
    Normal = 0,
    OutOfRange = 1,
    ChannelClosed = 2,
    Error = 3,
};

struct OverrideConfig {
    // Applied to control downlink after a successful C START.
    float max_power = 70.0f;
    float left_gain = 1.0f;
    float right_gain = 1.0f;
    float left_trim = 0.0f;
    float right_trim = 0.0f;
    int soft_hz = 50;
};

struct ParsedMessage {
    bool valid = false;
    bool is_realtime = false;
    bool is_config_start = false;
    bool is_config_stop = false;
    bool is_slam_report = false;

    std::uint32_t seq = 0;
    std::uint64_t client_ts_ms = 0;
    bool has_client_ts = false;

    ActionType action = ActionType::Invalid;
    OverrideConfig config;

    std::uint8_t control_state = 0;
    SlamStatus slam_status = SlamStatus::Normal;
    std::vector<std::uint32_t> slam_groups;

    std::string error;
};

class ControlDownlink {
public:
    bool sendAction(ActionType action, const OverrideConfig& cfg) {
        std::cout << "DOWNLINK action=" << actionToToken(action)
                  << " soft_hz=" << cfg.soft_hz
                  << " max_power=" << cfg.max_power
                  << " left_gain=" << cfg.left_gain
                  << " right_gain=" << cfg.right_gain
                  << " left_trim=" << cfg.left_trim
                  << " right_trim=" << cfg.right_trim << std::endl;
        return true;
    }

    bool forceZero() {
        std::cout << "DOWNLINK action=ZERO" << std::endl;
        return true;
    }

    static std::uint32_t packSlamGroup(std::uint8_t color_8bit,
                                       std::uint16_t distance_12bit,
                                       std::uint8_t status_4bit,
                                       std::uint8_t flags_8bit) {
        // [31:24]=color, [23:12]=distance, [11:8]=status, [7:0]=flags
        return (static_cast<std::uint32_t>(color_8bit) << 24) |
               ((static_cast<std::uint32_t>(distance_12bit) & 0x0FFFu) << 12) |
               ((static_cast<std::uint32_t>(status_4bit) & 0x0Fu) << 8) |
               static_cast<std::uint32_t>(flags_8bit);
    }

    std::string buildSlamFrame(std::uint32_t seq,
                               std::uint8_t control_state,
                               SlamStatus slam_status,
                               const std::vector<std::uint32_t>& groups) const {
        std::ostringstream oss;
        oss << "SL"
            << ' ' << seq
            << ' ' << static_cast<unsigned>(control_state)
            << ' ' << static_cast<unsigned>(slam_status)
            << ' ' << groups.size();

        for (const std::uint32_t group : groups) {
            oss << ' ' << toHex8(group);
        }
        return oss.str();
    }

private:
    static std::string actionToToken(ActionType action) {
        switch (action) {
            case ActionType::Forward:
                return "F";
            case ActionType::Backward:
                return "B";
            case ActionType::Left:
                return "L";
            case ActionType::Right:
                return "R";
            default:
                return "?";
        }
    }

    static std::string toHex8(std::uint32_t value) {
        std::ostringstream oss;
        oss << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
        return oss.str();
    }
};

class CommunicationLayer {
public:
    explicit CommunicationLayer(ControlDownlink downlink)
        : downlink_(std::move(downlink)) {}

    std::string onReceive(const std::string& line) {
        const auto recv_tp = std::chrono::steady_clock::now();
        const std::uint64_t recv_ms = nowMs(recv_tp);

        ParsedMessage msg = parseLine(line, active_cfg_);
        if (!msg.valid) {
            return buildAck("ERR", msg.seq, "bad_msg:" + msg.error, recv_ms, -1.0, recv_tp);
        }

        if (msg.is_config_start) {
            // Start session: config is latched and used for subsequent realtime actions.
            active_cfg_ = msg.config;
            active_cfg_.soft_hz = std::clamp(active_cfg_.soft_hz, 1, kHardMaxHz);
            session_active_ = true;
            has_last_rt_ = false;
            return buildAck("OK", msg.seq, "cfg_start", recv_ms, computeUplinkMs(msg, recv_ms), recv_tp);
        }

        if (msg.is_config_stop) {
            // Stop session and force a safe zero output.
            session_active_ = false;
            has_last_rt_ = false;
            const bool ok = downlink_.forceZero();
            return buildAck(ok ? "OK" : "ERR", msg.seq, ok ? "cfg_stop" : "downlink_fail",
                            recv_ms, computeUplinkMs(msg, recv_ms), recv_tp);
        }

        if (msg.is_slam_report) {
            // SL frames are uplink payload frames and bypass control rate checks.
            const std::string frame = downlink_.buildSlamFrame(msg.seq, msg.control_state,
                                                               msg.slam_status, msg.slam_groups);
            return buildAck("OK", msg.seq, frame, recv_ms, computeUplinkMs(msg, recv_ms), recv_tp);
        }

        if (!session_active_) {
            return buildAck("ERR", msg.seq, "no_session", recv_ms, computeUplinkMs(msg, recv_ms), recv_tp);
        }

        const auto now_tp = std::chrono::steady_clock::now();
        if (has_last_rt_) {
            const auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(now_tp - last_rt_tp_).count();
            const double hz = (delta_us <= 0) ? 1000000.0 : (1000000.0 / static_cast<double>(delta_us));
            // Soft limit is negotiated by C START; hard bound is enforced on config parsing.
            if (hz > static_cast<double>(active_cfg_.soft_hz)) {
                std::ostringstream detail;
                detail << "rate_exceed:" << std::fixed << std::setprecision(1) << hz
                       << ">" << active_cfg_.soft_hz;
                return buildAck("ERR", msg.seq, detail.str(), recv_ms, computeUplinkMs(msg, recv_ms), recv_tp);
            }
        }

        const bool ok = downlink_.sendAction(msg.action, active_cfg_);
        last_rt_tp_ = now_tp;
        has_last_rt_ = true;
        return buildAck(ok ? "OK" : "ERR", msg.seq, ok ? "rt" : "downlink_fail",
                        recv_ms, computeUplinkMs(msg, recv_ms), recv_tp);
    }

private:
    static constexpr int kHardMaxHz = 100;

    static std::uint64_t nowMs(const std::chrono::steady_clock::time_point& tp) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count());
    }

    static ActionType parseActionToken(const std::string& token) {
        if (token == "F") {
            return ActionType::Forward;
        }
        if (token == "B") {
            return ActionType::Backward;
        }
        if (token == "L") {
            return ActionType::Left;
        }
        if (token == "R") {
            return ActionType::Right;
        }
        return ActionType::Invalid;
    }

    static bool parseUInt64(const std::string& s, std::uint64_t* out) {
        if (out == nullptr || s.empty()) {
            return false;
        }
        std::istringstream iss(s);
        std::uint64_t v = 0;
        iss >> v;
        if (!iss || !iss.eof()) {
            return false;
        }
        *out = v;
        return true;
    }

    static bool parseUInt32(const std::string& s, std::uint32_t* out) {
        if (out == nullptr || s.empty()) {
            return false;
        }
        std::istringstream iss(s);
        std::uint32_t v = 0;
        iss >> v;
        if (!iss || !iss.eof()) {
            return false;
        }
        *out = v;
        return true;
    }

    static bool parseFloat(const std::string& s, float* out) {
        if (out == nullptr || s.empty()) {
            return false;
        }
        std::istringstream iss(s);
        float v = 0.0f;
        iss >> v;
        if (!iss || !iss.eof()) {
            return false;
        }
        *out = v;
        return true;
    }

    static bool parseInt(const std::string& s, int* out) {
        if (out == nullptr || s.empty()) {
            return false;
        }
        std::istringstream iss(s);
        int v = 0;
        iss >> v;
        if (!iss || !iss.eof()) {
            return false;
        }
        *out = v;
        return true;
    }

    static bool parseHex32(const std::string& s, std::uint32_t* out) {
        if (out == nullptr || s.empty()) {
            return false;
        }
        std::istringstream iss(s);
        std::uint32_t v = 0;
        iss >> std::hex >> v;
        if (!iss || !iss.eof()) {
            return false;
        }
        *out = v;
        return true;
    }

    static ParsedMessage parseLine(const std::string& line, const OverrideConfig& base_cfg) {
        ParsedMessage msg;
        msg.config = base_cfg;

        std::istringstream iss(line);
        std::string mode;
        if (!(iss >> mode)) {
            msg.error = "empty";
            return msg;
        }

        if (mode == "R") {
            // Realtime short frame: R <seq> <F|B|L|R> [client_ts_ms]
            std::string seq_token;
            std::string action_token;
            if (!(iss >> seq_token >> action_token)) {
                msg.error = "rt_short_fields";
                return msg;
            }
            if (!parseUInt32(seq_token, &msg.seq)) {
                msg.error = "rt_bad_seq";
                return msg;
            }

            msg.action = parseActionToken(action_token);
            if (msg.action == ActionType::Invalid) {
                msg.error = "rt_bad_action";
                return msg;
            }

            std::string ts_token;
            if (iss >> ts_token) {
                if (!parseUInt64(ts_token, &msg.client_ts_ms)) {
                    msg.error = "rt_bad_ts";
                    return msg;
                }
                msg.has_client_ts = true;
            }

            msg.valid = true;
            msg.is_realtime = true;
            return msg;
        }

        if (mode == "SL") {
            // SLAM short frame: SL <seq> <tx_ms> <ctrl_state> <slam_status> <count> <groups...>
            std::string seq_token;
            std::string tx_token;
            int control_state = 0;
            int slam_status = 0;
            int count = 0;
            if (!(iss >> seq_token >> tx_token >> control_state >> slam_status >> count)) {
                msg.error = "sl_short_fields";
                return msg;
            }
            if (!parseUInt32(seq_token, &msg.seq) || !parseUInt64(tx_token, &msg.client_ts_ms)) {
                msg.error = "sl_bad_header";
                return msg;
            }
            if (count < 0 || count > 256) {
                msg.error = "sl_bad_count";
                return msg;
            }

            msg.is_slam_report = true;
            msg.valid = true;
            msg.control_state = static_cast<std::uint8_t>(std::clamp(control_state, 0, 255));
            msg.slam_status = static_cast<SlamStatus>(std::clamp(slam_status, 0, 3));

            for (int i = 0; i < count; ++i) {
                std::string group_token;
                if (!(iss >> group_token)) {
                    msg.error = "sl_missing_group";
                    msg.valid = false;
                    return msg;
                }
                std::uint32_t group = 0;
                if (!parseHex32(group_token, &group)) {
                    msg.error = "sl_bad_group";
                    msg.valid = false;
                    return msg;
                }
                msg.slam_groups.push_back(group);
            }

            return msg;
        }

        if (mode == "C") {
            // Config long frame family: C START ... / C STOP ...
            std::string sub;
            if (!(iss >> sub)) {
                msg.error = "cfg_no_sub";
                return msg;
            }

            if (sub == "STOP") {
                msg.valid = true;
                msg.is_config_stop = true;

                std::string ts_token;
                if (iss >> ts_token) {
                    if (!parseUInt64(ts_token, &msg.client_ts_ms)) {
                        msg.error = "cfg_stop_bad_ts";
                        msg.valid = false;
                        return msg;
                    }
                    msg.has_client_ts = true;
                }
                return msg;
            }

            if (sub == "START") {
                std::string kv;
                while (iss >> kv) {
                    const auto pos = kv.find('=');
                    if (pos == std::string::npos || pos == 0 || pos + 1 >= kv.size()) {
                        msg.error = "cfg_bad_kv";
                        return msg;
                    }

                    const std::string key = kv.substr(0, pos);
                    const std::string value = kv.substr(pos + 1);

                    if (key == "ts") {
                        if (!parseUInt64(value, &msg.client_ts_ms)) {
                            msg.error = "cfg_bad_ts";
                            return msg;
                        }
                        msg.has_client_ts = true;
                    } else if (key == "hz") {
                        if (!parseInt(value, &msg.config.soft_hz)) {
                            msg.error = "cfg_bad_hz";
                            return msg;
                        }
                    } else if (key == "max_power") {
                        if (!parseFloat(value, &msg.config.max_power)) {
                            msg.error = "cfg_bad_max_power";
                            return msg;
                        }
                    } else if (key == "left_gain") {
                        if (!parseFloat(value, &msg.config.left_gain)) {
                            msg.error = "cfg_bad_left_gain";
                            return msg;
                        }
                    } else if (key == "right_gain") {
                        if (!parseFloat(value, &msg.config.right_gain)) {
                            msg.error = "cfg_bad_right_gain";
                            return msg;
                        }
                    } else if (key == "left_trim") {
                        if (!parseFloat(value, &msg.config.left_trim)) {
                            msg.error = "cfg_bad_left_trim";
                            return msg;
                        }
                    } else if (key == "right_trim") {
                        if (!parseFloat(value, &msg.config.right_trim)) {
                            msg.error = "cfg_bad_right_trim";
                            return msg;
                        }
                    } else {
                        msg.error = "cfg_unknown_key";
                        return msg;
                    }
                }

                msg.valid = true;
                msg.is_config_start = true;
                return msg;
            }

            msg.error = "cfg_bad_sub";
            return msg;
        }

        msg.error = "bad_mode";
        return msg;
    }

    static double computeUplinkMs(const ParsedMessage& msg, std::uint64_t recv_ms) {
        if (!msg.has_client_ts || recv_ms < msg.client_ts_ms) {
            return -1.0;
        }
        return static_cast<double>(recv_ms - msg.client_ts_ms);
    }

    static std::string buildAck(const std::string& code,
                                std::uint32_t seq,
                                const std::string& detail,
                                std::uint64_t recv_ms,
                                double uplink_ms,
                                const std::chrono::steady_clock::time_point& recv_tp) {
        const auto send_tp = std::chrono::steady_clock::now();
        const double downlink_ms =
            std::chrono::duration_cast<std::chrono::microseconds>(send_tp - recv_tp).count() / 1000.0;

        std::ostringstream oss;
        oss << "ACK " << code
            << " seq=" << seq
            << " detail=" << detail
            << " rx_ms=" << recv_ms
            << " ul_ms=" << std::fixed << std::setprecision(2) << uplink_ms
            << " dl_ms=" << downlink_ms;
        return oss.str();
    }

    OverrideConfig active_cfg_;
    bool session_active_ = false;
    bool has_last_rt_ = false;
    std::chrono::steady_clock::time_point last_rt_tp_{};
    ControlDownlink downlink_;
};

void printUsage() {
    std::cout << "Communication Layer\n"
              << "Realtime short frame:\n"
              << "  R <seq> <F|B|L|R> [client_ts_ms]\n"
              << "SLAM short report frame:\n"
              << "  SL <seq> <tx_ms> <control_state> <slam_status> <count> <group8hex...>\n"
              << "  group8hex = color8 + distance12 + status4 + flags8\n"
              << "Config long frame:\n"
              << "  C START hz=<1..100> max_power=<v> left_gain=<v> right_gain=<v> left_trim=<v> right_trim=<v> ts=<ms>\n"
              << "  C STOP [client_ts_ms]\n"
              << "Quit:\n"
              << "  q\n";
}

}  // namespace

int main() {
    ControlDownlink downlink;
    CommunicationLayer comm(std::move(downlink));

    printUsage();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "q" || line == "Q") {
            break;
        }
        std::cout << comm.onReceive(line) << std::endl;
    }

    return 0;
}
