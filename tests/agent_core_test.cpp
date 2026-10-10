#include <cassert>
#include <iostream>

#include "portfolio/gate/agent_core.hpp"

using namespace portfolio::gate;

namespace {

CommandRequest command(
    const AgentState& state,
    std::string id,
    Action action,
    std::int64_t at) {
    CommandRequest result;
    result.command_id = std::move(id);
    result.idempotency_key = "idem-" + result.command_id;
    result.action = action;
    result.actor_id = "test";
    result.source = "web";
    result.requested_at_us = at;
    result.expires_at_us = at + 5'000'000;
    result.target_boot_id = state.boot_id;
    result.expected_state_revision = state.revision;
    result.ttl_ms = 5000;
    return result;
}

}  // namespace

int main() {
    const auto start = now_us();
    AgentCore expiry_core;
    expiry_core.set_transport(
        true, true, "a6-expiry", 1000, start);
    expiry_core.set_policy(true, true);
    expiry_core.on_rs232_frame(
        "GATE=DOWN OK,DETECTOR=OFF", start + 100);
    const auto stale_uptime_command = expiry_core.submit(
        command(
            expiry_core.state(),
            "cmd-stale-uptime",
            Action::open,
            start + 45'000'000),
        start + 45'000'000);
    assert(stale_uptime_command.a6_envelope);
    assert(stale_uptime_command.a6_envelope->at(
               "expires_at_uptime_ms") >= 51'000);

    AgentCore core;
    core.set_transport(true, true, "a6-boot-1", 1000, start);
    core.set_policy(true, true);
    core.set_ack_timeout_ms(2000);
    assert(core.on_rs232_frame(
        "GATE=DOWN OK,DETECTOR=OFF", start + 100).empty());

    const auto first = core.submit(
        command(core.state(), "cmd-open", Action::open, start + 200),
        start + 200);
    assert(first.result.accepted);
    assert(first.result.a6_tx_created);
    assert(first.a6_envelope);
    assert(core.operation()->phase == OperationPhase::opening_requested);
    assert(core.mark_sent("cmd-open", start + 250));

    const auto joined = core.submit(
        command(core.state(), "cmd-open-2", Action::open, start + 300),
        start + 300);
    assert(joined.result.accepted);
    assert(joined.result.joined);
    assert(!joined.a6_envelope);

    const auto close_while_opening = core.submit(
        command(
            core.state(),
            "cmd-close-too-early",
            Action::close_request,
            start + 310),
        start + 310);
    assert(!close_while_opening.result.accepted);

    const auto ack =
        core.on_a6_ack("cmd-open", true, {}, start + 400);
    assert(ack && ack->status == CommandStatus::acknowledged);
    assert(core.on_rs232_frame(
        "GATE UP ACTION", start + 500).size() == 1);
    assert(core.state().barrier == BarrierState::opening);
    assert(core.operation()->phase == OperationPhase::opening);
    core.tick(start + 60'000'000);
    assert(core.operation()->phase == OperationPhase::opening);
    assert(core.on_rs232_frame(
        "GATE UP OK", start + 600).size() == 1);
    assert(core.state().barrier == BarrierState::open);
    assert(core.operation()->phase == OperationPhase::terminal);

    const auto detector =
        core.on_rs232_frame("DETECTOR ON", start + 700);
    assert(detector.size() == 1);
    assert(detector.front().at("sensor") == "detector");
    assert(core.recent_physical_events().size() == 2);
    for (const auto& row : core.recent_physical_events()) {
        assert(!row.contains("command_id"));
        assert(!row.contains("operation_id"));
    }

    const auto close = core.submit(
        command(
            core.state(), "cmd-close", Action::close_request, start + 800),
        start + 800);
    assert(close.result.accepted && close.a6_envelope);
    assert(core.mark_sent("cmd-close", start + 810));
    core.tick(start + 2'900'000);
    assert(core.command("cmd-close")->status == CommandStatus::failed);
    assert(core.command("cmd-close")->reason == "a6_ack_timeout");

    AgentCore hold;
    hold.set_transport(true, true, "a6-boot-2", 1000, start);
    hold.set_policy(true, true);
    hold.on_rs232_frame("GATE UPLOCK", start + 1);
    const auto blocked = hold.submit(
        command(
            hold.state(),
            "cmd-close-hold",
            Action::close_request,
            start + 2),
        start + 2);
    assert(!blocked.result.accepted);
    assert(blocked.result.reason == "hold_release_required");

    AgentCore legacy_hold;
    legacy_hold.set_policy(true, true);
    legacy_hold.set_transport(
        true, true, "boot-legacy", 70'000, start);
    legacy_hold.on_rs232_frame(
        "GATE=DOWN OK,DETECTOR=OFF", start + 1);
    auto enable_hold = command(
        legacy_hold.state(),
        "hold-enable-legacy",
        Action::hold_enable,
        start + 2);
    const auto enable_decision =
        legacy_hold.submit(enable_hold, start + 2);
    assert(enable_decision.result.accepted);
    assert(
        legacy_hold.mark_sent(
            enable_hold.command_id, start + 3));
    legacy_hold.on_a6_ack(
        enable_hold.command_id,
        true,
        "uart_drained",
        start + 4);
    assert(
        legacy_hold.state().mode ==
        BarrierMode::hold_open);
    assert(
        legacy_hold.state().barrier ==
        BarrierState::closed);

    legacy_hold.on_rs232_frame(
        "GATE UP ACTION", start + 5);
    legacy_hold.on_rs232_frame(
        "GATE UP OK", start + 6);
    assert(
        legacy_hold.state().mode ==
        BarrierMode::hold_open);
    assert(
        legacy_hold.state().barrier ==
        BarrierState::open);

    auto disable_hold = command(
        legacy_hold.state(),
        "hold-disable-legacy",
        Action::hold_disable,
        start + 7);
    const auto disable_decision =
        legacy_hold.submit(disable_hold, start + 7);
    assert(disable_decision.result.accepted);
    assert(
        legacy_hold.mark_sent(
            disable_hold.command_id, start + 8));
    legacy_hold.on_a6_ack(
        disable_hold.command_id,
        true,
        "uart_drained",
        start + 9);
    assert(
        legacy_hold.state().mode ==
        BarrierMode::normal);
    assert(
        legacy_hold.state().barrier ==
        BarrierState::open);

    const auto capabilities = core.capabilities();
    assert(
        capabilities["status_policy"]["observer_sync"] == false);
    assert(capabilities["sensor_software_interlock"] == false);

    std::cout << "R6 agent core tests passed\n";
    return 0;
}
