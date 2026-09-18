#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "agent/hardware_trace.h"

#include "pic.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace dosbox_agent {
namespace {

struct Recorder {
    bool active = false;
    HardwareTraceConfig config;
    std::uint64_t next_sequence = 1;
    std::uint64_t dropped = 0;
    std::deque<HardwareTraceEvent> events;
};

Recorder recorder;

bool IncludesPort(const std::uint16_t port)
{
    if (recorder.config.ports.empty())
        return true;
    for (std::vector<HardwarePortRange>::const_iterator it = recorder.config.ports.begin();
         it != recorder.config.ports.end(); ++it) {
        if (port >= it->first && port <= it->last)
            return true;
    }
    return false;
}

bool IncludesIrq(const std::uint8_t irq)
{
    return recorder.config.irqs.empty() ||
           std::find(recorder.config.irqs.begin(), recorder.config.irqs.end(), irq) !=
                   recorder.config.irqs.end();
}

void Append(HardwareTraceEvent event)
{
    event.sequence = recorder.next_sequence++;
    event.emulated_time_ns = AGENT_EmulatedTimeNs();
    recorder.events.push_back(event);
    while (recorder.events.size() > recorder.config.capacity) {
        recorder.events.pop_front();
        ++recorder.dropped;
    }
}

void FillStatus(HardwareTracePage* status)
{
    status->active = recorder.active;
    status->capacity = recorder.config.capacity;
    status->dropped_event_count = recorder.dropped;
    status->first_available_sequence = recorder.events.empty() ?
            recorder.next_sequence : recorder.events.front().sequence;
}

} // namespace

std::uint64_t AGENT_EmulatedTimeNs()
{
    return static_cast<std::uint64_t>(std::llround(PIC_FullIndex() * 1000000.0));
}

bool AGENT_HardwareTraceStart(const HardwareTraceConfig& config)
{
    if (recorder.active || config.capacity == 0 || (!config.include_io && !config.include_irq))
        return false;
    recorder = Recorder();
    recorder.active = true;
    recorder.config = config;
    return true;
}

bool AGENT_HardwareTraceRead(const bool has_cursor,
                             const std::uint64_t cursor,
                             const std::size_t limit,
                             HardwareTracePage* page,
                             bool* cursor_expired)
{
    if (page == NULL || cursor_expired == NULL || limit == 0 || recorder.config.capacity == 0)
        return false;
    *page = HardwareTracePage();
    FillStatus(page);
    *cursor_expired = false;
    const std::uint64_t first = page->first_available_sequence;
    const std::uint64_t last = recorder.next_sequence - 1;
    if (has_cursor && (cursor + 1 < first || cursor > last)) {
        *cursor_expired = true;
        return false;
    }
    const std::uint64_t requested = has_cursor ? cursor + 1 : first;
    for (std::deque<HardwareTraceEvent>::const_iterator it = recorder.events.begin();
         it != recorder.events.end() && page->events.size() < limit; ++it) {
        if (it->sequence >= requested)
            page->events.push_back(*it);
    }
    if (!page->events.empty() && page->events.back().sequence < last) {
        page->has_next_cursor = true;
        page->next_cursor = page->events.back().sequence;
    }
    return true;
}

bool AGENT_HardwareTraceStop(HardwareTracePage* status)
{
    if (!recorder.active || status == NULL)
        return false;
    recorder.active = false;
    *status = HardwareTracePage();
    FillStatus(status);
    return true;
}

bool AGENT_HardwareTraceIsActive()
{
    return recorder.active;
}

void AGENT_HardwareTraceObserveIo(const bool write,
                                  const std::uint16_t port,
                                  const std::uint8_t byte_count,
                                  const std::uint32_t value,
                                  const std::uint16_t cs,
                                  const std::uint32_t instruction_pointer)
{
    if (!recorder.active || !recorder.config.include_io || !IncludesPort(port))
        return;
    HardwareTraceEvent event;
    event.kind = write ? HardwareTraceEventKind::IoWrite : HardwareTraceEventKind::IoRead;
    event.address.space = MemorySpace::Segmented;
    event.address.segment = cs;
    event.address.offset = instruction_pointer;
    event.port = port;
    event.byte_count = byte_count;
    event.value = value;
    Append(event);
}

void AGENT_HardwareTraceObserveIrq(const HardwareTraceEventKind kind,
                                   const std::uint8_t irq,
                                   const std::uint8_t vector,
                                   const std::uint16_t cs,
                                   const std::uint32_t instruction_pointer)
{
    if (!recorder.active || !recorder.config.include_irq || !IncludesIrq(irq))
        return;
    HardwareTraceEvent event;
    event.kind = kind;
    event.address.space = MemorySpace::Segmented;
    event.address.segment = cs;
    event.address.offset = instruction_pointer;
    event.irq = irq;
    event.vector = vector;
    Append(event);
}

} // namespace dosbox_agent
#endif
