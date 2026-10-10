#include <cassert>
#include <iostream>

#include "portfolio/gate/coordinator_core.hpp"

using namespace portfolio::gate;

namespace {

Envelope message(
    std::string type,
    json payload,
    std::string revision = "boot-1:1",
    std::uint64_t sequence = 1) {
    return make_envelope(
        std::move(type),
        std::move(payload),
        "boot-1",
        std::move(revision),
        sequence);
}

}  // namespace

int main() {
    const auto start = now_us();
    CoordinatorCore core;
    core.set_mqtt_connected(true, start);
    assert(core.observe_edge_status(
        message("edge.status", {{"online", true}}), start + 1));
    assert(core.observe_state(
        message(
            "gate.state",
            {{"state", "closed"}, {"mode", "normal"}}),
        false,
        start + 2));
    assert(core.state().ready);
    assert(!core.observer_connected(start + 3));

    assert(core.observe_state(
        message(
            "gate.state",
            {{"state", "closed"}, {"mode", "normal"}},
            "boot-1:10",
            10),
        false,
        start + 4));
    assert(core.state().ready);
    assert(core.state().sync_status == "live");

    const auto first = core.request_web_command(
        Action::open, "user-1", "idem-1", 5000, start + 10);
    assert(first.accepted && first.a6_tx_created);
    assert(first.operation->phase == OperationPhase::opening_requested);
    const auto joined = core.request_web_command(
        Action::open, "user-2", "idem-2", 5000, start + 11);
    assert(joined.accepted && joined.joined);
    const auto close = core.request_web_command(
        Action::close_request, "user-3", "idem-3", 5000, start + 12);
    assert(!close.accepted);
    assert(close.reason == "opening_in_progress");

    assert(core.observe_command_status(
        message(
            "command.status",
            {
                {"command_id", first.request->command_id},
                {"idempotency_key", "idem-1"},
                {"action", "open"},
                {"status", "acknowledged"},
                {"source", "web"},
                {"operation", *first.operation},
            }),
        start + 20));
    assert(core.observe_physical_event(
        message(
            "physical.event",
            {{"event", "gate_up_action"}},
            "boot-1:11",
            11),
        start + 30));
    assert(core.state().barrier == BarrierState::opening);
    assert(core.operation()->phase == OperationPhase::opening);
    assert(core.observe_physical_event(
        message(
            "physical.event",
            {{"event", "gate_up_ok"}},
            "boot-1:12",
            12),
        start + 40));
    assert(core.state().barrier == BarrierState::open);
    assert(core.operation()->phase == OperationPhase::terminal);
    for (const auto& row : core.recent_physical_events()) {
        assert(!row.contains("command_id"));
        assert(!row.contains("operation_id"));
    }

    const auto refresh = core.request_manual_refresh(start + 50);
    assert(refresh);
    assert(!core.request_manual_refresh(start + 51));
    assert(core.observe_sync_response(
        message(
            "sync.response",
            {
                {"request_id", refresh->payload.at("request_id")},
                {"status", "verified"},
                {"state", {{"state", "open"}, {"mode", "normal"}}},
            },
            "boot-1:12",
            12),
        start + 60));
    assert(core.state().ready);

    const json saved = core.checkpoint();
    assert(saved.at("schema_version") == 2);
    CoordinatorCore restored;
    restored.restore_checkpoint(saved);
    assert(restored.state().restored_unverified);
    assert(!restored.state().ready);
    assert(restored.operation());
    assert(restored.recent_physical_events().size() == 2);

    std::cout << "R6 coordinator core tests passed\n";
    return 0;
}
