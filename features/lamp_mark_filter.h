#pragma once
// Rooms keep their light when their lamps did not change (part of Night Lighting; see lamp_mark_filter.cpp).
#include <cstdint>
#include <string>
namespace LampMarkFilter {
bool Install(std::string& why); // Steam 1.67.2 only (fixed addresses, checked byte by byte); false: the game marks as before
void Uninstall();
bool IsInstalled();
// Render thread, every frame (Night Lighting): the lots whose lamps were switched or moved light again (nightLevel: the game's, -1
// = no world)
void OnPresent(float nightLevel);
// Many lamps switched by the player within a moment ("all the lights" of a lot): kMassSwitches or more user switches
// within kMassWindowMs, held for kMassHoldMs after the last one. Any thread.
bool MassSwitchActive();
// Grows by one at each new burst of switches
long MassEventId();
// Any player's switch, one lamp is enough (switched where it is; not a light switching itself, not at dusk or dawn), held
// for kMassHoldMs after the last one; its event id grows at each new one (a room takes the quick pass once per event).
// Any thread.
bool SwitchActive();
long SwitchEventId();
// GetTickCount of the current switch event's first switch
unsigned long SwitchEventStart();
// GetTickCount of the latest player switch (bit 0 set; 0 = none yet)
unsigned long SwitchLastTick();
// How many player switches so far (grows by one at each): SolvedSince compares solve starts with it, as a solve begun in the
// same GetTickCount step as the switch (10-16 ms) is one a tick cannot tell before from after
unsigned long SwitchSerial();
// 12/10, lamp edits all at once (AtriumHold): GetTickCount of the latest player edit (switch, value, move or drag step, a
// lamp into another story, a lamp added or removed; bit 0 set, 0 = none; never at dusk or dawn), and how many edits so far
// (RoomLightQueue stamps the solve starts with it, LevelLightShare the urgent marks). Any thread.
unsigned long EditLastTick();
unsigned long EditSerial();
// A lot's safety-net refresh (OnPresent) is due within withinMs, or overdue: the edit hold waits for the rooms it sends
// (12/10: a move's refresh comes 200 ms after it, after the hold's first check). Render thread.
bool LotRefreshDueSoon(unsigned long withinMs);
// Notes an edit LampMarkFilter's room marks do not see (a lamp added or removed: LevelLightShare's changed-room walk)
void NoteEdit();
// Render thread: on = a lamp entry update marks its room changed only when the lamp changed (default off since 2.13.1; Developer page)
void SetEnabled(bool on);
bool Enabled();
// Any thread: an edit of that light was taken by the GPU (Lamp lighting: Instant, Real-time outdoor lamps) within withinMs
bool HandledLately(uintptr_t light, unsigned long withinMs);
// The calling thread's room marks from lamp entry updates are kept back from BeginDeferMarks to the matching
// EndDeferMarks, then made as the game makes them (replay) or, for the marks of `entry`, dropped (its update changed
// nothing the rooms read: LevelLightShare's window activation recheck); marks of other entries always go through. Nests;
// the light tree thread.
void BeginDeferMarks();
void EndDeferMarks(bool replay, uintptr_t entry = 0);
long DeferredMarksDropped();
// Development tools (F6 recorder, Developer page): marks let through / kept back, and which light-manager messages flagged
// light entries (dev build)
std::string Status();
}
