#pragma once
// The restart host's out-of-process services (oop_channel.h): started before the host waits for the game, when the game
// created the helper section (at least one service switched on). Without the section nothing starts: today's behaviour.
#include "oop_channel.h"

namespace OopHost {
// Opens the section of game `pid`, starts the workers and marks the host ready. False when the section is absent.
bool Start(DWORD pid);
}

// Services: one file each (svc_<name>.cpp), registered in oop_host.cpp's handler table. The workers keep serving until
// the host ends (a service's data lives in this 64-bit process only, so it costs the game nothing after it ended).
bool SvcEcho(uint32_t op, uint32_t arg, uint8_t* io, uint32_t inLen, uint32_t* outLen);
