#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

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
    Invalid,
};

struct CommCommand {
    CommandType type = CommandType::Invalid;
    std::uint32_t seq = 0;
    std::uint64_t tx_ms = 0;
    ActionType action = ActionType::Invalid;
    float soft_limit_hz = 50.0f;
    bool has_override = false;
    struct OverrideConfig {
        float max_power = 70.0f;
        float left_gain = 1.0f;
        float right_gain = 1.0f;
        float left_trim = 0.0f;
        float right_trim = 0.0f;
    } override_cfg;
};

struct OverrideConfig {
    float max_power = 70.0f;
    float left_gain = 1.0f;
    float right_gain = 1.0f;
    float left_trim = 0.0f;
    float right_trim = 0.0f;
};

struct SessionState {
    bool active = false;
    float soft_limit_hz = 50.0f;
    OverrideConfig overrides;
    std::uint64_t last_rt_rx_ms = 0;
    bool has_last_rt = false;
};

bool parseActionToken(const std::string& token, ActionType* out) {
    if (out == nullptr) {
        return false;
    }
    if (token == "F") {
        *out = ActionType::Forward;
        return true;
    }
    if (token == "B") {
        *out = ActionType::Backward;
        return true;
    }
    if (token == "L") {
        *out = ActionType::TurnLeft;
        return true;
    }
    if (token == "R") {
        *out = ActionType::TurnRight;
        return true;
    }
    return false;
}

std::string actionToString(ActionType action) {
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

std::uint64_t nowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool parseCommand(const std::string& line, CommCommand* cmd) {
    if (cmd == nullptr) {
        return false;
    }

    std::istringstream iss(line);
    std::string head;
    if (!(iss >> head)) {
        return false;
    }

    if (head == "RT") {
        std::string action_token;
        if (!(iss >> cmd->seq >> cmd->tx_ms >> action_token)) {
            return false;
        }
        ActionType action = ActionType::Invalid;
        if (!parseActionToken(action_token, &action)) {
            return false;
        }
        cmd->type = CommandType::Realtime;
        cmd->action = action;
        return true;
    }

    if (head == "CS") {
        cmd->type = CommandType::ConfigStart;
        cmd->has_override = true;
        if (!(iss >> cmd->seq >> cmd->tx_ms >> cmd->soft_limit_hz
                  >> cmd->override_cfg.max_power
                  >> cmd->override_cfg.left_gain
                  >> cmd->override_cfg.right_gain
                  >> cmd->override_cfg.left_trim
                  >> cmd->override_cfg.right_trim)) {
            return false;
        }
        return true;
    }

    if (head == "CE") {
        cmd->type = CommandType::ConfigStop;
        if (!(iss >> cmd->seq >> cmd->tx_ms)) {
            return false;
        }
        return true;
    }

    return false;
}

class ControlDownlink {
public:
    bool sendAction(ActionType action, const OverrideConfig& cfg) {
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
};

class MainProcessor {
public:
    explicit MainProcessor(ControlDownlink downlink) : downlink_(std::move(downlink)) {}

    std::string onCommData(const std::string& payload) {
        const std::uint64_t rx_ms = nowMs();
        CommCommand cmd;
        if (!parseCommand(payload, &cmd)) {
            return "ACK ERR bad_command";
        }

        if (cmd.type == CommandType::ConfigStart) {
            if (cmd.soft_limit_hz <= 0.0f || cmd.soft_limit_hz > kHardLimitHz) {
                return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, "bad_soft_hz");
            }
            session_.active = true;
            session_.soft_limit_hz = cmd.soft_limit_hz;
            session_.overrides = cmd.override_cfg;
            session_.has_last_rt = false;
            const std::uint64_t t0 = nowMs();
            const bool ok = downlink_.sendAction(ActionType::Zero, session_.overrides);
            const std::uint64_t down_ms = nowMs() - t0;
            return buildAck(ok, cmd.seq, rx_ms, cmd.tx_ms, down_ms, ok ? "cfg_start" : "downlink");
        }

        if (cmd.type == CommandType::ConfigStop) {
            const std::uint64_t t0 = nowMs();
            const bool ok = downlink_.sendZero();
            const std::uint64_t down_ms = nowMs() - t0;
            session_.active = false;
            session_.has_last_rt = false;
            return buildAck(ok, cmd.seq, rx_ms, cmd.tx_ms, down_ms, ok ? "cfg_stop" : "downlink");
        }

        if (!session_.active) {
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, "no_session");
        }

        const std::string rate_err = checkRate(rx_ms);
        if (!rate_err.empty()) {
            return buildAck(false, cmd.seq, rx_ms, cmd.tx_ms, 0, rate_err);
        }

        const std::uint64_t t0 = nowMs();
        const bool ok = downlink_.sendAction(cmd.action, session_.overrides);
        const std::uint64_t down_ms = nowMs() - t0;
        session_.last_rt_rx_ms = rx_ms;
        session_.has_last_rt = true;
        return buildAck(ok, cmd.seq, rx_ms, cmd.tx_ms, down_ms, ok ? "rt" : "downlink");
    }

private:
    std::string checkRate(std::uint64_t rx_ms) const {
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
        if (hz > session_.soft_limit_hz + 1e-3f) {
            return "rate_over_soft";
        }
        return "";
    }

    static std::string buildAck(bool ok,
                                std::uint32_t seq,
                                std::uint64_t rx_ms,
                                std::uint64_t tx_ms,
                                std::uint64_t down_ms,
                                const std::string& tag) {
        const std::uint64_t up_ms = (tx_ms <= rx_ms) ? (rx_ms - tx_ms) : 0;
        std::ostringstream oss;
        oss << "ACK " << (ok ? "OK" : "ERR")
            << " seq=" << seq
            << " up_ms=" << up_ms
            << " down_ms=" << down_ms
            << " tag=" << tag;
        return oss.str();
    }

    static constexpr float kHardLimitHz = 100.0f;
    SessionState session_;
    ControlDownlink downlink_;
};

void printUsage() {
    std::cout << "Main Processor Communication Layer\n"
              << "Input from communication layer:\n"
              << "  CS <seq> <tx_ms> <soft_hz<=100> <max_power> <left_gain> <right_gain> <left_trim> <right_trim>\n"
              << "  RT <seq> <tx_ms> <F|B|L|R>\n"
              << "  CE <seq> <tx_ms>\n"
              << "  q\n";
}

}  // namespace

int main() {
    ControlDownlink downlink;
    MainProcessor process(std::move(downlink));

    printUsage();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "q" || line == "Q") {
            break;
        }
        std::cout << process.onCommData(line) << std::endl;
    }
    return 0;
}
