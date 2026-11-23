#pragma once
// ReShade Effect Shader Toggler (REST) configurations in Looks. Credits: @loinyx
// REST (github.com/4lex4nder/ReshadeEffectShaderToggler, MIT) is a ReShade add-on that runs effects when the game draws
// with chosen shaders, before the rest of the frame (a game's UI, or a Sim's hair, drawn on top afterwards). Its
// ReshadeEffectShaderToggler.ini lists, per group, the game's vertex and pixel shaders by the CRC32 of their bytecode (the
// bytecode Direct3D 9 gets, the same under the RTX Remix bridge and DXVK), where the effects run (before the draw, after
// it, or when the game then changes its render target) and which effects the group takes. This reads that file and
// answers the questions Looks asks while the frame is drawn (patches/looks_patch.cpp). What REST does beyond running
// effects (texture bindings, constant copies, toggle keys, shader hunting) is not read.
#include <cstdint>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace Looks::Rest {

enum Location : uint32_t { kBeforeDraw = 0, kAfterDraw = 1, kOnRenderTargetChange = 2 };
enum Match : uint32_t { kResolution = 0, kAspectRatio = 1, kExtendedAspectRatio = 2, kAnySize = 3 };

struct Group
{
    std::string name;
    bool active = false;
    uint32_t location = kBeforeDraw;
    uint32_t match = kResolution;     // the render target the effects draw on, against the back buffer
    bool requeue = false;             // a render target that does not match waits for the group's next draw
    bool allowAll = false;            // every effect of the look (but the listed ones with exceptions)...
    bool exceptions = false;
    std::set<std::string> techniques; // ...else only these
    std::unordered_set<uint32_t> vs, ps;
};

struct Config
{
    std::vector<Group> groups;    // in the file's order
    std::vector<std::string> notes; // what was read but not followed (one line each)
};

// The file's text (an .ini as REST writes it). False when it has no group.
bool Parse(const std::string& text, Config& out);
// The CRC32 of a shader's bytecode (zlib's, as REST computes it)
uint32_t Crc32(const void* data, size_t bytes);
// Whether a group runs this technique of this file ("Comic", "Comic.fx"). REST lists a technique as "Comic [Comic.fx]"
// (older files: "Comic")
bool Takes(const Group& g, const std::string& technique, const std::string& file);
// Whether a render target of w x h is one the group's effects may draw on (the back buffer is bw x bh)
bool SizeMatches(const Group& g, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh);

} // namespace Looks::Rest
