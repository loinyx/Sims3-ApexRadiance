#pragma once
// Stream arena (dev switch stream_arena; not shipped). Taken over from sims3-modernization's [StreamBuf] (#364), pool part only.
//
// EA::IO::MemoryStream never grows in place: every size change is a new buffer, a copy and a free, and the async resource
// loader sizes one stream per streamed record (1024x1024 textures: the 704 KB and 1408 KB classes). Each buffer of 128 KB
// or more is its own VirtualAlloc in the game allocator, 20-30 times a second while streaming. Here those buffers
// (128 KB to 8 MB) come from one 48 MB reservation made once at the top of the address space, cut into 64 KB slices that
// are committed on use and decommitted 5 s after their last use; a reused slice is zero-filled over the requested part.
// No free run, no arena or a failed commit: the game's own call, as before.
//
// Sites (Steam 1.67.2; each function's bytes checked in full, written while every other thread is suspended outside them):
//   0x004DEC0B  the SharedPointer constructor's allocator call (12 bytes) -> push name, allocator, stream, size; call StubAlloc
//   0x004DEA99  SharedPointer::Release's allocator->Free(data, 0) (16 bytes) -> push esi; call StubFree; jmp +8
//   0x004DEEF9  the same free inlined in MemoryStream::Realloc
// The game frees stream data only at those two sites, so an arena pointer never reaches the game's free; a pointer
// outside the arena goes to the game's free unchanged. The sites stay written for the session.
//
// Room to save: before a save the free slices are decommitted; when the largest free block is so low that Room to save
// lets its reserve go, Retire() stops new arena blocks and the reservation is released once none is in use.
#include <cstdint>
#include <string>

namespace StreamArena {

void Install();            // init thread, after the dev switches (AllocatorTuning::Install's place)
uint32_t PrepareForSave(); // KB decommitted
void Retire();             // address space tight: give the reservation back as soon as no block is in use
std::string StatusText();

} // namespace StreamArena
