#pragma once
// Out-of-process helper, the game side (native/ts3wrap/oop_channel.h): features that keep data in the restart host
// (ts3wrap.exe, 64-bit) instead of the game's address space. A feature calls Enable() while it installs, before
// RestartHost::Start() starts the host; without the restart host it keeps its data in the game. Every call can fail:
// the caller then serves it in the game, and after a timeout or a lost host the service stays in the game for the
// session. Credits: @loinyx
#include "../native/ts3wrap/oop_channel.h"
#include <cstdint>

namespace OopClient {

enum class Host { Off, Starting, Ready, Lost };

// True when the restart host is on and the helper section exists (created on the first call). False: the feature stays
// in the game; one log line names the service and asks for the restart host.
bool Enable(OopService service, const char* name);
// Lists the plugins of [oop] plugins for the host (oop_plugin.h), each as a service that is on. Nothing without a list.
void EnablePlugins();
// Waits up to `ms` for the host's workers (it starts with RestartHost::Start). False: not ready, or lost.
bool WaitReady(DWORD ms);
// One call (OopCall). kOopOk, or why not: kOopBusy (all slots taken: try again), kOopFailed (this call only: an error
// answer or one larger than outCap), kOopTimeout / kOopHostGone (every service stays in the game from now on).
OopResult Call(OopService service, uint32_t op, uint32_t arg, const void* in, uint32_t inLen, void* out, uint32_t outCap, uint32_t* outLen, DWORD timeoutMs = 2000);
Host State();

} // namespace OopClient
