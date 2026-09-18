#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "dosbox.h"
#include "agent/debugger_adapter.h"
#include "agent/hardware_trace.h"
#include "agent/agent_bridge.h"

#if C_DEBUG
#include "cpu.h"
#include "debug.h"
#include "dos_inc.h"
#include "dos_mcb.h"
#include "joystick.h"
#include "keyboard.h"
#include "mem.h"
#include "paging.h"
#include "pic.h"
#include "render.h"
#include "shell.h"
#include "vga.h"

extern bool ParseCommand(char* str);
extern char appname[];
extern char appargs[];
extern bool dos_program_running;
extern void runMount(const char* arguments);
extern std::string full_arguments;
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace dosbox_agent {

namespace {

#if C_DEBUG
static const KBD_KEYS kKeyboardKeys[] = {
    KBD_1, KBD_2, KBD_3, KBD_4, KBD_5, KBD_6, KBD_7, KBD_8, KBD_9, KBD_0,
    KBD_q, KBD_w, KBD_e, KBD_r, KBD_t, KBD_y, KBD_u, KBD_i, KBD_o, KBD_p,
    KBD_a, KBD_s, KBD_d, KBD_f, KBD_g, KBD_h, KBD_j, KBD_k, KBD_l,
    KBD_z, KBD_x, KBD_c, KBD_v, KBD_b, KBD_n, KBD_m,
    KBD_f1, KBD_f2, KBD_f3, KBD_f4, KBD_f5, KBD_f6,
    KBD_f7, KBD_f8, KBD_f9, KBD_f10, KBD_f11, KBD_f12,
    KBD_esc, KBD_tab, KBD_backspace, KBD_enter, KBD_space,
    KBD_leftalt, KBD_rightalt, KBD_leftctrl, KBD_rightctrl,
    KBD_leftshift, KBD_rightshift, KBD_capslock, KBD_scrolllock, KBD_numlock,
    KBD_grave, KBD_minus, KBD_equals, KBD_backslash, KBD_leftbracket,
    KBD_rightbracket, KBD_semicolon, KBD_quote, KBD_period, KBD_comma, KBD_slash,
    KBD_printscreen, KBD_pause,
    KBD_insert, KBD_home, KBD_pageup, KBD_delete, KBD_end, KBD_pagedown,
    KBD_left, KBD_up, KBD_down, KBD_right,
    KBD_kp1, KBD_kp2, KBD_kp3, KBD_kp4, KBD_kp5,
    KBD_kp6, KBD_kp7, KBD_kp8, KBD_kp9, KBD_kp0,
    KBD_kpdivide, KBD_kpmultiply, KBD_kpminus, KBD_kpplus,
    KBD_kpenter, KBD_kpperiod
};

static_assert(sizeof(kKeyboardKeys) / sizeof(kKeyboardKeys[0]) ==
                      static_cast<std::size_t>(KeyboardKey::Last),
              "Keyboard protocol and DOSBox key tables must stay aligned");

KBD_KEYS NativeKeyboardKey(const KeyboardKey key)
{
    const std::size_t index = static_cast<std::size_t>(key);
    return index < sizeof(kKeyboardKeys) / sizeof(kKeyboardKeys[0]) ?
            kKeyboardKeys[index] : KBD_NONE;
}

float NativeJoystickAxis(const std::int32_t value)
{
    return value < 0 ? static_cast<float>(value) / 32768.0f :
                       static_cast<float>(value) / 32767.0f;
}

std::int32_t ProtocolJoystickAxis(const float value)
{
    const float limited = (std::max)(-1.0f, (std::min)(1.0f, value));
    return limited < 0.0f ?
            static_cast<std::int32_t>(std::lround(limited * 32768.0f)) :
            static_cast<std::int32_t>(std::lround(limited * 32767.0f));
}
#endif

bool RequireEmulationThread(std::string* error)
{
    if (AGENT_EmulationQueue().IsBoundToCurrentThread())
        return true;
    if (error != NULL)
        *error = "Debugger access was not dispatched on the emulation thread";
    return false;
}

#if C_DEBUG
bool ExactWatchpointCoreSupported()
{
    return cpudecoder == &CPU_Core_Normal_Run ||
           cpudecoder == &CPU_Core286_Normal_Run ||
           cpudecoder == &CPU_Core8086_Normal_Run ||
           cpudecoder == &CPU_Core_Prefetch_Run ||
           cpudecoder == &CPU_Core286_Prefetch_Run ||
           cpudecoder == &CPU_Core8086_Prefetch_Run;
}

bool RegisterValue(const RegisterSnapshot& registers,
                   const std::string& name,
                   std::uint32_t* value)
{
    if (name == "eax") *value = registers.eax;
    else if (name == "ebx") *value = registers.ebx;
    else if (name == "ecx") *value = registers.ecx;
    else if (name == "edx") *value = registers.edx;
    else if (name == "esi") *value = registers.esi;
    else if (name == "edi") *value = registers.edi;
    else if (name == "ebp") *value = registers.ebp;
    else if (name == "esp") *value = registers.esp;
    else if (name == "cs") *value = registers.cs;
    else if (name == "ds") *value = registers.ds;
    else if (name == "es") *value = registers.es;
    else if (name == "fs") *value = registers.fs;
    else if (name == "gs") *value = registers.gs;
    else if (name == "ss") *value = registers.ss;
    else if (name == "instruction_pointer") *value = registers.instruction_pointer;
    else if (name == "flags") *value = registers.flags;
    else return false;
    return true;
}

bool IsSegmentRegister(const std::string& name)
{
    return name == "cs" || name == "ds" || name == "es" ||
           name == "fs" || name == "gs" || name == "ss";
}

void ApplyRegisterValues(const std::map<std::string, std::uint32_t>& values)
{
    for (std::map<std::string, std::uint32_t>::const_iterator it = values.begin();
         it != values.end(); ++it) {
        const std::string& name = it->first;
        const std::uint32_t value = it->second;
        if (name == "eax") reg_eax = value;
        else if (name == "ebx") reg_ebx = value;
        else if (name == "ecx") reg_ecx = value;
        else if (name == "edx") reg_edx = value;
        else if (name == "esi") reg_esi = value;
        else if (name == "edi") reg_edi = value;
        else if (name == "ebp") reg_ebp = value;
        else if (name == "esp") reg_esp = value;
        else if (name == "cs") SegSet16(cs, static_cast<std::uint16_t>(value));
        else if (name == "ds") SegSet16(ds, static_cast<std::uint16_t>(value));
        else if (name == "es") SegSet16(es, static_cast<std::uint16_t>(value));
        else if (name == "fs") SegSet16(fs, static_cast<std::uint16_t>(value));
        else if (name == "gs") SegSet16(gs, static_cast<std::uint16_t>(value));
        else if (name == "ss") SegSet16(ss, static_cast<std::uint16_t>(value));
        else if (name == "instruction_pointer") reg_eip = value;
        else if (name == "flags") CPU_SetFlags(value, FMASK_ALL);
    }
}

std::map<std::string, std::uint32_t> SnapshotValues(const RegisterSnapshot& snapshot)
{
    std::map<std::string, std::uint32_t> values;
    values["eax"] = snapshot.eax;
    values["ebx"] = snapshot.ebx;
    values["ecx"] = snapshot.ecx;
    values["edx"] = snapshot.edx;
    values["esi"] = snapshot.esi;
    values["edi"] = snapshot.edi;
    values["ebp"] = snapshot.ebp;
    values["esp"] = snapshot.esp;
    values["cs"] = snapshot.cs;
    values["ds"] = snapshot.ds;
    values["es"] = snapshot.es;
    values["fs"] = snapshot.fs;
    values["gs"] = snapshot.gs;
    values["ss"] = snapshot.ss;
    values["instruction_pointer"] = snapshot.instruction_pointer;
    values["flags"] = snapshot.flags;
    return values;
}
#endif

void SetAccessError(MemoryAccessError* access_error,
                    const char* reason,
                    const std::uint32_t failing_offset)
{
    if (access_error == NULL)
        return;
    access_error->reason = reason;
    access_error->failing_offset = failing_offset;
}

std::string Trim(const std::string& value)
{
    const std::string::size_type first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const std::string::size_type last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool IsReadOnlyDiagnosticCommand(const std::string& command)
{
    std::string normalized = Trim(command);
    for (std::string::iterator it = normalized.begin(); it != normalized.end(); ++it)
        *it = static_cast<char>(std::toupper(static_cast<unsigned char>(*it)));
    return normalized == "HELP" || normalized == "CPU" || normalized == "PIC";
}

#if C_DEBUG
bool ResolveSegmentedByte(const MemoryAddress& address,
                          const std::size_t index,
                          std::uint32_t* linear,
                          MemoryAccessError* access_error)
{
    const std::uint64_t raw_offset = static_cast<std::uint64_t>(address.offset) + index;
    if (raw_offset > (std::numeric_limits<std::uint32_t>::max)()) {
        SetAccessError(access_error, "address_overflow", address.offset);
        return false;
    }

    std::uint32_t offset = static_cast<std::uint32_t>(raw_offset);
    if (!cpu.pmode || (reg_flags & FLAG_VM)) {
        offset &= 0xffffu;
        *linear = (static_cast<std::uint32_t>(address.segment) << 4u) + offset;
        return true;
    }

    Descriptor descriptor;
    if (!cpu.gdt.GetDescriptor(address.segment, descriptor) || descriptor.Type() == 0 || !descriptor.saved.seg.p) {
        SetAccessError(access_error, "invalid_selector", static_cast<std::uint32_t>(raw_offset));
        return false;
    }

    if (!descriptor.Big())
        offset &= 0xffffu;

    const Bitu limit = descriptor.GetLimit();
    if ((!descriptor.GetExpandDown() && offset > limit) ||
        (descriptor.GetExpandDown() && offset <= limit)) {
        SetAccessError(access_error, "segment_limit", offset);
        return false;
    }

    const std::uint64_t resolved = static_cast<std::uint64_t>(descriptor.GetBase()) + offset;
    if (resolved > (std::numeric_limits<std::uint32_t>::max)()) {
        SetAccessError(access_error, "address_overflow", offset);
        return false;
    }
    *linear = static_cast<std::uint32_t>(resolved);
    return true;
}

bool ResolveLinearByte(const MemoryAddress& address,
                       const std::size_t index,
                       std::uint32_t* linear,
                       MemoryAccessError* access_error)
{
    const std::uint64_t resolved = static_cast<std::uint64_t>(address.offset) + index;
    if (resolved > (std::numeric_limits<std::uint32_t>::max)()) {
        SetAccessError(access_error, "address_overflow", address.offset);
        return false;
    }
    *linear = static_cast<std::uint32_t>(resolved);
    return true;
}

bool ResolveMemoryByte(const MemoryAddress& address,
                       const std::size_t index,
                       std::uint32_t* resolved,
                       MemoryAccessError* access_error)
{
    switch (address.space) {
    case MemorySpace::Segmented:
        return ResolveSegmentedByte(address, index, resolved, access_error);
    case MemorySpace::Linear:
    case MemorySpace::Physical:
        return ResolveLinearByte(address, index, resolved, access_error);
    }
    SetAccessError(access_error, "unsupported_address_space", address.offset);
    return false;
}

bool ReadResolvedByte(const MemoryAddress& address,
                      const std::uint32_t resolved,
                      std::uint8_t* value,
                      MemoryAccessError* access_error)
{
    if (address.space == MemorySpace::Physical) {
        if (resolved >= MemSize) {
            SetAccessError(access_error, "physical_out_of_range", resolved);
            return false;
        }
        *value = phys_readb(resolved);
        return true;
    }

    try {
        if (!mem_readb_checked(resolved, value))
            return true;
    } catch (const GuestPageFaultException&) {
    } catch (const GuestGenFaultException&) {
    }
    SetAccessError(access_error, "page_not_present", resolved);
    return false;
}

bool IsResolvedByteWritable(const MemoryAddress& address,
                            const std::uint32_t resolved,
                            MemoryAccessError* access_error)
{
    if (address.space == MemorySpace::Physical) {
        if (resolved < MemSize)
            return true;
        SetAccessError(access_error, "physical_out_of_range", resolved);
        return false;
    }

    if (get_tlb_write(resolved) != NULL)
        return true;
    SetAccessError(access_error, "write_not_mapped", resolved);
    return false;
}

void WriteResolvedByte(const MemoryAddress& address,
                       const std::uint32_t resolved,
                       const std::uint8_t value)
{
    if (address.space == MemorySpace::Physical)
        phys_writeb(resolved, value);
    else
        mem_writeb_inline(resolved, value);
}
#endif

} // namespace

bool DebuggerAdapter::IsAvailable() const
{
#if C_DEBUG
    return true;
#else
    return false;
#endif
}

bool DebuggerAdapter::RequireAvailable(std::string* error) const
{
    if (IsAvailable())
        return true;

    if (error != NULL)
        *error = "DEBUGGER_UNAVAILABLE: DOSBox-X was built without C_DEBUG";
    return false;
}

bool DebuggerAdapter::IsShellReady() const
{
#if C_DEBUG
    return first_shell != NULL && DOS_ShellGetPSP() != 0;
#else
    return false;
#endif
}

bool DebuggerAdapter::IsReadyForTargetStart() const
{
#if C_DEBUG
    return IsShellReady() && DEBUG_AgentCanStartTarget();
#else
    return false;
#endif
}

std::uint16_t DebuggerAdapter::CurrentPsp() const
{
#if C_DEBUG
    return dos.psp();
#else
    return 0;
#endif
}

std::uint64_t DebuggerAdapter::EntryBreakpointSequence() const
{
#if C_DEBUG
    return DEBUG_AgentEntryBreakpointSequence();
#else
    return 0;
#endif
}

bool DebuggerAdapter::Continue(std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error))
        return false;

#if C_DEBUG
    DEBUG_AgentClearLastBreakpoint();
    char command[] = "RUN";
    if (ParseCommand(command))
        return true;

    if (error != NULL)
        *error = "Debugger rejected RUN";
    return false;
#else
    return false;
#endif
}

bool DebuggerAdapter::Pause(std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error))
        return false;

#if C_DEBUG
    DEBUG_EnableDebugger();
    return true;
#else
    return false;
#endif
}

bool DebuggerAdapter::StartTargetAtEntry(const std::string& command,
                                         const std::vector<std::string>& arguments,
                                         const std::string& workdir,
                                         std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error))
        return false;

#if C_DEBUG
    if (!IsShellReady()) {
        if (error != NULL)
            *error = "DOS shell is not ready";
        return false;
    }
    dos.psp(DOS_ShellGetPSP());
    if (workdir.empty() || workdir.find('"') != std::string::npos) {
        if (error != NULL)
            *error = "Configured workdir cannot be mounted safely";
        return false;
    }

    const auto is_dos_token = [](const std::string& value, const bool allow_option_prefix) {
        if (value.empty())
            return false;
        std::string::const_iterator it = value.begin();
        if (allow_option_prefix && *it == '/') {
            ++it;
            if (it == value.end())
                return false;
        }
        for (; it != value.end(); ++it) {
            const unsigned char character = static_cast<unsigned char>(*it);
            if (!std::isalnum(character) && *it != '.' && *it != '_' && *it != '-')
                return false;
        }
        return true;
    };
    if (!is_dos_token(command, false)) {
        if (error != NULL)
            *error = "target.command must be a DOS filename without shell metacharacters";
        return false;
    }
    for (std::vector<std::string>::const_iterator it = arguments.begin(); it != arguments.end(); ++it) {
        if (!is_dos_token(*it, true)) {
            if (error != NULL)
                *error = "target.arguments must contain only DOS-safe tokens";
            return false;
        }
    }

    // The agent accepts one immutable, process-level C: mount. Re-running MOUNT
    // for every session is not idempotent: the DOS command reports an existing
    // drive through its normal output path and can leave a stale DOS error code.
    if (Drives[2] == NULL) {
        // DoCommand starts the DOS-side MOUNT.COM and can return before that
        // program has populated Drives[2].  runMount executes the same internal
        // program synchronously on this emulation-thread callback, so the check
        // below is a postcondition rather than a race with DOS execution.
        const std::string mount_arguments = "C \"" + workdir + "\" -Q";
        // MOUNT switches to the shell's long-command buffer above 100 bytes.
        // This direct invocation has no preceding shell parse, so make that
        // buffer describe the exact command that runMount is about to execute.
        full_arguments = mount_arguments;
        runMount(mount_arguments.c_str());
        if (Drives[2] == NULL) {
            if (error != NULL)
                *error = "Configured C drive mount did not become active";
            return false;
        }
    }
    if (!DOS_SetDrive(2)) {
        if (error != NULL)
            *error = "Configured C drive is unavailable";
        return false;
    }

    std::string debugbox_command = "DEBUGBOX " + command;
    for (std::vector<std::string>::const_iterator it = arguments.begin(); it != arguments.end(); ++it)
        debugbox_command += " " + *it;
    dos.errorcode = 0;
    first_shell->DoCommand(&debugbox_command[0]);
    if (dos.errorcode != 0) {
        if (error != NULL)
            *error = "DOS target launch failed with error " + std::to_string(dos.errorcode);
        return false;
    }
    return true;
#else
    (void)command;
    (void)arguments;
    (void)workdir;
    return false;
#endif
}

bool DebuggerAdapter::GetRegisters(RegisterSnapshot* registers, std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || registers == NULL)
        return false;

#if C_DEBUG
    registers->eax = reg_eax;
    registers->ebx = reg_ebx;
    registers->ecx = reg_ecx;
    registers->edx = reg_edx;
    registers->esi = reg_esi;
    registers->edi = reg_edi;
    registers->ebp = reg_ebp;
    registers->esp = reg_esp;
    registers->cs = SegValue(cs);
    registers->ds = SegValue(ds);
    registers->es = SegValue(es);
    registers->fs = SegValue(fs);
    registers->gs = SegValue(gs);
    registers->ss = SegValue(ss);
    registers->instruction_pointer = reg_eip;
    registers->flags = static_cast<std::uint32_t>(reg_flags);
    registers->cpu_mode = !cpu.pmode ? "real" : ((reg_flags & FLAG_VM) ? "v86" : "protected");
    return true;
#else
    return false;
#endif
}

bool DebuggerAdapter::SetRegistersGuarded(
        const std::map<std::string, std::uint32_t>& expected,
        const std::map<std::string, std::uint32_t>& values,
        RegisterWriteResult* result,
        std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || result == NULL)
        return false;
#if C_DEBUG
    *result = RegisterWriteResult();
    if (expected.empty() || values.empty()) {
        if (error != NULL)
            *error = "Register expected/set maps must be non-empty";
        return false;
    }
    if (!GetRegisters(&result->before, error))
        return false;
    if (result->before.cpu_mode != "real") {
        if (error != NULL)
            *error = "Guarded register writes currently require real CPU mode";
        return false;
    }
    for (std::map<std::string, std::uint32_t>::const_iterator it = values.begin();
         it != values.end(); ++it) {
        std::uint32_t ignored = 0;
        if (!RegisterValue(result->before, it->first, &ignored) ||
            (IsSegmentRegister(it->first) && it->second > 0xffffu)) {
            if (error != NULL)
                *error = "Register patch contains an unknown or out-of-range register";
            return false;
        }
        if (expected.find(it->first) == expected.end()) {
            if (error != NULL)
                *error = "Every written register requires an expected old value";
            return false;
        }
    }
    for (std::map<std::string, std::uint32_t>::const_iterator it = expected.begin();
         it != expected.end(); ++it) {
        std::uint32_t current = 0;
        if (!RegisterValue(result->before, it->first, &current) ||
            (IsSegmentRegister(it->first) && it->second > 0xffffu)) {
            if (error != NULL)
                *error = "Register precondition contains an unknown or out-of-range register";
            return false;
        }
        if (current != it->second) {
            result->precondition_failed = true;
            result->mismatch_register = it->first;
            if (error != NULL)
                *error = "Register precondition did not match live state";
            return false;
        }
    }

    ApplyRegisterValues(values);
    if (!GetRegisters(&result->after, error)) {
        ApplyRegisterValues(SnapshotValues(result->before));
        return false;
    }
    for (std::map<std::string, std::uint32_t>::const_iterator it = values.begin();
         it != values.end(); ++it) {
        std::uint32_t actual = 0;
        RegisterValue(result->after, it->first, &actual);
        if (actual != it->second) {
            ApplyRegisterValues(SnapshotValues(result->before));
            (void)GetRegisters(&result->after, error);
            if (error != NULL)
                *error = "Register write did not read back exactly and was rolled back";
            return false;
        }
    }
    return true;
#else
    (void)expected;
    (void)values;
    return false;
#endif
}

bool DebuggerAdapter::GetInputState(InputState* state, std::string* error) const
{
#if C_DEBUG
    if (!RequireEmulationThread(error) || !RequireAvailable(error))
        return false;
    if (state == NULL) {
        if (error != NULL)
            *error = "Input state output is null";
        return false;
    }

    state->pressed_keys.clear();
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(KeyboardKey::Last); ++index) {
        if (KEYBOARD_IsKeyPressed(kKeyboardKeys[index]))
            state->pressed_keys.push_back(static_cast<KeyboardKey>(index));
    }
    for (std::size_t index = 0; index < 2; ++index) {
        JoystickInputState& joystick = state->joysticks[index];
        joystick.enabled = JOYSTICK_IsEnabled(static_cast<Bitu>(index));
        joystick.x = ProtocolJoystickAxis(JOYSTICK_GetMove_X(static_cast<Bitu>(index)));
        joystick.y = ProtocolJoystickAxis(JOYSTICK_GetMove_Y(static_cast<Bitu>(index)));
        joystick.button0 = JOYSTICK_GetButton(static_cast<Bitu>(index), 0);
        joystick.button1 = JOYSTICK_GetButton(static_cast<Bitu>(index), 1);
    }
    return true;
#else
    (void)state;
    if (error != NULL)
        *error = "Debugger support is not compiled in";
    return false;
#endif
}

bool DebuggerAdapter::ApplyKeyboardInput(const std::vector<KeyboardInputEvent>& events,
                                         InputState* state,
                                         std::string* error) const
{
#if C_DEBUG
    if (!RequireEmulationThread(error) || !RequireAvailable(error))
        return false;
    for (std::vector<KeyboardInputEvent>::const_iterator event = events.begin();
         event != events.end(); ++event) {
        const KBD_KEYS key = NativeKeyboardKey(event->key);
        if (key == KBD_NONE) {
            if (error != NULL)
                *error = "Keyboard event contains an unsupported key";
            return false;
        }
        KEYBOARD_AddKey(key, event->pressed);
    }
    return GetInputState(state, error);
#else
    (void)events;
    (void)state;
    if (error != NULL)
        *error = "Debugger support is not compiled in";
    return false;
#endif
}

bool DebuggerAdapter::ApplyJoystickInput(const JoystickInputUpdate& update,
                                         InputState* state,
                                         std::string* error) const
{
#if C_DEBUG
    if (!RequireEmulationThread(error) || !RequireAvailable(error))
        return false;
    if (update.index >= 2) {
        if (error != NULL)
            *error = "Joystick index must be 0 or 1";
        return false;
    }
    const Bitu index = static_cast<Bitu>(update.index);
    if (update.has_enabled)
        JOYSTICK_Enable(index, update.enabled);
    if (update.has_x)
        JOYSTICK_Move_X(index, NativeJoystickAxis(update.x));
    if (update.has_y)
        JOYSTICK_Move_Y(index, NativeJoystickAxis(update.y));
    if (update.has_button0)
        JOYSTICK_Button(index, 0, update.button0);
    if (update.has_button1)
        JOYSTICK_Button(index, 1, update.button1);
    return GetInputState(state, error);
#else
    (void)update;
    (void)state;
    if (error != NULL)
        *error = "Debugger support is not compiled in";
    return false;
#endif
}

bool DebuggerAdapter::CaptureVideoSnapshot(VideoSnapshot* snapshot, std::string* error) const
{
    if (!RequireEmulationThread(error))
        return false;
    if (snapshot == NULL) {
        if (error != NULL)
            *error = "Video snapshot output is required";
        return false;
    }

    VideoSnapshot captured;

    MemoryAddress text_address;
    text_address.space = MemorySpace::Linear;
    text_address.offset = 0xb8000;
    MemoryAccessError access_error;
    if (!ReadMemory(text_address, 32768, &captured.text, &access_error, error))
        return false;

    MemoryAddress mode_address;
    mode_address.space = MemorySpace::Linear;
    mode_address.offset = 0x449;
    std::vector<std::uint8_t> mode;
    if (!ReadMemory(mode_address, 1, &mode, &access_error, error))
        return false;
    captured.video_mode = mode[0];

    captured.font.assign(16384, 0);
    const std::uintptr_t linear_begin = reinterpret_cast<std::uintptr_t>(vga.mem.linear);
    const std::uintptr_t linear_end = linear_begin + static_cast<std::uintptr_t>(vga.mem.memsize);
    for (int page = 0; page < 2; ++page) {
        const std::uint8_t* source = vga.draw.font_tables[page];
        if (source == NULL)
            source = &vga.draw.font[page * 8192];
        const std::uintptr_t source_address = reinterpret_cast<std::uintptr_t>(source);
        const bool planar = source_address >= linear_begin && source_address < linear_end;
        for (std::size_t index = 0; index < 8192; ++index) {
            if (planar) {
                const std::uintptr_t source_offset = source_address - linear_begin + index * 4u;
                if (source_offset < static_cast<std::uintptr_t>(vga.mem.memsize))
                    captured.font[static_cast<std::size_t>(page) * 8192 + index] =
                            vga.mem.linear[source_offset];
            } else {
                captured.font[static_cast<std::size_t>(page) * 8192 + index] = source[index];
            }
        }
    }

    captured.dac_palette.resize(768);
    for (std::size_t index = 0; index < 256; ++index) {
        captured.dac_palette[index * 3 + 0] = vga.dac.rgb[index].red;
        captured.dac_palette[index * 3 + 1] = vga.dac.rgb[index].green;
        captured.dac_palette[index * 3 + 2] = vga.dac.rgb[index].blue;
    }
    captured.dac_bits = vga.dac.bits;
    captured.dac_pel_mask = vga.dac.pel_mask;

    captured.renderer_palette.resize(sizeof(render.pal.rgb));
    std::memcpy(captured.renderer_palette.data(), &render.pal.rgb,
                captured.renderer_palette.size());

    if (scalerSourceCacheBuffer != NULL && render.src.width > 0 && render.src.height > 0 &&
        render.scale.cachePitch > 0) {
        const std::size_t frame_bytes = static_cast<std::size_t>(render.scale.cachePitch) *
                                        static_cast<std::size_t>(render.src.height);
        if (frame_bytes <= scalerSourceCacheBufferSize) {
            captured.frame.assign(scalerSourceCacheBuffer,
                                  scalerSourceCacheBuffer + frame_bytes);
            captured.frame_width = static_cast<std::uint32_t>(render.src.width);
            captured.frame_height = static_cast<std::uint32_t>(render.src.height);
            captured.frame_bpp = static_cast<std::uint32_t>(render.src.bpp);
            captured.frame_pitch = static_cast<std::uint32_t>(render.scale.cachePitch);
            captured.frame_dblw = render.src.dblw;
            captured.frame_dblh = render.src.dblh;
        }
    }

    const std::uint8_t crtc_values[] = {
        vga.crtc.horizontal_total, vga.crtc.horizontal_display_end,
        vga.crtc.start_horizontal_blanking, vga.crtc.end_horizontal_blanking,
        vga.crtc.start_horizontal_retrace, vga.crtc.end_horizontal_retrace,
        vga.crtc.vertical_total, vga.crtc.overflow, vga.crtc.preset_row_scan,
        vga.crtc.maximum_scan_line, vga.crtc.cursor_start, vga.crtc.cursor_end,
        vga.crtc.start_address_high, vga.crtc.start_address_low,
        vga.crtc.cursor_location_high, vga.crtc.cursor_location_low,
        vga.crtc.vertical_retrace_start, vga.crtc.vertical_retrace_end,
        vga.crtc.vertical_display_end, vga.crtc.offset, vga.crtc.underline_location,
        vga.crtc.start_vertical_blanking, vga.crtc.end_vertical_blanking,
        vga.crtc.mode_control, vga.crtc.line_compare,
    };
    captured.crtc.assign(32, 0);
    std::copy(crtc_values, crtc_values + sizeof(crtc_values), captured.crtc.begin());

    captured.ticks = static_cast<std::uint64_t>(PIC_Ticks);
    captured.text_columns = static_cast<std::uint32_t>(vga.draw.blocks);
    captured.glyph_height = static_cast<std::uint32_t>((vga.crtc.maximum_scan_line & 0x1fu) + 1u);
    captured.text_offset = static_cast<std::uint32_t>(
            (static_cast<std::uint16_t>(vga.crtc.start_address_high) << 8u |
             vga.crtc.start_address_low) * 2u);
    captured.char9dot = vga.draw.char9dot;
    captured.blinking = static_cast<std::uint32_t>(vga.draw.blinking);
    captured.blink_phase = vga.draw.blink;
    captured.attr_mode_control = vga.attr.mode_control;
    captured.underline_location = vga.crtc.underline_location & 0x1fu;
    captured.panning = static_cast<std::uint32_t>(vga.draw.panning);
    captured.draw_address = static_cast<std::uint32_t>(vga.draw.address);

    *snapshot = std::move(captured);
    return true;
}

bool DebuggerAdapter::Step(const StepMode mode, bool* continued, std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || continued == NULL)
        return false;

#if C_DEBUG
    if (!DEBUG_AgentStep(mode == StepMode::Over, continued)) {
        if (error != NULL)
            *error = "Debugger is not stopped and ready to single-step";
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool DebuggerAdapter::ReadMemory(const MemoryAddress& address,
                                 const std::size_t length,
                                 std::vector<std::uint8_t>* data,
                                 MemoryAccessError* access_error,
                                 std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || data == NULL || length == 0)
        return false;

#if C_DEBUG
    if (access_error != NULL)
        *access_error = MemoryAccessError();
    data->clear();
    data->reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        std::uint32_t resolved = 0;
        std::uint8_t value = 0;
        if (!ResolveMemoryByte(address, index, &resolved, access_error) ||
            !ReadResolvedByte(address, resolved, &value, access_error))
            return false;
        data->push_back(value);
    }
    return true;
#else
    (void)address;
    (void)length;
    (void)access_error;
    return false;
#endif
}

bool DebuggerAdapter::WriteMemory(const MemoryAddress& address,
                                  const std::vector<std::uint8_t>& data,
                                  std::vector<std::uint8_t>* after,
                                  MemoryAccessError* access_error,
                                  std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || after == NULL || data.empty())
        return false;

#if C_DEBUG
    if (access_error != NULL)
        *access_error = MemoryAccessError();
    std::vector<std::uint32_t> resolved_addresses;
    resolved_addresses.reserve(data.size());
    for (std::size_t index = 0; index < data.size(); ++index) {
        std::uint32_t resolved = 0;
        std::uint8_t unused = 0;
        if (!ResolveMemoryByte(address, index, &resolved, access_error) ||
            !ReadResolvedByte(address, resolved, &unused, access_error) ||
            !IsResolvedByteWritable(address, resolved, access_error))
            return false;
        resolved_addresses.push_back(resolved);
    }

    for (std::size_t index = 0; index < data.size(); ++index)
        WriteResolvedByte(address, resolved_addresses[index], data[index]);

    return ReadMemory(address, data.size(), after, access_error, error);
#else
    (void)address;
    (void)data;
    (void)access_error;
    return false;
#endif
}

bool DebuggerAdapter::CreateBreakpoint(const BreakpointKind kind,
                                       const MemoryAddress& address,
                                       const std::uint32_t length,
                                       const bool once,
                                       const BreakpointCondition& condition,
                                       const BreakpointHitFilter& hit_filter,
                                       NativeBreakpoint* breakpoint,
                                       MemoryAccessError* access_error,
                                       std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || breakpoint == NULL)
        return false;

#if C_DEBUG
    if (access_error != NULL)
        *access_error = MemoryAccessError();
    std::vector<std::uint8_t> probe;
    if (length == 0 || !ReadMemory(address, length, &probe, access_error, error))
        return false;

    std::uintptr_t handle = 0;
    if (kind == BreakpointKind::Execution) {
        if (address.space != MemorySpace::Segmented) {
            if (error != NULL)
                *error = "Execution breakpoints currently require segmented addresses";
            return false;
        }
        if (!DEBUG_AgentCreateExecutionBreakpoint(address.segment, address.offset, once, &handle)) {
            if (error != NULL)
                *error = "Debugger rejected the execution breakpoint";
            return false;
        }
    } else if (kind == BreakpointKind::MemoryChange) {
#if C_HEAVY_DEBUG
        if (once) {
            if (error != NULL)
                *error = "memory_change breakpoints do not support once";
            return false;
        }
        if (address.space == MemorySpace::Physical) {
            if (error != NULL)
                *error = "Physical memory-change breakpoints are not supported by the debugger";
            return false;
        }
        const bool protected_mode = address.space == MemorySpace::Segmented && cpu.pmode && !(reg_flags & FLAG_VM);
        const bool linear = address.space == MemorySpace::Linear;
        if (!DEBUG_AgentCreateMemoryBreakpoint(address.segment, address.offset, protected_mode, linear, &handle)) {
            if (error != NULL)
                *error = "Debugger rejected the memory-change breakpoint";
            return false;
        }
#else
        if (error != NULL)
            *error = "Memory-change breakpoints require C_HEAVY_DEBUG";
        return false;
#endif
    } else {
#if C_HEAVY_DEBUG
        if (!ExactWatchpointCoreSupported()) {
            if (error != NULL)
                *error = "Exact access watchpoints require core=normal";
            return false;
        }
        if (address.space == MemorySpace::Physical) {
            if (error != NULL)
                *error = "Exact access watchpoints require segmented or linear addresses";
            return false;
        }
        std::uint32_t linear_address = 0;
        for (std::uint32_t index = 0; index < length; ++index) {
            std::uint32_t resolved = 0;
            if (!ResolveMemoryByte(address, index, &resolved, access_error))
                return false;
            if (index == 0)
                linear_address = resolved;
            else if (resolved != linear_address + index) {
                if (error != NULL)
                    *error = "Exact access watchpoint range is not contiguous in linear memory";
                return false;
            }
        }
        const bool on_read = kind == BreakpointKind::MemoryRead ||
                             kind == BreakpointKind::MemoryAccess;
        const bool on_write = kind == BreakpointKind::MemoryWrite ||
                              kind == BreakpointKind::MemoryAccess;
        if (!DEBUG_AgentCreateAccessWatchpoint(linear_address, length, on_read,
                                               on_write, once, &handle)) {
            if (error != NULL)
                *error = "Debugger rejected the exact access watchpoint";
            return false;
        }
#else
        if (error != NULL)
            *error = "Exact access watchpoints require C_HEAVY_DEBUG";
        return false;
#endif
    }

    DEBUG_AgentBreakpointPolicy policy;
    policy.register_name = condition.register_name.c_str();
    policy.condition_enabled = condition.enabled;
    policy.condition_equal = condition.equal;
    policy.condition_value = condition.value;
    policy.skip = hit_filter.skip;
    policy.every = hit_filter.every;
    if (!DEBUG_AgentConfigureBreakpoint(handle, &policy)) {
        (void)DEBUG_AgentDeleteBreakpoint(handle);
        if (error != NULL)
            *error = "Debugger rejected the breakpoint condition or hit filter";
        return false;
    }

    breakpoint->handle = handle;
    breakpoint->kind = kind;
    breakpoint->address = address;
    breakpoint->length = length;
    breakpoint->once = once;
    breakpoint->condition = condition;
    breakpoint->hit_filter = hit_filter;
    return true;
#else
    (void)kind;
    (void)address;
    (void)length;
    (void)once;
    (void)condition;
    (void)hit_filter;
    (void)breakpoint;
    (void)access_error;
    return false;
#endif
}

bool DebuggerAdapter::CreateInterruptBreakpoint(
        const InterruptBreakpointSelector& selector,
        const bool once,
        const BreakpointCondition& condition,
        const BreakpointHitFilter& hit_filter,
        NativeBreakpoint* breakpoint,
        std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || breakpoint == NULL)
        return false;

#if C_DEBUG
    const std::uint16_t wildcard = 0x100;
    std::uintptr_t handle = 0;
    if (!DEBUG_AgentCreateInterruptBreakpoint(
                selector.number,
                selector.has_ah ? selector.ah : wildcard,
                selector.has_al ? selector.al : wildcard,
                once,
                &handle)) {
        if (error != NULL)
            *error = "Debugger rejected the software-interrupt breakpoint";
        return false;
    }

    DEBUG_AgentBreakpointPolicy policy;
    policy.register_name = condition.register_name.c_str();
    policy.condition_enabled = condition.enabled;
    policy.condition_equal = condition.equal;
    policy.condition_value = condition.value;
    policy.skip = hit_filter.skip;
    policy.every = hit_filter.every;
    if (!DEBUG_AgentConfigureBreakpoint(handle, &policy)) {
        (void)DEBUG_AgentDeleteBreakpoint(handle);
        if (error != NULL)
            *error = "Debugger rejected the interrupt breakpoint condition or hit filter";
        return false;
    }

    breakpoint->handle = handle;
    breakpoint->kind = BreakpointKind::Interrupt;
    breakpoint->interrupt = selector;
    breakpoint->length = 0;
    breakpoint->once = once;
    breakpoint->condition = condition;
    breakpoint->hit_filter = hit_filter;
    return true;
#else
    (void)selector;
    (void)once;
    (void)condition;
    (void)hit_filter;
    (void)breakpoint;
    return false;
#endif
}

bool DebuggerAdapter::DeleteBreakpoint(const NativeBreakpoint& breakpoint, std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error))
        return false;

#if C_DEBUG
    if (breakpoint.handle != 0 && DEBUG_AgentDeleteBreakpoint(breakpoint.handle))
        return true;
    if (error != NULL)
        *error = "Breakpoint no longer exists in the debugger";
    return false;
#else
    (void)breakpoint;
    return false;
#endif
}

bool DebuggerAdapter::ConsumeLastBreakpointHit(BreakpointHit* hit) const
{
#if C_DEBUG
    if (hit == NULL)
        return false;
    DEBUG_AgentBreakpointHit native;
    if (!DEBUG_AgentConsumeBreakpointHit(&native))
        return false;
    hit->handle = native.handle;
    hit->hit_count = native.hit_count;
    hit->interrupt.valid = native.interrupt;
    hit->interrupt.software = native.software;
    hit->interrupt.number = native.interrupt_number;
    hit->interrupt.ah = native.ah;
    hit->interrupt.al = native.al;
    return true;
#else
    (void)hit;
    return false;
#endif
}

bool DebuggerAdapter::ConsumeLastWatchpointHit(WatchpointHit* hit) const
{
#if C_DEBUG && C_HEAVY_DEBUG
    if (hit == NULL)
        return false;
    DEBUG_AgentWatchpointHit native;
    if (!DEBUG_AgentConsumeWatchpointHit(&native))
        return false;
    hit->handle = native.handle;
    hit->write = native.write;
    hit->address.space = MemorySpace::Linear;
    hit->address.offset = native.linear_address;
    hit->instruction_address.space = MemorySpace::Segmented;
    hit->instruction_address.segment = native.instruction_cs;
    hit->instruction_address.offset = native.instruction_ip;
    hit->before.assign(native.before, native.before + native.byte_count);
    hit->after.assign(native.after, native.after + native.byte_count);
    return true;
#else
    (void)hit;
    return false;
#endif
}

bool DebuggerAdapter::GetDosMemoryMap(DosMemoryMap* memory_map,
                                      std::string* error) const
{
#if C_DEBUG
    if (!RequireEmulationThread(error) || memory_map == NULL)
        return false;
    memory_map->current_psp = dos.psp();
    memory_map->first_mcb = dos.firstMCB;
    memory_map->blocks.clear();
    std::uint16_t segment = dos.firstMCB;
    if (segment == 0) {
        if (error != NULL)
            *error = "DOS MCB chain is unavailable";
        return false;
    }
    for (std::size_t count = 0; count < 4096; ++count) {
        DOS_MCB mcb(segment);
        if (!mcb.isValid()) {
            if (error != NULL)
                *error = "DOS MCB chain contains an invalid block";
            return false;
        }
        DosMemoryBlock block;
        block.mcb_segment = segment;
        block.data_segment = static_cast<std::uint16_t>(segment + 1u);
        block.paragraphs = mcb.GetSize();
        block.owner_psp = mcb.GetPSPSeg();
        block.name = Trim(mcb.GetFileName());
        block.last = mcb.isLastMCB();
        block.process = block.owner_psp == block.data_segment;
        if (block.process) {
            DOS_PSP psp(block.owner_psp);
            block.parent_psp = psp.GetParent();
            block.environment_segment = psp.GetEnvironment();
        }
        memory_map->blocks.push_back(block);
        if (block.last)
            return true;
        const std::uint32_t next = static_cast<std::uint32_t>(segment) +
                                   block.paragraphs + 1u;
        if (next > 0xffffu || next <= segment) {
            if (error != NULL)
                *error = "DOS MCB chain does not advance";
            return false;
        }
        segment = static_cast<std::uint16_t>(next);
    }
    if (error != NULL)
        *error = "DOS MCB chain exceeds the safety limit";
    return false;
#else
    (void)memory_map;
    (void)error;
    return false;
#endif
}

bool DebuggerAdapter::CaptureCheckpoint(CheckpointState* checkpoint,
                                        std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || checkpoint == NULL)
        return false;
#if C_DEBUG
    SaveState::MemoryImage image;
    std::string checkpoint_error;
    if (!SaveState::instance().captureMemory(image, checkpoint_error)) {
        if (error != NULL)
            *error = checkpoint_error;
        return false;
    }
    checkpoint->components.swap(image);
    return true;
#else
    (void)checkpoint;
    return false;
#endif
}

bool DebuggerAdapter::RestoreCheckpoint(const CheckpointState& checkpoint,
                                        std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error))
        return false;
#if C_DEBUG
    std::string checkpoint_error;
    if (!SaveState::instance().restoreMemory(checkpoint.components, checkpoint_error)) {
        if (error != NULL)
            *error = checkpoint_error;
        return false;
    }
    // Render's save-state component restores guest-visible video state but
    // deliberately rebuilds its host buffers, leaving the scaler source cache
    // cleared. A stopped text-mode target has no next VGA frame to fill it.
    // Recreate the same source frame used at every headless debugger stop so a
    // checkpoint restore also restores what SHOT/video.snapshot observes and
    // what an exposed window redraws, without executing a guest instruction.
    if (VGA_DebugRenderCurrentTextFrame())
        RENDER_CaptureFrameForRedraw();
    else
        RENDER_DiscardFrameForRedraw();
    return true;
#else
    (void)checkpoint;
    return false;
#endif
}

bool DebuggerAdapter::ExecuteDiagnosticCommand(const std::string& command,
                                               std::string* raw_output,
                                               std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || raw_output == NULL)
        return false;

#if C_DEBUG
    if (!IsReadOnlyDiagnosticCommand(command)) {
        if (error != NULL)
            *error = "Only HELP, CPU, and PIC are permitted debugger diagnostics";
        return false;
    }

    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    if (!DEBUG_AgentBeginOutputCapture()) {
        if (error != NULL)
            *error = "Debugger output capture is already active";
        return false;
    }
    const bool accepted = ParseCommand(&mutable_command[0]);
    *raw_output = DEBUG_AgentEndOutputCapture();
    if (!accepted && error != NULL)
        *error = "Debugger rejected diagnostic command";
    return accepted;
#else
    (void)command;
    return false;
#endif
}

bool DebuggerAdapter::StartTrace(const std::string& detail,
                                 const std::uint32_t instruction_count,
                                 std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error))
        return false;

#if C_HEAVY_DEBUG
    if (detail != "short" && detail != "normal" && detail != "long" && detail != "csip") {
        if (error != NULL)
            *error = "Trace detail must be short, normal, long, or csip";
        return false;
    }
    if (!DEBUG_AgentStartTrace(instruction_count)) {
        if (error != NULL)
            *error = "A CPU trace is already active or instruction_count is invalid";
        return false;
    }
    return true;
#else
    (void)detail;
    (void)instruction_count;
    if (error != NULL)
        *error = "CPU trace requires C_HEAVY_DEBUG";
    return false;
#endif
}

bool DebuggerAdapter::ReadTrace(std::vector<TraceSample>* samples,
                                bool* active,
                                std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || samples == NULL || active == NULL)
        return false;

#if C_HEAVY_DEBUG
    std::vector<DEBUG_AgentTraceEvent> native_events;
    DEBUG_AgentCopyTraceEvents(&native_events);
    samples->clear();
    samples->reserve(native_events.size());
    for (std::vector<DEBUG_AgentTraceEvent>::const_iterator it = native_events.begin(); it != native_events.end(); ++it) {
        TraceSample sample;
        sample.address.space = MemorySpace::Segmented;
        sample.address.segment = it->cs;
        sample.address.offset = it->instruction_pointer;
        sample.registers.eax = it->eax;
        sample.registers.ebx = it->ebx;
        sample.registers.ecx = it->ecx;
        sample.registers.edx = it->edx;
        sample.registers.esi = it->esi;
        sample.registers.edi = it->edi;
        sample.registers.ebp = it->ebp;
        sample.registers.esp = it->esp;
        sample.registers.cs = it->cs;
        sample.registers.ds = it->ds;
        sample.registers.es = it->es;
        sample.registers.fs = it->fs;
        sample.registers.gs = it->gs;
        sample.registers.ss = it->ss;
        sample.registers.instruction_pointer = it->instruction_pointer;
        sample.registers.flags = it->flags;
        sample.registers.cpu_mode = !cpu.pmode ? "real" : ((reg_flags & FLAG_VM) ? "v86" : "protected");
        sample.instruction = Trim(it->instruction);
        sample.analysis = Trim(it->analysis);
        sample.effects.reserve(it->effects.size());
        for (std::vector<DEBUG_AgentTraceEffect>::const_iterator effect = it->effects.begin();
             effect != it->effects.end(); ++effect) {
            TraceEffect converted;
            switch (effect->kind) {
            case DEBUG_AgentTraceEffectKind::MemoryRead:
                converted.kind = TraceEffectKind::MemoryRead;
                break;
            case DEBUG_AgentTraceEffectKind::MemoryWrite:
                converted.kind = TraceEffectKind::MemoryWrite;
                break;
            case DEBUG_AgentTraceEffectKind::IoRead:
                converted.kind = TraceEffectKind::IoRead;
                break;
            case DEBUG_AgentTraceEffectKind::IoWrite:
                converted.kind = TraceEffectKind::IoWrite;
                break;
            }
            converted.byte_count = effect->byte_count;
            if (converted.kind == TraceEffectKind::MemoryRead ||
                converted.kind == TraceEffectKind::MemoryWrite) {
                converted.address.space = MemorySpace::Linear;
                converted.address.offset = effect->address;
                converted.before.assign(effect->before, effect->before + effect->byte_count);
                converted.after.assign(effect->after, effect->after + effect->byte_count);
            } else {
                converted.port = static_cast<std::uint16_t>(effect->address);
                converted.value = effect->value;
            }
            sample.effects.push_back(converted);
        }
        samples->push_back(sample);
    }
    *active = DEBUG_AgentTraceIsActive();
    return true;
#else
    (void)samples;
    (void)active;
    if (error != NULL)
        *error = "CPU trace requires C_HEAVY_DEBUG";
    return false;
#endif
}

bool DebuggerAdapter::StopTrace(std::size_t* event_count, std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || event_count == NULL)
        return false;

#if C_HEAVY_DEBUG
    if (!DEBUG_AgentTraceIsActive()) {
        if (error != NULL)
            *error = "No CPU trace is active";
        return false;
    }
    std::uint32_t native_event_count = 0;
    if (!DEBUG_AgentStopTrace(&native_event_count)) {
        if (error != NULL)
            *error = "Debugger failed to stop the CPU trace";
        return false;
    }
    *event_count = native_event_count;
    return true;
#else
    if (error != NULL)
        *error = "CPU trace requires C_HEAVY_DEBUG";
    return false;
#endif
}

bool DebuggerAdapter::IsTraceComplete() const
{
#if C_HEAVY_DEBUG
    return !DEBUG_AgentTraceIsActive();
#else
    return false;
#endif
}

bool DebuggerAdapter::StartHardwareTrace(const HardwareTraceConfig& config,
                                         std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error))
        return false;
    if (!AGENT_HardwareTraceStart(config)) {
        if (error != NULL)
            *error = "A hardware trace is already active or its configuration is invalid";
        return false;
    }
    return true;
}

bool DebuggerAdapter::ReadHardwareTrace(const bool has_cursor,
                                        const std::uint64_t cursor,
                                        const std::size_t limit,
                                        HardwareTracePage* page,
                                        bool* cursor_expired,
                                        std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) ||
        page == NULL || cursor_expired == NULL)
        return false;
    if (!AGENT_HardwareTraceRead(has_cursor, cursor, limit, page, cursor_expired)) {
        if (!*cursor_expired && error != NULL)
            *error = "No hardware trace has been started";
        return false;
    }
    return true;
}

bool DebuggerAdapter::StopHardwareTrace(HardwareTracePage* status,
                                        std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error) || status == NULL)
        return false;
    if (!AGENT_HardwareTraceStop(status)) {
        if (error != NULL)
            *error = "No hardware trace is active";
        return false;
    }
    return true;
}

bool DebuggerAdapter::TerminateTarget(std::string* error) const
{
    if (!RequireAvailable(error) || !RequireEmulationThread(error))
        return false;

#if C_DEBUG
    DOS_Terminate(dos.psp(), false, 0);

    // Keep the externally initiated exit equivalent to DOS INT 21h/AH=4Ch.
    // DOS_Terminate owns PSP/vector restoration; the interrupt handler owns
    // these process-lifecycle fields after it returns.
    dos_program_running = false;
    appname[0] = 0;
    appargs[0] = 0;
    reg_ax = 0x3e01;

    // DOS_Terminate prepares an IRET frame for the INT 20h/21h termination
    // handler. Agent termination bypasses that handler, so complete the same
    // frame transition before resuming the shell.
    const std::uint16_t return_stack_segment = SegValue(ss);
    const std::uint16_t return_instruction_pointer = real_readw(return_stack_segment, reg_sp);
    const std::uint16_t return_code_segment = real_readw(return_stack_segment, reg_sp + 2u);
    const std::uint16_t return_flags = real_readw(return_stack_segment, reg_sp + 4u);
    reg_sp += 6u;
    SegSet16(cs, return_code_segment);
    reg_ip = return_instruction_pointer;
    reg_flags = (reg_flags & 0xffff0000u) | return_flags;

    if (DEBUG_AgentResumeAfterTerminate())
        return true;
    if (error != NULL)
        *error = "Debugger did not resume the DOS shell after target termination";
    return false;
#else
    return false;
#endif
}

} // namespace dosbox_agent
#endif // defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
