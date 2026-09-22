#include "debug_mcp.h"

#include "config.h"

#if defined(C_DEBUG) && C_DEBUG && \
        ((defined(C_SDL_NET) && C_SDL_NET) || \
         (defined(C_SDL2_NET) && C_SDL2_NET))

#include "debug.h"

/* For the MEM, KEY, SHOT and PAUSE commands added by the harness patch. */
#include "bios.h"
#include "mem.h"
#include "inout.h"
#include "pic.h"
#include "paging.h"   /* mem_readb_checked: the handler-aware reader */
#include "vga.h"     /* the character-generator pages SHOT dumps */
#include "render.h"  /* the frame and palette SHOT dumps */
#include "regs.h"    /* reg_ax..reg_eip and SegValue, for REGS */
#include "dos_inc.h"  /* dos.psp(): where DOS put the program (PROG) */
/* dos_inc.h pulls in cross.h, which does `#define snprintf _snprintf` for
   MSVC. This file uses std::snprintf throughout, and that macro turns every
   one of them into a missing member of `std`: six errors, all pointing at
   lines that were fine before the include. Drop the macro beside the include
   that brought it in, rather than working around it at each use site. */
#ifdef snprintf
#undef snprintf
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(C_SDL2_NET) && C_SDL2_NET
#include <SDL2/SDL_net.h>
#else
#include <SDL_net.h>
#endif

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* ---------------------------------------------------------------------------
   Transport layer for the AI-driven reverse-engineering harness

   Upstream, the debugger's control channel is a TCP *client*: DOSBox-X connects to
   an external MCP server on 127.0.0.1, which needs SDL_net. Two things follow from
   that, both of which got in the way of scripted debugging here:

     * in the Visual Studio builds SDL_net comes from the legacy vs/config.h
       (C_SDL_NET), so the channel is present but insists on a TCP server;
     * when there is no listener the channel just retries in the background and
       never reads anything from the parent process.

   This layer keeps the same line protocol but makes the transport a choice:

     * stdio  -- the parent process writes REQ lines to our stdin and reads the
                 replies from our stdout. No sockets, no external server, no extra
                 dependency; works the same on Windows and POSIX. Selected with
                 [dosbox] mcp_stdio=1.
     * tcp    -- the upstream behaviour, unchanged, for when an external MCP
                 server is in use ([dosbox] mcp_server=<port>).

   The two commands the reverse-engineering work needs are served the same way over
   either transport:

     MEM <p|l|s> <address> <length>   hex dump of guest memory (video RAM: 0xb8000
                                      for the text plane, plane 2 of 0xa0000 for the
                                      character generator)
     KEY <name | scan[:ascii]>        inject a keystroke through the BIOS keyboard
                                      buffer, so a DOS program reads it normally
--------------------------------------------------------------------------- */

/* The emulator's own pause flag (sdlmain.cpp). PAUSE/RESUME flip it directly, so
   no interactive debugger session is involved. */
extern bool is_paused;

enum ControlTransport { CONTROL_TRANSPORT_TCP, CONTROL_TRANSPORT_STDIO };

ControlTransport g_transport = CONTROL_TRANSPORT_TCP;
bool g_stdio_ready = false;
bool g_stdio_opened = false;
std::string g_transport_error;

#if defined(WIN32)
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#if (defined(C_SDL_NET) && C_SDL_NET) || (defined(C_SDL2_NET) && C_SDL2_NET)
TCPsocket g_socket = nullptr;
SDLNet_SocketSet g_socket_set = nullptr;
#endif

static bool Ct_Init(const ControlTransport transport)
{
    g_transport = transport;
    g_transport_error.clear();

    if (transport == CONTROL_TRANSPORT_STDIO) {
#if defined(WIN32)
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        g_stdio_opened = true;
        g_stdio_ready = false;
        return true;
    }

#if (defined(C_SDL_NET) && C_SDL_NET) || (defined(C_SDL2_NET) && C_SDL2_NET)
    return SDLNet_Init() >= 0;
#else
    g_transport_error = "this build has no SDL_net; use [dosbox] mcp_stdio=1";
    return false;
#endif
}

static void Ct_Close()
{
    if (g_transport == CONTROL_TRANSPORT_STDIO) {
        g_stdio_opened = false;
        g_stdio_ready = false;
        return;
    }

#if (defined(C_SDL_NET) && C_SDL_NET) || (defined(C_SDL2_NET) && C_SDL2_NET)
    if (g_socket) {
        if (g_socket_set)
            SDLNet_TCP_DelSocket(g_socket_set, g_socket);

        SDLNet_TCP_Close(g_socket);
        g_socket = nullptr;
    }

    if (g_socket_set) {
        SDLNet_FreeSocketSet(g_socket_set);
        g_socket_set = nullptr;
    }
#endif
}

static bool Ct_Connect(const char* host, uint16_t port)
{
    if (g_transport == CONTROL_TRANSPORT_STDIO) {
        /* Nothing to connect: the parent process is already on the other end. */
        return true;
    }

#if (defined(C_SDL_NET) && C_SDL_NET) || (defined(C_SDL2_NET) && C_SDL2_NET)
    IPaddress address = {};

    if (SDLNet_ResolveHost(&address, host, port) < 0) {
        g_transport_error = SDLNet_GetError();
        return false;
    }

    TCPsocket socket = SDLNet_TCP_Open(&address);

    if (!socket)
        return false;

    SDLNet_SocketSet socket_set = SDLNet_AllocSocketSet(1);

    if (!socket_set) {
        SDLNet_TCP_Close(socket);
        return false;
    }

    if (SDLNet_TCP_AddSocket(socket_set, socket) < 0) {
        SDLNet_FreeSocketSet(socket_set);
        SDLNet_TCP_Close(socket);
        return false;
    }

    g_socket = socket;
    g_socket_set = socket_set;
    return true;
#else
    (void)host; (void)port;
    return false;
#endif
}

/* Is there something to read? Answers for both transports. */
static bool Ct_WaitReadable(uint32_t timeout_ms)
{
    if (g_transport == CONTROL_TRANSPORT_STDIO) {
        if (!g_stdio_opened) {
            g_stdio_ready = false;
            return false;
        }

#if defined(WIN32)
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD available = 0;

        if (input != NULL && input != INVALID_HANDLE_VALUE &&
                PeekNamedPipe(input, NULL, 0, NULL, &available, NULL) &&
                available > 0) {
            g_stdio_ready = true;
            return true;
        }
#else
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(0, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 1000;
        if (select(1, &fds, NULL, NULL, &tv) > 0) {
            g_stdio_ready = true;
            return true;
        }
#endif

        if (timeout_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        g_stdio_ready = false;
        return false;
    }

#if (defined(C_SDL_NET) && C_SDL_NET) || (defined(C_SDL2_NET) && C_SDL2_NET)
    if (!g_socket_set)
        return false;

    const int ready = SDLNet_CheckSockets(g_socket_set, timeout_ms);

    if (ready > 0 && g_socket)
        return SDLNet_SocketReady(g_socket) != 0;

    if (ready == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    return false;
#else
    (void)timeout_ms;
    return false;
#endif
}

static int Ct_Recv(void* data, int maxlen)
{
    if (maxlen <= 0)
        return 0;

    if (g_transport == CONTROL_TRANSPORT_STDIO) {
        g_stdio_ready = false;

#if defined(WIN32)
        const int got = _read(_fileno(stdin), data, maxlen);
#else
        const int got = (int)read(0, data, (size_t)maxlen);
#endif
        if (got <= 0)
            g_transport_error = "stdin closed by the parent process";

        return got;
    }

#if (defined(C_SDL_NET) && C_SDL_NET) || (defined(C_SDL2_NET) && C_SDL2_NET)
    if (!g_socket)
        return 0;

    return SDLNet_TCP_Recv(g_socket, data, maxlen);
#else
    return 0;
#endif
}

static int Ct_Send(const void* data, int len)
{
    if (len <= 0)
        return 0;

    if (g_transport == CONTROL_TRANSPORT_STDIO) {
        const size_t written = std::fwrite(data, 1, (size_t)len, stdout);
        std::fflush(stdout);
        return (int)written;
    }

#if (defined(C_SDL_NET) && C_SDL_NET) || (defined(C_SDL2_NET) && C_SDL2_NET)
    if (!g_socket)
        return 0;

    return SDLNet_TCP_Send(g_socket, data, len);
#else
    return 0;
#endif
}

static const char* Ct_Error()
{
#if (defined(C_SDL_NET) && C_SDL_NET) || (defined(C_SDL2_NET) && C_SDL2_NET)
    if (g_transport == CONTROL_TRANSPORT_TCP)
        return SDLNet_GetError();
#endif
    return g_transport_error.c_str();
}


namespace {

constexpr const char* CONTROL_HOST = "127.0.0.1";

constexpr int SOCKET_POLL_TIMEOUT_MS = 50;
constexpr int RECONNECT_DELAY_MS = 1000;

constexpr size_t MAX_INCOMING_MESSAGES = 1024;
constexpr size_t MAX_OUTGOING_MESSAGES = 1024;
constexpr size_t MAX_CAPTURE_LINES = 16384;

// Protect against a peer continuously sending data without '\n'.
constexpr size_t MAX_RECEIVE_BUFFER = 1024 * 1024;

std::atomic<bool> g_running{false};
std::atomic<bool> g_connected{false};

uint16_t g_port = 0;

std::thread g_thread;

std::mutex g_incoming_mutex;
std::deque<std::string> g_incoming;

std::mutex g_outgoing_mutex;
std::deque<std::string> g_outgoing;

std::mutex g_capture_mutex;
bool g_capture_active = false;
size_t g_capture_discarded = 0;
std::vector<std::string> g_capture_lines;

//
// All socket operations happen ONLY in the I/O thread.
//
// This is important because SDLNet_TCP_Send/Recv/Close don't
// have to be synchronized with the DOSBox-X emulation thread.
//

std::string g_receive_buffer;

struct ControlRequest {
    std::string id;
    std::string command;
    std::string payload;
};

bool IsSpace(const char c)
{
    return c == ' ' || c == '\t';
}

std::string TrimLeft(std::string value)
{
    while (!value.empty() && IsSpace(value.front()))
        value.erase(value.begin());

    return value;
}

std::string UpperAscii(std::string value)
{
    for (auto& c : value) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - ('a' - 'A'));
    }

    return value;
}

std::string SanitizeProtocolLine(std::string line)
{
    for (auto& c : line) {
        if (c == '\r' || c == '\n')
            c = ' ';
    }

    return line;
}

std::string FirstWordUpper(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return {};

    const auto end = value.find_first_of(" \t", begin);
    if (end == std::string::npos)
        return UpperAscii(value.substr(begin));

    return UpperAscii(value.substr(begin, end - begin));
}

bool IsResumeDebuggerCommand(const std::string& command)
{
    const auto word = FirstWordUpper(command);
    return word == "RUN" || word == "RUNWATCH" || word == "VRT";
}

void BeginCapture()
{
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    g_capture_lines.clear();
    g_capture_discarded = 0;
    g_capture_active = true;
}

std::vector<std::string> EndCapture()
{
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    g_capture_active = false;

    std::vector<std::string> lines;
    lines.swap(g_capture_lines);

    if (g_capture_discarded > 0) {
        char message[128];
        std::snprintf(message,
                      sizeof(message),
                      "[%lu debug output lines discarded]",
                      static_cast<unsigned long>(g_capture_discarded));
        lines.emplace_back(message);
        g_capture_discarded = 0;
    }

    return lines;
}

void CloseConnection()
{
    g_connected.store(false);
    Ct_Close();
    g_receive_buffer.clear();
}

bool Connect()
{
    if (!Ct_Connect(CONTROL_HOST, g_port))
        return false;

    g_receive_buffer.clear();
    g_connected.store(true);
    return true;
}

bool SendAll(const std::string& data)
{
    if (!g_connected.load())
        return false;

    const char* ptr = data.data();
    size_t remaining = data.size();

    while (remaining > 0) {
        const int chunk =
                remaining > static_cast<size_t>(INT_MAX)
                        ? INT_MAX
                        : static_cast<int>(remaining);

        const int sent = Ct_Send(
                ptr,
                chunk);

        if (sent <= 0)
            return false;

        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }

    return true;
}

void QueueIncoming(std::string message)
{
    std::lock_guard<std::mutex> lock(g_incoming_mutex);

    if (g_incoming.size() >= MAX_INCOMING_MESSAGES)
        g_incoming.pop_front();

    g_incoming.emplace_back(std::move(message));
}

bool PopIncoming(std::string& message)
{
    std::lock_guard<std::mutex> lock(g_incoming_mutex);

    if (g_incoming.empty())
        return false;

    message = std::move(g_incoming.front());
    g_incoming.pop_front();
    return true;
}

void ProcessReceiveBuffer()
{
    for (;;) {
        const auto newline = g_receive_buffer.find('\n');

        if (newline == std::string::npos)
            break;

        std::string line =
                g_receive_buffer.substr(0, newline);

        g_receive_buffer.erase(0, newline + 1);

        // Support both "\n" and "\r\n".
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.empty())
            continue;

        QueueIncoming(std::move(line));
    }
}

bool Receive()
{
    char buffer[8192];

    const int received =
            Ct_Recv(
                    buffer,
                    sizeof(buffer));

    if (received <= 0)
        return false;

    g_receive_buffer.append(
            buffer,
            static_cast<size_t>(received));

    if (g_receive_buffer.size() > MAX_RECEIVE_BUFFER) {
        std::fprintf(stderr,
                     "[ControlClient] receive buffer overflow\n");

        return false;
    }

    ProcessReceiveBuffer();

    return true;
}

bool FlushOutgoing()
{
    for (;;) {
        std::string message;

        {
            std::lock_guard<std::mutex> lock(g_outgoing_mutex);

            if (g_outgoing.empty())
                return true;

            message = g_outgoing.front();
        }

        if (!SendAll(message))
            return false;

        {
            std::lock_guard<std::mutex> lock(g_outgoing_mutex);

            if (!g_outgoing.empty())
                g_outgoing.pop_front();
        }
    }
}

void SleepReconnectDelay()
{
    //
    // Do it in short steps so ControlServer_Stop()
    // doesn't have to wait a full second.
    //

    constexpr int step_ms = 50;

    for (int elapsed = 0;
         elapsed < RECONNECT_DELAY_MS && g_running.load();
         elapsed += step_ms) {

        std::this_thread::sleep_for(
                std::chrono::milliseconds(step_ms));
    }
}

void ThreadMain()
{
    while (g_running.load()) {

        //
        // Connect / reconnect
        //

        if (!g_socket) {
            if (!Connect()) {
                SleepReconnectDelay();
                continue;
            }
        }

        //
        // Send everything currently queued.
        //

        if (!FlushOutgoing()) {
            std::fprintf(stderr,
                         "[ControlClient] connection lost while sending\n");

            CloseConnection();
            continue;
        }

        //
        // Wait for incoming data.
        //

        if (!Ct_WaitReadable(SOCKET_POLL_TIMEOUT_MS))
            continue;

        {
            if (!Receive()) {
                std::fprintf(stderr,
                             "[ControlClient] connection closed\n");

                CloseConnection();
                continue;
            }
        }
    }

    CloseConnection();
}

bool ParseControlRequest(const std::string& line,
                         ControlRequest& request,
                         std::string& error)
{
    if (line.compare(0, 4, "REQ ") != 0) {
        error = "expected REQ <id> <PING|BREAK|EXEC>";
        return false;
    }

    const auto id_begin = line.find_first_not_of(" \t", 4);
    if (id_begin == std::string::npos) {
        error = "missing request id";
        return false;
    }

    const auto id_end = line.find_first_of(" \t", id_begin);
    if (id_end == std::string::npos) {
        error = "missing request command";
        return false;
    }

    request.id = line.substr(id_begin, id_end - id_begin);

    const auto command_begin = line.find_first_not_of(" \t", id_end);
    if (command_begin == std::string::npos) {
        error = "missing request command";
        return false;
    }

    const auto command_end = line.find_first_of(" \t", command_begin);
    if (command_end == std::string::npos) {
        request.command = UpperAscii(line.substr(command_begin));
        request.payload.clear();
    } else {
        request.command = UpperAscii(
                line.substr(command_begin, command_end - command_begin));
        request.payload = TrimLeft(line.substr(command_end));
    }

    if (request.command != "PING" &&
            request.command != "BREAK" &&
            request.command != "EXEC" &&
            request.command != "MEM" &&
            request.command != "KEY" &&
            request.command != "REGS" &&
            request.command != "PROG" &&
            request.command != "SHOT" &&
            request.command != "PAUSE" &&
            request.command != "RESUME") {
        error = "unknown request command";
        return false;
    }

    if (request.command == "EXEC" && request.payload.empty()) {
        error = "missing debugger command";
        return false;
    }

    return true;
}

void SendResponse(const std::string& id,
                  const bool ok,
                  const std::vector<std::string>& lines)
{
    std::string response = std::string("BEGIN ") + id + (ok ? " OK" : " ERR");

    for (const auto& line : lines) {
        response.push_back('\n');
        response += SanitizeProtocolLine(line);
    }

    response += "\nEND ";
    response += id;

    ControlServer_Send(std::move(response));
}

void SendErrorResponse(const std::string& id, const std::string& error)
{
    SendResponse(id.empty() ? "0" : id, false, {error});
}

bool OutgoingQueueEmpty()
{
    std::lock_guard<std::mutex> lock(g_outgoing_mutex);
    return g_outgoing.empty();
}

void WaitForOutgoingDrain()
{
    constexpr int max_wait_ms = 250;
    constexpr int sleep_step_ms = 5;

    for (int elapsed_ms = 0;
         elapsed_ms < max_wait_ms && g_running.load();
         elapsed_ms += sleep_step_ms) {
        if (OutgoingQueueEmpty() || !g_connected.load())
            return;

        std::this_thread::sleep_for(
                std::chrono::milliseconds(sleep_step_ms));
    }
}

/* ---------------------------------------------------------------------------
   MEM and KEY for the AI-driven reverse-engineering harness
--------------------------------------------------------------------------- */

/* Read one byte of guest memory the way the debugger's own dump commands do:
   through the memory-map handlers, not the raw RAM array. The VGA window
   (0xa0000/0xb8000) is only reachable this way. */
static uint8_t GuestReadByte(const uint32_t address)
{
    uint8_t value = 0;

    if (mem_readb_checked(static_cast<PhysPt>(address), &value))
        value = 0;

    return value;
}

bool ParseWord(std::string& text, std::string& word)
{
    text = TrimLeft(text);

    if (text.empty())
        return false;

    const auto end = text.find_first_of(" \t");

    if (end == std::string::npos) {
        word = text;
        text.clear();
        return true;
    }

    word = text.substr(0, end);
    text = TrimLeft(text.substr(end + 1));
    return true;
}

bool ParseHexNumber(const std::string& text, uint32_t& value)
{
    if (text.empty())
        return false;

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 16);

    if (end == text.c_str() || *end != '\0')
        return false;

    value = static_cast<uint32_t>(parsed);
    return true;
}

/* MEM <p|l|s> <address> <length>: one line per sixteen bytes,
   "address: xx xx ...".  p = physical, l = linear/virtual, s = seg:off. */
bool ControlReadMemory(const std::string& payload,
                       std::vector<std::string>& lines,
                       std::string& error)
{
    std::string rest = payload;
    std::string space, address, length;

    if (!ParseWord(rest, space) || !ParseWord(rest, address) || !ParseWord(rest, length)) {
        error = "usage: MEM <p|l|s> <address> <length>";
        return false;
    }

    const char which = static_cast<char>(std::toupper(static_cast<unsigned char>(space[0])));
    uint32_t base = 0;
    uint32_t count = 0;

    if (which == 'S') {
        const auto colon = address.find(':');

        if (colon == std::string::npos) {
            error = "segmented address must be seg:off";
            return false;
        }

        uint32_t segment = 0;
        uint32_t offset = 0;

        if (!ParseHexNumber(address.substr(0, colon), segment) ||
                !ParseHexNumber(address.substr(colon + 1), offset)) {
            error = "bad segmented address";
            return false;
        }

        base = (segment << 4) + offset;
    } else if (which == 'P' || which == 'L') {
        if (!ParseHexNumber(address, base)) {
            error = "bad address";
            return false;
        }
    } else {
        error = "address space must be p (physical), l (linear) or s (segmented)";
        return false;
    }

    if (!ParseHexNumber(length, count)) {
        error = "bad length";
        return false;
    }

    if (count == 0 || count > 65536) {
        error = "length must be 1..65536";
        return false;
    }

    for (uint32_t at = 0; at < count; at += 16) {
        char prefix[32];
        std::snprintf(prefix, sizeof(prefix), "%08x:", static_cast<unsigned>(base + at));

        std::string line(prefix);
        const uint32_t stop = (at + 16 < count) ? (at + 16) : count;

        for (uint32_t i = at; i < stop; ++i) {
            const uint8_t value = GuestReadByte(base + i);
            (void)which;

            char byte[8];
            std::snprintf(byte, sizeof(byte), " %02x", value);
            line += byte;
        }

        lines.emplace_back(std::move(line));
    }

    return true;
}

/* KEY <name | scan[:ascii]>: one keystroke into the BIOS keyboard buffer, which
   is what a DOS program reads through INT 16h or the BIOS data area. */
bool ControlInjectKey(const std::string& payload, std::string& error)
{
    static const struct { const char* name; uint16_t code; } kNamed[] = {
        {"ESC", 0x011B}, {"ENTER", 0x1C0D}, {"SPACE", 0x3920}, {"TAB", 0x0F09},
        {"BACKSPACE", 0x0E08}, {"UP", 0x4800}, {"DOWN", 0x5000}, {"LEFT", 0x4B00},
        {"RIGHT", 0x4D00}, {"PGUP", 0x4900}, {"PGDN", 0x5100}, {"HOME", 0x4700},
        {"END", 0x4F00}, {"INS", 0x5200}, {"DEL", 0x5300},
        {"F1", 0x3B00}, {"F2", 0x3C00}, {"F3", 0x3D00}, {"F4", 0x3E00},
        {"F5", 0x3F00}, {"F6", 0x4000}, {"F7", 0x4100}, {"F8", 0x4200},
        {"F9", 0x4300}, {"F10", 0x4400},
    };

    std::string rest = payload;
    std::string word;

    if (!ParseWord(rest, word)) {
        error = "usage: KEY <name | scan[:ascii]>";
        return false;
    }

    const std::string upper = UpperAscii(word);

    for (const auto& named : kNamed) {
        if (upper == named.name) {
            if (!BIOS_AddKeyToBuffer(named.code)) {
                error = "keyboard buffer is full";
                return false;
            }
            return true;
        }
    }

    uint32_t scan = 0;
    uint32_t ascii = 0;
    const auto colon = word.find(':');

    if (colon == std::string::npos) {
        if (!ParseHexNumber(word, scan)) {
            error = "unknown key name, and not a hexadecimal scancode";
            return false;
        }
    } else if (!ParseHexNumber(word.substr(0, colon), scan) ||
               !ParseHexNumber(word.substr(colon + 1), ascii)) {
        error = "bad scan:ascii";
        return false;
    }

    if (scan > 0xFF || ascii > 0xFF) {
        error = "scan and ascii must be single bytes";
        return false;
    }

    if (!BIOS_AddKeyToBuffer(static_cast<uint16_t>((scan << 8) | ascii))) {
        error = "keyboard buffer is full";
        return false;
    }

    return true;
}


/* ---------------------------------------------------------------------------
   REGS for the AI-driven reverse-engineering harness
--------------------------------------------------------------------------- */

/* REGS: the live CPU registers, one line each for the 16- and 32-bit views.
   The interactive debugger draws these straight from the same globals into its
   curses view, and **no textual command returns them** -- which cost a real
   debugging session: a script cannot learn CS:IP to build a breakpoint address,
   and the only way to read a register was `LOG n`, which writes the whole ring
   buffer to LOGCPU.TXT and needs a file round-trip for one line of data.
   `EXEC CPU` looks like the answer and is not: it prints cr0/cr3, the GDT/IDT
   and the flags, never the general registers.

   Read-only and payload-free: it takes no argument and only reports what the
   CPU holds right now. The values are the ones the guest will use on its next
   instruction, so this is what a breakpoint handshake reads to confirm it
   stopped where it was asked to. */
bool ControlReadRegisters(std::vector<std::string>& lines, std::string& error)
{
    (void)error;

    char buffer[512];

    std::snprintf(buffer, sizeof(buffer),
                  "CS=%04x IP=%04x DS=%04x ES=%04x SS=%04x SP=%04x BP=%04x "
                  "AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x FLAGS=%04x",
                  (unsigned)SegValue(cs), (unsigned)reg_ip,
                  (unsigned)SegValue(ds), (unsigned)SegValue(es),
                  (unsigned)SegValue(ss), (unsigned)reg_sp, (unsigned)reg_bp,
                  (unsigned)reg_ax, (unsigned)reg_bx, (unsigned)reg_cx,
                  (unsigned)reg_dx, (unsigned)reg_si, (unsigned)reg_di,
                  (unsigned)reg_flags);
    lines.push_back(buffer);

    std::snprintf(buffer, sizeof(buffer),
                  "EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x "
                  "EBP=%08x ESP=%08x EIP=%08x",
                  (unsigned)reg_eax, (unsigned)reg_ebx, (unsigned)reg_ecx,
                  (unsigned)reg_edx, (unsigned)reg_esi, (unsigned)reg_edi,
                  (unsigned)reg_ebp, (unsigned)reg_esp, (unsigned)reg_eip);
    lines.push_back(buffer);

    return true;
}


/* ---------------------------------------------------------------------------
   PROG for the AI-driven reverse-engineering harness
--------------------------------------------------------------------------- */

/* PROG: where DOS put the program that is running.

   This is the question that costs a session when it cannot be asked. The load
   segment is decided by DOS at run time and appears nowhere in the executable,
   so a load-relative offset from the disassembly (`0x10f21`) cannot be turned
   into a breakpoint address without it -- and the obvious substitutes are all
   wrong: the CS at an arbitrary moment may be a BIOS or DOS segment (the game
   sits in INT 16h while it waits for a key), and scanning memory for the
   image is a whole-640 KB dump for one number.

   Reported: the PSP, and the load segment as DOS's own convention (PSP + 0x10,
   which is what the loader used for this EXE). Both are printed rather than
   just the load segment, so the convention is visible and can be checked
   against memory instead of trusted. */
bool ControlReadProgram(std::vector<std::string>& lines, std::string& error)
{
    (void)error;

    const unsigned psp = static_cast<unsigned>(dos.psp());
    char buffer[256];

    std::snprintf(buffer, sizeof(buffer), "PSP=%04x LOAD=%04x", psp, psp + 0x10u);
    lines.push_back(buffer);

    /* The PSP's environment block holds the running program's full path after
       its variables, which is the cheapest way to say *which* program this is:
       a segment number alone does not tell a reader whether the game or a shell
       is resident. Layout: zero-terminated variables, a terminating empty
       string, a word count, then the path.

       Offsets inside the PSP are offsets inside a *paragraph*, so the word at
       PSP:0x2c is at linear `psp * 16 + 0x2c` -- not `(psp + 0x2c) * 16`, which
       is what this read for a while: it looked at a different paragraph 0x2c
       segments up, found something that was not a segment, and printed no ENV
       line at all (an earlier version also never noticed the difference). */
    const unsigned psp_base = psp * 16u;
    const unsigned environment = static_cast<unsigned>(GuestReadByte(psp_base + 0x2cu)) |
                                 (static_cast<unsigned>(GuestReadByte(psp_base + 0x2du)) << 8);

    std::string name;

    if (environment > 0 && environment < 0xffffu) {
        const unsigned base = environment * 16u;
        unsigned at = 0;

        while (at < 32768u) {
            if (GuestReadByte(base + at) == 0) {
                if (GuestReadByte(base + at + 1u) == 0) {
                    at += 2u;
                    break;
                }
            }

            ++at;
        }

        at += 2u; /* the count word before the path */

        for (unsigned index = 0; index < 260u; ++index) {
            const uint8_t value = GuestReadByte(base + at + index);

            if (value == 0)
                break;

            name.push_back(static_cast<char>(value < 0x20u ? ' ' : value));
        }
    }

    if (!name.empty())
        lines.push_back("ENV=" + name);

    return true;
}


/* ---------------------------------------------------------------------------
   SHOT and PAUSE/RESUME for the AI-driven reverse-engineering harness
--------------------------------------------------------------------------- */

/* PAUSE and RESUME flip the emulator's own pause flag. Nothing goes through the
   interactive debugger, so this works with stdout on a pipe, and the harness
   patch makes the pause loop keep servicing this channel. */
bool ControlSetPaused(const bool paused, std::string& error)
{
    ::is_paused = paused;
    (void)error;
    return true;
}

/* The character generator: page 0 or 1, the very pointer the emulator renders
   from. Reading the same bytes through the CPU's planar aperture is not
   equivalent -- in text modes that aperture is not mapped for reads, which is
   how an earlier version of this command produced 8 KB of 0xff and made every
   glyph solid. */
static const uint8_t* HarnessFontPage(const int page)
{
    const uint8_t* data = vga.draw.font_tables[page];

    if (data == NULL)
        data = &vga.draw.font[page * 8192];

    return data;
}

/* SHOT <base path>: write a complete, self-describing snapshot of the screen.
   Files written (all host paths: DOSBox-X runs on the host):
     <base>.text   32768 bytes  the whole text window (0xb8000..0xbffff)
     <base>.font   16384 bytes  both character-generator pages: 256 glyphs of 32
                                bytes each (planar memory de-interleaved)
     <base>.pal      768 bytes  256 RGB triples read from the DAC (6-bit values)
     <base>.rgb     1024 bytes  the palette the renderer applied (8-bit, RGBA)
     <base>.frame   pitch x height bytes  the last frame the emulator drew
     <base>.json    geometry: video mode, ticks, CRTC registers, text columns,
                    glyph height, text start offset, frame layout

   The frame is the ground truth for the picture: it already contains the
   nine-dot character cells, the panning, the cursor, and the palette as
   applied. The planes underneath are what a reverse-engineer reads, because the
   game uploads its own glyphs and reprograms the DAC, so neither can be
   assumed. One SHOT request writes both, so the two views can never disagree
   about which moment they describe. */
/* A snapshot has to be one moment. A guest may change its palette while it runs,
   so reading the planes and the frame without stopping the emulator can produce
   pieces of different moments -- which is exactly how the first version of this
   produced a picture whose colours the planes could not explain. Pausing here
   costs one frame and makes every file describe the same instant; RAII, so the
   emulator is left running even if a write fails. */
struct ScopedPause {
    bool was_paused;

    ScopedPause() : was_paused(::is_paused) { ::is_paused = true; }
    ~ScopedPause() { ::is_paused = was_paused; }
};

bool ControlWriteSnapshot(const std::string& payload, std::vector<std::string>& lines, std::string& error)
{
    const ScopedPause paused_for_consistency;
    (void)paused_for_consistency;
    std::string rest = payload;
    std::string base;

    if (!ParseWord(rest, base) || base.empty()) {
        error = "usage: SHOT <base path without extension>";
        return false;
    }

    /* the text window: all 32 KB, so the reader can apply the CRTC's start
       address (the games pan by moving it) and still see every cell */
    std::vector<uint8_t> text(32768);
    for (size_t i = 0; i < text.size(); ++i)
        text[i] = GuestReadByte(0xb8000 + static_cast<uint32_t>(i));

    /* the character generator: both pages, de-interleaved.
       The font lives in the VGA's planar (four-plane) memory, and DOSBox keeps
       that memory as one array with the four planes interleaved a byte at a
       time: byte k of the character generator is at offset 4*k from the font
       pointer. Reading the pointers linearly yields a mix of the character
       plane, the attribute plane and the two others -- which is exactly how an
       earlier version of this command produced 16 KB of plausible-looking
       noise, and a 100 % "lit" screen when the harness rendered from it. The
       glyph this reads for 0x41 on the probe is the BIOS 8x16 "A"
       (00 00 7e 81 a5 81 81 bd 99 81 81 7e), and rendering the text plane
       through it reproduces the emulator's own frame to within the cursor. */
    std::vector<uint8_t> font(16384);
    const size_t linear_bytes = static_cast<size_t>(vga.mem.memsize);
    for (int page = 0; page < 2; ++page) {
        const uint8_t* source = HarnessFontPage(page);
        /* The subtraction has to come *after* the range check: when the tables
           are not set up, HarnessFontPage() falls back to vga.draw.font, which
           is not inside vga.mem.linear, and subtracting the two unrelated
           pointers is undefined behaviour (it happened to work and would not
           necessarily keep working). */
        const bool in_linear = source >= vga.mem.linear &&
                               source < vga.mem.linear + linear_bytes;

        for (size_t k = 0; k < 8192; ++k) {
            if (!in_linear)
                continue;

            const size_t offset = static_cast<size_t>(source - vga.mem.linear) + k * 4u;

            if (offset < linear_bytes)
                font[static_cast<size_t>(page) * 8192 + k] = vga.mem.linear[offset];
        }
    }

    /* the DAC: index 0, then three reads per entry */
    std::vector<uint8_t> palette(768);
    IO_WriteB(0x3c7, 0);
    for (int i = 0; i < 256; ++i) {
        palette[i * 3 + 0] = IO_ReadB(0x3c9);
        palette[i * 3 + 1] = IO_ReadB(0x3c9);
        palette[i * 3 + 2] = IO_ReadB(0x3c9);
    }

    /* the palette the renderer applied: one 4-byte RGBA entry per index */
    std::vector<uint8_t> rgb(1024);
    std::memcpy(rgb.data(), &render.pal.rgb, rgb.size());

    /* the last frame the emulator drew */
    std::vector<uint8_t> frame;
    unsigned frame_width = 0, frame_height = 0, frame_bpp = 0, frame_pitch = 0;
    unsigned frame_dblw = 0, frame_dblh = 0;

    if (scalerSourceCacheBuffer != NULL && render.src.width > 0 && render.src.height > 0) {
        const size_t bytes = static_cast<size_t>(render.scale.cachePitch) * render.src.height;

        if (render.scale.cachePitch > 0 && bytes <= scalerSourceCacheBufferSize) {
            frame_width = static_cast<unsigned>(render.src.width);
            frame_height = static_cast<unsigned>(render.src.height);
            frame_bpp = static_cast<unsigned>(render.src.bpp);
            frame_pitch = static_cast<unsigned>(render.scale.cachePitch);
            frame_dblw = render.src.dblw ? 1u : 0u;
            frame_dblh = render.src.dblh ? 1u : 0u;
            frame.resize(bytes);
            std::memcpy(frame.data(), scalerSourceCacheBuffer, bytes);
        }
    }

    /* CRTC geometry, so the reader knows the width and the glyph height */
    uint8_t crtc[32] = {};
    for (int index = 0; index < 32; ++index) {
        IO_WriteB(0x3d4, static_cast<uint8_t>(index));
        crtc[index] = IO_ReadB(0x3d5);
    }

    const uint8_t video_mode = GuestReadByte(0x449);
    const unsigned long ticks = static_cast<unsigned long>(PIC_Ticks);
    const unsigned columns = static_cast<unsigned>(vga.draw.blocks);
    const unsigned glyph_height = static_cast<unsigned>((crtc[9] & 0x1F) + 1);
    const unsigned text_offset =
        (static_cast<unsigned>(crtc[0x0c]) | (static_cast<unsigned>(crtc[0x0d]) << 8)) * 2u;

    struct { const char* suffix; const uint8_t* data; size_t size; } files[] = {
        {"text",  text.data(), text.size()},
        {"font",  font.data(), font.size()},
        {"pal",   palette.data(), palette.size()},
        {"rgb",   rgb.data(), rgb.size()},
        {"frame", frame.empty() ? NULL : frame.data(), frame.size()},
    };

    for (const auto& file : files) {
        if (file.data == NULL)
            continue;

        const std::string path = base + "." + file.suffix;
        std::FILE* handle = std::fopen(path.c_str(), "wb");

        if (handle == NULL) {
            error = "cannot write " + path;
            return false;
        }

        const size_t written = std::fwrite(file.data, 1, file.size, handle);
        std::fclose(handle);

        if (written != file.size) {
            error = "short write to " + path;
            return false;
        }
    }

    const std::string header_path = base + ".json";
    std::FILE* header = std::fopen(header_path.c_str(), "w");

    if (header == NULL) {
        error = "cannot write " + header_path;
        return false;
    }

    std::fprintf(header,
                 "{\n"
                 "  \"video_mode\": %u,\n"
                 "  \"ticks\": %lu,\n"
                 "  \"text_columns\": %u,\n"
                 "  \"glyph_height\": %u,\n"
                 "  \"text_offset\": %u,\n"
                 "  \"frame_width\": %u,\n"
                 "  \"frame_height\": %u,\n"
                 "  \"frame_bpp\": %u,\n"
                 "  \"frame_pitch\": %u,\n"
                 "  \"frame_dblw\": %u,\n"
                 "  \"frame_dblh\": %u,\n"
                 "  \"font_stride\": 32,\n"
                 "  \"char9dot\": %u,\n"
                 "  \"blinking\": %u,\n"
                 "  \"blink_phase\": %u,\n"
                 "  \"attr_mode_control\": %u,\n"
                 "  \"underline_location\": %u,\n"
                 "  \"panning\": %u,\n"
                 "  \"draw_address\": %u,\n"
                 "  \"paused_for_snapshot\": 1,\n"
                 "  \"text_bytes\": %lu,\n"
                 "  \"font_bytes\": %lu,\n"
                 "  \"palette_bytes\": %lu,\n"
                 "  \"rgb_bytes\": %lu,\n"
                 "  \"frame_bytes\": %lu,\n"
                 "  \"crtc\": [",
                 static_cast<unsigned>(video_mode),
                 ticks,
                 columns,
                 glyph_height,
                 text_offset,
                 frame_width,
                 frame_height,
                 frame_bpp,
                 frame_pitch,
                 frame_dblw,
                 frame_dblh,
                 vga.draw.char9dot ? 1u : 0u,
                 static_cast<unsigned>(vga.draw.blinking),
                 vga.draw.blink ? 1u : 0u,
                 /* The attribute controller's mode register and the CRTC's
                    underline row: the two text-mode rules a reader cannot
                    derive from the planes. `attr_mode_control` bit 2 decides
                    whether a line-drawing glyph is extended into the ninth dot
                    (only for characters c0..df); `underline_location` is CRTC
                    register 0x0a bits 0..4. Both are read here rather than
                    assumed, because a renderer that guesses them either
                    invents pixels or calls real ones a disagreement. */
                 static_cast<unsigned>(vga.attr.mode_control),
                 static_cast<unsigned>(vga.crtc.underline_location & 0x1f),
                 static_cast<unsigned>(vga.draw.panning),
                 static_cast<unsigned>(vga.draw.address),
                 static_cast<unsigned long>(text.size()),
                 static_cast<unsigned long>(font.size()),
                 static_cast<unsigned long>(palette.size()),
                 static_cast<unsigned long>(rgb.size()),
                 static_cast<unsigned long>(frame.size()));

    for (int index = 0; index < 32; ++index)
        std::fprintf(header, "%s%u", index ? ", " : "", static_cast<unsigned>(crtc[index]));

    std::fprintf(header, "]\n}\n");
    std::fclose(header);

    lines.push_back("text  " + base + ".text " + std::to_string(text.size()));
    lines.push_back("font  " + base + ".font " + std::to_string(font.size()));
    lines.push_back("pal   " + base + ".pal " + std::to_string(palette.size()));
    lines.push_back("rgb   " + base + ".rgb " + std::to_string(rgb.size()));
    lines.push_back("frame " + base + ".frame " + std::to_string(frame.size()));
    lines.push_back("json  " + header_path);
    return true;
}
void ProcessControlCommand(const std::string& line)
{
    ControlRequest request;
    std::string error;

    if (!ParseControlRequest(line, request, error)) {
        SendErrorResponse(request.id, error);
        return;
    }

    if (request.command == "PING") {
        SendResponse(request.id, true, {"PONG"});
        return;
    }

    if (request.command == "EXEC" &&
            IsResumeDebuggerCommand(request.payload)) {
        SendResponse(request.id, true, {"OK"});
        WaitForOutgoingDrain();
        DEBUG_ExecuteCommand(request.payload.c_str());
        return;
    }

    BeginCapture();

    bool ok = true;
    if (request.command == "PROG") {
        std::vector<std::string> program_lines;
        std::string program_error;

        if (!ControlReadProgram(program_lines, program_error)) {
            SendErrorResponse(request.id, program_error);
            return;
        }

        SendResponse(request.id, true, program_lines);
        return;
    }

    if (request.command == "REGS") {
        std::vector<std::string> register_lines;
        std::string register_error;

        if (!ControlReadRegisters(register_lines, register_error)) {
            SendErrorResponse(request.id, register_error);
            return;
        }

        SendResponse(request.id, true, register_lines);
        return;
    }

    if (request.command == "SHOT") {
        std::vector<std::string> shot_lines;
        std::string shot_error;

        if (!ControlWriteSnapshot(request.payload, shot_lines, shot_error)) {
            SendErrorResponse(request.id, shot_error);
            return;
        }

        SendResponse(request.id, true, shot_lines);
        return;
    }

    if (request.command == "PAUSE" || request.command == "RESUME") {
        std::string pause_error;

        if (!ControlSetPaused(request.command == "PAUSE", pause_error)) {
            SendErrorResponse(request.id, pause_error);
            return;
        }

        SendResponse(request.id, true, {request.command == "PAUSE" ? "PAUSED" : "RUNNING"});
        return;
    }

    if (request.command == "MEM") {
        std::vector<std::string> read_lines;
        std::string read_error;

        if (!ControlReadMemory(request.payload, read_lines, read_error)) {
            SendErrorResponse(request.id, read_error);
            return;
        }

        SendResponse(request.id, true, read_lines);
        return;
    }

    if (request.command == "KEY") {
        std::string key_error;

        if (!ControlInjectKey(request.payload, key_error)) {
            SendErrorResponse(request.id, key_error);
            return;
        }

        SendResponse(request.id, true, {"OK"});
        return;
    }

    if (request.command == "BREAK") {
        DEBUG_EnableDebugger();
    } else {
        ok = DEBUG_ExecuteCommand(request.payload.c_str());
    }

    auto output = EndCapture();
    SendResponse(request.id, ok, output);
}

} // namespace

static bool BeginControlServer(const ControlTransport transport, const uint16_t port)
{
    if (g_running.load())
        return false;

    if (!Ct_Init(transport)) {
        std::fprintf(stderr,
                     "[ControlClient] transport init failed: %s\n",
                     Ct_Error());

        return false;
    }

    g_port = port;
    return true;
}

static void LaunchControlServer()
{
    {
        std::lock_guard<std::mutex> lock(g_incoming_mutex);
        g_incoming.clear();
    }

    {
        std::lock_guard<std::mutex> lock(g_outgoing_mutex);
        g_outgoing.clear();
    }

    g_running.store(true);

    g_thread = std::thread(ThreadMain);
}

/* Serve the protocol over stdin/stdout: the parent process is the other end.
   Selected with [dosbox] mcp_stdio=1. */
void ControlServer_StartStdio()
{
    if (!BeginControlServer(CONTROL_TRANSPORT_STDIO, 0))
        return;

    LaunchControlServer();
}

/* Upstream behaviour: connect to an external MCP server on 127.0.0.1:port. */
void ControlServer_Start(const uint16_t port)
{
    if (port == 0)
        return;

    if (!BeginControlServer(CONTROL_TRANSPORT_TCP, port))
        return;

    LaunchControlServer();
}

void ControlServer_Stop()
{
    if (!g_running.exchange(false))
        return;

    if (g_thread.joinable())
        g_thread.join();

    g_connected.store(false);
    g_port = 0;

    {
        std::lock_guard<std::mutex> lock(g_incoming_mutex);
        g_incoming.clear();
    }

    {
        std::lock_guard<std::mutex> lock(g_outgoing_mutex);
        g_outgoing.clear();
    }

    EndCapture();
}

bool ControlServer_IsConnected()
{
    return g_connected.load();
}

void ControlServer_Send(std::string message)
{
    if (!g_running.load())
        return;

    message.push_back('\n');

    std::lock_guard<std::mutex> lock(g_outgoing_mutex);

    if (g_outgoing.size() >= MAX_OUTGOING_MESSAGES)
        g_outgoing.pop_front();

    g_outgoing.emplace_back(std::move(message));
}

void ControlServer_SendEvent(
        const std::string& event,
        const std::string& data)
{
    ControlServer_Send(std::string("BEGIN event OK\n") +
                       SanitizeProtocolLine(event) + " " +
                       SanitizeProtocolLine(data) +
                       "\nEND event");
}

void ControlServer_Poll()
{
    std::string command;
    while (PopIncoming(command)) {
        std::fprintf(stderr,
                     "[ControlClient] mcp command %s\n",
                     command.c_str());

        ProcessControlCommand(command);
    }
}

bool DEBUG_MCP_IsCapturingOutput()
{
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    return g_capture_active;
}

void DEBUG_MCP_CaptureMessage(const char* message)
{
    std::lock_guard<std::mutex> lock(g_capture_mutex);

    if (!g_capture_active)
        return;

    if (g_capture_lines.size() >= MAX_CAPTURE_LINES) {
        ++g_capture_discarded;
        return;
    }

    g_capture_lines.emplace_back(message ? message : "");
}

#else

void ControlServer_Start(uint16_t) {}
void ControlServer_StartStdio() {}
void ControlServer_Stop() {}
bool ControlServer_IsConnected() { return false; }
void ControlServer_Send(std::string) {}
void ControlServer_SendEvent(const std::string&, const std::string&) {}
void ControlServer_Poll() {}
bool DEBUG_MCP_IsCapturingOutput() { return false; }
void DEBUG_MCP_CaptureMessage(const char*) {}

#endif
