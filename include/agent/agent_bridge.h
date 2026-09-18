#ifndef DOSBOX_AGENT_BRIDGE_H
#define DOSBOX_AGENT_BRIDGE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace dosbox_agent {

class EmulationThreadQueue {
public:
    typedef std::function<void(std::uint64_t)> Command;

    EmulationThreadQueue();
    ~EmulationThreadQueue();

    EmulationThreadQueue(const EmulationThreadQueue&) = delete;
    EmulationThreadQueue& operator=(const EmulationThreadQueue&) = delete;

    bool BindToCurrentThread();
    bool IsBoundToCurrentThread() const;
    std::uint64_t Submit(Command command);
    std::uint64_t SubmitAfter(std::uint32_t delay_ms, Command command);
    std::size_t Pump();
    void Shutdown();

private:
    class Impl;
    Impl* impl;
};

typedef std::function<void(std::uint16_t segment, std::uint32_t instruction_pointer)> DebuggerStopListener;
typedef std::function<void(std::uint16_t psp, std::uint8_t exit_code, bool tsr)> ProgramExitListener;
struct ProgramLoadInfo {
    std::string name;
    std::uint16_t psp = 0;
    std::uint16_t load_segment = 0;
    bool com = false;
    std::uint32_t image_bytes = 0;
    std::uint16_t entry_cs = 0;
    std::uint16_t entry_ip = 0;
    std::uint16_t initial_ss = 0;
    std::uint16_t initial_sp = 0;
};
typedef std::function<void(const ProgramLoadInfo& info)> ProgramLoadListener;

EmulationThreadQueue& AGENT_EmulationQueue();
void AGENT_BridgeAttachToCurrentThread();
std::size_t AGENT_BridgePump();
void AGENT_BridgeShutdown();
void AGENT_SetDebuggerStopListener(DebuggerStopListener listener);
void AGENT_NotifyDebuggerStopped(std::uint16_t segment, std::uint32_t instruction_pointer);
void AGENT_SetProgramExitListener(ProgramExitListener listener);
void AGENT_NotifyProgramExited(std::uint16_t psp, std::uint8_t exit_code, bool tsr);
void AGENT_SetProgramLoadListener(ProgramLoadListener listener);
void AGENT_NotifyProgramLoaded(const ProgramLoadInfo& info);
bool AGENT_RunQueueSelfTest(std::string* error);

} // namespace dosbox_agent

#endif
