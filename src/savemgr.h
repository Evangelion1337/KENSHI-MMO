#pragma once

namespace kmmo {

// Engine save-manager bridge. The save/load engine functions may only be
// called from the game's main thread, so all requests are issued from a
// per-frame hook on GameFrameUpdate.
namespace savemgr {

// Resolve the SaveManager module + GameFrameUpdate function and install the
// main-thread pump hook. Safe to call multiple times.
bool Init();

// Queue a "start new game in save slot <slotName>" or "load save <slotName>".
// Returns a nonzero request id once queued, or 0 on failure (too short name
// or module not loaded). The game processes the request on its own thread.
int RequestNewGame(const char* slotName);
int RequestLoad(const char* slotName);

// The id of the newest request the engine hook has handed to SaveManager,
// i.e. the engine accepted it. Compare against the id Request* returned.
int DispatchCount();

// True once the engine has run its accept->settle cycle (the world transition
// the current request triggered has completed): readonly, no world rebuilds.
bool TransitionSettled();

// Restore the patched GameFrameUpdate prologue (plugin unload).
void Shutdown();

} // namespace savemgr
} // namespace kmmo