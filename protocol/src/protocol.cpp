#include "portfolio/gate/protocol.hpp"

#include <array>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace portfolio::gate {
namespace {

bool valid_identifier(const std::string& value) {
    return !value.empty() && value.size() <= 128;
}

}  // namespace

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string make_id(std::string_view prefix) {
    std::array<unsigned char, 16> bytes{};
    std::random_device source;
    for (auto& value : bytes) {
        value = static_cast<unsigned char>(source());
    }
    std::ostringstream out;
    out << prefix << '_';
    for (const auto value : bytes) {
        out << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(value);
    }
    return out.str();
}

std::string to_string(Action value) {
    switch (value) {
        case Action::open:
            return "open";
        case Action::close_request:
            return "close_request";
        case Action::hold_enable:
            return "hold_enable";
        case Action::hold_disable:
            return "hold_disable";
    }
    return "open";
}

std::string to_string(BarrierState value) {
    switch (value) {
        case BarrierState::unknown:
            return "unknown";
        case BarrierState::closed:
            return "closed";
        case BarrierState::opening:
            return "opening";
        case BarrierState::open:
            return "open";
        case BarrierState::closing:
            return "closing";
        case BarrierState::fault:
            return "fault";
    }
    return "unknown";
}

std::string to_string(BarrierMode value) {
    return value == BarrierMode::hold_open ? "hold_open" : "normal";
}

std::string to_string(CommandStatus value) {
    switch (value) {
        case CommandStatus::accepted:
            return "accepted";
        case CommandStatus::pi_received:
            return "pi_received";
        case CommandStatus::sent:
            return "sent";
        case CommandStatus::acknowledged:
            return "acknowledged";
        case CommandStatus::physically_started:
            return "physically_started";
        case CommandStatus::physically_completed:
            return "physically_completed";
        case CommandStatus::rejected:
            return "rejected";
        case CommandStatus::expired:
            return "expired";
        case CommandStatus::failed:
            return "failed";
        case CommandStatus::result_unknown:
            return "result_unknown";
    }
    return "failed";
}

std::string to_string(OperationDirection value) {
    return value == OperationDirection::opening ? "opening" : "closing";
}

std::string to_string(OperationPhase value) {
    switch (value) {
        case OperationPhase::opening_requested:
            return "opening_requested";
        case OperationPhase::opening:
            return "opening";
        case OperationPhase::closing_requested:
            return "closing_requested";
        case OperationPhase::closing:
            return "closing";
        case OperationPhase::terminal:
            return "terminal";
        case OperationPhase::interrupted:
            return "interrupted";
        case OperationPhase::failed:
            return "failed";
    }
    return "failed";
}

std::optional<Action> action_from_string(std::string_view value) {
    if (value == "open") {
        return Action::open;
    }
    if (value == "close_request") {
        return Action::close_request;
    }
    if (value == "hold_enable") {
        return Action::hold_enable;
    }
    if (value == "hold_disable") {
        return Action::hold_disable;
    }
    return std::nullopt;
}

std::optional<BarrierState> barrier_state_from_string(
    std::string_view value) {
    if (value == "unknown") {
        return BarrierState::unknown;
    }
    if (value == "closed") {
        return BarrierState::closed;
    }
    if (value == "opening") {
        return BarrierState::opening;
    }
    if (value == "open") {
        return BarrierState::open;
    }
    if (value == "closing") {
        return BarrierState::closing;
    }
    if (value == "fault") {
        return BarrierState::fault;
    }
    return std::nullopt;
}

std::optional<BarrierMode> barrier_mode_from_string(
    std::string_view value) {
    if (value == "normal") {
        return BarrierMode::normal;
    }
    if (value == "hold_open") {
        return BarrierMode::hold_open;
    }
    return std::nullopt;
}

std::optional<CommandStatus> command_status_from_string(
    std::string_view value) {
    if (value == "accepted") {
        return CommandStatus::accepted;
    }
    if (value == "pi_received") {
        return CommandStatus::pi_received;
    }
    if (value == "sent") {
        return CommandStatus::sent;
    }
    if (value == "acknowledged") {
        return CommandStatus::acknowledged;
    }
    if (value == "physically_started") {
        return CommandStatus::physically_started;
    }
    if (value == "physically_completed") {
        return CommandStatus::physically_completed;
    }
    if (value == "rejected") {
        return CommandStatus::rejected;
    }
    if (value == "expired") {
        return CommandStatus::expired;
    }
    if (value == "failed") {
        return CommandStatus::failed;
    }
    if (value == "result_unknown") {
        return CommandStatus::result_unknown;
    }
    return std::nullopt;
}

std::optional<OperationDirection> operation_direction_from_string(
    std::string_view value) {
    if (value == "opening") {
        return OperationDirection::opening;
    }
    if (value == "closing") {
        return OperationDirection::closing;
    }
    return std::nullopt;
}

std::optional<OperationPhase> operation_phase_from_string(
    std::string_view value) {
    if (value == "opening_requested") {
        return OperationPhase::opening_requested;
    }
    if (value == "opening") {
        return OperationPhase::opening;
    }
    if (value == "closing_requested") {
        return OperationPhase::closing_requested;
    }
    if (value == "closing") {
        return OperationPhase::closing;
    }
    if (value == "terminal") {
        return OperationPhase::terminal;
    }
    if (value == "interrupted") {
        return OperationPhase::interrupted;
    }
    if (value == "failed") {
        return OperationPhase::failed;
    }
    return std::nullopt;
}

bool terminal(CommandStatus value) {
    return value == CommandStatus::acknowledged ||
           value == CommandStatus::physically_completed ||
           value == CommandStatus::rejected ||
           value == CommandStatus::expired ||
           value == CommandStatus::failed ||
           value == CommandStatus::result_unknown;
}

bool is_actuation_action(Action) {
    return true;
}

bool is_persisted_physical_event(std::string_view value) {
    return value == "gate_up_action" || value == "gate_up_ok" ||
           value == "gate_down_action" || value == "gate_down_ok" ||
           value == "uplock" || value == "unlock" ||
           value == "gate_error";
}

void to_json(json& target, const Envelope& value) {
    target = {
        {"schema_version", value.schema_version},
        {"type", value.type},
        {"event_id", value.event_id},
        {"occurred_at_us", value.occurred_at_us},
        {"site_id", value.site_id},
        {"gate_id", value.gate_id},
        {"boot_id", value.boot_id},
        {"revision", value.revision},
        {"sequence", value.sequence},
        {"payload", value.payload},
    };
}

void from_json(const json& source, Envelope& value) {
    source.at("schema_version").get_to(value.schema_version);
    source.at("type").get_to(value.type);
    source.at("event_id").get_to(value.event_id);
    source.at("occurred_at_us").get_to(value.occurred_at_us);
    source.at("site_id").get_to(value.site_id);
    source.at("gate_id").get_to(value.gate_id);
    value.boot_id = source.value("boot_id", "");
    value.revision = source.value("revision", "");
    value.sequence = source.value("sequence", std::uint64_t{0});
    source.at("payload").get_to(value.payload);
}

void to_json(json& target, const CommandRequest& value) {
    target = {
        {"command_id", value.command_id},
        {"idempotency_key", value.idempotency_key},
        {"action", to_string(value.action)},
        {"actor_id", value.actor_id},
        {"source", value.source},
        {"requested_at_us", value.requested_at_us},
        {"expires_at_us", value.expires_at_us},
        {"target_boot_id", value.target_boot_id},
        {"expected_state_revision", value.expected_state_revision},
        {"ttl_ms", value.ttl_ms},
    };
}

void from_json(const json& source, CommandRequest& value) {
    source.at("command_id").get_to(value.command_id);
    source.at("idempotency_key").get_to(value.idempotency_key);
    const auto action =
        action_from_string(source.at("action").get<std::string>());
    if (!action) {
        throw std::invalid_argument("unsupported action");
    }
    value.action = *action;
    value.actor_id = source.value("actor_id", "");
    value.source = source.value("source", "");
    source.at("requested_at_us").get_to(value.requested_at_us);
    source.at("expires_at_us").get_to(value.expires_at_us);
    source.at("target_boot_id").get_to(value.target_boot_id);
    source.at("expected_state_revision")
        .get_to(value.expected_state_revision);
    value.ttl_ms = source.value("ttl_ms", 5000);
}

void to_json(json& target, const GateOperation& value) {
    target = {
        {"operation_id", value.operation_id},
        {"direction", to_string(value.direction)},
        {"phase", to_string(value.phase)},
        {"boot_id", value.boot_id},
        {"started_at_us", value.started_at_us},
        {"updated_at_us", value.updated_at_us},
        {"terminal_reason", value.terminal_reason},
        {"observed", value.observed},
    };
}

void from_json(const json& source, GateOperation& value) {
    source.at("operation_id").get_to(value.operation_id);
    const auto direction = operation_direction_from_string(
        source.at("direction").get<std::string>());
    const auto phase = operation_phase_from_string(
        source.at("phase").get<std::string>());
    if (!direction || !phase) {
        throw std::invalid_argument("unsupported operation");
    }
    value.direction = *direction;
    value.phase = *phase;
    value.boot_id = source.value("boot_id", "");
    value.started_at_us = source.value("started_at_us", std::int64_t{0});
    value.updated_at_us = source.value("updated_at_us", std::int64_t{0});
    value.terminal_reason = source.value("terminal_reason", "");
    value.observed = source.value("observed", false);
}

Envelope make_envelope(
    std::string type,
    json payload,
    std::string boot_id,
    std::string revision,
    std::uint64_t sequence) {
    Envelope result;
    result.type = std::move(type);
    result.event_id = make_id("event");
    result.occurred_at_us = now_us();
    result.boot_id = std::move(boot_id);
    result.revision = std::move(revision);
    result.sequence = sequence;
    result.payload = std::move(payload);
    return result;
}

std::optional<std::string> validate_envelope(const Envelope& envelope) {
    if (envelope.schema_version != kSchemaVersion) {
        return "unsupported_schema_version";
    }
    if (envelope.site_id != kSiteId || envelope.gate_id != kGateId) {
        return "target_mismatch";
    }
    if (!valid_identifier(envelope.type) ||
        !valid_identifier(envelope.event_id) ||
        envelope.occurred_at_us <= 0 || !envelope.payload.is_object()) {
        return "invalid_envelope";
    }
    return std::nullopt;
}

std::optional<std::string> validate_command(
    const CommandRequest& command,
    std::int64_t current_us) {
    if (!valid_identifier(command.command_id) ||
        !valid_identifier(command.idempotency_key) ||
        !valid_identifier(command.actor_id) ||
        !valid_identifier(command.source) ||
        !valid_identifier(command.target_boot_id) ||
        !valid_identifier(command.expected_state_revision)) {
        return "invalid_command_fields";
    }
    if (command.ttl_ms < 500 || command.ttl_ms > 30000) {
        return "invalid_ttl";
    }
    if (command.requested_at_us <= 0 ||
        command.expires_at_us <= current_us ||
        command.expires_at_us <= command.requested_at_us) {
        return "request_expired";
    }
    return std::nullopt;
}

}  // namespace portfolio::gate
