#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace portfolio::gate {

using json = nlohmann::json;

inline constexpr int kSchemaVersion = 1;
inline constexpr std::string_view kSiteId = "demo-site";
inline constexpr std::string_view kGateId = "demo-gate";

inline constexpr std::string_view kEdgeStatusTopic =
    "gate/v1/demo-gate/edge/status";
inline constexpr std::string_view kStateTopic =
    "gate/v1/demo-gate/state";
inline constexpr std::string_view kPhysicalEventTopic =
    "gate/v1/demo-gate/events/physical";
inline constexpr std::string_view kOperationTopic =
    "gate/v1/demo-gate/operation";
inline constexpr std::string_view kOperationEventTopic =
    "gate/v1/demo-gate/events/operation";
inline constexpr std::string_view kDetectorEventTopic =
    "gate/v1/demo-gate/sensors/detector/events";
inline constexpr std::string_view kDetectorStateTopic =
    "gate/v1/demo-gate/sensors/detector/state";
inline constexpr std::string_view kSyncRequestTopic =
    "gate/v1/demo-gate/sync/request";
inline constexpr std::string_view kSyncResponseTopic =
    "gate/v1/demo-gate/sync/response";
inline constexpr std::string_view kCommandRequestTopic =
    "gate/v1/demo-gate/commands/request";
inline constexpr std::string_view kCommandStatusTopic =
    "gate/v1/demo-gate/commands/status";
inline constexpr std::string_view kCommandQueryTopic =
    "gate/v1/demo-gate/commands/query";

enum class Action {
    open,
    close_request,
    hold_enable,
    hold_disable,
};

enum class BarrierState {
    unknown,
    closed,
    opening,
    open,
    closing,
    fault,
};

enum class BarrierMode {
    normal,
    hold_open,
};

enum class CommandStatus {
    accepted,
    pi_received,
    sent,
    acknowledged,
    physically_started,
    physically_completed,
    rejected,
    expired,
    failed,
    result_unknown,
};

enum class OperationDirection {
    opening,
    closing,
};

enum class OperationPhase {
    opening_requested,
    opening,
    closing_requested,
    closing,
    terminal,
    interrupted,
    failed,
};

struct GateOperation {
    std::string operation_id;
    OperationDirection direction = OperationDirection::opening;
    OperationPhase phase = OperationPhase::opening_requested;
    std::string boot_id;
    std::int64_t started_at_us = 0;
    std::int64_t updated_at_us = 0;
    std::string terminal_reason;
    bool observed = false;
};

struct Envelope {
    int schema_version = kSchemaVersion;
    std::string type;
    std::string event_id;
    std::int64_t occurred_at_us = 0;
    std::string site_id = std::string(kSiteId);
    std::string gate_id = std::string(kGateId);
    std::string boot_id;
    std::string revision;
    std::uint64_t sequence = 0;
    json payload = json::object();
};

struct CommandRequest {
    std::string command_id;
    std::string idempotency_key;
    Action action = Action::open;
    std::string actor_id;
    std::string source;
    std::int64_t requested_at_us = 0;
    std::int64_t expires_at_us = 0;
    std::string target_boot_id;
    std::string expected_state_revision;
    int ttl_ms = 5000;
};

struct CommandResult {
    bool accepted = false;
    CommandStatus status = CommandStatus::rejected;
    std::string reason;
    int retry_after_ms = 0;
    std::optional<CommandRequest> request;
    std::optional<GateOperation> operation;
    bool joined = false;
    bool a6_tx_created = false;
};

std::int64_t now_us();
std::string make_id(std::string_view prefix);
std::string to_string(Action value);
std::string to_string(BarrierState value);
std::string to_string(BarrierMode value);
std::string to_string(CommandStatus value);
std::string to_string(OperationDirection value);
std::string to_string(OperationPhase value);
std::optional<Action> action_from_string(std::string_view value);
std::optional<BarrierState> barrier_state_from_string(std::string_view value);
std::optional<BarrierMode> barrier_mode_from_string(std::string_view value);
std::optional<CommandStatus> command_status_from_string(
    std::string_view value);
std::optional<OperationDirection> operation_direction_from_string(
    std::string_view value);
std::optional<OperationPhase> operation_phase_from_string(
    std::string_view value);
bool terminal(CommandStatus value);
bool is_actuation_action(Action value);
bool is_persisted_physical_event(std::string_view value);

void to_json(json& target, const Envelope& value);
void from_json(const json& source, Envelope& value);
void to_json(json& target, const CommandRequest& value);
void from_json(const json& source, CommandRequest& value);
void to_json(json& target, const GateOperation& value);
void from_json(const json& source, GateOperation& value);

Envelope make_envelope(
    std::string type,
    json payload,
    std::string boot_id = {},
    std::string revision = {},
    std::uint64_t sequence = 0);

std::optional<std::string> validate_envelope(const Envelope& envelope);
std::optional<std::string> validate_command(
    const CommandRequest& command,
    std::int64_t current_us);

}  // namespace portfolio::gate
