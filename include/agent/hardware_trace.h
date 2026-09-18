#ifndef DOSBOX_AGENT_HARDWARE_TRACE_H
#define DOSBOX_AGENT_HARDWARE_TRACE_H

#include "agent/debugger_adapter.h"

namespace dosbox_agent {

bool AGENT_HardwareTraceStart(const HardwareTraceConfig& config);
bool AGENT_HardwareTraceRead(bool has_cursor,
                             std::uint64_t cursor,
                             std::size_t limit,
                             HardwareTracePage* page,
                             bool* cursor_expired);
bool AGENT_HardwareTraceStop(HardwareTracePage* status);
bool AGENT_HardwareTraceIsActive();
void AGENT_HardwareTraceObserveIo(bool write,
                                  std::uint16_t port,
                                  std::uint8_t byte_count,
                                  std::uint32_t value,
                                  std::uint16_t cs,
                                  std::uint32_t instruction_pointer);
void AGENT_HardwareTraceObserveIrq(HardwareTraceEventKind kind,
                                   std::uint8_t irq,
                                   std::uint8_t vector,
                                   std::uint16_t cs,
                                   std::uint32_t instruction_pointer);

} // namespace dosbox_agent

#endif
