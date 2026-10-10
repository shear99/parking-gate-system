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

#include "portfolio/gate/protocol.hpp"

namespace portfolio::gate {

struct CoordinatorLimits {
    std::size_t command_history = 256;
    std::size_t physical_history = 512;
    std::size_t idempotency_keys = 2048;
    std::size_t seen_event_ids = 4096;
    std::size_t sse_events = 4096;
};

struct CoordinatorTiming {
    int pi_result_fallback_timeout_ms = 3000;
    int refresh_timeout_ms = 2000;
};

struct StatusAggregate {
    std::uint64_t requested = 0;
    std::uint64_t succeeded = 0;
    std::uint64_t failed = 0;
    std::uint64_t consecutive_failures = 0;
    std::int64_t last_requested_at_us = 0;
    std::int64_t last_succeeded_at_us = 0;
    std::int64_t last_rtt_us = 0;
};

struct CoordinatorState {
    bool mqtt_connected = false;
    bool edge_online = false;
    bool verified = false;
    bool ready = false;
    bool restored_unverified = false;
    BarrierState barrier = BarrierState::unknown;
    BarrierMode mode = BarrierMode::normal;
    std::string boot_id;
    std::string revision;
    std::uint64_t sequence = 0;
    std::int64_t state_at_us = 0;
    std::int64_t verified_at_us = 0;
    std::string sync_status = "starting";
};

struct CommandRecord {
    CommandRequest command;
    CommandStatus status = CommandStatus::accepted;
    std::string reason;
    std::int64_t accepted_at_us = 0;
    std::int64_t updated_at_us = 0;
    std::int64_t fallback_deadline_us = 0;
    bool web_owned = false;
    bool joined = false;
};

struct SseEvent {
    std::uint64_t cursor = 0;
    std::string name;
    json data = json::object();
};

class CoordinatorCore {
public:
    explicit CoordinatorCore(
        CoordinatorLimits limits = {},
        CoordinatorTiming timing = {});

    void set_mqtt_connected(bool value, std::int64_t current_us);
    bool observe_edge_status(
        const Envelope& envelope,
        std::int64_t current_us);
    bool observe_state(
        const Envelope& envelope,
        bool verified_by_sync,
        std::int64_t current_us);
    bool observe_command_status(
        const Envelope& envelope,
        std::int64_t current_us);
    bool observe_physical_event(
        const Envelope& envelope,
        std::int64_t current_us);
    bool observe_operation(
        const Envelope& envelope,
        std::int64_t current_us);
    bool observe_sync_response(
        const Envelope& envelope,
        std::int64_t current_us);

    std::optional<Envelope> observer_connected(std::int64_t current_us);
    std::optional<Envelope> take_pending_sync(
        std::int64_t current_us);
    std::optional<Envelope> request_manual_refresh(
        std::int64_t current_us);
    void set_timing(CoordinatorTiming timing);
    CommandResult request_web_command(
        Action action,
        std::string actor_id,
        std::string idempotency_key,
        int ttl_ms,
        std::int64_t current_us);
    void tick(std::int64_t current_us);

    CoordinatorState state() const;
    std::optional<CommandRecord> command(std::string_view command_id) const;
    std::vector<CommandRecord> recent_commands() const;
    std::vector<json> recent_physical_events() const;
    StatusAggregate status_aggregate() const;
    std::optional<GateOperation> operation() const;
    std::vector<SseEvent> events_after(
        std::uint64_t cursor,
        std::size_t limit = 64) const;
    std::uint64_t event_cursor() const;
    json checkpoint() const;
    void restore_checkpoint(const json& checkpoint);

private:
    bool remember_event_id_locked(std::string event_id);
    void publish_locked(std::string name, json data);
    void recompute_ready_locked();
    void release_transport_if_terminal_locked(const CommandRecord& record);
    void remember_command_locked(const CommandRecord& record);
    void apply_state_payload_locked(
        const Envelope& envelope,
        bool verified_by_sync,
        std::int64_t current_us);
    CommandResult reject_locked(std::string reason) const;
    bool joins_operation_locked(Action action) const;
    void update_operation_from_physical_locked(
        std::string_view event,
        std::int64_t current_us);

    CoordinatorLimits limits_;
    CoordinatorTiming timing_;
    mutable std::mutex mutex_;
    CoordinatorState state_;
    StatusAggregate status_aggregate_;
    std::deque<std::string> seen_event_order_;
    std::unordered_set<std::string> seen_event_ids_;
    std::deque<std::string> idempotency_order_;
    std::unordered_map<std::string, std::string> idempotency_to_command_;
    std::unordered_map<std::string, CommandRecord> commands_;
    std::deque<std::string> command_order_;
    std::deque<json> physical_events_;
    std::optional<std::string> transport_command_id_;
    std::optional<GateOperation> operation_;
    std::optional<std::string> sync_request_id_;
    std::int64_t sync_started_at_us_ = 0;
    std::uint64_t event_cursor_ = 0;
    std::deque<SseEvent> sse_events_;
};

json public_state_json(const CoordinatorState& state);
json command_record_json(const CommandRecord& record);

}  // namespace portfolio::gate
