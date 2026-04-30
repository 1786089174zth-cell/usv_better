#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

struct GatewaySwitches {
    bool legacy_alias_enabled = true;
    bool sli_enabled = true;
    std::uint32_t route_timeout_ms = 50;
};

struct GatewayMetrics {
    std::uint64_t rx_total = 0;
    std::uint64_t parse_fail = 0;
    std::uint64_t route_timeout = 0;
    std::uint64_t sli_drop = 0;
    std::uint64_t ack_ok = 0;
    std::uint64_t ack_err = 0;
    std::deque<double> ack_dl_ms_samples;
};

class ProcessorClient {
public:
    std::string dispatch(const std::string& normalized_payload) {
        // Placeholder for IPC dispatch to usv_control / main processor.
        // In production, this should forward normalized_payload to MainProcessor
        // and return its ACK in the format: ACK OK/ERR seq=... up_ms=... down_ms=... tag=... detail=...
        // For now, returning a valid sample ACK that parseProcessorAck can handle.
        std::ostringstream oss;
        oss << "ACK OK seq=0 up_ms=0 down_ms=5 tag=gw_forwarded detail=placeholder";
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

        std::string mgmt_response;
        if (tryHandleGatewayCommand(line, recv_ms, recv_tp, &mgmt_response)) {
            return mgmt_response;
        }

        metrics_.rx_total += 1;

        const std::string trace_id = nextTraceId();
        const ParsedMessage msg = parseLine(line, switches_);
        ParsedMessage traced_msg = msg;
        traced_msg.trace_id = trace_id;

        if (!traced_msg.valid) {
            metrics_.parse_fail += 1;
            if (traced_msg.error == "sli_disabled") {
                metrics_.sli_drop += 1;
            }
            const std::string ack = buildAck(false, traced_msg.seq, 0, 0,
                                             "gw_bad_msg", traced_msg.error,
                                             trace_id, "parse_reject");
            recordAck(false, 0.0);
            return ack;
        }

        const auto route_begin = std::chrono::steady_clock::now();
        const std::string processor_ack = processor_client_.dispatch(traced_msg.normalized_payload);
        const double route_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - route_begin).count() / 1000.0;
        if (route_ms > static_cast<double>(switches_.route_timeout_ms)) {
            metrics_.route_timeout += 1;
            const std::string ack = buildAck(false, traced_msg.seq, 0, 0,
                                             "gw_route_timeout", "timeout",
                                             trace_id, "route_timeout");
            recordAck(false, 0.0);
            return ack;
        }

        // Parse processor ACK and append gateway context
        ProcessorAckFields processor_fields = parseProcessorAck(processor_ack);
        const std::string gw_ack = buildAck(processor_fields.ok, processor_fields.seq,
                                             processor_fields.up_ms, processor_fields.down_ms,
                                             processor_fields.tag, processor_fields.detail,
                                             trace_id, "forwarded");
        recordAck(processor_fields.ok, static_cast<double>(processor_fields.down_ms));
        return gw_ack;
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

    static ParsedMessage parseLine(const std::string& line, const GatewaySwitches& switches) {
        ParsedMessage msg;

        std::istringstream iss(line);
        std::string mode;
        if (!(iss >> mode)) {
            msg.error = "empty";
            return msg;
        }

        if (mode == "R") {
            // Realtime short frame: R <seq> <F|L|R> [client_ts_ms]
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
            if (!(action_token == "F" || action_token == "L" || action_token == "R")) {
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
            if (!switches.legacy_alias_enabled) {
                msg.error = "alias_disabled";
                return msg;
            }
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
            if (!(action_token == "F" || action_token == "L" || action_token == "R")) {
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
            if (!switches.sli_enabled) {
                msg.error = "sli_disabled";
                return msg;
            }
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
            if (!switches.legacy_alias_enabled) {
                msg.error = "alias_disabled";
                return msg;
            }
            if (!switches.sli_enabled) {
                msg.error = "sli_disabled";
                return msg;
            }
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
            if (!switches.legacy_alias_enabled) {
                msg.error = "alias_disabled";
                return msg;
            }
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

    struct ProcessorAckFields {
        bool ok = false;
        std::uint32_t seq = 0;
        std::uint64_t up_ms = 0;
        std::uint64_t down_ms = 0;
        std::string tag = "unknown";
        std::string detail = "parse_error";
    };

    static ProcessorAckFields parseProcessorAck(const std::string& ack_str) {
        ProcessorAckFields fields;
        std::istringstream iss(ack_str);
        
        std::string ack_kw, status;
        if (!(iss >> ack_kw >> status)) {
            return fields;  // Invalid format
        }
        
        fields.ok = (status == "OK");
        
        std::string kv;
        while (iss >> kv) {
            std::string key, value;
            if (!parseKVToken(kv, &key, &value)) {
                continue;
            }
            if (key == "seq") {
                parseUInt32(value, &fields.seq);
            } else if (key == "up_ms") {
                parseUInt64(value, &fields.up_ms);
            } else if (key == "down_ms") {
                parseUInt64(value, &fields.down_ms);
            } else if (key == "tag") {
                fields.tag = value;
            } else if (key == "detail") {
                fields.detail = value;
            }
        }
        return fields;
    }

    static std::string buildAck(bool ok,
                                std::uint32_t seq,
                                std::uint64_t up_ms,
                                std::uint64_t down_ms,
                                const std::string& tag,
                                const std::string& detail,
                                const std::string& gw_trace,
                                const std::string& gw_route) {
        std::ostringstream oss;
        oss << "ACK " << (ok ? "OK" : "ERR")
            << " seq=" << seq
            << " up_ms=" << up_ms
            << " down_ms=" << down_ms
            << " tag=" << tag
            << " detail=" << detail
            << " gw_trace=" << gw_trace
            << " gw_route=" << gw_route;
        return oss.str();
    }

    static double computeUplinkMs(const ParsedMessage& msg, std::uint64_t recv_ms) {
        if (!msg.has_client_ts || recv_ms < msg.client_ts_ms) {
            return -1.0;
        }
        return static_cast<double>(recv_ms - msg.client_ts_ms);
    }

    static bool parseOnOff(const std::string& value, bool* out) {
        if (out == nullptr) {
            return false;
        }
        if (value == "on") {
            *out = true;
            return true;
        }
        if (value == "off") {
            *out = false;
            return true;
        }
        return false;
    }

    std::string nextTraceId() {
        std::ostringstream oss;
        oss << "gw-" << ++trace_counter_;
        return oss.str();
    }

    void recordAck(bool ok, double downlink_ms) {
        if (ok) {
            metrics_.ack_ok += 1;
        } else {
            metrics_.ack_err += 1;
        }
        // Only record downlink_ms if > 0 (gateway processing delay)
        if (downlink_ms > 0.0) {
            metrics_.ack_dl_ms_samples.push_back(downlink_ms);
            if (metrics_.ack_dl_ms_samples.size() > kMaxAckSamples) {
                metrics_.ack_dl_ms_samples.pop_front();
            }
        }
    }

    static double percentile(const std::deque<double>& samples, double p) {
        if (samples.empty()) {
            return 0.0;
        }
        std::vector<double> sorted(samples.begin(), samples.end());
        std::sort(sorted.begin(), sorted.end());
        const double rank = (p / 100.0) * static_cast<double>(sorted.size() - 1);
        const std::size_t idx = static_cast<std::size_t>(rank);
        return sorted[idx];
    }

    bool rollbackRecommended() const {
        if (metrics_.rx_total < 20) {
            return false;
        }
        const double parse_fail_rate = static_cast<double>(metrics_.parse_fail) / static_cast<double>(metrics_.rx_total);
        const double route_timeout_rate = static_cast<double>(metrics_.route_timeout) / static_cast<double>(metrics_.rx_total);
        return parse_fail_rate > 0.10 || route_timeout_rate > 0.05;
    }

    std::string buildHealthSnapshot() const {
        const double ack_p95 = percentile(metrics_.ack_dl_ms_samples, 95.0);
        const double ack_p99 = percentile(metrics_.ack_dl_ms_samples, 99.0);
        std::ostringstream oss;
        oss << "GW HEALTH"
            << " legacy_alias=" << (switches_.legacy_alias_enabled ? "on" : "off")
            << " sli_enabled=" << (switches_.sli_enabled ? "on" : "off")
            << " route_timeout_ms=" << switches_.route_timeout_ms
            << " rx_total=" << metrics_.rx_total
            << " parse_fail=" << metrics_.parse_fail
            << " route_timeout=" << metrics_.route_timeout
            << " sli_drop=" << metrics_.sli_drop
            << " ack_ok=" << metrics_.ack_ok
            << " ack_err=" << metrics_.ack_err
            << " ack_p95_ms=" << std::fixed << std::setprecision(2) << ack_p95
            << " ack_p99_ms=" << ack_p99
            << " rollback_recommended=" << (rollbackRecommended() ? 1 : 0);
        return oss.str();
    }

    bool tryHandleGatewayCommand(const std::string& line,
                                 std::uint64_t recv_ms,
                                 const std::chrono::steady_clock::time_point& recv_tp,
                                 std::string* response) {
        if (response == nullptr) {
            return false;
        }

        std::istringstream iss(line);
        std::string h0;
        std::string h1;
        if (!(iss >> h0)) {
            return false;
        }
        if (h0 != "GW") {
            return false;
        }
        if (!(iss >> h1)) {
            *response = buildAck(false, 0, 0, 0, "gw_bad_cmd", "cmd_missing",
                                 nextTraceId(), "mgmt");
            recordAck(false, 0.0);
            return true;
        }

        if (h1 == "HEALTH") {
            *response = buildHealthSnapshot();
            return true;
        }

        if (h1 == "ROLLBACK") {
            switches_.legacy_alias_enabled = true;
            switches_.sli_enabled = true;
            switches_.route_timeout_ms = 80;
            *response = buildAck(true, 0, 0, 0, "gw_rollback", "ok",
                                 nextTraceId(), "mgmt");
            recordAck(true, 0.0);
            return true;
        }

        if (h1 == "SWITCH") {
            std::string kv;
            bool saw_any = false;
            while (iss >> kv) {
                std::string key;
                std::string value;
                if (!parseKVToken(kv, &key, &value)) {
                    *response = buildAck(false, 0, 0, 0, "gw_bad_switch", "bad_kv",
                                         nextTraceId(), "mgmt");
                    recordAck(false, 0.0);
                    return true;
                }
                if (key == "legacy_alias") {
                    bool parsed = false;
                    if (!parseOnOff(value, &parsed)) {
                        *response = buildAck(false, 0, 0, 0, "gw_bad_switch", "bad_legacy_alias",
                                             nextTraceId(), "mgmt");
                        recordAck(false, 0.0);
                        return true;
                    }
                    switches_.legacy_alias_enabled = parsed;
                } else if (key == "sli_enabled") {
                    bool parsed = false;
                    if (!parseOnOff(value, &parsed)) {
                        *response = buildAck(false, 0, 0, 0, "gw_bad_switch", "bad_sli_enabled",
                                             nextTraceId(), "mgmt");
                        recordAck(false, 0.0);
                        return true;
                    }
                    switches_.sli_enabled = parsed;
                } else if (key == "route_timeout_ms") {
                    int timeout_ms = 0;
                    if (!parseInt(value, &timeout_ms) || timeout_ms <= 0 || timeout_ms > 5000) {
                        *response = buildAck(false, 0, 0, 0, "gw_bad_switch", "bad_route_timeout",
                                             nextTraceId(), "mgmt");
                        recordAck(false, 0.0);
                        return true;
                    }
                    switches_.route_timeout_ms = static_cast<std::uint32_t>(timeout_ms);
                } else {
                    *response = buildAck(false, 0, 0, 0, "gw_bad_switch", "unknown_key",
                                         nextTraceId(), "mgmt");
                    recordAck(false, 0.0);
                    return true;
                }
                saw_any = true;
            }

            if (!saw_any) {
                *response = buildAck(false, 0, 0, 0, "gw_bad_switch", "empty",
                                     nextTraceId(), "mgmt");
                recordAck(false, 0.0);
                return true;
            }

            *response = buildAck(true, 0, 0, 0, "gw_switch", "applied",
                                 nextTraceId(), "mgmt");
            recordAck(true, 0.0);
            return true;
        }

        *response = buildAck(false, 0, 0, 0, "gw_bad_cmd", "unknown",
                             nextTraceId(), "mgmt");
        recordAck(false, 0.0);
        return true;
    }

    ProcessorClient processor_client_;
    GatewaySwitches switches_;
    GatewayMetrics metrics_;
    std::uint64_t trace_counter_ = 0;

    static constexpr std::size_t kMaxAckSamples = 256;
};

void printUsage() {
    std::cout << "Communication Layer (Gateway Adapter)\n"
              << "Realtime short frame:\n"
              << "  R <seq> <F|L|R> [client_ts_ms]\n"
              << "  RT <seq> <F|L|R> [client_ts_ms]  # legacy alias\n"
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
              << "Gateway management:\n"
              << "  GW HEALTH\n"
              << "  GW SWITCH legacy_alias=<on|off> sli_enabled=<on|off> route_timeout_ms=<1..5000>\n"
              << "  GW ROLLBACK\n"
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
