#ifndef DOSBOX_AGENT_DEBUGGER_ADAPTER_H
#define DOSBOX_AGENT_DEBUGGER_ADAPTER_H

#include <cstdint>
#include <string>
#include <vector>

namespace dosbox_agent {

enum class MemorySpace {
    Segmented,
    Linear,
    Physical
};

struct SegmentedMemoryAddress {
    std::uint16_t segment = 0;
    std::uint32_t offset = 0;
};

struct MemoryAddress {
    MemorySpace space = MemorySpace::Segmented;
    std::uint16_t segment = 0;
    std::uint32_t offset = 0;
};

struct MemoryAccessError {
    std::string reason;
    std::uint32_t failing_offset = 0;
};

struct RegisterSnapshot {
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;
    std::uint32_t esi = 0;
    std::uint32_t edi = 0;
    std::uint32_t ebp = 0;
    std::uint32_t esp = 0;
    std::uint16_t cs = 0;
    std::uint16_t ds = 0;
    std::uint16_t es = 0;
    std::uint16_t fs = 0;
    std::uint16_t gs = 0;
    std::uint16_t ss = 0;
    std::uint32_t instruction_pointer = 0;
    std::uint32_t flags = 0;
    std::string cpu_mode;
};

enum class TraceEffectKind {
    MemoryRead,
    MemoryWrite,
    IoRead,
    IoWrite
};

struct TraceEffect {
    TraceEffectKind kind = TraceEffectKind::MemoryRead;
    MemoryAddress address;
    std::uint16_t port = 0;
    std::uint8_t byte_count = 0;
    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> after;
    std::uint32_t value = 0;
};

struct TraceSample {
    MemoryAddress address;
    RegisterSnapshot registers;
    std::string instruction;
    std::string analysis;
    std::vector<TraceEffect> effects;
};

struct VideoSnapshot {
    std::uint8_t video_mode = 0;
    std::uint64_t ticks = 0;
    std::uint32_t text_columns = 0;
    std::uint32_t glyph_height = 0;
    std::uint32_t text_offset = 0;
    std::uint32_t font_stride = 32;
    bool char9dot = false;
    std::uint32_t blinking = 0;
    bool blink_phase = false;
    std::uint8_t attr_mode_control = 0;
    std::uint8_t underline_location = 0;
    std::uint32_t panning = 0;
    std::uint32_t draw_address = 0;
    std::uint32_t frame_width = 0;
    std::uint32_t frame_height = 0;
    std::uint32_t frame_bpp = 0;
    std::uint32_t frame_pitch = 0;
    bool frame_dblw = false;
    bool frame_dblh = false;
    std::uint8_t dac_bits = 0;
    std::uint8_t dac_pel_mask = 0;
    std::vector<std::uint8_t> text;
    std::vector<std::uint8_t> font;
    std::vector<std::uint8_t> dac_palette;
    std::vector<std::uint8_t> renderer_palette;
    std::vector<std::uint8_t> frame;
    std::vector<std::uint8_t> crtc;
};

enum class StepMode {
    Into,
    Over
};

enum class BreakpointKind {
    Execution,
    Interrupt,
    MemoryChange,
    MemoryRead,
    MemoryWrite,
    MemoryAccess
};

struct InterruptBreakpointSelector {
    std::uint8_t number = 0;
    bool has_ah = false;
    std::uint8_t ah = 0;
    bool has_al = false;
    std::uint8_t al = 0;
};

struct InterruptEvent {
    bool valid = false;
    bool software = false;
    std::uint8_t number = 0;
    std::uint8_t ah = 0;
    std::uint8_t al = 0;
};

struct BreakpointCondition {
    bool enabled = false;
    std::string register_name;
    bool equal = true;
    std::uint32_t value = 0;
};

struct BreakpointHitFilter {
    std::uint32_t skip = 0;
    std::uint32_t every = 1;
};

struct NativeBreakpoint {
    std::uintptr_t handle = 0;
    BreakpointKind kind = BreakpointKind::Execution;
    MemoryAddress address;
    InterruptBreakpointSelector interrupt;
    std::uint32_t length = 1;
    bool once = false;
    BreakpointCondition condition;
    BreakpointHitFilter hit_filter;
};

struct BreakpointHit {
    std::uintptr_t handle = 0;
    std::uint64_t hit_count = 0;
    InterruptEvent interrupt;
};

struct WatchpointHit {
    std::uintptr_t handle = 0;
    bool write = false;
    MemoryAddress address;
    MemoryAddress instruction_address;
    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> after;
};

struct DosMemoryBlock {
    std::uint16_t mcb_segment = 0;
    std::uint16_t data_segment = 0;
    std::uint16_t paragraphs = 0;
    std::uint16_t owner_psp = 0;
    std::string name;
    bool last = false;
    bool process = false;
    std::uint16_t parent_psp = 0;
    std::uint16_t environment_segment = 0;
};

struct DosMemoryMap {
    std::uint16_t current_psp = 0;
    std::uint16_t first_mcb = 0;
    std::vector<DosMemoryBlock> blocks;
};

class DebuggerAdapter {
public:
    bool IsAvailable() const;
    bool RequireAvailable(std::string* error) const;
    bool IsShellReady() const;
    bool IsReadyForTargetStart() const;
    std::uint16_t CurrentPsp() const;
    std::uint64_t EntryBreakpointSequence() const;
    bool Continue(std::string* error) const;
    bool Pause(std::string* error) const;
    bool StartTargetAtEntry(const std::string& command,
                            const std::vector<std::string>& arguments,
                            const std::string& workdir,
                            std::string* error) const;
    bool GetRegisters(RegisterSnapshot* registers, std::string* error) const;
    bool CaptureVideoSnapshot(VideoSnapshot* snapshot, std::string* error) const;
    bool Step(StepMode mode, bool* continued, std::string* error) const;
    bool ReadMemory(const MemoryAddress& address,
                    std::size_t length,
                    std::vector<std::uint8_t>* data,
                    MemoryAccessError* access_error,
                    std::string* error) const;
    bool WriteMemory(const MemoryAddress& address,
                     const std::vector<std::uint8_t>& data,
                     std::vector<std::uint8_t>* after,
                     MemoryAccessError* access_error,
                     std::string* error) const;
    bool CreateBreakpoint(BreakpointKind kind,
                          const MemoryAddress& address,
                          std::uint32_t length,
                          bool once,
                          const BreakpointCondition& condition,
                          const BreakpointHitFilter& hit_filter,
                          NativeBreakpoint* breakpoint,
                          MemoryAccessError* access_error,
                          std::string* error) const;
    bool CreateInterruptBreakpoint(const InterruptBreakpointSelector& selector,
                                   bool once,
                                   const BreakpointCondition& condition,
                                   const BreakpointHitFilter& hit_filter,
                                   NativeBreakpoint* breakpoint,
                                   std::string* error) const;
    bool DeleteBreakpoint(const NativeBreakpoint& breakpoint, std::string* error) const;
    bool ConsumeLastBreakpointHit(BreakpointHit* hit) const;
    bool ConsumeLastWatchpointHit(WatchpointHit* hit) const;
    bool GetDosMemoryMap(DosMemoryMap* memory_map, std::string* error) const;
    bool ExecuteDiagnosticCommand(const std::string& command,
                                  std::string* raw_output,
                                  std::string* error) const;
    bool StartTrace(const std::string& detail, std::uint32_t instruction_count, std::string* error) const;
    bool ReadTrace(std::vector<TraceSample>* samples, bool* active, std::string* error) const;
    bool StopTrace(std::size_t* event_count, std::string* error) const;
    bool IsTraceComplete() const;
    bool TerminateTarget(std::string* error) const;
};

} // namespace dosbox_agent

#endif
