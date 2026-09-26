#include "debug_mcp.h"

// Link the production fallback translation unit, including the entry used by
// DEBUG_Init when a debug build has no SDL networking (Win9x configuration).
int main()
{
    ControlServer_Start(1234);
    ControlServer_StartStdio();
    ControlServer_Send("probe");
    ControlServer_SendEvent("probe", "data");
    ControlServer_Poll();
    DEBUG_MCP_CaptureMessage("probe");
    const bool unexpectedly_active = ControlServer_IsConnected() ||
                                     DEBUG_MCP_IsCapturingOutput();
    ControlServer_Stop();
    return unexpectedly_active ? 1 : 0;
}
