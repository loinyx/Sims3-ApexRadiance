// String table index (features/string_table_index_core.h) offline checks, x86 only, on fake string tables laid out as
// the game's (list node, table, hash map of 7919 buckets, entry):
//   1. the answers equal the game's walk: first table in list order with a non-empty text, empty and null texts passed
//      over, unloaded nodes skipped, missing keys answered with the list's end, bucket collisions;
//   2. no answer (the list's first node) while the index builds, while a list change runs and after one; rebuilt after;
//   3. a list changed without the hooks (other ends) is not trusted; a text changed behind its back is caught by the
//      verification and turns the index off; over the byte cap it answers nothing;
//   4. 3000 tables: every key equal to the walk, and the time per lookup of both;
//   5. the stand-down's culprit: each jump form decodes to its target (fixture bytes), other bytes do not; known mods
//      are matched without case and get their setting per site, an unknown file its name, no file the in-memory text;
//   6. --exe <TS3W.exe> [--sigs <framework/game_addresses.cpp>]: each "Stbl" signature matches exactly once.
//   check.exe [--exe <TS3W.exe>] [--sigs <game_addresses.cpp>]
#include "../../features/string_table_index_core.h"
#include "../../features/string_table_index_culprit.h"
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>
#include <string>
#include <vector>

using namespace StringTableIndex::Core;

static int g_fail = 0;
static void Expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    g_fail += !ok;
}

// ---- fake game memory ----
struct Entry {
    Entry* next = nullptr;
    uint32_t pad = 0;
    uint32_t lo = 0, hi = 0;
    const wchar_t* text = nullptr;
};
static_assert(offsetof(Entry, lo) == kEntryKeyLo && offsetof(Entry, hi) == kEntryKeyHi && offsetof(Entry, text) == kEntryText);
struct Table {
    uint8_t head[kTableMap] = {};
    Entry* buckets[kBuckets + 1] = {};
    std::deque<Entry> entries;
    void Add(uint32_t lo, uint32_t hi, const wchar_t* text) {
        Entry& e = entries.emplace_back();
        e.lo = lo, e.hi = hi, e.text = text;
        e.next = buckets[lo % kBuckets];
        buckets[lo % kBuckets] = &e;
    }
    Entry* Get(uint32_t lo, uint32_t hi) {
        for (Entry* e = buckets[lo % kBuckets]; e; e = e->next)
            if (e->lo == lo && e->hi == hi) return e;
        return nullptr;
    }
};
static_assert(offsetof(Table, buckets) == kTableMap);
struct Node {
    Node* next;
    Node* prev;
    uint8_t key[16] = {};
    Table* table = nullptr;
};
static_assert(offsetof(Node, table) == kNodeTable);
struct List {
    Node sentinel{&sentinel, &sentinel};
    std::deque<Node> nodes;
    std::deque<Table> tables;
    uintptr_t S() const { return reinterpret_cast<uintptr_t>(&sentinel); }
    Node* PushBack(Table* t) {
        Node& n = nodes.emplace_back();
        n.table = t, n.prev = sentinel.prev, n.next = &sentinel;
        sentinel.prev->next = &n, sentinel.prev = &n;
        return &n;
    }
    Node* PushFront(Table* t) {
        Node& n = nodes.emplace_back();
        n.table = t, n.next = sentinel.next, n.prev = &sentinel;
        sentinel.next->prev = &n, sentinel.next = &n;
        return &n;
    }
    Table* NewTable() { return &tables.emplace_back(); }
};

static uintptr_t Ask(Index& ix, const List& l, uint32_t lo, uint32_t hi) {
    const uint32_t key[2] = {lo, hi};
    return ix.Start(l.S(), key);
}
static void Settle(Index& ix, const List& l) {
    for (int i = 0; i < 100000 && !ix.Ready(); i++) Ask(ix, l, 1, 1);
}
static uintptr_t U(const void* p) { return reinterpret_cast<uintptr_t>(p); }

// ---- 5: signatures ----
static int CountPattern(const std::vector<uint8_t>& exe, const std::string& pattern) {
    std::vector<int> pat;
    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] == ' ') { i++; continue; }
        pat.push_back(pattern[i] == '?' ? -1 : std::stoi(pattern.substr(i, 2), nullptr, 16));
        i += 2;
    }
    int n = 0;
    for (size_t i = 0; i + pat.size() <= exe.size(); i++) {
        size_t k = 0;
        while (k < pat.size() && (pat[k] < 0 || exe[i + k] == pat[k])) k++;
        n += k == pat.size();
    }
    return n;
}

int main(int argc, char** argv) {
    // ---- 1: semantics ----
    {
        List l;
        Index ix;
        ix.stepMs = 1000;
        l.PushBack(nullptr);                 // a node whose table is not loaded
        Table* t1 = l.NewTable();
        t1->Add(10, 0, L"");                 // A: empty here
        t1->Add(20, 0, L"b1");               // B
        Table* t2 = l.NewTable();
        t2->Add(10, 0, L"a2");               // A wins here
        t2->Add(20, 0, L"b2");               // B: t1 won already
        t2->Add(30, 0, nullptr);             // C: no text
        t2->Add(10 + kBuckets, 7, L"x2");    // same bucket as A, other key
        Table* t3 = l.NewTable();
        t3->Add(30, 0, L"c3");               // C wins here
        t3->Add(10, 1, L"a-high");           // same low dword as A, other high dword
        Node* n1 = l.PushBack(t1);
        Node* n2 = l.PushBack(t2);
        Node* n3 = l.PushBack(t3);
        Expect(Ask(ix, l, 10, 0) == U(l.sentinel.next), "no answer (the list's first node) while the index builds");
        Settle(ix, l);
        Expect(ix.Ready(), "the index is built");
        Expect(Ask(ix, l, 10, 0) == U(n2), "first non-empty text wins (an empty one is passed over)");
        Expect(Ask(ix, l, 20, 0) == U(n1), "the first table in list order wins");
        Expect(Ask(ix, l, 30, 0) == U(n3), "a null text is passed over");
        Expect(Ask(ix, l, 10 + kBuckets, 7) == U(n2), "a key in the same bucket");
        Expect(Ask(ix, l, 10, 1) == U(n3), "the high dword is part of the key");
        Expect(Ask(ix, l, 99, 0) == l.S(), "a key no table has: the list's end");
        for (uint32_t k : {10u, 20u, 30u, 99u, 10u + kBuckets})
            for (uint32_t h : {0u, 1u, 7u}) Expect(Ask(ix, l, k, h) == WalkLikeGame(l.S(), k, h), "equal to the walk");
        Expect(ix.stats.hits.load() > 0 && ix.stats.misses.load() > 0 && ix.stats.mismatches.load() == 0, "hits and misses counted, no difference");
        Expect(ix.Entries() == 5, "five keys indexed (A, B, C, A-high, the bucket twin)");

        // ---- 2: list changes ----
        ix.MutationBegin();
        Expect(Ask(ix, l, 10, 0) == U(l.sentinel.next), "no answer while a list change runs");
        Table* t0 = l.NewTable();
        t0->Add(99, 0, L"new");
        Node* n0 = l.PushFront(t0); // the change notice puts new tables in front
        ix.MutationEnd();
        Expect(Ask(ix, l, 99, 0) == U(n0), "after a change: the next lookup starts the rebuild and gets no answer");
        Settle(ix, l);
        Expect(Ask(ix, l, 99, 0) == U(n0), "rebuilt: the new table answers");
        Expect(Ask(ix, l, 10, 0) == U(n2), "rebuilt: the old answers stay");

        // ---- 3: changes behind the hooks ----
        Table* t9 = l.NewTable();
        t9->Add(77, 0, L"late");
        Node* n9 = l.PushBack(t9); // no hook ran: the list's last node differs
        Expect(Ask(ix, l, 77, 0) == U(l.sentinel.next), "a list whose ends moved is not trusted");
        Settle(ix, l);
        Expect(Ask(ix, l, 77, 0) == U(n9), "and is rebuilt");
        t2->Get(10, 0)->text = L""; // the winning text emptied behind the index's back
        bool caught = false;
        for (int i = 0; i < 300 && !caught; i++) caught = Ask(ix, l, 10, 0) == U(l.sentinel.next) && ix.broken.load();
        Expect(caught && ix.stats.mismatches.load() == 1, "a changed text is caught by the verification");
        Expect(Ask(ix, l, 20, 0) == U(l.sentinel.next), "and the index answers nothing more");
    }
    {
        List l;
        Index ix;
        Table* t = l.NewTable();
        t->Add(5, 0, L"five");
        l.PushBack(t);
        static bool layout = false;
        ix.layoutOk = [] { return layout; };
        Expect(Ask(ix, l, 5, 0) == U(l.sentinel.next) && ix.broken.load() && !ix.Ready(), "a changed table layout: no build, the game walks");
    }
    {
        List l;
        Index ix;
        ix.stepMs = 1000;
        ix.maxBytes = 64 * 1024; // < the first slot array (65536 slots)
        Table* t = l.NewTable();
        t->Add(1, 0, L"x");
        l.PushBack(t);
        Settle(ix, l);
        Expect(!ix.Ready() && ix.stats.overCap.load() == 1, "over the byte cap: no index");
        Expect(Ask(ix, l, 1, 0) == U(l.sentinel.next) && ix.stats.overCap.load() == 1, "and the game's walk, without retrying until the list changes");
    }
    // ---- 4: many tables ----
    {
        List l;
        Index ix;
        std::mt19937 rng(7);
        std::vector<std::wstring> texts = {L"", L"t"};
        constexpr int kTables = 3000, kPer = 40;
        std::vector<std::pair<uint32_t, uint32_t>> keys;
        for (int t = 0; t < kTables; t++) {
            Table* tb = l.NewTable();
            for (int i = 0; i < kPer; i++) {
                const uint32_t lo = rng() % 60000, hi = rng() % 3; // many keys shared between tables
                if (tb->Get(lo, hi)) continue;
                tb->Add(lo, hi, (rng() % 8) ? L"t" : L"");
                keys.push_back({lo, hi});
            }
            l.PushBack(tb);
        }
        for (int i = 0; i < 2000; i++) keys.push_back({60000 + i, 0}); // missing
        ix.stepMs = 2.0;
        int steps = 0;
        while (!ix.Ready() && steps < 100000) Ask(ix, l, 1, 1), steps++;
        std::printf("     3000 tables: %u strings, %u KB, built in %u ms over %d lookups\n", ix.stats.lastEntries.load(), ix.stats.lastBytes.load() / 1024,
                    ix.stats.lastBuildMs.load(), steps);
        Expect(ix.Ready() && steps > 1, "built in several steps of 2 ms");
        ix.verifyEvery = 0;
        int same = 0;
        for (auto [lo, hi] : keys) same += Ask(ix, l, lo, hi) == WalkLikeGame(l.S(), lo, hi);
        Expect(same == static_cast<int>(keys.size()), "every key equal to the walk");
        using C = std::chrono::steady_clock;
        volatile uintptr_t sink = 0;
        auto t0 = C::now();
        for (int r = 0; r < 5; r++)
            for (auto [lo, hi] : keys) sink = sink + Ask(ix, l, lo, hi);
        auto t1 = C::now();
        for (int r = 0; r < 5; r++)
            for (auto [lo, hi] : keys) sink = sink + WalkLikeGame(l.S(), lo, hi);
        auto t2 = C::now();
        const double n = 5.0 * keys.size();
        std::printf("     per lookup: index %.3f us, walk %.3f us\n", std::chrono::duration<double, std::micro>(t1 - t0).count() / n,
                    std::chrono::duration<double, std::micro>(t2 - t1).count() / n);
    }
    // ---- 5: culprit ----
    {
        namespace C = StringTableIndex::Culprit;
        constexpr uintptr_t at = 0x0059C5B0;
        const uint8_t e9[] = {0xE9, 0x4B, 0x3A, 0xA6, 0x0F};             // jmp 0x10000000
        const uint8_t e9back[] = {0xE9, 0xFB, 0xFF, 0xFF, 0xFF};         // jmp at
        const uint8_t eb[] = {0xEB, 0xFE};                               // jmp at
        const uint8_t ff25[] = {0xFF, 0x25, 0x00, 0x10, 0x00, 0x20};     // jmp [0x20001000]
        const uint8_t pushRet[] = {0x68, 0x78, 0x56, 0x34, 0x12, 0xC3};  // push 0x12345678; ret
        const uint8_t movJmp[] = {0xB8, 0x00, 0x20, 0x00, 0x30, 0xFF, 0xE0}; // mov eax,0x30002000; jmp eax
        const uint8_t movEdx[] = {0xBA, 0x00, 0x20, 0x00, 0x30, 0xFF, 0xE2}; // mov edx,..; jmp edx
        const uint8_t movBad[] = {0xBA, 0x00, 0x20, 0x00, 0x30, 0xFF, 0xE0}; // mov edx,..; jmp eax: not one jump
        const uint8_t entry[] = {0x83, 0xEC, 0x78, 0x53, 0x55, 0x56, 0x8B, 0xD9, 0x57}; // the game's own entry
        const uint8_t s3ppLayout[] = {0xBF, 0xFD, 0x01, 0x00, 0x00, 0xF7, 0xF7}; // mov edi,509; div edi (in place)
        auto read = [](uintptr_t a, uintptr_t* v) { return a == 0x20001000 ? (*v = 0x40001234, true) : false; };
        Expect(C::JumpTarget(e9, sizeof e9, at, read) == 0x10000000 && C::JumpTarget(e9back, sizeof e9back, at, read) == at, "jmp rel32, forward and back");
        Expect(C::JumpTarget(eb, sizeof eb, at, read) == at, "jmp rel8");
        Expect(C::JumpTarget(ff25, sizeof ff25, at, read) == 0x40001234 && C::JumpTarget(ff25, sizeof ff25, at, nullptr) == 0, "jmp [abs32], through the read");
        Expect(C::JumpTarget(pushRet, sizeof pushRet, at, read) == 0x12345678, "push/ret");
        Expect(C::JumpTarget(movJmp, sizeof movJmp, at, read) == 0x30002000 && C::JumpTarget(movEdx, sizeof movEdx, at, read) == 0x30002000, "mov reg/jmp reg");
        Expect(!C::JumpTarget(movBad, sizeof movBad, at, read) && !C::JumpTarget(entry, sizeof entry, at, read) &&
                   !C::JumpTarget(s3ppLayout, sizeof s3ppLayout, at, read) && !C::JumpTarget(e9, 4, at, read),
               "no target: other registers, the game's own bytes, an immediate patch, too few bytes");
        Expect(C::FindKnown("sims3performance.ASI") && !C::FindKnown("Sims3Performance.asi.bak") && !C::FindKnown(""), "known mods without case, whole name");
        const std::string lsc = C::Describe(C::Site::Entry, at, "Sims3Performance.asi", false);
        Expect(lsc == "the text lookup's entry (0x0059c5b0) was changed by Sims 3 Performance Patch (Sims3Performance.asi). To use Faster text lookups, "
                      "set LocalizedStringCache=0 in the [Patches] section of Sims3Performance.ini (Game\\Bin), then restart the game",
               "S3PP on the entry: LocalizedStringCache");
        Expect(C::Describe(C::Site::Layout, at, "Sims3Performance.asi", false, true).find("changed likely by Sims 3 Performance Patch (Sims3Performance.asi). To use "
                                                                                     "Faster text lookups, set StblBucketLayoutTuning=0") != std::string::npos,
               "S3PP on the layout (loaded, no jump): StblBucketLayoutTuning");
        Expect(C::Describe(C::Site::Walk, at, "Other.asi", false) == "the table walk (0x0059c5b0) was changed by Other.asi. To use Faster text lookups, turn off the "
                                                                    "part of that mod that changes text lookups or remove Other.asi, then restart the game",
               "unknown module: its file name");
        Expect(C::Describe(C::Site::ListFunctions, at, "TS3NATIVE.asi", false).find("by Sims 3 Modernization (TS3NATIVE.asi). To use Faster text lookups, set "
                                                                                  "Threads=0 in the [Verify] section of ts3native.ini") != std::string::npos,
               "ts3native on the list functions: its [Verify] Threads probes");
        Expect(C::Describe(C::Site::Entry, at, "ts3native.asi", false).find("To use Faster text lookups, turn off the part of that mod") != std::string::npos,
               "known mod without a setting for the site: generic advice");
        Expect(C::Describe(C::Site::Entry, at, "", true).ends_with("changed by a patch in memory (no file), likely a loader or trainer") &&
                   C::Describe(C::Site::Entry, at, "", false).ends_with("(its file was not found)"),
               "no module: in memory, or not found");
    }
    // ---- 6: signatures ----
    const char* exePath = nullptr;
    const char* sigPath = nullptr;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--exe") == 0) exePath = argv[i + 1];
        if (std::strcmp(argv[i], "--sigs") == 0) sigPath = argv[i + 1];
    }
    if (exePath && sigPath) {
        std::ifstream f(exePath, std::ios::binary);
        const std::vector<uint8_t> exe((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::ifstream s(sigPath);
        const std::regex re(R"re(\{Id::(Stbl\w+), K::Sig, W::Text, None, 0, \{\{"([0-9A-F? ]+)")re");
        std::string line;
        int sigs = 0;
        while (std::getline(s, line)) {
            std::smatch m;
            if (!std::regex_search(line, m, re)) continue;
            sigs++;
            const int n = CountPattern(exe, m[2]);
            std::printf("     %s: %d match(es)\n", m[1].str().c_str(), n);
            Expect(n == 1, "the signature matches once in the executable");
        }
        Expect(sigs == 5, "five Stbl signatures read");
    }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
