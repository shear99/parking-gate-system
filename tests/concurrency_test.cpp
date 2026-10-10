#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

#include "portfolio/gate/agent_core.hpp"
#include "portfolio/gate/coordinator_core.hpp"

using namespace portfolio::gate;

namespace {

void ready(CoordinatorCore& core, std::int64_t start) {
    core.set_mqtt_connected(true, start);
    core.observe_edge_status(
        make_envelope(
            "edge.status",
            {{"online", true}},
            "boot-1",
            "boot-1:1",
            1),
        start + 1);
    core.observe_state(
        make_envelope(
            "gate.state",
            {{"state", "closed"}, {"mode", "normal"}},
            "boot-1",
            "boot-1:1",
            1),
        false,
        start + 2);
    assert(core.state().ready);
}

CommandRequest request(
    const AgentState& state,
    int index,
    std::int64_t start) {
    CommandRequest value;
    value.command_id = "cmd-" + std::to_string(index);
    value.idempotency_key = "idem-" + std::to_string(index);
    value.action = Action::open;
    value.actor_id = "device-" + std::to_string(index);
    value.source = index % 2 == 0 ? "web-coordinator" : "direct-mtls";
    value.requested_at_us = start + index;
    value.expires_at_us = start + 5'000'000;
    value.target_boot_id = state.boot_id;
    value.expected_state_revision = state.revision;
    return value;
}

}  // namespace

int main() {
    const auto start = now_us();
    {
        CoordinatorCore core;
        ready(core, start);
        std::atomic<int> accepted{0};
        std::atomic<int> tx_created{0};
        std::vector<std::thread> threads;
        for (int index = 0; index < 50; ++index) {
            threads.emplace_back([&, index] {
                const auto result = core.request_web_command(
                    Action::open,
                    "web-" + std::to_string(index),
                    "idem-" + std::to_string(index),
                    5000,
                    start + 100 + index);
                accepted += result.accepted ? 1 : 0;
                tx_created += result.a6_tx_created ? 1 : 0;
                assert(result.accepted);
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        assert(accepted == 50);
        assert(tx_created == 1);
    }
    {
        AgentCore core;
        core.set_transport(true, true, "a6-boot", 1000, start);
        core.set_policy(true, true);
        core.on_rs232_frame("GATE=DOWN OK,DETECTOR=OFF", start + 1);
        const auto state = core.state();
        std::atomic<int> accepted{0};
        std::atomic<int> tx_created{0};
        std::vector<std::thread> threads;
        for (int index = 0; index < 50; ++index) {
            threads.emplace_back([&, index] {
                const auto result =
                    core.submit(request(state, index, start + 100), start + 100 + index);
                accepted += result.result.accepted ? 1 : 0;
                tx_created += result.a6_envelope ? 1 : 0;
                assert(result.result.accepted);
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        assert(accepted == 50);
        assert(tx_created == 1);
    }
    {
        CoordinatorCore core;
        ready(core, start);
        std::atomic<int> created{0};
        std::vector<std::thread> threads;
        for (int index = 0; index < 50; ++index) {
            threads.emplace_back([&] {
                if (core.request_manual_refresh(start + 500)) {
                    ++created;
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        assert(created == 1);
        for (int index = 0; index < 100; ++index) {
            assert(!core.observer_connected(start + 1000 + index));
        }
        assert(core.status_aggregate().requested == 1);
    }
    std::cout << "R6 concurrency tests passed\n";
    return 0;
}
