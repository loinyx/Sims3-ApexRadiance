// A sample plugin of the restart host (native/ts3wrap/oop_plugin.h), built x64 by run.ps1 for the checks in check.cpp:
// op 0 answers the request, op 1 answers the plugin's tag, any other op is an error answer.
#include "../../native/ts3wrap/oop_plugin.h"
#include <cstring>

#ifndef PLUGIN_TAG
#define PLUGIN_TAG 0x31484345u  // "ECH1"
#endif
#ifndef PLUGIN_ABI
#define PLUGIN_ABI kApexOopAbi
#endif

static bool Handler(uint32_t op, uint32_t, uint8_t* io, uint32_t inLen, uint32_t* outLen) {
    if (op == 0) return *outLen = inLen, true;
    const uint32_t tag = PLUGIN_TAG;
    memcpy(io, &tag, sizeof tag);
    *outLen = sizeof tag;
    return op == 1;
}

extern "C" __declspec(dllexport) uint32_t ApexOopPluginInit(const ApexOopHostApi* api, ApexOopPluginInfo* info) {
    api->log(L"[oop_helper_test] echo plugin loaded\n");
    *info = {PLUGIN_TAG, 7, Handler};
    return PLUGIN_ABI;
}
