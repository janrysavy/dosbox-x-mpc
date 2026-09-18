#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "agent/hardware_trace.h"

#include "pic.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iomanip>
#include <sstream>

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

struct DosFileRecorder {
    bool active = false;
    DosFileTraceConfig config;
    std::uint64_t next_sequence = 1;
    std::uint64_t next_correlation_id = 1;
    std::uint64_t dropped = 0;
    std::deque<DosFileTraceEvent> events;
};

DosFileRecorder dos_file_recorder;

std::uint32_t RotateRight(const std::uint32_t value, const unsigned int amount)
{
    return (value >> amount) | (value << (32u - amount));
}

std::string Sha256Hex(const std::uint8_t* data, const std::size_t size)
{
    static const std::uint32_t constants[] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    std::vector<std::uint8_t> padded;
    if (data != NULL && size != 0)
        padded.assign(data, data + size);
    padded.push_back(0x80u);
    while ((padded.size() % 64) != 56)
        padded.push_back(0);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(size) * 8u;
    for (int shift = 56; shift >= 0; shift -= 8)
        padded.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffu));
    std::uint32_t state[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                              0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    for (std::size_t block = 0; block < padded.size(); block += 64) {
        std::uint32_t words[64];
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t o = block + i * 4;
            words[i] = (static_cast<std::uint32_t>(padded[o]) << 24u) |
                       (static_cast<std::uint32_t>(padded[o + 1]) << 16u) |
                       (static_cast<std::uint32_t>(padded[o + 2]) << 8u) |
                       static_cast<std::uint32_t>(padded[o + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = RotateRight(words[i - 15],7) ^ RotateRight(words[i - 15],18) ^ (words[i - 15] >> 3u);
            const std::uint32_t s1 = RotateRight(words[i - 2],17) ^ RotateRight(words[i - 2],19) ^ (words[i - 2] >> 10u);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        std::uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = RotateRight(e,6) ^ RotateRight(e,11) ^ RotateRight(e,25);
            const std::uint32_t t1 = h + s1 + ((e & f) ^ ((~e) & g)) + constants[i] + words[i];
            const std::uint32_t s0 = RotateRight(a,2) ^ RotateRight(a,13) ^ RotateRight(a,22);
            const std::uint32_t t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
        state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
    }
    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t i = 0; i < 8; ++i)
        output << std::setw(8) << state[i];
    return output.str();
}

void FillDosFileStatus(DosFileTracePage* status)
{
    status->active = dos_file_recorder.active;
    status->capacity = dos_file_recorder.config.capacity;
    status->payload_preview_bytes = dos_file_recorder.config.payload_preview_bytes;
    status->target_psp = dos_file_recorder.config.target_psp;
    status->dropped_event_count = dos_file_recorder.dropped;
    status->first_available_sequence = dos_file_recorder.events.empty() ?
            dos_file_recorder.next_sequence : dos_file_recorder.events.front().sequence;
}

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

bool AGENT_DosFileTraceStart(const DosFileTraceConfig& config)
{
    if (dos_file_recorder.active || config.capacity == 0 || config.target_psp == 0)
        return false;
    dos_file_recorder = DosFileRecorder();
    dos_file_recorder.active = true;
    dos_file_recorder.config = config;
    return true;
}

bool AGENT_DosFileTraceRead(const bool has_cursor,
                            const std::uint64_t cursor,
                            const std::size_t limit,
                            DosFileTracePage* page,
                            bool* cursor_expired)
{
    if (page == NULL || cursor_expired == NULL || limit == 0 || dos_file_recorder.config.capacity == 0)
        return false;
    *page = DosFileTracePage();
    FillDosFileStatus(page);
    *cursor_expired = false;
    const std::uint64_t first = page->first_available_sequence;
    const std::uint64_t last = dos_file_recorder.next_sequence - 1;
    if (has_cursor && (cursor + 1 < first || cursor > last)) {
        *cursor_expired = true;
        return false;
    }
    const std::uint64_t requested = has_cursor ? cursor + 1 : first;
    for (std::deque<DosFileTraceEvent>::const_iterator it = dos_file_recorder.events.begin();
         it != dos_file_recorder.events.end() && page->events.size() < limit; ++it) {
        if (it->sequence >= requested)
            page->events.push_back(*it);
    }
    if (!page->events.empty() && page->events.back().sequence < last) {
        page->has_next_cursor = true;
        page->next_cursor = page->events.back().sequence;
    }
    return true;
}

bool AGENT_DosFileTraceStop(DosFileTracePage* status)
{
    if (!dos_file_recorder.active || status == NULL)
        return false;
    dos_file_recorder.active = false;
    *status = DosFileTracePage();
    FillDosFileStatus(status);
    return true;
}

bool AGENT_DosFileTraceIsActive()
{
    return dos_file_recorder.active;
}

void AGENT_DosFileTraceObserve(DosFileTraceEvent event,
                               const std::uint8_t* payload,
                               const std::size_t payload_size)
{
    if (!dos_file_recorder.active || event.target_psp != dos_file_recorder.config.target_psp)
        return;
    event.sequence = dos_file_recorder.next_sequence++;
    event.correlation_id = dos_file_recorder.next_correlation_id++;
    event.emulated_time_ns = AGENT_EmulatedTimeNs();
    if (payload != NULL) {
        event.payload_sha256 = Sha256Hex(payload, payload_size);
        const std::size_t preview_size = std::min(payload_size,
                                                  dos_file_recorder.config.payload_preview_bytes);
        event.payload_preview.assign(payload, payload + preview_size);
        event.payload_truncated = preview_size < payload_size;
    }
    dos_file_recorder.events.push_back(event);
    while (dos_file_recorder.events.size() > dos_file_recorder.config.capacity) {
        dos_file_recorder.events.pop_front();
        ++dos_file_recorder.dropped;
    }
}

} // namespace dosbox_agent
#endif
