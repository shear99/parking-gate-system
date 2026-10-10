#include "portfolio/gate/a6_codec.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace portfolio::gate {
namespace {

std::string normalize(std::string_view raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end) {
        const unsigned char value =
            static_cast<unsigned char>(raw[begin]);
        if (value == 0x02 || value == 0x03 || value == 0 ||
            std::isspace(value) != 0) {
            ++begin;
        } else {
            break;
        }
    }
    while (end > begin) {
        const unsigned char value =
            static_cast<unsigned char>(raw[end - 1]);
        if (value == 0x02 || value == 0x03 || value == 0 ||
            std::isspace(value) != 0) {
            --end;
        } else {
            break;
        }
    }
    std::string result(raw.substr(begin, end - begin));
    for (const char item : result) {
        const auto value = static_cast<unsigned char>(item);
        if (value < 0x20 || value > 0x7e) {
            return {};
        }
    }
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::toupper(value));
        });
    return result;
}

std::string spaced_hex(std::string_view value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index != 0) {
            out << ' ';
        }
        out << std::setw(2)
            << static_cast<unsigned int>(
                   static_cast<unsigned char>(value[index]));
    }
    return out.str();
}

void decode_status(
    std::string_view gate,
    DecodedA6Frame& frame) {
    frame.status = true;
    frame.kind = A6FrameKind::status;
    if (gate == "UP OK") {
        frame.barrier = BarrierState::open;
    } else if (gate == "DOWN OK") {
        frame.barrier = BarrierState::closed;
    } else if (gate == "UP ACTION") {
        frame.barrier = BarrierState::opening;
    } else if (gate == "DOWN ACTION") {
        frame.barrier = BarrierState::closing;
    } else if (gate == "UPLOCK") {
        frame.barrier = BarrierState::open;
        frame.mode = BarrierMode::hold_open;
    } else if (gate == "UNLOCK") {
        frame.mode = BarrierMode::normal;
    } else if (gate == "ERROR") {
        frame.barrier = BarrierState::fault;
    }
}

}  // namespace

std::string a6_command_ascii(Action action) {
    switch (action) {
        case Action::open:
            return "GATE UP";
        case Action::close_request:
            return "GATE DOWN";
        case Action::hold_enable:
            return "GATE UPLOCK";
        case Action::hold_disable:
            return "GATE UNLOCK";
    }
    throw std::invalid_argument("unsupported action");
}

std::string a6_frame_hex(Action action) {
    std::string frame(1, static_cast<char>(0x02));
    frame += a6_command_ascii(action);
    frame.push_back(static_cast<char>(0x03));
    return spaced_hex(frame);
}

DecodedA6Frame decode_a6_frame(std::string_view raw) {
    DecodedA6Frame result;
    result.normalized = normalize(raw);
    const std::string& value = result.normalized;
    if (value.empty()) {
        return result;
    }
    if (value == "SYSTEM INIT") {
        result.kind = A6FrameKind::system_init;
    } else if (value == "GATE UP ACTION") {
        result.kind = A6FrameKind::gate_up_action;
        result.barrier = BarrierState::opening;
    } else if (value == "GATE UP OK") {
        result.kind = A6FrameKind::gate_up_ok;
        result.barrier = BarrierState::open;
    } else if (value == "GATE DOWN ACTION") {
        result.kind = A6FrameKind::gate_down_action;
        result.barrier = BarrierState::closing;
    } else if (value == "GATE DOWN OK") {
        result.kind = A6FrameKind::gate_down_ok;
        result.barrier = BarrierState::closed;
    } else if (value == "GATE UPLOCK") {
        result.kind = A6FrameKind::uplock;
        result.barrier = BarrierState::open;
        result.mode = BarrierMode::hold_open;
    } else if (value == "GATE UNLOCK") {
        result.kind = A6FrameKind::unlock;
        result.mode = BarrierMode::normal;
    } else if (value == "DETECTOR ON" ||
               value == "DETECTOR=ON") {
        result.kind = A6FrameKind::detector_on;
        result.detector = true;
        result.detector_active = true;
    } else if (value == "DETECTOR OFF" ||
               value == "DETECTOR=OFF") {
        result.kind = A6FrameKind::detector_off;
        result.detector = true;
        result.detector_active = false;
    } else if (value == "GATE ERROR") {
        result.kind = A6FrameKind::gate_error;
        result.barrier = BarrierState::fault;
    } else if (value.starts_with("GATE=")) {
        const std::size_t detector = value.find(",DETECTOR=");
        const std::string_view gate = detector == std::string::npos
            ? std::string_view(value).substr(5)
            : std::string_view(value).substr(5, detector - 5);
        decode_status(gate, result);
        if (detector != std::string::npos) {
            const std::string_view detector_value =
                std::string_view(value).substr(detector + 10);
            if (detector_value == "ON") {
                result.detector = true;
                result.detector_active = true;
            } else if (detector_value == "OFF") {
                result.detector = true;
                result.detector_active = false;
            }
        }
    } else {
        result.kind = A6FrameKind::unknown;
    }
    return result;
}

std::string to_string(A6FrameKind kind) {
    switch (kind) {
        case A6FrameKind::system_init:
            return "system_init";
        case A6FrameKind::gate_up_action:
            return "gate_up_action";
        case A6FrameKind::gate_up_ok:
            return "gate_up_ok";
        case A6FrameKind::gate_down_action:
            return "gate_down_action";
        case A6FrameKind::gate_down_ok:
            return "gate_down_ok";
        case A6FrameKind::uplock:
            return "uplock";
        case A6FrameKind::unlock:
            return "unlock";
        case A6FrameKind::detector_on:
            return "detector_on";
        case A6FrameKind::detector_off:
            return "detector_off";
        case A6FrameKind::gate_error:
            return "gate_error";
        case A6FrameKind::status:
            return "status";
        case A6FrameKind::unknown:
            return "unknown";
        case A6FrameKind::invalid:
            return "invalid";
    }
    return "invalid";
}

nlohmann::json guarded_a6_envelope(
    Action action,
    std::string_view command_id,
    std::string_view a6_boot_id,
    std::int64_t a6_uptime_ms,
    int ttl_ms) {
    if (command_id.empty() || command_id.size() > 128 ||
        a6_boot_id.empty() || a6_uptime_ms < 0 ||
        ttl_ms < 500 || ttl_ms > 30000) {
        throw std::invalid_argument("invalid guarded A6 envelope");
    }
    return {
        {"command_id", command_id},
        {"target_boot_id", a6_boot_id},
        {"expires_at_uptime_ms",
         a6_uptime_ms + static_cast<std::int64_t>(ttl_ms)},
        {"encoding", "hex"},
        {"frame_hex", a6_frame_hex(action)},
    };
}

}  // namespace portfolio::gate
