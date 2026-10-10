#include "portfolio/gate/coordinator_core.hpp"

#include <algorithm>
#include <stdexcept>

namespace portfolio::gate {
namespace {

BarrierState payload_state(const json& payload) {
    return barrier_state_from_string(
               payload.value("state", "unknown"))
        .value_or(BarrierState::unknown);
}

BarrierMode payload_mode(const json& payload) {
    return barrier_mode_from_string(
               payload.value("mode", "normal"))
        .value_or(BarrierMode::normal);
}

bool active_phase(OperationPhase phase) {
    return phase == OperationPhase::opening_requested ||
           phase == OperationPhase::opening ||
           phase == OperationPhase::closing_requested ||
           phase == OperationPhase::closing;
}

bool action_valid(
    Action action,
    BarrierState state,
    BarrierMode mode) {
    switch (action) {
        case Action::open:
            return mode == BarrierMode::normal &&
                   (state == BarrierState::closed ||
                    state == BarrierState::closing);
        case Action::close_request:
            return state == BarrierState::open &&
                   mode == BarrierMode::normal;
        case Action::hold_enable:
            return state == BarrierState::closed &&
                   mode == BarrierMode::normal;
        case Action::hold_disable:
            return state == BarrierState::open &&
                   mode == BarrierMode::hold_open;
    }
    return false;
}

}  // namespace

CoordinatorCore::CoordinatorCore(
    CoordinatorLimits limits,
    CoordinatorTiming timing)
    : limits_(limits), timing_(timing) {}

void CoordinatorCore::set_timing(CoordinatorTiming timing) {
    if (timing.pi_result_fallback_timeout_ms < 500 ||
        timing.pi_result_fallback_timeout_ms > 10000 ||
        timing.refresh_timeout_ms < 500 ||
        timing.refresh_timeout_ms > 5000) {
        throw std::invalid_argument("invalid coordinator timing");
    }
    std::lock_guard lock(mutex_);
    timing_ = timing;
}

void CoordinatorCore::set_mqtt_connected(
    bool value,
    std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    state_.mqtt_connected = value;
    if (!value && transport_command_id_) {
        auto& record = commands_.at(*transport_command_id_);
        record.status = CommandStatus::result_unknown;
        record.reason = "coordinator_transport_lost";
        record.updated_at_us = current_us;
        publish_locked("gate-command", command_record_json(record));
        transport_command_id_.reset();
    }
    recompute_ready_locked();
    publish_locked(
        "gate-connectivity",
        {{"mqtt_connected", value}, {"at_us", current_us}});
}

bool CoordinatorCore::remember_event_id_locked(std::string event_id) {
    if (seen_event_ids_.contains(event_id)) {
        return false;
    }
    seen_event_ids_.insert(event_id);
    seen_event_order_.push_back(std::move(event_id));
    while (seen_event_order_.size() > limits_.seen_event_ids) {
        seen_event_ids_.erase(seen_event_order_.front());
        seen_event_order_.pop_front();
    }
    return true;
}

bool CoordinatorCore::observe_edge_status(
    const Envelope& envelope,
    std::int64_t current_us) {
    if (validate_envelope(envelope)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!remember_event_id_locked(envelope.event_id)) {
        return false;
    }
    state_.edge_online = envelope.payload.value("online", false);
    if (!envelope.boot_id.empty() &&
        !state_.boot_id.empty() &&
        envelope.boot_id != state_.boot_id) {
        state_.barrier = BarrierState::unknown;
        state_.verified = false;
        state_.sync_status = "boot_changed";
        if (operation_ && active_phase(operation_->phase)) {
            operation_->phase = OperationPhase::failed;
            operation_->terminal_reason = "a6_boot_changed";
            operation_->updated_at_us = current_us;
            publish_locked("gate-operation", json(*operation_));
        }
    }
    recompute_ready_locked();
    publish_locked(
        "gate-connectivity",
        {{"edge_online", state_.edge_online}, {"at_us", current_us}});
    return true;
}

void CoordinatorCore::apply_state_payload_locked(
    const Envelope& envelope,
    bool verified_by_sync,
    std::int64_t current_us) {
    if (!envelope.boot_id.empty()) {
        state_.boot_id = envelope.boot_id;
    }
    if (!envelope.revision.empty()) {
        state_.revision = envelope.revision;
    }
    state_.sequence = envelope.sequence;
    state_.barrier = payload_state(envelope.payload);
    state_.mode = payload_mode(envelope.payload);
    state_.state_at_us = envelope.occurred_at_us > 0
                             ? envelope.occurred_at_us
                             : current_us;
    if (verified_by_sync) {
        state_.verified = true;
        state_.verified_at_us = current_us;
        state_.restored_unverified = false;
        state_.sync_status = "verified";
    } else {
        state_.sync_status = "live";
    }
    recompute_ready_locked();
}

bool CoordinatorCore::observe_state(
    const Envelope& envelope,
    bool verified_by_sync,
    std::int64_t current_us) {
    if (validate_envelope(envelope)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!remember_event_id_locked(envelope.event_id)) {
        return false;
    }
    if (envelope.boot_id == state_.boot_id &&
        envelope.sequence != 0 && state_.sequence != 0 &&
        envelope.sequence <= state_.sequence) {
        return false;
    }
    if (envelope.boot_id == state_.boot_id &&
        state_.sequence != 0 &&
        envelope.sequence > state_.sequence + 1) {
        state_.sync_status = "live_with_sequence_gap";
    }
    apply_state_payload_locked(
        envelope, verified_by_sync, current_us);
    publish_locked("gate-state", public_state_json(state_));
    return true;
}

bool CoordinatorCore::observe_command_status(
    const Envelope& envelope,
    std::int64_t current_us) {
    if (validate_envelope(envelope)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!remember_event_id_locked(envelope.event_id)) {
        return false;
    }
    const std::string command_id =
        envelope.payload.value("command_id", "");
    const auto status = command_status_from_string(
        envelope.payload.value("status", ""));
    const auto action = action_from_string(
        envelope.payload.value("action", ""));
    if (command_id.empty() || !status || !action) {
        return false;
    }
    auto found = commands_.find(command_id);
    if (found == commands_.end()) {
        CommandRecord external;
        external.command.command_id = command_id;
        external.command.idempotency_key =
            envelope.payload.value("idempotency_key", command_id);
        external.command.action = *action;
        external.command.actor_id = "direct-device";
        external.command.source =
            envelope.payload.value("source", "direct");
        external.command.requested_at_us = current_us;
        external.command.expires_at_us =
            envelope.payload.value(
                "expires_at_us", current_us + 5'000'000);
        external.command.target_boot_id = envelope.boot_id;
        external.command.expected_state_revision = envelope.revision;
        external.status = *status;
        external.reason = envelope.payload.value("reason", "");
        external.accepted_at_us = current_us;
        external.updated_at_us = current_us;
        external.web_owned = false;
        external.joined =
            envelope.payload.value("joined", false);
        remember_command_locked(external);
        found = commands_.find(command_id);
    } else {
        found->second.status = *status;
        found->second.reason = envelope.payload.value("reason", "");
        found->second.updated_at_us = current_us;
        found->second.joined =
            envelope.payload.value("joined", found->second.joined);
    }
    if (envelope.payload.contains("operation") &&
        envelope.payload["operation"].is_object()) {
        try {
            operation_ =
                envelope.payload["operation"].get<GateOperation>();
            publish_locked("gate-operation", json(*operation_));
        } catch (const std::exception&) {
        }
    }
    if (!terminal(found->second.status)) {
        transport_command_id_ = command_id;
    } else {
        release_transport_if_terminal_locked(found->second);
    }
    publish_locked("gate-command", command_record_json(found->second));
    return true;
}

void CoordinatorCore::update_operation_from_physical_locked(
    std::string_view event,
    std::int64_t current_us) {
    const auto begin_observed =
        [&](OperationDirection direction, OperationPhase phase) {
            if (operation_ && active_phase(operation_->phase) &&
                operation_->direction == direction) {
                operation_->phase = phase;
                operation_->observed = true;
                operation_->updated_at_us = current_us;
            } else {
                operation_ = GateOperation{
                    make_id("op_observed"),
                    direction,
                    phase,
                    state_.boot_id,
                    current_us,
                    current_us,
                    {},
                    true,
                };
            }
        };
    if (event == "gate_up_action") {
        begin_observed(
            OperationDirection::opening, OperationPhase::opening);
    } else if (event == "gate_down_action") {
        begin_observed(
            OperationDirection::closing, OperationPhase::closing);
    } else if (event == "gate_up_ok" || event == "uplock") {
        if (!operation_ ||
            operation_->direction != OperationDirection::opening) {
            begin_observed(
                OperationDirection::opening,
                OperationPhase::opening);
        }
        operation_->phase = OperationPhase::terminal;
        operation_->terminal_reason = "open";
        operation_->updated_at_us = current_us;
    } else if (event == "gate_down_ok") {
        if (!operation_ ||
            operation_->direction != OperationDirection::closing) {
            begin_observed(
                OperationDirection::closing,
                OperationPhase::closing);
        }
        operation_->phase = OperationPhase::terminal;
        operation_->terminal_reason = "closed";
        operation_->updated_at_us = current_us;
    } else if (event == "gate_error" && operation_) {
        operation_->phase = OperationPhase::failed;
        operation_->terminal_reason = "controller_error";
        operation_->updated_at_us = current_us;
    }
}

bool CoordinatorCore::observe_physical_event(
    const Envelope& envelope,
    std::int64_t current_us) {
    if (validate_envelope(envelope)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!remember_event_id_locked(envelope.event_id)) {
        return false;
    }
    const std::string event =
        envelope.payload.value("event", "");
    if (!is_persisted_physical_event(event)) {
        return false;
    }
    json record = envelope.payload;
    record.erase("command_id");
    record.erase("operation_id");
    record["event_id"] = envelope.event_id;
    record["occurred_at_us"] = envelope.occurred_at_us;
    record["boot_id"] = envelope.boot_id;
    record["revision"] = envelope.revision;
    record["sequence"] = envelope.sequence;
    record["received_at_us"] = current_us;
    physical_events_.push_back(record);
    while (physical_events_.size() > limits_.physical_history) {
        physical_events_.pop_front();
    }
    if (!envelope.boot_id.empty()) {
        state_.boot_id = envelope.boot_id;
    }
    if (!envelope.revision.empty()) {
        state_.revision = envelope.revision;
    }
    if (envelope.sequence != 0) {
        state_.sequence = envelope.sequence;
    }
    if (event == "gate_up_action") {
        state_.barrier = BarrierState::opening;
    } else if (event == "gate_up_ok" || event == "uplock") {
        state_.barrier = BarrierState::open;
    } else if (event == "gate_down_action") {
        state_.barrier = BarrierState::closing;
    } else if (event == "gate_down_ok") {
        state_.barrier = BarrierState::closed;
    } else if (event == "gate_error") {
        state_.barrier = BarrierState::fault;
    }
    if (event == "uplock") {
        state_.mode = BarrierMode::hold_open;
    } else if (event == "unlock") {
        state_.mode = BarrierMode::normal;
    }
    state_.state_at_us = envelope.occurred_at_us;
    update_operation_from_physical_locked(event, current_us);
    recompute_ready_locked();
    publish_locked("gate-physical", record);
    publish_locked("gate-state", public_state_json(state_));
    if (operation_) {
        publish_locked("gate-operation", json(*operation_));
    }
    return true;
}

bool CoordinatorCore::observe_operation(
    const Envelope& envelope,
    std::int64_t) {
    if (validate_envelope(envelope) ||
        !envelope.payload.is_object()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!remember_event_id_locked(envelope.event_id)) {
        return false;
    }
    try {
        GateOperation incoming =
            envelope.payload.get<GateOperation>();
        if (!state_.boot_id.empty() &&
            !incoming.boot_id.empty() &&
            incoming.boot_id != state_.boot_id) {
            return false;
        }
        operation_ = std::move(incoming);
        publish_locked("gate-operation", json(*operation_));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool CoordinatorCore::observe_sync_response(
    const Envelope& envelope,
    std::int64_t current_us) {
    if (validate_envelope(envelope)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!remember_event_id_locked(envelope.event_id)) {
        return false;
    }
    const std::string request_id =
        envelope.payload.value("request_id", "");
    if (!sync_request_id_ || request_id != *sync_request_id_) {
        return false;
    }
    const bool ok =
        envelope.payload.value("status", "") == "verified";
    if (ok && envelope.payload.contains("state") &&
        envelope.payload["state"].is_object()) {
        Envelope state_envelope = envelope;
        state_envelope.payload = envelope.payload["state"];
        apply_state_payload_locked(
            state_envelope, true, current_us);
        ++status_aggregate_.succeeded;
        status_aggregate_.consecutive_failures = 0;
        status_aggregate_.last_succeeded_at_us = current_us;
    } else {
        state_.sync_status = "refresh_failed";
        ++status_aggregate_.failed;
        ++status_aggregate_.consecutive_failures;
    }
    status_aggregate_.last_rtt_us =
        std::max<std::int64_t>(0, current_us - sync_started_at_us_);
    sync_request_id_.reset();
    recompute_ready_locked();
    publish_locked(
        "gate-refresh",
        {
            {"refresh_id", request_id},
            {"status", ok ? "verified" : "failed"},
            {"completed_at_us", current_us},
        });
    publish_locked("gate-state", public_state_json(state_));
    return true;
}

std::optional<Envelope> CoordinatorCore::observer_connected(
    std::int64_t) {
    return std::nullopt;
}

std::optional<Envelope> CoordinatorCore::take_pending_sync(
    std::int64_t) {
    return std::nullopt;
}

std::optional<Envelope> CoordinatorCore::request_manual_refresh(
    std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    if (sync_request_id_) {
        return std::nullopt;
    }
    if (!state_.mqtt_connected || !state_.edge_online) {
        return std::nullopt;
    }
    sync_request_id_ = make_id("refresh");
    sync_started_at_us_ = current_us;
    state_.sync_status = "refreshing";
    ++status_aggregate_.requested;
    status_aggregate_.last_requested_at_us = current_us;
    return make_envelope(
        "sync.request",
        {
            {"request_id", *sync_request_id_},
            {"reason", "manual_refresh"},
        },
        state_.boot_id,
        state_.revision,
        state_.sequence);
}

bool CoordinatorCore::joins_operation_locked(Action action) const {
    if (!operation_ || !active_phase(operation_->phase)) {
        return false;
    }
    return (action == Action::open &&
            operation_->direction == OperationDirection::opening) ||
           (action == Action::close_request &&
            operation_->direction == OperationDirection::closing);
}

CommandResult CoordinatorCore::reject_locked(std::string reason) const {
    CommandResult result;
    result.status = CommandStatus::rejected;
    result.reason = std::move(reason);
    result.operation = operation_;
    return result;
}

CommandResult CoordinatorCore::request_web_command(
    Action action,
    std::string actor_id,
    std::string idempotency_key,
    int ttl_ms,
    std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    const auto known = idempotency_to_command_.find(idempotency_key);
    if (known != idempotency_to_command_.end()) {
        if (const auto found = commands_.find(known->second);
            found != commands_.end()) {
            CommandResult replay;
            replay.accepted =
                found->second.status != CommandStatus::rejected;
            replay.status = found->second.status;
            replay.reason = found->second.reason;
            replay.request = found->second.command;
            replay.operation = operation_;
            replay.joined = found->second.joined;
            return replay;
        }
    }
    if (!state_.ready) {
        return reject_locked("gate_not_ready");
    }
    if (action == Action::close_request &&
        state_.mode == BarrierMode::hold_open) {
        return reject_locked("hold_release_required");
    }
    if (joins_operation_locked(action)) {
        CommandResult joined;
        joined.accepted = true;
        joined.status = CommandStatus::acknowledged;
        joined.reason = "joined_operation";
        joined.operation = operation_;
        joined.joined = true;
        return joined;
    }
    if (operation_ && active_phase(operation_->phase) &&
        operation_->direction == OperationDirection::opening &&
        action == Action::close_request) {
        return reject_locked("opening_in_progress");
    }
    if (transport_command_id_) {
        return reject_locked("command_in_progress");
    }
    if (!action_valid(action, state_.barrier, state_.mode)) {
        return reject_locked("action_not_valid_for_current_state");
    }
    if (ttl_ms < 500 || ttl_ms > 30000 ||
        actor_id.empty() || idempotency_key.empty()) {
        return reject_locked("invalid_request");
    }

    CommandRequest command;
    command.command_id = make_id("cmd");
    command.idempotency_key = std::move(idempotency_key);
    command.action = action;
    command.actor_id = std::move(actor_id);
    command.source = "web";
    command.requested_at_us = current_us;
    command.expires_at_us =
        current_us + static_cast<std::int64_t>(ttl_ms) * 1000;
    command.target_boot_id = state_.boot_id;
    command.expected_state_revision = state_.revision;
    command.ttl_ms = ttl_ms;

    CommandRecord record;
    record.command = command;
    record.status = CommandStatus::accepted;
    record.accepted_at_us = current_us;
    record.updated_at_us = current_us;
    record.fallback_deadline_us =
        current_us +
        static_cast<std::int64_t>(
            timing_.pi_result_fallback_timeout_ms) *
            1000;
    record.web_owned = true;
    remember_command_locked(record);
    transport_command_id_ = command.command_id;
    idempotency_to_command_[command.idempotency_key] =
        command.command_id;
    idempotency_order_.push_back(command.idempotency_key);
    while (idempotency_order_.size() > limits_.idempotency_keys) {
        idempotency_to_command_.erase(idempotency_order_.front());
        idempotency_order_.pop_front();
    }
    if (action == Action::open ||
        action == Action::close_request ||
        action == Action::hold_enable) {
        const auto direction =
            action == Action::close_request
                ? OperationDirection::closing
                : OperationDirection::opening;
        operation_ = GateOperation{
            command.command_id,
            direction,
            direction == OperationDirection::opening
                ? OperationPhase::opening_requested
                : OperationPhase::closing_requested,
            state_.boot_id,
            current_us,
            current_us,
            {},
            false,
        };
        publish_locked("gate-operation", json(*operation_));
    }
    publish_locked("gate-command", command_record_json(record));

    CommandResult result;
    result.accepted = true;
    result.status = CommandStatus::accepted;
    result.request = command;
    result.operation = operation_;
    result.a6_tx_created = true;
    return result;
}

void CoordinatorCore::tick(std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    if (sync_request_id_ &&
        current_us - sync_started_at_us_ >
            static_cast<std::int64_t>(timing_.refresh_timeout_ms) *
                1000) {
        const std::string id = *sync_request_id_;
        sync_request_id_.reset();
        state_.sync_status = "refresh_timeout";
        ++status_aggregate_.failed;
        ++status_aggregate_.consecutive_failures;
        publish_locked(
            "gate-refresh",
            {
                {"refresh_id", id},
                {"status", "timeout"},
                {"completed_at_us", current_us},
            });
    }
    if (transport_command_id_) {
        auto found = commands_.find(*transport_command_id_);
        if (found != commands_.end() &&
            found->second.fallback_deadline_us <= current_us &&
            !terminal(found->second.status)) {
            found->second.status = CommandStatus::result_unknown;
            found->second.reason = "pi_result_timeout";
            found->second.updated_at_us = current_us;
            const auto record = found->second;
            transport_command_id_.reset();
            publish_locked(
                "gate-command", command_record_json(record));
        }
    }
    recompute_ready_locked();
}

CoordinatorState CoordinatorCore::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

std::optional<CommandRecord> CoordinatorCore::command(
    std::string_view command_id) const {
    std::lock_guard lock(mutex_);
    const auto found = commands_.find(std::string(command_id));
    return found == commands_.end()
               ? std::nullopt
               : std::optional<CommandRecord>(found->second);
}

std::vector<CommandRecord> CoordinatorCore::recent_commands() const {
    std::lock_guard lock(mutex_);
    std::vector<CommandRecord> result;
    for (const auto& id : command_order_) {
        if (const auto found = commands_.find(id);
            found != commands_.end()) {
            result.push_back(found->second);
        }
    }
    return result;
}

std::vector<json> CoordinatorCore::recent_physical_events() const {
    std::lock_guard lock(mutex_);
    return {physical_events_.begin(), physical_events_.end()};
}

StatusAggregate CoordinatorCore::status_aggregate() const {
    std::lock_guard lock(mutex_);
    return status_aggregate_;
}

std::optional<GateOperation> CoordinatorCore::operation() const {
    std::lock_guard lock(mutex_);
    return operation_;
}

std::vector<SseEvent> CoordinatorCore::events_after(
    std::uint64_t cursor,
    std::size_t limit) const {
    std::lock_guard lock(mutex_);
    std::vector<SseEvent> result;
    for (const auto& event : sse_events_) {
        if (event.cursor > cursor) {
            result.push_back(event);
            if (result.size() >= limit) {
                break;
            }
        }
    }
    return result;
}

std::uint64_t CoordinatorCore::event_cursor() const {
    std::lock_guard lock(mutex_);
    return event_cursor_;
}

void CoordinatorCore::publish_locked(std::string name, json data) {
    sse_events_.push_back(
        {++event_cursor_, std::move(name), std::move(data)});
    while (sse_events_.size() > limits_.sse_events) {
        sse_events_.pop_front();
    }
}

void CoordinatorCore::recompute_ready_locked() {
    state_.ready =
        state_.mqtt_connected && state_.edge_online &&
        state_.barrier != BarrierState::unknown &&
        state_.barrier != BarrierState::fault &&
        !state_.boot_id.empty() && !state_.revision.empty();
}

void CoordinatorCore::release_transport_if_terminal_locked(
    const CommandRecord& record) {
    if (terminal(record.status) && transport_command_id_ &&
        *transport_command_id_ == record.command.command_id) {
        transport_command_id_.reset();
    }
}

void CoordinatorCore::remember_command_locked(
    const CommandRecord& record) {
    const bool inserted =
        !commands_.contains(record.command.command_id);
    commands_[record.command.command_id] = record;
    if (inserted) {
        command_order_.push_back(record.command.command_id);
    }
    while (command_order_.size() > limits_.command_history) {
        const std::string removed = command_order_.front();
        command_order_.pop_front();
        commands_.erase(removed);
    }
}

json CoordinatorCore::checkpoint() const {
    std::lock_guard lock(mutex_);
    json command_rows = json::array();
    for (const auto& id : command_order_) {
        if (const auto found = commands_.find(id);
            found != commands_.end()) {
            command_rows.push_back(command_record_json(found->second));
        }
    }
    return {
        {"schema_version", 2},
        {"written_at_us", now_us()},
        {"state", public_state_json(state_)},
        {"operation", operation_ ? json(*operation_) : json(nullptr)},
        {"commands", command_rows},
        {"physical_events", physical_events_},
        {"status_aggregate",
         {
             {"requested", status_aggregate_.requested},
             {"succeeded", status_aggregate_.succeeded},
             {"failed", status_aggregate_.failed},
             {"consecutive_failures",
              status_aggregate_.consecutive_failures},
             {"last_requested_at_us",
              status_aggregate_.last_requested_at_us},
             {"last_succeeded_at_us",
              status_aggregate_.last_succeeded_at_us},
             {"last_rtt_us", status_aggregate_.last_rtt_us},
         }},
    };
}

void CoordinatorCore::restore_checkpoint(const json& checkpoint) {
    if (!checkpoint.is_object()) {
        throw std::invalid_argument("unsupported checkpoint");
    }
    const int version = checkpoint.value("schema_version", 0);
    if (version != 1 && version != 2) {
        throw std::invalid_argument("unsupported checkpoint");
    }
    std::lock_guard lock(mutex_);
    const json state = checkpoint.value("state", json::object());
    state_.barrier = payload_state(state);
    state_.mode = payload_mode(state);
    state_.boot_id = state.value("boot_id", "");
    state_.revision = state.value("revision", "");
    state_.sequence = state.value("sequence", std::uint64_t{0});
    state_.state_at_us = state.value("state_at_us", std::int64_t{0});
    state_.mqtt_connected = false;
    state_.edge_online = false;
    state_.verified = false;
    state_.ready = false;
    state_.restored_unverified = true;
    state_.sync_status = "restored";
    operation_.reset();
    if (version == 2 && checkpoint.contains("operation") &&
        checkpoint["operation"].is_object()) {
        try {
            operation_ =
                checkpoint["operation"].get<GateOperation>();
            if (active_phase(operation_->phase)) {
                operation_->observed = true;
            }
        } catch (const std::exception&) {
            operation_.reset();
        }
    }
    commands_.clear();
    command_order_.clear();
    idempotency_to_command_.clear();
    idempotency_order_.clear();
    transport_command_id_.reset();
    for (const auto& item :
         checkpoint.value("commands", json::array())) {
        if (!item.is_object() ||
            !item.contains("command") ||
            !item["command"].is_object()) {
            continue;
        }
        try {
            CommandRecord record;
            record.command =
                item.at("command").get<CommandRequest>();
            record.status = command_status_from_string(
                                item.value(
                                    "status", "result_unknown"))
                                .value_or(
                                    CommandStatus::result_unknown);
            record.reason = item.value("reason", "");
            record.accepted_at_us =
                item.value("accepted_at_us", std::int64_t{0});
            record.updated_at_us =
                item.value("updated_at_us", std::int64_t{0});
            record.fallback_deadline_us = 0;
            record.web_owned = item.value("web_owned", false);
            record.joined = item.value("joined", false);
            remember_command_locked(record);
            idempotency_to_command_[record.command.idempotency_key] =
                record.command.command_id;
            idempotency_order_.push_back(
                record.command.idempotency_key);
        } catch (const std::exception&) {
        }
    }
    physical_events_.clear();
    for (const auto& item :
         checkpoint.value("physical_events", json::array())) {
        if (is_persisted_physical_event(
                item.value("event", ""))) {
            physical_events_.push_back(item);
        }
    }
    while (physical_events_.size() > limits_.physical_history) {
        physical_events_.pop_front();
    }
    const json aggregate =
        checkpoint.value("status_aggregate", json::object());
    status_aggregate_.requested =
        aggregate.value("requested", std::uint64_t{0});
    status_aggregate_.succeeded =
        aggregate.value("succeeded", std::uint64_t{0});
    status_aggregate_.failed =
        aggregate.value("failed", std::uint64_t{0});
    status_aggregate_.consecutive_failures =
        aggregate.value(
            "consecutive_failures", std::uint64_t{0});
    status_aggregate_.last_requested_at_us =
        aggregate.value("last_requested_at_us", std::int64_t{0});
    status_aggregate_.last_succeeded_at_us =
        aggregate.value("last_succeeded_at_us", std::int64_t{0});
    status_aggregate_.last_rtt_us =
        aggregate.value("last_rtt_us", std::int64_t{0});
}

json public_state_json(const CoordinatorState& state) {
    return {
        {"site_id", kSiteId},
        {"gate_id", kGateId},
        {"mqtt_connected", state.mqtt_connected},
        {"edge_online", state.edge_online},
        {"verified", state.verified},
        {"ready", state.ready},
        {"restored_unverified", state.restored_unverified},
        {"state", to_string(state.barrier)},
        {"mode", to_string(state.mode)},
        {"boot_id", state.boot_id},
        {"revision", state.revision},
        {"sequence", state.sequence},
        {"state_at_us", state.state_at_us},
        {"verified_at_us", state.verified_at_us},
        {"sync_status", state.sync_status},
    };
}

json command_record_json(const CommandRecord& record) {
    return {
        {"command", record.command},
        {"status", to_string(record.status)},
        {"reason", record.reason},
        {"accepted_at_us", record.accepted_at_us},
        {"updated_at_us", record.updated_at_us},
        {"fallback_deadline_us", record.fallback_deadline_us},
        {"web_owned", record.web_owned},
        {"joined", record.joined},
    };
}

}  // namespace portfolio::gate
