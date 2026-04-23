#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

struct ParsedMessage {
    bool valid = false;
    std::string normalized_payload;

    std::uint32_t seq = 0;
    std::uint64_t client_ts_ms = 0;
    bool has_client_ts = false;
    std::string trace_id = "gateway-local";

    std::string error;
};

class ProcessorClient {
public:
    std::string dispatch(const std::string& normalized_payload) {
        // Placeholder for IPC dispatch to usv_control / main processor.
        std::ostringstream oss;
        oss << "ACK OK routed detail=forwarded payload=\"" << normalized_payload << "\"";
        return oss.str();
    }
};

class CommunicationLayer {
public:
    explicit CommunicationLayer(ProcessorClient processor_client)
        : processor_client_(std::move(processor_client)) {}

    std::string onReceive(const std::string& line) {
        const auto recv_tp = std::chrono::steady_clock::now();
        const std::uint64_t recv_ms = nowMs(recv_tp);

        const ParsedMessage msg = parseLine(line);
        if (!msg.valid) {
            return buildAck("ERR", msg.seq, "gw_bad_msg:" + msg.error,
                            recv_ms, -1.0, recv_tp, "parse_reject", msg.trace_id);
        }

        const std::string processor_ack = processor_client_.dispatch(msg.normalized_payload);
        return buildAck("OK", msg.seq, "gw_forwarded", recv_ms,
                        computeUplinkMs(msg, recv_ms), recv_tp, processor_ack, msg.trace_id);
    }

private:
    static std::uint64_t nowMs(const std::chrono::steady_clock::time_point& tp) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count());
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

    static bool parseKVToken(const std::string& token, std::string* key, std::string* value) {
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

    static ParsedMessage parseLine(const std::string& line) {
        ParsedMessage msg;

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
            if (!(action_token == "F" || action_token == "B" || action_token == "L" || action_token == "R")) {
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

            std::ostringstream normalized;
            normalized << "R " << msg.seq << ' ' << (msg.has_client_ts ? msg.client_ts_ms : 0) << ' ' << action_token;
            msg.normalized_payload = normalized.str();
            msg.valid = true;
            return msg;
        }

        if (mode == "RT") {
            msg.trace_id = "alias-RT";
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
            if (!(action_token == "F" || action_token == "B" || action_token == "L" || action_token == "R")) {
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

            std::ostringstream normalized;
            normalized << "R " << msg.seq << ' ' << (msg.has_client_ts ? msg.client_ts_ms : 0) << ' ' << action_token;
            msg.normalized_payload = normalized.str();
            msg.valid = true;
            return msg;
        }

        if (mode == "SLI") {
            // SLAM image ingest frame: SLI <seq> <tx_ms> key=value...
            std::string seq_token;
            std::string tx_token;
            if (!(iss >> seq_token >> tx_token)) {
                msg.error = "sl_short_fields";
                return msg;
            }
            if (!parseUInt32(seq_token, &msg.seq) || !parseUInt64(tx_token, &msg.client_ts_ms)) {
                msg.error = "sl_bad_header";
                return msg;
            }

            bool has_frame_id = false;
            bool has_size = false;
            std::ostringstream normalized;
            normalized << "SLI " << msg.seq << ' ' << msg.client_ts_ms;
            std::string kv;
            while (iss >> kv) {
                std::string key;
                std::string value;
                if (!parseKVToken(kv, &key, &value)) {
                    msg.error = "sli_bad_kv";
                    return msg;
                }
                if (key == "frame_id") {
                    std::uint32_t frame_id = 0;
                    if (!parseUInt32(value, &frame_id) || frame_id == 0) {
                        msg.error = "sli_bad_frame_id";
                        return msg;
                    }
                    has_frame_id = true;
                }
                if (key == "width" || key == "height") {
                    int v = 0;
                    if (!parseInt(value, &v) || v <= 0) {
                        msg.error = "sli_bad_shape";
                        return msg;
                    }
                    has_size = true;
                }
                normalized << ' ' << key << '=' << value;
            }
            if (!has_frame_id || !has_size) {
                msg.error = "sli_incomplete";
                return msg;
            }
            msg.normalized_payload = normalized.str();
            msg.has_client_ts = true;
            msg.valid = true;
            return msg;
        }

        if (mode == "SL") {
            msg.trace_id = "alias-SL";
            std::string seq_token;
            std::string tx_token;
            if (!(iss >> seq_token >> tx_token)) {
                msg.error = "sl_missing_header";
                return msg;
            }
            if (!parseUInt32(seq_token, &msg.seq) || !parseUInt64(tx_token, &msg.client_ts_ms)) {
                msg.error = "sl_bad_header";
                return msg;
            }

            bool has_frame_id = false;
            bool has_size = false;
            std::ostringstream normalized;
            normalized << "SLI " << msg.seq << ' ' << msg.client_ts_ms;

            std::string kv;
            while (iss >> kv) {
                std::string key;
                std::string value;
                if (!parseKVToken(kv, &key, &value)) {
                    msg.error = "sl_bad_kv";
                    return msg;
                }
                if (key == "frame_id") {
                    std::uint32_t frame_id = 0;
                    if (!parseUInt32(value, &frame_id) || frame_id == 0) {
                        msg.error = "sl_bad_frame_id";
                        return msg;
                    }
                    has_frame_id = true;
                }
                if (key == "width" || key == "height") {
                    int v = 0;
                    if (!parseInt(value, &v) || v <= 0) {
                        msg.error = "sl_bad_shape";
                        return msg;
                    }
                    has_size = true;
                }
                normalized << ' ' << key << '=' << value;
            }
            if (!has_frame_id || !has_size) {
                msg.error = "sl_incomplete";
                return msg;
            }
            msg.normalized_payload = normalized.str();
            msg.has_client_ts = true;
            msg.valid = true;
            return msg;
        }

        if (mode == "CS" || mode == "CE") {
            std::ostringstream normalized;
            normalized << (mode == "CS" ? "C START" : "C STOP");

            std::string kv;
            while (iss >> kv) {
                std::string key;
                std::string value;
                if (!parseKVToken(kv, &key, &value)) {
                    msg.error = "cfg_alias_bad_kv";
                    return msg;
                }
                if (key == "seq") {
                    if (!parseUInt32(value, &msg.seq)) {
                        msg.error = "cfg_alias_bad_seq";
                        return msg;
                    }
                } else if (key == "ts") {
                    if (!parseUInt64(value, &msg.client_ts_ms)) {
                        msg.error = "cfg_alias_bad_ts";
                        return msg;
                    }
                    msg.has_client_ts = true;
                }
                normalized << ' ' << key << '=' << value;
            }
            msg.normalized_payload = normalized.str();
            msg.valid = true;
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
                std::ostringstream normalized;
                normalized << "C STOP";

                std::string kv;
                while (iss >> kv) {
                    std::string key;
                    std::string value;
                    if (!parseKVToken(kv, &key, &value)) {
                        msg.error = "cfg_stop_bad_kv";
                        return msg;
                    }
                    if (key == "seq") {
                        if (!parseUInt32(value, &msg.seq)) {
                            msg.error = "cfg_stop_bad_seq";
                            return msg;
                        }
                    } else if (key == "ts") {
                        if (!parseUInt64(value, &msg.client_ts_ms)) {
                            msg.error = "cfg_stop_bad_ts";
                            return msg;
                        }
                        msg.has_client_ts = true;
                    }
                    normalized << ' ' << key << '=' << value;
                }
                msg.normalized_payload = normalized.str();
                msg.valid = true;
                return msg;
            }

            if (sub == "START") {
                std::ostringstream normalized;
                normalized << "C START";
                std::string kv;
                while (iss >> kv) {
                    std::string key;
                    std::string value;
                    if (!parseKVToken(kv, &key, &value)) {
                        msg.error = "cfg_bad_kv";
                        return msg;
                    }
                    if (key == "seq") {
                        if (!parseUInt32(value, &msg.seq)) {
                            msg.error = "cfg_bad_seq";
                            return msg;
                        }
                    }
                    if (key == "ts") {
                        if (!parseUInt64(value, &msg.client_ts_ms)) {
                            msg.error = "cfg_bad_ts";
                            return msg;
                        }
                        msg.has_client_ts = true;
                    }
                    normalized << ' ' << key << '=' << value;
                }
                msg.normalized_payload = normalized.str();
                msg.valid = true;
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
                                const std::chrono::steady_clock::time_point& recv_tp,
                                const std::string& route_result,
                                const std::string& trace_id) {
        const auto send_tp = std::chrono::steady_clock::now();
        const double downlink_ms =
            std::chrono::duration_cast<std::chrono::microseconds>(send_tp - recv_tp).count() / 1000.0;

        std::ostringstream oss;
        oss << "ACK " << code
            << " seq=" << seq
            << " detail=" << detail
            << " rx_ms=" << recv_ms
            << " ul_ms=" << std::fixed << std::setprecision(2) << uplink_ms
            << " dl_ms=" << downlink_ms
            << " trace=" << trace_id
            << " route=" << route_result;
        return oss.str();
    }

    ProcessorClient processor_client_;
};

void printUsage() {
    std::cout << "Communication Layer (Gateway Adapter)\n"
              << "Realtime short frame:\n"
              << "  R <seq> <F|B|L|R> [client_ts_ms]\n"
              << "  RT <seq> <F|B|L|R> [client_ts_ms]  # legacy alias\n"
              << "SLAM image ingest frame:\n"
              << "  SLI <seq> <tx_ms> frame_id=<n> width=<w> height=<h> pixel_fmt=<fmt> keyframe=<0|1> quality_hint=<0..100> payload_ref=<id>\n"
              << "  SL <seq> <tx_ms> frame_id=<n> width=<w> height=<h> pixel_fmt=<fmt> keyframe=<0|1> quality_hint=<0..100> payload_ref=<id>  # legacy alias\n"
              << "Config long frame:\n"
              << "  C START seq=<n> ts=<ms> soft_hz=<v> max_power=<v> left_gain=<v> right_gain=<v> left_trim=<v> right_trim=<v> slam_max_fps=<v> slam_timeout_ms=<v> slam_max_groups=<v> slam_min_quality=<v> slam_drop_policy=<reject|oldest|newest>\n"
              << "  CS seq=<n> ts=<ms> ...  # legacy alias\n"
              << "  C STOP seq=<n> ts=<ms>\n"
              << "  CE seq=<n> ts=<ms>  # legacy alias\n"
              << "ACK template:\n"
              << "  ACK <OK|ERR> seq=<n> detail=<code> rx_ms=<n> ul_ms=<n> dl_ms=<n> trace=<id> route=<result>\n"
              << "Quit:\n"
              << "  q\n";
}

}  // namespace

int main() {
    ProcessorClient processor_client;
    CommunicationLayer comm(std::move(processor_client));

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
