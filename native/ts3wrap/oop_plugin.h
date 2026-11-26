#pragma once
// Plugins of the restart host's out-of-process services (oop_channel.h): a 64-bit DLL that serves requests inside
// ts3wrap.exe, and the calls ApexRadiance.asi exports so another ASI in the game can reach it. This header is all a
// plugin and its caller need.
//   [oop] plugins = ["name.dll"]   in ApexRadiance.toml: bare file names (none by default), at most 8 of 31 characters
// The host loads each listed DLL from its own folder only, before it reports ready, and calls the DLL's one export:
//   extern "C" __declspec(dllexport) uint32_t ApexOopPluginInit(const ApexOopHostApi* api, ApexOopPluginInfo* info);
// The plugin fills `info` and returns kApexOopAbi (0: it could not start). Another major version (the high 16 bits) is
// refused. Plugins get the service ids 16-31 in list order. The handler runs on the host's two workers, which every
// service shares, and must return fast: a plugin keeps threads of its own for long work.
#include <stdint.h>

static const uint32_t kApexOopAbi = 0x00010000;  // major << 16 | minor

// Reads `inLen` bytes from `io`, writes the answer over it (at most ApexOopHostApi::payloadBytes) and sets *outLen.
// False: an error answer.
typedef bool (*ApexOopHandler)(uint32_t op, uint32_t arg, uint8_t* io, uint32_t inLen, uint32_t* outLen);

struct ApexOopHostApi {
    uint32_t abi, channelVersion, payloadBytes, gamePid;
    void (*log)(const wchar_t* line);  // to the debugger output; the host keeps no log file
};
struct ApexOopPluginInfo {
    uint32_t tag, pluginVersion;  // the tag names the service to its callers (a four-character code, not 0)
    ApexOopHandler handler;
};
typedef uint32_t (*ApexOopPluginInitFn)(const ApexOopHostApi* api, ApexOopPluginInfo* info);

// How a listed plugin loaded. Missing: no such file, or no ApexOopPluginInit in it. Refused: the name holds a path.
enum ApexOopPluginState : uint32_t { kApexOopNone = 0, kApexOopLoaded, kApexOopMissing, kApexOopWrongAbi, kApexOopInitFailed, kApexOopRefused };
enum ApexOopHostState : uint32_t { kApexOopHostOff = 0, kApexOopHostStarting, kApexOopHostReady, kApexOopHostGone };
enum OopResult { kOopOk = 0, kOopBusy, kOopFailed, kOopTimeout, kOopHostGone };
struct ApexOopServiceInfo { uint32_t serviceId, abi, pluginVersion, state; };  // state: ApexOopPluginState

// The game side: exports of ApexRadiance.asi (x86, __cdecl), found with GetProcAddress.
//   uint32_t ApexOop_Abi();        kApexOopAbi of the loaded ASI
//   uint32_t ApexOop_HostState();  ApexOopHostState; Off also when no plugin is listed or the restart host is off
//   uint32_t ApexOop_Service(uint32_t tag, ApexOopServiceInfo* info);
//       The service id of the loaded plugin with this tag, else 0. `info` (optional) is filled whenever a listed
//       plugin reported the tag, so a wrong ABI can be told from a plugin that is not there. Valid once the host is ready.
//   uint32_t ApexOop_Call(uint32_t serviceId, uint32_t op, uint32_t arg, const void* in, uint32_t inLen,
//                         void* out, uint32_t outCap, uint32_t* outLen, uint32_t timeoutMs);
//       One request to a plugin service (16-31) or the echo service (0); returns an OopResult. Busy: every slot is
//       taken, try again. Failed: this call only. Timeout / HostGone: the host counts as lost for every caller until
//       the game ends, so keep a blocking handler off the workers. timeoutMs: 100-2000, 0 = 2000. Any thread may call;
//       a request and its answer are at most 256 KB each.
