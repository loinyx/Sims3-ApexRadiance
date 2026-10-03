// Real capture storage/status regression checks, isolated from the game and real Documents.
// Include the production implementation to exercise asynchronous PNG completion without a game device.
#include "framework/apex_log.h"
#include "framework/apex_paths.h"
#include "framework/d3d9_hooks.h"
#include "framework/overlay.h"
#include "framework/game_version.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <thread>
#include <chrono>
std::wstring testDirectory, testGameDirectory;
bool testOverlayVisible = false, testCaptureSuppressed = false;
int testGameKeyPresses = 0;
namespace ApexPaths { const std::wstring& ApexDirectory() { return testDirectory; } const std::wstring& GameDocumentsDirectory() { return testGameDirectory; } bool EnsureApexDirectory() { return true; } }
namespace ApexLog { void Write(Level, const std::string&, const std::source_location&) {} }
namespace Overlay {
bool IsVisible() { return testOverlayVisible; }
void SetVisible(bool visible) { testOverlayVisible = visible; }
void SetCaptureSuppressed(bool suppressed) { testCaptureSuppressed = suppressed; }
bool PostGameKeyPress(WPARAM vk) { if (vk == VK_F10) ++testGameKeyPresses; return true; }
HWND Window() { return nullptr; }
}
namespace D3D9Hooks { bool RegisterPresent(const std::string&, PresentHook, Priority) { return true; } }
const char* GetGameVersionName() { return "isolated test"; }
#include "features/captures.cpp"

int checks = 0;
void Check(bool passed, const char* name) {
    if (!passed) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
    ++checks;
}
std::string Read(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void EncodeQueued(bool fail) {
    const auto jobs = std::move(Captures::g_shots);
    Captures::g_shots.clear();
    Check(!jobs.empty(), "screenshot queued");
    for (auto job : jobs) {
        if (fail) std::filesystem::create_directory(job.file);
        Captures::WritePng(std::move(job), std::vector<BYTE>(8 * 8 * 3, 127), 8, 8);
    }
    for (int n = 0; Captures::Saving() && n < 300; ++n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(!Captures::Saving(), "WIC worker reports completion");
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    namespace fs = std::filesystem;
    testDirectory = (fs::absolute(argv[1]) / std::format("storage-{}-{}", GetCurrentProcessId(), GetTickCount64())).wstring();
    testGameDirectory = (fs::path(testDirectory) / L"Documents" / L"Electronic Arts" / L"The Sims 3").wstring();
    fs::create_directories(testDirectory);
    Captures::SetScreenshots(false);
    fs::path log = fs::path(testDirectory) / L"ApexRadiance_LOG.txt";
    std::ofstream(log) << "original log\n";
    std::ofstream(fs::path(testDirectory) / L"ApexRadiance.toml") << "[ui]\n";
    Captures::SetDescription("Descrição / descripción / description\nSecond line");
    const auto first = Captures::NewFolder("Report");
    Captures::Finish(first, "report");
    Check(!Captures::LastSave().failed && !Captures::LastSave().saving, "report completes with screenshot off");
    Check(Read(first / L"User notes.txt").find("Descrição") != std::string::npos, "UTF-8 multiline notes written");
    Check(Read(first / L"ApexRadiance_LOG.txt") == Read(log), "log snapshot copied byte for byte");
    Check(Captures::SaveDescription("Descrição depois da gravação"), "description can be added after saving");
    Check(Read(first / L"User notes.txt").find("depois") != std::string::npos && Read(first / L"ApexRadiance_LOG.txt") == Read(log), "post-capture description does not rewrite diagnostic data");
    Check(!Captures::SaveDescription("   \n") && Read(first / L"User notes.txt").find("depois") != std::string::npos, "empty description does not erase existing note");
    Check(!Captures::SaveFolderDescription(first.filename().string(), "Title only", " \n"), "title cannot replace the required description");
    Check(Captures::SaveFolderDescription(first.filename().string(), "Título do problema", "Descrição obrigatória"), "historical capture can receive title and description");
    const auto annotation = Captures::ReadDescription(first.filename().string());
    Check(annotation.Complete() && annotation.title == "Título do problema" && annotation.text.find("obrigatória") != std::string::npos, "annotation round trip preserves UTF-8 title and body");
    const auto notePath = first / L"User notes.txt";
    const auto before = Read(notePath);
    const auto tempPath = std::filesystem::path(notePath.wstring() + L".tmp");
    HANDLE locked = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "create locked atomic-write fixture");
    Check(!Captures::SaveFolderDescription(first.filename().string(), "Changed title", "Changed text") && Read(notePath) == before, "failed atomic annotation edit preserves saved description");
    CloseHandle(locked);
    Check(fs::remove(tempPath), "remove exact locked atomic-write fixture");
    Check(!Captures::SaveFolderDescription("../outside", "Title", "Description"), "annotation target rejects traversal");
    Check(Captures::Files(first.filename().string()).size() == 4, "contents reflect actual files only");
    const auto second = Captures::NewFolder("Report");
    Check(first != second, "same-second captures get distinct paths");
    Captures::Finish(second, "second report");
    Check(!Captures::Remove("..") && !Captures::Remove("../outside") && !Captures::Remove("C:\\outside") && !Captures::Remove(".Removed"), "removal rejects traversal and recovery area");
    Check(Captures::Remove(first.filename().string()), "remove capture reversibly");
    Check(!fs::exists(first) && fs::exists(Captures::Root() / L".Removed" / first.filename()), "removed files remain in recovery folder");
    Check(Captures::List().size() == 1 && Captures::Scan().count == 1, "recovery area excluded from library and totals");
    fs::create_directory(first); // name collision must not overwrite either folder
    Check(Captures::UndoRemoval() == 0 && Captures::CanUndoRemoval(), "undo collision keeps recoverable data");
    Check(fs::remove(first), "remove only the empty test collision directory");
    Check(Captures::UndoRemoval() == 1 && Read(first / L"User notes.txt").find("Descrição") != std::string::npos, "undo restores original files");

    Captures::BeginSession();
    const auto collection = Captures::SessionFolder();
    Check(!collection.empty() && !Captures::Remove(collection), "open collection protected");
    const auto grouped = Captures::NewFolder("Recording");
    Captures::WriteText(grouped / L"Recording.txt", "retained recording");
    Captures::Finish(grouped, "recording");
    Check(Captures::LastSave().folder == collection && Captures::SessionCaptures() == 1, "group receipt opens collection and counts capture once");
    Captures::EndSession();
    Check(!Captures::SessionActive() && fs::exists(grouped.parent_path() / L"About this session.txt"), "collection ends after manifest is written");
    Check(Captures::SaveDescription("Finished collection description"), "description can be added to completed collection");
    const auto collectionNotes = grouped.parent_path() / L"User notes.txt";
    Check(fs::remove(collectionNotes), "remove exact fixture description file");
    fs::create_directory(collectionNotes);
    Check(!Captures::SaveDescription("New collection description") && Captures::LastSave().failed, "failed post-save description reaches retry state");
    Check(fs::remove(collectionNotes), "remove empty description blocker");
    Captures::RetrySave();
    Check(!Captures::LastSave().failed && Read(collectionNotes).find("New collection") != std::string::npos, "retry restores note of a completed collection");
    const int removed = Captures::RemoveAll();
    Check(removed == 3 && Captures::List().empty(), "batch removal excludes recovery area");
    Check(Captures::UndoRemoval() == 3 && Captures::List().size() == 3, "undo restores batch");
    Captures::BeginSession();
    const auto failingCollection = Captures::Root() / Captures::SessionFolder();
    fs::create_directory(failingCollection / L"About this session.txt");
    Captures::EndSession();
    Check(Captures::SessionActive() && Captures::LastSave().failed, "manifest failure keeps collection open and reports failure");
    Check(fs::remove(failingCollection / L"About this session.txt"), "remove empty test manifest blocker");
    Captures::RetrySave();
    Check(!Captures::SessionActive() && !Captures::LastSave().failed, "retry completes failed collection manifest");

    // A blocked path fails a real write; retry keeps the measured text in memory.
    const auto blocked = Captures::Root() / L"blocked";
    std::ofstream(blocked) << "file instead of directory";
    Check(!Captures::WriteText(blocked / L"Recording.txt", "recording that must not be lost"), "failed primary text write is detected");
    Captures::Finish(blocked, "recording with blocked folder");
    Check(Captures::LastSave().failed, "metadata or primary failure is never called saved");
    Check(fs::remove(blocked), "remove exact test blocking file");
    fs::create_directory(blocked);
    Captures::RetrySave();
    Check(!Captures::LastSave().failed && Read(blocked / L"Recording.txt") == "recording that must not be lost", "retry writes retained data without recording again");
    Check(!Captures::WriteText(Captures::Root() / L"missing folder" / L"Recording.txt", "old failed capture"), "simulate older failed recording");
    Captures::Finish(Captures::NewFolder("Report"), "newest report owns retry");
    Check(Captures::g_failedText.empty(), "new receipt releases older failed buffers to bound memory");

    Captures::SetScreenshots(true);
    const auto png = Captures::NewFolder("Report");
    Captures::Finish(png, "report with image");
    Check(Captures::LastSave().saving && Captures::Saving(), "capture remains saving until PNG worker finishes");
    Check(!Captures::Remove(png.filename().string()), "pending capture cannot be removed");
    EncodeQueued(true);
    Check(Captures::LastSave().failed, "PNG failure reaches visible receipt");
    Check(fs::remove(png / L"Screenshot.png"), "remove empty test PNG blocker");
    Captures::RetrySave();
    Check(Captures::LastSave().saving, "retry queues a new screenshot");
    EncodeQueued(false);
    Check(!Captures::LastSave().failed && fs::file_size(png / L"Screenshot.png") > 8, "real WIC retry writes PNG and completes receipt");
    Check(Captures::CurrentNote().text.find("Capture saved") != std::string::npos, "completion notice appears after encoding");

    Check(Captures::RequestPlayerScreenshot(true), "player screenshot can be queued with game UI hiding enabled");
    Check(testGameKeyPresses == 1, "player screenshot posts one game UI toggle before capture");
    Check(Captures::g_shots.size() == 1 && !Captures::g_shots.front().report && Captures::g_shots.front().skipPresents == 1,
          "player screenshot waits one present and is not a report capture");
    Check(Captures::g_shots.front().file.parent_path() == fs::path(testGameDirectory) / L"Screenshots",
          "player screenshot targets the game's standard Documents Screenshots folder");
    Check(!Captures::ScreenshotPending(), "player screenshot does not mark the Report screenshot pending");
    const auto playerPhoto = Captures::g_shots.front();
    Captures::g_shots.clear();
    Captures::WritePng(playerPhoto, std::vector<BYTE>(8 * 8 * 3, 127), 8, 8);
    for (int n = 0; n < 300 && !fs::exists(playerPhoto.file); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(fs::exists(playerPhoto.file) && fs::file_size(playerPhoto.file) > 8, "player screenshot writes a WIC PNG to its separate folder");
    Captures::RestorePlayerPhoto();
    Check(testGameKeyPresses == 2, "player screenshot restores the previous game UI state after capture");
    Check(!testCaptureSuppressed && !Captures::g_playerPhoto.active, "player screenshot restores the overlay capture state");
    std::printf("%d checks passed; isolated files kept at %ls\n", checks, testDirectory.c_str());
}
