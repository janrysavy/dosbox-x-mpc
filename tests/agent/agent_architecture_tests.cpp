#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "agent/agent_bridge.h"
#include "agent/agent_protocol.h"
#include "agent/agent_server.h"
#include "agent/hardware_trace.h"
#include "agent/trace_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

dosbox_agent::AgentConfig MakeTestConfig()
{
    dosbox_agent::AgentConfig config;
    config.transport = dosbox_agent::AgentTransport::NamedPipe;
    config.endpoint = R"(\\.\pipe\dosbox-agent-unit-test)";
    config.dosbox_workdir = "tests/agent/runtime";
    config.request_timeout_ms = 5000;
    config.max_message_bytes = 1024;
    config.max_memory_read_bytes = 64;
    config.max_trace_events = 16;
    return config;
}

std::string StartFixtureSession(dosbox_agent::AgentServer* server)
{
    const std::string response = server->HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"start\",\"method\":\"session.start\","
            "\"params\":{\"target\":{\"command\":\"AGENTFIX.COM\",\"arguments\":[]},"
            "\"mounts\":[{\"drive\":\"C\",\"host_path\":\"tests/agent/runtime\"}],\"break_at\":\"entry\"}}");
    EXPECT_NE(std::string::npos, response.find("\"session_id\":\"ses-1\""));
    EXPECT_NE(std::string::npos, response.find("\"state\":\"stopped\""));
    EXPECT_NE(std::string::npos, response.find("\"kind\":\"startup\""));
    return response;
}

TEST(AgentConfig, LoadsOnlyTheExplicitConfigFile)
{
    dosbox_agent::AgentConfig config;
    std::string error;

    ASSERT_TRUE(dosbox_agent::AGENT_LoadConfigFile("tests/agent/agent-test.env", &config, &error)) << error;
    EXPECT_EQ(dosbox_agent::AgentTransport::NamedPipe, config.transport);
    EXPECT_EQ("\\\\.\\pipe\\dosbox-agent-test", config.endpoint);
    EXPECT_EQ(5000U, config.request_timeout_ms);
    EXPECT_EQ(static_cast<std::size_t>(1048576), config.max_message_bytes);
    EXPECT_EQ(static_cast<std::size_t>(65536), config.max_memory_read_bytes);
    EXPECT_EQ(static_cast<std::size_t>(10000), config.max_trace_events);
    EXPECT_TRUE(config.test_profile);
    EXPECT_NE(std::string::npos, config.dosbox_workdir.find("tests"));
    EXPECT_NE(std::string::npos, config.dosbox_workdir.find("runtime"));

    EXPECT_FALSE(dosbox_agent::AGENT_LoadConfigFile("tests/agent/missing.env", &config, &error));
    EXPECT_FALSE(error.empty());
}

TEST(AgentConfig, StartupLogContainsResolvedLimits)
{
    dosbox_agent::AgentConfig config;
    std::string error;
    ASSERT_TRUE(dosbox_agent::AGENT_LoadConfigFile("tests/agent/agent-test.env", &config, &error)) << error;

    const std::string log = dosbox_agent::AGENT_FormatStartupLog(config);
    EXPECT_NE(std::string::npos, log.find("\"endpoint\":\"\\\\\\\\.\\\\pipe\\\\dosbox-agent-test\""));
    EXPECT_NE(std::string::npos, log.find("\"request_timeout_ms\":5000"));
    EXPECT_NE(std::string::npos, log.find("\"max_memory_read_bytes\":65536"));
}

TEST(AgentBridge, RejectsPumpFromNonEmulationThread)
{
    dosbox_agent::EmulationThreadQueue queue;
    ASSERT_TRUE(queue.BindToCurrentThread());

    std::atomic<int> calls(0);
    ASSERT_NE(0U, queue.Submit([&calls](const std::uint64_t) { ++calls; }));

    std::size_t foreign_pump_count = 1;
    std::thread foreign_thread([&queue, &foreign_pump_count]() {
        foreign_pump_count = queue.Pump();
    });
    foreign_thread.join();

    EXPECT_EQ(0U, foreign_pump_count);
    EXPECT_EQ(0, calls.load());
    EXPECT_EQ(1U, queue.Pump());
    EXPECT_EQ(1, calls.load());
}

TEST(AgentBridge, PreservesSubmissionOrderUnderConcurrentLoad)
{
    const int command_count = 1000;
    dosbox_agent::EmulationThreadQueue queue;
    ASSERT_TRUE(queue.BindToCurrentThread());

    std::vector<std::uint64_t> completed;
    completed.reserve(command_count);
    std::atomic<int> submitted(0);
    std::atomic<int> rejected(0);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&queue, &completed, &submitted, &rejected, command_count]() {
            for (;;) {
                const int index = submitted.fetch_add(1);
                if (index >= command_count)
                    return;
                if (queue.Submit([&completed](const std::uint64_t sequence) {
                        completed.push_back(sequence);
                    }) == 0) {
                    ++rejected;
                }
            }
        });
    }
    for (std::vector<std::thread>::iterator it = workers.begin(); it != workers.end(); ++it)
        it->join();

    EXPECT_EQ(0, rejected.load());
    EXPECT_EQ(static_cast<std::size_t>(command_count), queue.Pump());
    ASSERT_EQ(static_cast<std::size_t>(command_count), completed.size());
    for (std::size_t index = 0; index < completed.size(); ++index)
        EXPECT_EQ(static_cast<std::uint64_t>(index + 1), completed[index]);
}

TEST(AgentBridge, DelaysRetriesAndDropsPendingCommandsOnShutdown)
{
    dosbox_agent::EmulationThreadQueue queue;
    ASSERT_TRUE(queue.BindToCurrentThread());

    std::atomic<int> calls(0);
    ASSERT_NE(0U, queue.SubmitAfter(20, [&calls](const std::uint64_t) { ++calls; }));
    EXPECT_EQ(0U, queue.Pump());
    EXPECT_EQ(0, calls.load());

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_EQ(1U, queue.Pump());
    EXPECT_EQ(1, calls.load());

    ASSERT_NE(0U, queue.SubmitAfter(20, [&calls](const std::uint64_t) { ++calls; }));
    queue.Shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_EQ(0U, queue.Pump());
    EXPECT_EQ(1, calls.load());
}

TEST(AgentProtocol, RejectsInvalidRequestsAndOversizedMessages)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;

    EXPECT_NE(std::string::npos, server.HandleJsonRpc("{\"method\":\"agent.capabilities\"}").find("\"code\":-32600"));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc("{\"jsonrpc\":\"2.0\",\"id\":\"unknown\",\"method\":\"unknown.method\"}").find("\"code\":-32601"));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc("{\"jsonrpc\":\"2.0\",\"id\":\"bad\",\"method\":\"agent.capabilities\",\"params\":[]}").find("\"code\":-32600"));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc(std::string(1025, 'x')).find("REQUEST_TOO_LARGE"));
}

TEST(AgentProtocol, ReportsBuildCapabilitiesAndLimits)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;

    const std::string response = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"capabilities\",\"method\":\"agent.capabilities\"}");
    EXPECT_NE(std::string::npos, response.find("\"debugger\":true"));
#ifdef C_HEAVY_DEBUG
    EXPECT_NE(std::string::npos, response.find("\"cpu\":true"));
    EXPECT_NE(std::string::npos, response.find("\"memory_io_effects\":true"));
    EXPECT_NE(std::string::npos, response.find("\"memory_read\":true"));
    EXPECT_NE(std::string::npos, response.find("\"memory_write\":true"));
    EXPECT_NE(std::string::npos, response.find("\"memory_access\":true"));
#else
    EXPECT_NE(std::string::npos, response.find("\"cpu\":false"));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"trace\",\"method\":\"trace.start\",\"params\":{}}").find("CAPABILITY_UNAVAILABLE"));
#endif
    EXPECT_NE(std::string::npos, response.find("\"segmented\""));
    EXPECT_NE(std::string::npos, response.find("\"linear\""));
    EXPECT_NE(std::string::npos, response.find("\"physical\""));
    EXPECT_NE(std::string::npos, response.find("\"max_memory_read_bytes\":64"));
    EXPECT_NE(std::string::npos, response.find("\"snapshot\":true"));
    EXPECT_NE(std::string::npos, response.find("\"dos\":{\"loader_metadata\":true,\"memory_map\":true}"));
    EXPECT_NE(std::string::npos, response.find("\"exact_access_requires_normal_core\":true"));
    EXPECT_NE(std::string::npos, response.find("\"condition_registers\":[\"eax\""));
    EXPECT_NE(std::string::npos, response.find("\"condition_operators\":[\"eq\",\"ne\"]"));
    EXPECT_NE(std::string::npos, response.find("\"software_interrupt\":true"));
    EXPECT_NE(std::string::npos, response.find("\"interrupt_phase\":\"before_handler\""));
    EXPECT_NE(std::string::npos, response.find("\"conditional_kinds\":[\"execution\",\"interrupt\"]"));
    EXPECT_NE(std::string::npos, response.find("\"hit_filter\":true"));
    EXPECT_NE(std::string::npos, response.find("\"run_until\":true"));
    EXPECT_NE(std::string::npos, response.find("\"run_until_atomic\":true"));
    EXPECT_NE(std::string::npos, response.find("\"run_until_emulated_time_limit\":true"));
    EXPECT_NE(std::string::npos, response.find("\"stop_emulated_timestamp_ns\":true"));
#ifdef C_HEAVY_DEBUG
    EXPECT_NE(std::string::npos, response.find("\"emulated_timestamp_ns\":true"));
#endif
    EXPECT_NE(std::string::npos, response.find("\"checkpoints\":{\"create\":true"));
    EXPECT_NE(std::string::npos, response.find("\"host_files\":false"));
    EXPECT_NE(std::string::npos, response.find("\"max_checkpoint_bytes\":536870912"));
    EXPECT_NE(std::string::npos, response.find("\"max_checkpoints\":8"));
    EXPECT_NE(std::string::npos, response.find("\"device_keyboard\":true"));
    EXPECT_NE(std::string::npos, response.find("\"ordered_keyboard_batch\":true"));
    EXPECT_NE(std::string::npos, response.find("\"keyboard_keys\":[\"1\""));
    EXPECT_NE(std::string::npos, response.find("\"joystick_axis_min\":-32768"));
    EXPECT_NE(std::string::npos, response.find("\"joystick_axis_max\":32767"));
}

TEST(AgentInput, DeliversOrderedDeviceEventsAndReportsExactStateWhileRunning)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string invalid = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"bad-key\",\"method\":\"input.keyboard\","
            "\"params\":{\"session_id\":\"ses-1\",\"events\":[{\"key\":\"not-a-key\",\"pressed\":true}]}}" );
    EXPECT_NE(std::string::npos, invalid.find("-32602"));

    const std::string pressed = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"keys-down\",\"method\":\"input.keyboard\","
            "\"params\":{\"session_id\":\"ses-1\",\"events\":["
            "{\"key\":\"left_shift\",\"pressed\":true},"
            "{\"key\":\"up\",\"pressed\":true}]}}" );
    EXPECT_NE(std::string::npos, pressed.find("\"pressed\":[\"left_shift\",\"up\"]"));
    EXPECT_NE(std::string::npos, pressed.find("\"state_revision\":2"));

    const std::string joystick = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"joy\",\"method\":\"input.joystick\","
            "\"params\":{\"session_id\":\"ses-1\",\"index\":0,\"enabled\":true,"
            "\"x\":-32768,\"y\":32767,\"button0\":true,\"button1\":false}}" );
    EXPECT_NE(std::string::npos, joystick.find("\"axes\":{\"x\":-32768,\"y\":32767}"));
    EXPECT_NE(std::string::npos, joystick.find("\"buttons\":[true,false]"));

    const std::string running = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"run\",\"method\":\"execution.continue\","
            "\"params\":{\"session_id\":\"ses-1\"}}" );
    EXPECT_NE(std::string::npos, running.find("\"state\":\"running\""));

    const std::string released = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"keys-up\",\"method\":\"input.keyboard\","
            "\"params\":{\"session_id\":\"ses-1\",\"events\":["
            "{\"key\":\"up\",\"pressed\":false},"
            "{\"key\":\"left_shift\",\"pressed\":false}]}}" );
    EXPECT_NE(std::string::npos, released.find("\"pressed\":[]"));

    const std::string state = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"input-state\",\"method\":\"input.state\","
            "\"params\":{\"session_id\":\"ses-1\"}}" );
    EXPECT_NE(std::string::npos, state.find("\"pressed\":[]"));
    EXPECT_NE(std::string::npos, state.find("\"enabled\":true"));
}

TEST(AgentRunUntil, InstallsResumesStopsAndRemovesOnePrivatePredicate)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string invalid = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"bad-until\",\"method\":\"execution.run_until\","
            "\"params\":{\"session_id\":\"ses-1\",\"predicate\":{\"kind\":\"execution\","
            "\"address\":{\"space\":\"segmented\",\"segment\":\"0x1000\",\"offset\":\"0x00000106\"},"
            "\"once\":true}}}");
    EXPECT_NE(std::string::npos, invalid.find("implicitly one-shot"));

    const std::string started = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"until\",\"method\":\"execution.run_until\","
            "\"params\":{\"session_id\":\"ses-1\",\"predicate\":{\"kind\":\"execution\","
            "\"address\":{\"space\":\"segmented\",\"segment\":\"0x1000\",\"offset\":\"0x00000106\"},"
            "\"length\":1,\"hit_filter\":{\"skip\":0,\"every\":1}}}}" );
    EXPECT_NE(std::string::npos, started.find("\"operation_id\":\"op-1\""));
    EXPECT_NE(std::string::npos, started.find("\"predicate_id\":\"until-1\""));

    const std::string stopped = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"wait-until\",\"method\":\"execution.wait\","
            "\"params\":{\"session_id\":\"ses-1\",\"operation_id\":\"op-1\",\"timeout_ms\":1000}}");
    EXPECT_NE(std::string::npos, stopped.find("\"kind\":\"run_until\""));
    EXPECT_NE(std::string::npos, stopped.find("\"breakpoint_id\":\"until-1\""));
    EXPECT_NE(std::string::npos, stopped.find("\"hit_count\":1"));

    const std::string listed = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"list-after-until\",\"method\":\"breakpoints.list\","
            "\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, listed.find("\"breakpoints\":[]"));
    EXPECT_EQ(std::string::npos, listed.find("until-1"));
}

TEST(AgentRunUntil, StopsAtBoundedEmulatedTimeAndReportsOvershoot)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string invalid = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"bad-time\",\"method\":\"execution.run_until\","
            "\"params\":{\"session_id\":\"ses-1\",\"max_emulated_ns\":0,\"predicate\":{"
            "\"kind\":\"execution\",\"address\":{\"space\":\"segmented\","
            "\"segment\":\"0x1000\",\"offset\":\"0x0000FFFF\"}}}}" );
    EXPECT_NE(std::string::npos, invalid.find("positive 64-bit integer"));

    const std::string started = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"bounded\",\"method\":\"execution.run_until\","
            "\"params\":{\"session_id\":\"ses-1\",\"max_emulated_ns\":100,\"predicate\":{"
            "\"kind\":\"execution\",\"address\":{\"space\":\"segmented\","
            "\"segment\":\"0x1000\",\"offset\":\"0x0000FFFF\"}}}}" );
    EXPECT_NE(std::string::npos, started.find("\"max_emulated_ns\":100"));

    const std::string stopped = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"wait-bounded\",\"method\":\"execution.wait\","
            "\"params\":{\"session_id\":\"ses-1\",\"operation_id\":\"op-1\",\"timeout_ms\":1000}}" );
    EXPECT_NE(std::string::npos, stopped.find("\"kind\":\"emulated_time_limit\""));
    EXPECT_NE(std::string::npos, stopped.find("\"emulated_time_ns\":1000107"));
    EXPECT_NE(std::string::npos, stopped.find("\"requested_duration_ns\":100"));
    EXPECT_NE(std::string::npos, stopped.find("\"start_emulated_time_ns\":1000000"));
    EXPECT_NE(std::string::npos, stopped.find("\"deadline_emulated_time_ns\":1000100"));
    EXPECT_NE(std::string::npos, stopped.find("\"overshoot_ns\":7"));
    EXPECT_NE(std::string::npos, stopped.find("\"reached\":true"));

    const std::string listed = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"list-after-time\",\"method\":\"breakpoints.list\","
            "\"params\":{\"session_id\":\"ses-1\"}}" );
    EXPECT_NE(std::string::npos, listed.find("\"breakpoints\":[]"));

    const std::string predicate_started = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"bounded-predicate\",\"method\":\"execution.run_until\","
            "\"params\":{\"session_id\":\"ses-1\",\"max_emulated_ns\":100,\"predicate\":{"
            "\"kind\":\"execution\",\"address\":{\"space\":\"segmented\","
            "\"segment\":\"0x1000\",\"offset\":\"0x00000106\"}}}}" );
    EXPECT_NE(std::string::npos, predicate_started.find("\"operation_id\":\"op-2\""));
    const std::string predicate_stopped = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"wait-predicate\",\"method\":\"execution.wait\","
            "\"params\":{\"session_id\":\"ses-1\",\"operation_id\":\"op-2\",\"timeout_ms\":1000}}" );
    EXPECT_NE(std::string::npos, predicate_stopped.find("\"kind\":\"run_until\""));
    EXPECT_NE(std::string::npos, predicate_stopped.find("\"reached\":false"));
    EXPECT_NE(std::string::npos, predicate_stopped.find("\"overshoot_ns\":0"));
}

TEST(AgentCheckpoint, CreatesRestoresListsDeletesAndInvalidatesOldResponses)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string status_request =
            "{\"jsonrpc\":\"2.0\",\"id\":\"revision-status\",\"method\":\"session.status\","
            "\"params\":{\"session_id\":\"ses-1\"}}";
    EXPECT_NE(std::string::npos,
              server.HandleJsonRpc(status_request).find("\"state_revision\":1"));

    const std::string created = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"checkpoint-create\",\"method\":\"checkpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"label\":\"entry\"}}" );
    EXPECT_NE(std::string::npos, created.find("\"checkpoint_id\":\"checkpoint-1\""));
    EXPECT_NE(std::string::npos, created.find("\"label\":\"entry\""));
    EXPECT_NE(std::string::npos, created.find("\"captured_revision\":1"));
    EXPECT_NE(std::string::npos, created.find("\"byte_count\":10"));
    EXPECT_NE(std::string::npos, created.find("\"sha256\":"));

    const std::string listed = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"checkpoint-list\",\"method\":\"checkpoints.list\","
            "\"params\":{\"session_id\":\"ses-1\"}}" );
    EXPECT_NE(std::string::npos, listed.find("\"checkpoint_id\":\"checkpoint-1\""));
    EXPECT_NE(std::string::npos, listed.find("\"retained_bytes\":10"));

    const std::string restored = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"checkpoint-restore\",\"method\":\"checkpoints.restore\","
            "\"params\":{\"session_id\":\"ses-1\",\"checkpoint_id\":\"checkpoint-1\"}}" );
    EXPECT_NE(std::string::npos, restored.find("\"kind\":\"checkpoint_restore\""));
    EXPECT_NE(std::string::npos, restored.find("\"state_revision\":2"));
    EXPECT_NE(std::string::npos, restored.find("\"segment\":\"0x1000\""));
    EXPECT_NE(std::string::npos, restored.find("\"instruction_pointer\":\"0x00000100\""));

    const std::string refreshed_status = server.HandleJsonRpc(status_request);
    EXPECT_NE(std::string::npos, refreshed_status.find("\"state_revision\":2"));

    const std::string deleted = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"checkpoint-delete\",\"method\":\"checkpoints.delete\","
            "\"params\":{\"session_id\":\"ses-1\",\"checkpoint_id\":\"checkpoint-1\"}}" );
    EXPECT_NE(std::string::npos, deleted.find("\"deleted\":true"));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"checkpoint-missing\",\"method\":\"checkpoints.restore\","
            "\"params\":{\"session_id\":\"ses-1\",\"checkpoint_id\":\"checkpoint-1\"}}" ).find(
                    "CHECKPOINT_NOT_FOUND"));
}

TEST(AgentTrace, PreservesOrderedTypedEffectsThroughPaging)
{
    dosbox_agent::TraceStore trace(4);
    trace.Begin("normal");
    dosbox_agent::TraceSample sample;
    sample.emulated_time_ns = 123456789;
    sample.address.space = dosbox_agent::MemorySpace::Segmented;
    sample.address.segment = 0x1234;
    sample.address.offset = 0x0100;

    dosbox_agent::TraceEffect memory;
    memory.kind = dosbox_agent::TraceEffectKind::MemoryWrite;
    memory.address.space = dosbox_agent::MemorySpace::Linear;
    memory.address.offset = 0x12540;
    memory.byte_count = 2;
    memory.before.push_back(0x00);
    memory.before.push_back(0x00);
    memory.after.push_back(0x34);
    memory.after.push_back(0x12);
    sample.effects.push_back(memory);

    dosbox_agent::TraceEffect io;
    io.kind = dosbox_agent::TraceEffectKind::IoWrite;
    io.port = 0x0080;
    io.byte_count = 1;
    io.value = 0x34;
    sample.effects.push_back(io);

    trace.Merge(std::vector<dosbox_agent::TraceSample>(1, sample));
    dosbox_agent::TracePage page;
    bool expired = false;
    ASSERT_TRUE(trace.Read(false, 0, 4, &page, &expired));
    ASSERT_FALSE(expired);
    ASSERT_EQ(1U, page.events.size());
    EXPECT_EQ(123456789U, page.events[0].sample.emulated_time_ns);
    ASSERT_EQ(2U, page.events[0].sample.effects.size());
    EXPECT_EQ(dosbox_agent::TraceEffectKind::MemoryWrite,
              page.events[0].sample.effects[0].kind);
    EXPECT_EQ(0x12540U, page.events[0].sample.effects[0].address.offset);
    EXPECT_EQ(0x34, page.events[0].sample.effects[0].after[0]);
    EXPECT_EQ(dosbox_agent::TraceEffectKind::IoWrite,
              page.events[0].sample.effects[1].kind);
    EXPECT_EQ(0x0080, page.events[0].sample.effects[1].port);
    EXPECT_EQ(0x34U, page.events[0].sample.effects[1].value);
}

TEST(AgentHardwareTrace, BoundsFiltersPagesAndExpiresOverwrittenCursors)
{
    dosbox_agent::HardwareTraceConfig config;
    config.capacity = 2;
    config.include_io = true;
    config.include_irq = true;
    dosbox_agent::HardwarePortRange port;
    port.first = 0x80;
    port.last = 0x80;
    config.ports.push_back(port);
    config.irqs.push_back(1);
    ASSERT_TRUE(dosbox_agent::AGENT_HardwareTraceStart(config));

    dosbox_agent::AGENT_HardwareTraceObserveIo(true, 0x81, 1, 0xff, 0x1000, 0x0100);
    dosbox_agent::AGENT_HardwareTraceObserveIo(true, 0x80, 1, 0x11, 0x1000, 0x0101);
    dosbox_agent::AGENT_HardwareTraceObserveIrq(dosbox_agent::HardwareTraceEventKind::IrqRaise,
                                  1, 0, 0x1000, 0x0102);
    dosbox_agent::AGENT_HardwareTraceObserveIrq(dosbox_agent::HardwareTraceEventKind::IrqDispatch,
                                  1, 9, 0x1000, 0x0103);

    dosbox_agent::HardwareTracePage page;
    bool expired = false;
    ASSERT_TRUE(dosbox_agent::AGENT_HardwareTraceRead(false, 0, 1, &page, &expired));
    ASSERT_FALSE(expired);
    ASSERT_EQ(1u, page.dropped_event_count);
    ASSERT_EQ(2u, page.first_available_sequence);
    ASSERT_EQ(1u, page.events.size());
    EXPECT_EQ(2u, page.events[0].sequence);
    EXPECT_EQ(dosbox_agent::HardwareTraceEventKind::IrqRaise, page.events[0].kind);
    ASSERT_TRUE(page.has_next_cursor);
    EXPECT_EQ(2u, page.next_cursor);

    dosbox_agent::HardwareTracePage second;
    ASSERT_TRUE(dosbox_agent::AGENT_HardwareTraceRead(true, page.next_cursor, 1, &second, &expired));
    ASSERT_EQ(1u, second.events.size());
    EXPECT_EQ(3u, second.events[0].sequence);
    EXPECT_EQ(9u, second.events[0].vector);
    EXPECT_LE(page.events[0].emulated_time_ns, second.events[0].emulated_time_ns);

    dosbox_agent::HardwareTracePage unavailable;
    EXPECT_FALSE(dosbox_agent::AGENT_HardwareTraceRead(true, 0, 1, &unavailable, &expired));
    EXPECT_TRUE(expired);

    dosbox_agent::HardwareTracePage stopped;
    ASSERT_TRUE(dosbox_agent::AGENT_HardwareTraceStop(&stopped));
    EXPECT_FALSE(stopped.active);
    EXPECT_EQ(1u, stopped.dropped_event_count);
}

TEST(AgentDosFileTrace, FiltersPspHashesPayloadAndBoundsPages)
{
    dosbox_agent::DosFileTraceConfig config;
    config.capacity = 2;
    config.payload_preview_bytes = 2;
    config.target_psp = 0x1234;
    ASSERT_TRUE(dosbox_agent::AGENT_DosFileTraceStart(config));

    const std::uint8_t payload[] = {'a', 'b', 'c'};
    dosbox_agent::DosFileTraceEvent ignored;
    ignored.kind = dosbox_agent::DosFileTraceEventKind::Read;
    ignored.target_psp = 0x9999;
    dosbox_agent::AGENT_DosFileTraceObserve(ignored, payload, sizeof(payload));

    for (unsigned int index = 0; index < 3; ++index) {
        dosbox_agent::DosFileTraceEvent event;
        event.kind = dosbox_agent::DosFileTraceEventKind::Read;
        event.target_psp = 0x1234;
        event.handle = static_cast<std::uint16_t>(index + 1);
        event.requested_count = sizeof(payload);
        event.actual_count = sizeof(payload);
        event.success = true;
        dosbox_agent::AGENT_DosFileTraceObserve(event, payload, sizeof(payload));
    }

    dosbox_agent::DosFileTracePage page;
    bool expired = false;
    ASSERT_TRUE(dosbox_agent::AGENT_DosFileTraceRead(false, 0, 1, &page, &expired));
    ASSERT_FALSE(expired);
    EXPECT_EQ(1u, page.dropped_event_count);
    EXPECT_EQ(2u, page.first_available_sequence);
    ASSERT_EQ(1u, page.events.size());
    EXPECT_EQ(2u, page.events[0].sequence);
    EXPECT_EQ(2u, page.events[0].correlation_id);
    EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              page.events[0].payload_sha256);
    ASSERT_EQ(2u, page.events[0].payload_preview.size());
    EXPECT_EQ('a', page.events[0].payload_preview[0]);
    EXPECT_EQ('b', page.events[0].payload_preview[1]);
    EXPECT_TRUE(page.events[0].payload_truncated);
    EXPECT_TRUE(page.has_next_cursor);

    dosbox_agent::DosFileTracePage second;
    ASSERT_TRUE(dosbox_agent::AGENT_DosFileTraceRead(
            true, page.next_cursor, 1, &second, &expired));
    ASSERT_EQ(1u, second.events.size());
    EXPECT_EQ(3u, second.events[0].sequence);
    EXPECT_LE(page.events[0].emulated_time_ns, second.events[0].emulated_time_ns);

    dosbox_agent::DosFileTracePage unavailable;
    EXPECT_FALSE(dosbox_agent::AGENT_DosFileTraceRead(true, 0, 1, &unavailable, &expired));
    EXPECT_TRUE(expired);

    dosbox_agent::DosFileTracePage stopped;
    ASSERT_TRUE(dosbox_agent::AGENT_DosFileTraceStop(&stopped));
    EXPECT_FALSE(stopped.active);
    EXPECT_EQ(1u, stopped.dropped_event_count);
}

TEST(AgentDosFileTrace, RpcStartReadAndStopUseTargetPsp)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string start = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"dos-start\",\"method\":\"dos.trace.start\","
            "\"params\":{\"session_id\":\"ses-1\",\"capacity\":4,\"payload_preview_bytes\":2}}");
    EXPECT_NE(std::string::npos, start.find("\"active\":true"));
    EXPECT_NE(std::string::npos, start.find("\"target_psp\":\"0x1000\""));

    const std::string read = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"dos-read\",\"method\":\"dos.trace.read\","
            "\"params\":{\"session_id\":\"ses-1\",\"cursor\":null,\"limit\":4}}");
    EXPECT_NE(std::string::npos, read.find("\"events\":[]"));
    EXPECT_NE(std::string::npos, read.find("\"payload_preview_bytes\":2"));

    const std::string stop = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"dos-stop\",\"method\":\"dos.trace.stop\","
            "\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, stop.find("\"active\":false"));
    server.Stop();
}

TEST(AgentDos, ReturnsLoaderMetadataAndTypedMcbOwnership)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string response = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"dos-map\",\"method\":\"dos.memory_map\","
            "\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, response.find("\"current_psp\":\"0x1000\""));
    EXPECT_NE(std::string::npos, response.find("\"load_segment\":\"0x1010\""));
    EXPECT_NE(std::string::npos, response.find("\"format\":\"com\""));
    EXPECT_NE(std::string::npos, response.find("\"image_bytes\":25"));
    EXPECT_NE(std::string::npos, response.find("\"target_owned\":true"));
    EXPECT_NE(std::string::npos, response.find("\"environment_segment\":\"0x0F00\""));
}

TEST(AgentProtocol, ValidatesExactWatchpointKindsAndLengthsBeforeDispatch)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string invalid_execution = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"execution-length\",\"method\":\"breakpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"kind\":\"execution\",\"length\":2,"
            "\"address\":{\"space\":\"segmented\",\"segment\":\"0x1000\",\"offset\":\"0x00000100\"}}}");
    EXPECT_NE(std::string::npos, invalid_execution.find("\"code\":-32602"));
    EXPECT_NE(std::string::npos, invalid_execution.find("require length 1"));

    const std::string empty_watch = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"empty-watch\",\"method\":\"breakpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"kind\":\"memory_read\",\"length\":0,"
            "\"address\":{\"space\":\"linear\",\"offset\":\"0x00010200\"}}}");
    EXPECT_NE(std::string::npos, empty_watch.find("\"code\":-32602"));

    const std::string oversized_watch = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"large-watch\",\"method\":\"breakpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"kind\":\"memory_access\",\"length\":65,"
            "\"address\":{\"space\":\"linear\",\"offset\":\"0x00010200\"}}}");
    EXPECT_NE(std::string::npos, oversized_watch.find("REQUEST_TOO_LARGE"));

    const std::string invalid_condition = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"bad-condition\",\"method\":\"breakpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"kind\":\"execution\","
            "\"condition\":{\"register\":\"AX\",\"operator\":\"eq\",\"value\":\"0x00000001\"},"
            "\"address\":{\"space\":\"segmented\",\"segment\":\"0x1000\",\"offset\":\"0x00000100\"}}}");
    EXPECT_NE(std::string::npos, invalid_condition.find("\"code\":-32602"));
    EXPECT_NE(std::string::npos, invalid_condition.find("supported lowercase register"));

    const std::string invalid_filter = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"bad-filter\",\"method\":\"breakpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"kind\":\"execution\","
            "\"hit_filter\":{\"skip\":0,\"every\":0},"
            "\"address\":{\"space\":\"segmented\",\"segment\":\"0x1000\",\"offset\":\"0x00000100\"}}}");
    EXPECT_NE(std::string::npos, invalid_filter.find("\"code\":-32602"));
    EXPECT_NE(std::string::npos, invalid_filter.find("hit_filter.every must be positive"));

    const std::string watch_condition = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"watch-condition\",\"method\":\"breakpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"kind\":\"memory_write\","
            "\"condition\":{\"register\":\"ax\",\"operator\":\"eq\",\"value\":\"0x00000001\"},"
            "\"address\":{\"space\":\"linear\",\"offset\":\"0x00010200\"}}}");
    EXPECT_NE(std::string::npos, watch_condition.find("\"code\":-32602"));
    EXPECT_NE(std::string::npos, watch_condition.find("only on execution and interrupt breakpoints"));
}

TEST(AgentProtocol, CreatesAndListsTypedSoftwareInterruptBreakpoints)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string missing_event = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"missing-event\",\"method\":\"breakpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"kind\":\"interrupt\"}}");
    EXPECT_NE(std::string::npos, missing_event.find("\"code\":-32602"));
    EXPECT_NE(std::string::npos, missing_event.find("requires event as an object"));

    const std::string malformed_filter = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"bad-event\",\"method\":\"breakpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"kind\":\"interrupt\","
            "\"event\":{\"type\":\"software_interrupt\",\"number\":\"0x21\",\"ah\":\"0x4C00\"}}}");
    EXPECT_NE(std::string::npos, malformed_filter.find("\"code\":-32602"));
    EXPECT_NE(std::string::npos, malformed_filter.find("optional ah/al 0xNN"));

    const std::string mixed_schema = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"mixed\",\"method\":\"breakpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"kind\":\"interrupt\","
            "\"address\":{\"space\":\"segmented\",\"segment\":\"0x1000\",\"offset\":\"0x00000100\"},"
            "\"event\":{\"type\":\"software_interrupt\",\"number\":\"0x21\"}}}");
    EXPECT_NE(std::string::npos, mixed_schema.find("\"code\":-32602"));
    EXPECT_NE(std::string::npos, mixed_schema.find("event instead of address/length"));

    const std::string created = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"create-int\",\"method\":\"breakpoints.create\","
            "\"params\":{\"session_id\":\"ses-1\",\"kind\":\"interrupt\",\"once\":true,"
            "\"condition\":{\"register\":\"bx\",\"operator\":\"ne\",\"value\":\"0x00001234\"},"
            "\"hit_filter\":{\"skip\":2,\"every\":3},"
            "\"event\":{\"type\":\"software_interrupt\",\"number\":\"0x21\",\"ah\":\"0x4C\"}}}");
    EXPECT_NE(std::string::npos, created.find("\"breakpoint_id\":\"bp-1\""));
    EXPECT_NE(std::string::npos, created.find("\"kind\":\"interrupt\""));
    EXPECT_NE(std::string::npos, created.find("\"event\":{\"ah\":\"0x4C\",\"number\":\"0x21\",\"type\":\"software_interrupt\"}"));
    EXPECT_EQ(std::string::npos, created.find("\"address\""));
    EXPECT_EQ(std::string::npos, created.find("\"length\""));

    const std::string listed = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"list-int\",\"method\":\"breakpoints.list\","
            "\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, listed.find("\"breakpoint_id\":\"bp-1\""));
    EXPECT_NE(std::string::npos, listed.find("\"skip\":2"));
    EXPECT_NE(std::string::npos, listed.find("\"every\":3"));
    EXPECT_EQ(std::string::npos, listed.find("\"address\""));
}

TEST(AgentVideo, ReturnsOneAtomicTypedSnapshotAndRejectsRunningTargets)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string snapshot = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"video\",\"method\":\"video.snapshot\",\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, snapshot.find("\"captured_ticks\":42"));
    EXPECT_NE(std::string::npos, snapshot.find("\"video_mode\":3"));
    EXPECT_NE(std::string::npos, snapshot.find("\"snapshot_id\":\"shot-1\""));
    EXPECT_EQ(std::string::npos, snapshot.find("data_base64"));
    EXPECT_NE(std::string::npos, snapshot.find("\"kind\":\"renderer_source_cache\""));
    EXPECT_NE(std::string::npos, snapshot.find("\"columns\":80"));

    const std::string text = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"video-text\",\"method\":\"video.snapshot.read\","
            "\"params\":{\"session_id\":\"ses-1\",\"snapshot_id\":\"shot-1\",\"component\":\"text\",\"offset\":0,\"length\":2}}");
    EXPECT_NE(std::string::npos, text.find("\"data_base64\":\"QR8=\""));
    EXPECT_NE(std::string::npos, text.find("\"eof\":true"));

    const std::string second_snapshot = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"video-2\",\"method\":\"video.snapshot\",\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, second_snapshot.find("\"snapshot_id\":\"shot-2\""));
    const std::string retried_snapshot = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"video\",\"method\":\"video.snapshot\",\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_EQ(snapshot, retried_snapshot);
    const std::string retained_text = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"video-text-retained\",\"method\":\"video.snapshot.read\","
            "\"params\":{\"session_id\":\"ses-1\",\"snapshot_id\":\"shot-1\",\"component\":\"text\",\"offset\":0,\"length\":2}}");
    EXPECT_NE(std::string::npos, retained_text.find("\"data_base64\":\"QR8=\""));

    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"continue-video\",\"method\":\"execution.continue\",\"params\":{\"session_id\":\"ses-1\"}}").find("\"state\":\"running\""));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"video-running\",\"method\":\"video.snapshot\",\"params\":{\"session_id\":\"ses-1\"}}").find("TARGET_RUNNING"));
}

TEST(AgentLifecycle, StopsAndRestartsTheSameServer)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    EXPECT_TRUE(server.IsStarted());

    server.Stop();
    EXPECT_FALSE(server.IsStarted());

    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    EXPECT_TRUE(server.IsStarted());
    server.Stop();
    EXPECT_FALSE(server.IsStarted());
}

TEST(AgentSession, EnforcesSingleSessionAndStartupStopState)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string status = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"status\",\"method\":\"session.status\",\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, status.find("\"state\":\"stopped\""));
    EXPECT_NE(std::string::npos, status.find("\"state_revision\":1"));

    const std::string second_start = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"second\",\"method\":\"session.start\","
            "\"params\":{\"target\":{\"command\":\"SECOND.COM\"},\"mounts\":[{\"drive\":\"C\",\"host_path\":\"tests/agent/runtime\"}],\"break_at\":\"entry\"}}");
    EXPECT_NE(std::string::npos, second_start.find("SESSION_BUSY"));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"status-after\",\"method\":\"session.status\",\"params\":{\"session_id\":\"ses-1\"}}").find("\"state_revision\":1"));
}

TEST(AgentSession, ReusesCompletedStartRequestAndRejectsConflicts)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;

    const std::string request =
            "{\"jsonrpc\":\"2.0\",\"id\":\"start\",\"method\":\"session.start\","
            "\"params\":{\"target\":{\"command\":\"AGENTFIX.COM\",\"arguments\":[]},"
            "\"mounts\":[{\"drive\":\"C\",\"host_path\":\"tests/agent/runtime\"}],\"break_at\":\"entry\"}}";
    const std::string first = server.HandleJsonRpc(request);
    const std::string retry = server.HandleJsonRpc(request);
    EXPECT_EQ(first, retry);
    EXPECT_NE(std::string::npos, retry.find("\"session_id\":\"ses-1\""));

    const std::string conflict = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"start\",\"method\":\"session.start\","
            "\"params\":{\"target\":{\"command\":\"OTHER.COM\",\"arguments\":[]},"
            "\"mounts\":[{\"drive\":\"C\",\"host_path\":\"tests/agent/runtime\"}],\"break_at\":\"entry\"}}");
    EXPECT_NE(std::string::npos, conflict.find("REQUEST_ID_CONFLICT"));
}

TEST(AgentSession, BlocksStateOperationsWhileRunningAndWaitsForPause)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string continued = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"continue\",\"method\":\"execution.continue\",\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, continued.find("\"operation_id\":\"op-1\""));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"registers\",\"method\":\"state.get_registers\",\"params\":{\"session_id\":\"ses-1\"}}").find("TARGET_RUNNING"));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"read\",\"method\":\"memory.read\",\"params\":{\"session_id\":\"ses-1\"}}").find("TARGET_RUNNING"));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"write\",\"method\":\"memory.write\",\"params\":{\"session_id\":\"ses-1\"}}").find("TARGET_RUNNING"));

    const std::string timed_out = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"wait\",\"method\":\"execution.wait\",\"params\":{\"session_id\":\"ses-1\",\"operation_id\":\"op-1\",\"timeout_ms\":1}}");
    EXPECT_NE(std::string::npos, timed_out.find("\"running\":true"));
    EXPECT_EQ(std::string::npos, timed_out.find("stop_reason"));

    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"pause\",\"method\":\"execution.pause\",\"params\":{\"session_id\":\"ses-1\"}}").find("\"operation_id\":\"op-2\""));
    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"wait-pause\",\"method\":\"execution.wait\",\"params\":{\"session_id\":\"ses-1\",\"operation_id\":\"op-2\",\"timeout_ms\":1}}").find("\"kind\":\"pause\""));
}

TEST(AgentSession, ReportsOnlyTheTargetPspAsAProgramExit)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string continued = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"continue-exit\",\"method\":\"execution.continue\",\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, continued.find("\"operation_id\":\"op-1\""));

    dosbox_agent::AGENT_NotifyProgramExited(0x2000, 7, false);
    EXPECT_NE(std::string::npos, server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"wrong-psp\",\"method\":\"session.status\",\"params\":{\"session_id\":\"ses-1\"}}").find("\"state\":\"running\""));

    dosbox_agent::AGENT_NotifyProgramExited(0x1000, 42, false);
    const std::string exited = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"wait-exit\",\"method\":\"execution.wait\",\"params\":{\"session_id\":\"ses-1\",\"operation_id\":\"op-1\",\"timeout_ms\":1}}");
    EXPECT_NE(std::string::npos, exited.find("\"state\":\"exited\""));
    EXPECT_NE(std::string::npos, exited.find("\"kind\":\"program_exit\""));
    EXPECT_NE(std::string::npos, exited.find("\"psp\":4096"));
    EXPECT_NE(std::string::npos, exited.find("\"exit_code\":42"));
    EXPECT_NE(std::string::npos, exited.find("\"tsr\":false"));
}

TEST(AgentSession, LabelsControllerTerminationAsSessionStop)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string stopped = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"stop\",\"method\":\"session.stop\",\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, stopped.find("\"operation_id\":\"op-1\""));
    const std::string waited = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"wait-stop\",\"method\":\"execution.wait\",\"params\":{\"session_id\":\"ses-1\",\"operation_id\":\"op-1\",\"timeout_ms\":1000}}");
    EXPECT_NE(std::string::npos, waited.find("\"state\":\"exited\""));
    EXPECT_NE(std::string::npos, waited.find("\"kind\":\"session_stop\""));
    EXPECT_EQ(std::string::npos, waited.find("\"kind\":\"program_exit\""));
}

TEST(AgentSession, ReusesCompletedRequestResultsAndRejectsConflicts)
{
    dosbox_agent::AgentServer server;
    std::string error;
    ASSERT_TRUE(server.StartForTest(MakeTestConfig(), &error)) << error;
    StartFixtureSession(&server);

    const std::string request =
            "{\"jsonrpc\":\"2.0\",\"id\":\"operation\",\"method\":\"execution.continue\",\"params\":{\"session_id\":\"ses-1\"}}";
    const std::string first = server.HandleJsonRpc(request);
    const std::string retry = server.HandleJsonRpc(request);
    EXPECT_EQ(first, retry);
    EXPECT_NE(std::string::npos, retry.find("\"operation_id\":\"op-1\""));

    const std::string conflict = server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":\"operation\",\"method\":\"execution.pause\",\"params\":{\"session_id\":\"ses-1\"}}");
    EXPECT_NE(std::string::npos, conflict.find("REQUEST_ID_CONFLICT"));
}

} // namespace
#endif
