#include "portfolio/gate/agent_core.hpp"

#include <algorithm>

namespace portfolio::gate {
namespace {

bool active_phase(OperationPhase phase) {
    return phase == OperationPhase::opening_requested ||
           phase == OperationPhase::opening ||
           phase == OperationPhase::closing_requested ||
           phase == OperationPhase::closing;
}

OperationDirection direction_for(Action action) {
    return action == Action::close_request
               ? OperationDirection::closing
               : OperationDirection::opening;
}

OperationPhase requested_phase(OperationDirection direction) {
    return direction == OperationDirection::opening
               ? OperationPhase::opening_requested
               : OperationPhase::closing_requested;
}

}  // namespace

AgentCore::AgentCore(AgentLimits limits)
    : limits_(limits) {}

void AgentCore::set_transport(
    bool mqtt_connected,
    bool a6_online,
    std::string boot_id,
    std::int64_t a6_uptime_ms,
    std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    const bool boot_changed =
        !state_.boot_id.empty() && !boot_id.empty() &&
        state_.boot_id != boot_id;
    const bool transport_lost =
        (state_.mqtt_connected && !mqtt_connected) ||
        (state_.a6_online && !a6_online);
    if ((boot_changed || transport_lost) && transport_command_id_) {
        complete_transport_locked(
            CommandStatus::result_unknown,
            boot_changed ? "a6_boot_changed" : "transport_lost",
            current_us);
    }
    if ((boot_changed || transport_lost) && operation_ &&
        active_phase(operation_->phase)) {
        operation_->phase = OperationPhase::failed;
        operation_->terminal_reason =
            boot_changed ? "a6_boot_changed" : "transport_lost";
        operation_->updated_at_us = current_us;
    }
    state_.mqtt_connected = mqtt_connected;
    state_.a6_online = a6_online;
    if (!boot_id.empty()) {
        state_.boot_id = std::move(boot_id);
    }
    state_.a6_uptime_ms = a6_uptime_ms;
    state_.a6_observed_at_us = current_us;
    state_.fresh = mqtt_connected && a6_online &&
                   !state_.boot_id.empty() && a6_uptime_ms >= 0;
    state_.state_at_us = current_us;
    if (boot_changed) {
        state_.barrier = BarrierState::unknown;
        state_.mode = BarrierMode::normal;
        update_revision_locked();
    }
}

void AgentCore::set_policy(
    bool commands_enabled,
    bool explicit_close_enabled) {
    std::lock_guard lock(mutex_);
    state_.commands_enabled = commands_enabled;
    state_.explicit_close_enabled = explicit_close_enabled;
}

void AgentCore::set_ack_timeout_ms(int value) {
    if (value < 200 || value > 5000) {
        throw std::invalid_argument("invalid A6 ACK timeout");
    }
    std::lock_guard lock(mutex_);
    ack_timeout_ms_ = value;
}

CommandResult AgentCore::reject_locked(std::string reason) const {
    CommandResult result;
    result.status = CommandStatus::rejected;
    result.reason = std::move(reason);
    if (operation_ && active_phase(operation_->phase)) {
        result.operation = operation_;
    }
    return result;
}

bool AgentCore::action_valid_locked(Action action) const {
    switch (action) {
        case Action::open:
            return state_.mode == BarrierMode::normal &&
                   (state_.barrier == BarrierState::closed ||
                    state_.barrier == BarrierState::closing);
        case Action::close_request:
            return state_.explicit_close_enabled &&
                   state_.barrier == BarrierState::open &&
                   state_.mode == BarrierMode::normal;
        case Action::hold_enable:
            return state_.barrier == BarrierState::closed &&
                   state_.mode == BarrierMode::normal;
        case Action::hold_disable:
            return state_.barrier == BarrierState::open &&
                   state_.mode == BarrierMode::hold_open;
    }
    return false;
}

bool AgentCore::joins_operation_locked(Action action) const {
    if (!operation_ || !active_phase(operation_->phase)) {
        return false;
    }
    return (action == Action::open &&
            operation_->direction == OperationDirection::opening) ||
           (action == Action::close_request &&
            operation_->direction == OperationDirection::closing);
}

AgentDecision AgentCore::submit(
    CommandRequest command,
    std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    AgentDecision decision;
    const auto reject = [&](std::string reason) {
        decision.result = reject_locked(std::move(reason));
        decision.result.request = command;
    };
    const auto duplicate =
        idempotency_to_command_.find(command.idempotency_key);
    if (duplicate != idempotency_to_command_.end()) {
        const auto found = commands_.find(duplicate->second);
        if (found != commands_.end()) {
            decision.result.accepted =
                found->second.status != CommandStatus::rejected;
            decision.result.status = found->second.status;
            decision.result.reason = found->second.reason;
            decision.result.request = found->second.command;
            decision.result.operation = operation_;
            decision.result.joined =
                found->second.reason == "joined_operation";
            return decision;
        }
    }
    if (const auto error = validate_command(command, current_us)) {
        reject(*error);
        return decision;
    }
    if (!state_.commands_enabled) {
        reject("commands_disabled");
        return decision;
    }
    if (!state_.fresh) {
        reject("fresh_state_unavailable");
        return decision;
    }
    if (command.target_boot_id != state_.boot_id) {
        reject("target_boot_changed");
        return decision;
    }
    if (command.action == Action::close_request &&
        state_.mode == BarrierMode::hold_open) {
        reject("hold_release_required");
        return decision;
    }

    if (joins_operation_locked(command.action)) {
        AgentCommandRecord joined;
        joined.command = command;
        joined.status = CommandStatus::acknowledged;
        joined.reason = "joined_operation";
        joined.updated_at_us = current_us;
        remember_command_locked(joined);
        idempotency_to_command_[command.idempotency_key] =
            command.command_id;
        idempotency_order_.push_back(command.idempotency_key);
        decision.result.accepted = true;
        decision.result.status = joined.status;
        decision.result.reason = joined.reason;
        decision.result.request = command;
        decision.result.operation = operation_;
        decision.result.joined = true;
        return decision;
    }

    if (operation_ && active_phase(operation_->phase) &&
        operation_->direction == OperationDirection::closing &&
        command.action == Action::open) {
        operation_->phase = OperationPhase::interrupted;
        operation_->terminal_reason = "open_requested_while_closing";
        operation_->updated_at_us = current_us;
    } else if (operation_ && active_phase(operation_->phase)) {
        reject("operation_in_progress");
        return decision;
    }
    if (transport_command_id_) {
        reject("command_in_progress");
        return decision;
    }
    if (!action_valid_locked(command.action)) {
        reject("action_not_valid_for_current_state");
        return decision;
    }

    AgentCommandRecord record;
    record.command = command;
    record.status = CommandStatus::pi_received;
    record.updated_at_us = current_us;
    remember_command_locked(record);
    transport_command_id_ = command.command_id;
    idempotency_to_command_[command.idempotency_key] =
        command.command_id;
    idempotency_order_.push_back(command.idempotency_key);
    while (idempotency_order_.size() > limits_.idempotency_keys) {
        idempotency_to_command_.erase(idempotency_order_.front());
        idempotency_order_.pop_front();
    }

    if (command.action == Action::open ||
        command.action == Action::close_request ||
        command.action == Action::hold_enable) {
        const auto direction = direction_for(command.action);
        operation_ = GateOperation{
            command.command_id,
            direction,
            requested_phase(direction),
            state_.boot_id,
            current_us,
            current_us,
            {},
            false,
        };
    }
    push_rs232_locked(
        {
            current_us,
            "tx",
            to_string(command.action),
            a6_command_ascii(command.action),
            command.command_id,
        });

    decision.result.accepted = true;
    decision.result.status = CommandStatus::pi_received;
    decision.result.request = command;
    decision.result.operation = operation_;
    decision.result.a6_tx_created = true;
    const std::int64_t estimated_uptime_ms =
        state_.a6_uptime_ms +
        std::max<std::int64_t>(
            0,
            (current_us - state_.a6_observed_at_us) / 1000);
    decision.a6_envelope = guarded_a6_envelope(
        command.action,
        command.command_id,
        state_.boot_id,
        estimated_uptime_ms,
        command.ttl_ms);
    return decision;
}

std::optional<AgentCommandRecord> AgentCore::mark_sent(
    std::string_view command_id,
    std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    const auto found = commands_.find(std::string(command_id));
    if (found == commands_.end() ||
        found->second.status != CommandStatus::pi_received) {
        return std::nullopt;
    }
    found->second.status = CommandStatus::sent;
    found->second.reason.clear();
    found->second.updated_at_us = current_us;
    found->second.ack_deadline_us =
        current_us +
        static_cast<std::int64_t>(ack_timeout_ms_) * 1000;
    return found->second;
}

std::optional<AgentCommandRecord> AgentCore::on_a6_ack(
    std::string_view command_id,
    bool accepted,
    std::string reason,
    std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    const auto found = commands_.find(std::string(command_id));
    if (found == commands_.end()) {
        return std::nullopt;
    }
    push_rs232_locked(
        {
            current_us,
            "ack",
            accepted ? "accepted" : "rejected",
            {},
            std::string(command_id),
        });
    found->second.updated_at_us = current_us;
    found->second.ack_deadline_us = 0;
    if (!accepted) {
        found->second.status = CommandStatus::failed;
        found->second.reason =
            reason.empty() ? "a6_rejected" : std::move(reason);
        if (operation_ && operation_->operation_id == command_id) {
            operation_->phase = OperationPhase::failed;
            operation_->terminal_reason = found->second.reason;
            operation_->updated_at_us = current_us;
        }
    } else {
        found->second.status = CommandStatus::acknowledged;
        found->second.reason.clear();
        // BLDC_FM_0803 legacy ASCII does not emit a distinct UPLOCK/UNLOCK
        // confirmation frame. Preserve physical position as an RS232
        // ACTION/OK concern, but advance the logical mode when A6 confirms
        // that the requested bytes were drained to the controller UART.
        const BarrierMode previous_mode = state_.mode;
        if (found->second.command.action == Action::hold_enable) {
            state_.mode = BarrierMode::hold_open;
        } else if (
            found->second.command.action == Action::hold_disable) {
            state_.mode = BarrierMode::normal;
        }
        if (state_.mode != previous_mode) {
            state_.state_at_us = current_us;
            update_revision_locked();
        }
    }
    if (transport_command_id_ &&
        *transport_command_id_ == command_id) {
        transport_command_id_.reset();
    }
    return found->second;
}

void AgentCore::start_observed_operation_locked(
    OperationDirection direction,
    OperationPhase phase,
    std::int64_t current_us) {
    if (operation_ && active_phase(operation_->phase) &&
        operation_->direction == direction) {
        operation_->phase = phase;
        operation_->observed = true;
        operation_->updated_at_us = current_us;
        return;
    }
    if (operation_ && active_phase(operation_->phase)) {
        operation_->phase = OperationPhase::interrupted;
        operation_->terminal_reason = "opposite_physical_motion";
        operation_->updated_at_us = current_us;
    }
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

void AgentCore::update_operation_from_physical_locked(
    std::string_view event,
    std::int64_t current_us) {
    if (event == "gate_up_action") {
        start_observed_operation_locked(
            OperationDirection::opening,
            OperationPhase::opening,
            current_us);
    } else if (event == "gate_down_action") {
        start_observed_operation_locked(
            OperationDirection::closing,
            OperationPhase::closing,
            current_us);
    } else if (event == "gate_up_ok" || event == "uplock") {
        if (!operation_ ||
            operation_->direction != OperationDirection::opening) {
            start_observed_operation_locked(
                OperationDirection::opening,
                OperationPhase::opening,
                current_us);
        }
        operation_->phase = OperationPhase::terminal;
        operation_->terminal_reason = "open";
        operation_->updated_at_us = current_us;
    } else if (event == "gate_down_ok") {
        if (!operation_ ||
            operation_->direction != OperationDirection::closing) {
            start_observed_operation_locked(
                OperationDirection::closing,
                OperationPhase::closing,
                current_us);
        }
        operation_->phase = OperationPhase::terminal;
        operation_->terminal_reason = "closed";
        operation_->updated_at_us = current_us;
    } else if (event == "gate_error") {
        if (operation_) {
            operation_->phase = OperationPhase::failed;
            operation_->terminal_reason = "controller_error";
            operation_->updated_at_us = current_us;
        }
    }
}

std::vector<nlohmann::json> AgentCore::on_rs232_frame(
    std::string_view raw,
    std::int64_t current_us) {
    const DecodedA6Frame decoded = decode_a6_frame(raw);
    std::lock_guard lock(mutex_);
    std::vector<nlohmann::json> emitted;
    if (decoded.kind == A6FrameKind::invalid ||
        decoded.kind == A6FrameKind::unknown) {
        return emitted;
    }
    if (decoded.status) {
        const BarrierState previous_barrier = state_.barrier;
        const BarrierMode previous_mode = state_.mode;
        if (decoded.barrier) {
            state_.barrier = *decoded.barrier;
        }
        if (decoded.mode) {
            state_.mode = *decoded.mode;
        }
        state_.state_at_us = current_us;
        if (state_.barrier != previous_barrier ||
            state_.mode != previous_mode ||
            state_.revision.empty()) {
            update_revision_locked();
        }
        if (operation_ && active_phase(operation_->phase) &&
            ((operation_->direction == OperationDirection::opening &&
              state_.barrier == BarrierState::open) ||
             (operation_->direction == OperationDirection::closing &&
              state_.barrier == BarrierState::closed))) {
            operation_->phase = OperationPhase::terminal;
            operation_->terminal_reason =
                state_.barrier == BarrierState::open ? "open" : "closed";
            operation_->updated_at_us = current_us;
        }
        return emitted;
    }
    if (decoded.detector) {
        emitted.push_back(
            {
                {"sensor", "detector"},
                {"active", decoded.detector_active.value_or(false)},
                {"quality", "observed"},
                {"occurred_at_us", current_us},
            });
        return emitted;
    }

    const std::string event = to_string(decoded.kind);
    if (is_persisted_physical_event(event)) {
        push_rs232_locked(
            {current_us, "rx", event, decoded.normalized, {}});
    }
    if (decoded.barrier) {
        state_.barrier = *decoded.barrier;
    }
    if (decoded.mode) {
        state_.mode = *decoded.mode;
    }
    state_.state_at_us = current_us;
    update_revision_locked();
    update_operation_from_physical_locked(event, current_us);
    if (event == "uplock") {
        state_.mode = BarrierMode::hold_open;
    } else if (event == "unlock") {
        state_.mode = BarrierMode::normal;
    } else if (event == "gate_error") {
        state_.barrier = BarrierState::fault;
    }
    emitted.push_back(physical_event_locked(event, current_us));
    return emitted;
}

void AgentCore::status_requested(std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    ++status_aggregate_.requested;
    status_aggregate_.last_requested_at_us = current_us;
}

void AgentCore::status_completed(
    bool success,
    std::int64_t rtt_us,
    std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    status_aggregate_.last_rtt_us = rtt_us;
    if (success) {
        ++status_aggregate_.succeeded;
        status_aggregate_.consecutive_failures = 0;
        status_aggregate_.last_succeeded_at_us = current_us;
    } else {
        ++status_aggregate_.failed;
        ++status_aggregate_.consecutive_failures;
    }
}

void AgentCore::tick(std::int64_t current_us) {
    std::lock_guard lock(mutex_);
    if (!transport_command_id_) {
        return;
    }
    auto& record = commands_.at(*transport_command_id_);
    if (record.ack_deadline_us > 0 &&
        record.ack_deadline_us <= current_us &&
        !terminal(record.status)) {
        record.status = CommandStatus::failed;
        record.reason = "a6_ack_timeout";
        record.updated_at_us = current_us;
        if (operation_ &&
            operation_->operation_id == record.command.command_id) {
            operation_->phase = OperationPhase::failed;
            operation_->terminal_reason = record.reason;
            operation_->updated_at_us = current_us;
        }
        transport_command_id_.reset();
    }
}

AgentState AgentCore::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

std::optional<AgentCommandRecord> AgentCore::command(
    std::string_view command_id) const {
    std::lock_guard lock(mutex_);
    const auto found = commands_.find(std::string(command_id));
    return found == commands_.end()
               ? std::nullopt
               : std::optional<AgentCommandRecord>(found->second);
}

std::vector<AgentCommandRecord> AgentCore::recent_commands() const {
    std::lock_guard lock(mutex_);
    std::vector<AgentCommandRecord> result;
    for (const auto& id : command_order_) {
        if (const auto found = commands_.find(id);
            found != commands_.end()) {
            result.push_back(found->second);
        }
    }
    return result;
}

std::vector<nlohmann::json> AgentCore::recent_physical_events() const {
    std::lock_guard lock(mutex_);
    return {physical_events_.begin(), physical_events_.end()};
}

std::vector<Rs232Record> AgentCore::recent_rs232() const {
    std::lock_guard lock(mutex_);
    return {rs232_.begin(), rs232_.end()};
}

AgentStatusAggregate AgentCore::status_aggregate() const {
    std::lock_guard lock(mutex_);
    return status_aggregate_;
}

std::optional<GateOperation> AgentCore::operation() const {
    std::lock_guard lock(mutex_);
    return operation_;
}

nlohmann::json AgentCore::public_state() const {
    std::lock_guard lock(mutex_);
    return {
        {"site_id", kSiteId},
        {"gate_id", kGateId},
        {"commands_enabled", state_.commands_enabled},
        {"explicit_close_enabled", state_.explicit_close_enabled},
        {"mqtt_connected", state_.mqtt_connected},
        {"a6_online", state_.a6_online},
        {"fresh", state_.fresh},
        {"state", to_string(state_.barrier)},
        {"mode", to_string(state_.mode)},
        {"boot_id", state_.boot_id},
        {"revision", state_.revision},
        {"sequence", state_.sequence},
        {"a6_uptime_ms", state_.a6_uptime_ms},
        {"a6_observed_at_us", state_.a6_observed_at_us},
        {"state_at_us", state_.state_at_us},
        {"transport_command_id", transport_command_id_.value_or("")},
        {"operation", operation_ ? nlohmann::json(*operation_)
                                  : nlohmann::json(nullptr)},
    };
}

nlohmann::json AgentCore::capabilities() const {
    return {
        {"schema_version", 1},
        {"actions", {"open", "close_request", "hold_enable", "hold_disable"}},
        {"global_state_based_coalescing", true},
        {"sensor_software_interlock", false},
        {"status_policy",
         {
             {"startup", true},
             {"a6_boot_change", true},
             {"interval_seconds", 3600},
             {"manual_refresh", true},
             {"observer_sync", false},
             {"pre_command", false},
         }},
    };
}

void AgentCore::update_revision_locked() {
    ++state_.sequence;
    state_.revision =
        state_.boot_id + ":" + std::to_string(state_.sequence);
}

void AgentCore::remember_command_locked(
    const AgentCommandRecord& record) {
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

void AgentCore::complete_transport_locked(
    CommandStatus status,
    std::string reason,
    std::int64_t current_us) {
    if (!transport_command_id_) {
        return;
    }
    auto& command = commands_.at(*transport_command_id_);
    command.status = status;
    command.reason = std::move(reason);
    command.updated_at_us = current_us;
    command.ack_deadline_us = 0;
    transport_command_id_.reset();
}

void AgentCore::push_rs232_locked(Rs232Record record) {
    rs232_.push_back(std::move(record));
    while (rs232_.size() > limits_.rs232_records) {
        rs232_.pop_front();
    }
}

nlohmann::json AgentCore::physical_event_locked(
    std::string event,
    std::int64_t current_us,
    std::string) {
    nlohmann::json result = {
        {"event", std::move(event)},
        {"occurred_at_us", current_us},
        {"state", to_string(state_.barrier)},
        {"mode", to_string(state_.mode)},
        {"revision", state_.revision},
        {"sequence", state_.sequence},
    };
    physical_events_.push_back(result);
    while (physical_events_.size() > limits_.physical_events) {
        physical_events_.pop_front();
    }
    return result;
}

nlohmann::json agent_command_json(
    const AgentCommandRecord& value) {
    return {
        {"command", value.command},
        {"status", to_string(value.status)},
        {"reason", value.reason},
        {"updated_at_us", value.updated_at_us},
        {"ack_deadline_us", value.ack_deadline_us},
    };
}

}  // namespace portfolio::gate
