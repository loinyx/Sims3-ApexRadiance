#pragma once
// Looks: the shader packs of ReShade's official list (features/looks_packs_index.h, generated), downloaded on the
// player's click from each author's GitHub into Apex Radiance\Shaders\Packs\<folder>\ (Shaders\ and Textures\ inside).
// Nothing goes online until the player clicks Download (ReShade's list is read then too, once a session). A
// downloaded pack is never changed or removed except by the player's Update or Remove.
//
// Downloads run on a worker thread, one pack at a time: HTTPS (WinHTTP) to GitHub hosts only, the zip of the pack's
// branch read with miniz, only .fx, .fxh and image files kept, every size, count and path checked.
#include "looks_packs_index.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Looks::Packs {

inline constexpr uint64_t kMaxZipBytes = 64ull << 20;        // one download
inline constexpr uint64_t kMaxExtractedBytes = 128ull << 20; // the files kept from it
inline constexpr size_t kMaxFiles = 5000;                    // files kept from it
inline constexpr size_t kMaxZipEntries = 100000;             // entries in the zip

struct Pack
{
    std::string name, author, folder, repo, branch;
    std::string licence;       // SPDX id, "Author's terms", or "" (none stated)
    bool licenceKnown = true;  // false: a pack the built-in index does not know (licence not read)
    uint32_t downloadKB = 0;   // 0: unknown
    int techniques = 0;        // in the index
    int rank = 0;              // place on ReShade's list
    bool inList = true;        // on ReShade's list as last read (false: downloaded, then dropped from the list)
    const IndexPack* index = nullptr;
};

// ---------------------------------------------------------------------------------------------- the list
// ReShade's list (EffectPackages.ini text; "" = the built-in index) joined with the index and the downloaded folders:
// a downloaded pack no longer on the list stays (inList false), any other pack not on the list is left out. By name.
std::vector<Pack> MergeList(const std::string& effectPackagesIni, const std::vector<std::string>& downloadedFolders);
// Render thread: the current list (built-in at first; ReShade's after RefreshList read it)
const std::vector<Pack>& List();
// Render thread, once a frame before the menu draws (OnPresent): takes a new list text or downloaded folders into List()
void BeginFrame();
// Reads ReShade's list once a session (worker; Download calls it); offline keeps the built-in one
void RefreshList();
enum class ListState { BuiltIn, Reading, Read, Offline };
ListState CurrentListState();
const Pack* Find(const std::string& folder);
// The pack (on the list, not known to be unavailable) that has a .fx of that name, preferring one with the technique; nullptr if none
const Pack* PackForFile(const std::string& file, const std::string& technique);
// When no pack of the list has a file of that name (a GShade preset's renamed file): a file of a pack that has the same
// technique, preferring a downloaded pack, then a file whose name shares the original's first word ("mxao"), then
// ReShade's order; "" when none
std::string FileForTechnique(const std::string& file, const std::string& technique);
std::wstring PacksFolder(); // Apex Radiance\Shaders\Packs\ (trailing backslash)
std::string GitHubUrl(const Pack& p);

// ---------------------------------------------------------------------------------------------- downloads
enum class Failure { None, Offline, Gone, Busy, TooLarge, Refused, BadZip, Disk, Stopped, Failed };
const char* FailureText(Failure f); // English menu text

enum class RowState { Waiting, Downloading, Done, Failed };
struct Row { std::string folder; RowState state = RowState::Waiting; float progress = 0; Failure failure = Failure::None; };
struct Batch { std::vector<Row> rows; bool running = false; };

// Queues downloads (folders of List()); the standard effects (ReShade.fxh, which most packs include) go first when they
// are not downloaded yet. A pack already queued is not queued twice.
void Download(const std::vector<std::string>& folders);
void StopRest();      // the current download stops too; what is done stays
Batch Status();
void ClearFinished(); // forgets a finished batch (the modal closed)
bool Unavailable(const std::string& folder); // a download found the pack gone from GitHub this session
bool Downloaded(const std::string& folder);  // a complete download is in Shaders\Packs (as the last check found)
void Remove(const std::string& folder);      // worker: deletes Shaders\Packs\<folder>, then the folder is scanned again

// ---------------------------------------------------------------------------------------------- parts, for tests
// HTTPS to github.com, codeload.github.com, objects.githubusercontent.com or raw.githubusercontent.com only
bool AllowedUrl(const std::wstring& url);
// An HTTP answer that is not 200: 404, 410 and 451 are a pack gone; 403 or 429 with a rate limit is GitHub busy
Failure FailureFor(int status, bool rateLimited);
// Unpacks a pack zip into dest (created): .fx and .fxh into dest\Shaders\, images into dest\Textures\, each path taken
// after the zip's top folder from its first "Shaders" or "Textures" folder; anything else, absolute or ".." paths, and
// files over the caps are left out. False (and nothing kept) when the zip can't be read or is over the caps.
struct Unpacked { int shaders = 0, textures = 0, skipped = 0; };
Failure Unpack(const void* zip, size_t size, const std::wstring& dest, Unpacked* out);
// How a download reaches the network (tests replace it): the body of url, following allowed redirects only
using Progress = std::function<bool(uint64_t got, uint64_t total)>; // false: stop
using FetchFn = std::function<Failure(const std::string& url, uint64_t cap, std::string& body, const Progress& progress)>;
void SetFetch(FetchFn f); // nullptr: WinHTTP
void SetListText(const std::string& text, bool offline); // tests: as if RefreshList had read this
void WaitIdle();          // tests: until the worker is done

} // namespace Looks::Packs
