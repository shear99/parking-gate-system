#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "portfolio/gate/a6_codec.hpp"
#include "portfolio/gate/protocol.hpp"

namespace portfolio::gate {

struct AgentLimits {
    std::size_t command_history = 512;
    std::size_t idempotency_keys = 4096;
    std::size_t rs232_records = 2048;
    std::size_t physical_events = 1024;
};

struct AgentState {
    bool commands_enabled = true;
    bool explicit_close_enabled = true;
    bool a6_online = false;
    bool mqtt_connected = false;
    bool fresh = false;
    BarrierState barrier = BarrierState::unknown;
    BarrierMode mode = BarrierMode::normal;
    std::string boot_id;
    std::string revision;
    std::uint64_t sequence = 0;
    std::int64_t a6_uptime_ms = -1;
    std::int64_t a6_observed_at_us = 0;
    std::int64_t state_at_us = 0;
};

struct AgentCommandRecord {
    CommandRequest command;
    CommandStatus status = CommandStatus::pi_received;
    std::string reason;
    std::int64_t updated_at_us = 0;
    std::int64_t ack_deadline_us = 0;
};

struct AgentDecision {
    CommandResult result;
    std::optional<nlohmann::json> a6_envelope;
};

struct Rs232Record {
    std::int64_t at_us = 0;
    std::string direction;
    std::string kind;
    std::string value;
    std::string command_id;
};

struct AgentStatusAggregate {
    std::uint64_t requested = 0;
    std::uint64_t succeeded = 0;
    std::uint64_t failed = 0;
    std::uint64_t consecutive_failures = 0;
    std::int64_t last_requested_at_us = 0;
    std::int64_t last_succeeded_at_us = 0;
    std::int64_t last_rtt_us = 0;
};

class AgentCore {
public:
    explicit AgentCore(AgentLimits limits = {});

    void set_transport(
        bool mqtt_connected,
        bool a6_online,
        std::string boot_id,
        std::int64_t a6_uptime_ms,
        std::int64_t current_us);
    void set_policy(bool commands_enabled, bool explicit_close_enabled);
    void set_ack_timeout_ms(int value);
    AgentDecision submit(
        CommandRequest command,
        std::int64_t current_us);
    std::optional<AgentCommandRecord> mark_sent(
        std::string_view command_id,
        std::int64_t current_us);
    std::optional<AgentCommandRecord> on_a6_ack(
        std::string_view command_id,
        bool accepted,
        std::string reason,
        std::int64_t current_us);
    std::vector<nlohmann::json> on_rs232_frame(
        std::string_view raw,
        std::int64_t current_us);
    void status_requested(std::int64_t current_us);
    void status_completed(
        bool success,
        std::int64_t rtt_us,
        std::int64_t current_us);
    void tick(std::int64_t current_us);

    AgentState state() const;
    std::optional<AgentCommandRecord> command(
        std::string_view command_id) const;
    std::vector<AgentCommandRecord> recent_commands() const;
    std::vector<nlohmann::json> recent_physical_events() const;
    std::vector<Rs232Record> recent_rs232() const;
    AgentStatusAggregate status_aggregate() const;
    std::optional<GateOperation> operation() const;
    nlohmann::json public_state() const;
    nlohmann::json capabilities() const;

private:
    CommandResult reject_locked(std::string reason) const;
    bool action_valid_locked(Action action) const;
    void update_revision_locked();
    void remember_command_locked(const AgentCommandRecord& record);
    void complete_transport_locked(
        CommandStatus status,
        std::string reason,
        std::int64_t current_us);
    void start_observed_operation_locked(
        OperationDirection direction,
        OperationPhase phase,
        std::int64_t current_us);
    void update_operation_from_physical_locked(
        std::string_view event,
        std::int64_t current_us);
    bool joins_operation_locked(Action action) const;
    void push_rs232_locked(Rs232Record record);
    nlohmann::json physical_event_locked(
        std::string event,
        std::int64_t current_us,
        std::string command_id = {});

    AgentLimits limits_;
    mutable std::mutex mutex_;
    AgentState state_;
    AgentStatusAggregate status_aggregate_;
    int ack_timeout_ms_ = 2000;
    std::deque<std::string> command_order_;
    std::unordered_map<std::string, AgentCommandRecord> commands_;
    std::deque<std::string> idempotency_order_;
    std::unordered_map<std::string, std::string> idempotency_to_command_;
    std::optional<std::string> transport_command_id_;
    std::optional<GateOperation> operation_;
    std::deque<nlohmann::json> physical_events_;
    std::deque<Rs232Record> rs232_;
};

nlohmann::json agent_command_json(const AgentCommandRecord& value);

}  // namespace portfolio::gate
