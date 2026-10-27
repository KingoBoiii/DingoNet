#pragma once

// Force-included into ENet's translation units (premake5.lua) so a game that reaches only ENet still links
// Winsock and winmm without naming them.
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
