#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "portfolio/gate/protocol.hpp"

namespace portfolio::gate {

enum class A6FrameKind {
    system_init,
    gate_up_action,
    gate_up_ok,
    gate_down_action,
    gate_down_ok,
    uplock,
    unlock,
    detector_on,
    detector_off,
    gate_error,
    status,
    unknown,
    invalid,
};

struct DecodedA6Frame {
    A6FrameKind kind = A6FrameKind::invalid;
    std::string normalized;
    std::optional<BarrierState> barrier;
    std::optional<BarrierMode> mode;
    bool detector = false;
    std::optional<bool> detector_active;
    bool status = false;
};

std::string a6_command_ascii(Action action);
std::string a6_frame_hex(Action action);
DecodedA6Frame decode_a6_frame(std::string_view raw);
std::string to_string(A6FrameKind kind);

nlohmann::json guarded_a6_envelope(
    Action action,
    std::string_view command_id,
    std::string_view a6_boot_id,
    std::int64_t a6_uptime_ms,
    int ttl_ms);

}  // namespace portfolio::gate

