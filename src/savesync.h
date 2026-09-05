#pragma once

#include <cstddef>

namespace kmmo {
namespace savesync {

// Remove all local native game saves (the game's save/ directory) so every
// session starts clean; the world state is then (re)applied from the server.
// Returns the number of entries removed, or -1 if none were present.
int WipeLocalSaves();

// The name (no slashes) of the most recently-written save-slot folder under
// the game save root (the folder holding the newest quick.save). Returns 0
// when no folder has a quick.save yet. The returned pointer is a static
// buffer invalidated by the next call.
const char* LatestSaveFolder();

// True when a save-slot folder contains a written quick.save, i.e. a real
// world save the engine produced. At the main menu (or before a character is
// born) no such folder exists, so this doubles as the live-character check
// for quicksave uploads.
bool FolderHasWorld(const char* folderName);

// Remove every save-slot folder except the names given (the one just
// uploaded) and the engine's transient `_current1` directory, so only the
// latest save remains locally. Returns the number of folders removed.
int WipeOtherSlots(const char* keepA, const char* keepB = nullptr);

// Pack a whole save-slot folder into a single malloc'd blob. Blob layout:
//   u32 'KMBS', u32 version=1, u32 nameLen, name bytes, u32 fileCount,
//   then fileCount records of { u32 pathLen, path (forward slashes),
//   u64 size, bytes }.
// Returns 0 on failure; outLen receives the byte count. Free the result.
unsigned char* PackSaveFolder(const char* folderName, size_t* outLen);

// Write a blob produced by PackSaveFolder back under the game save root,
// recreating the folder tree. Returns the number of files written, or -1 on
// a corrupt blob. outName (if non-null) receives the embedded folder name.
int UnpackSaveBlob(const unsigned char* blob, size_t len,
                   char* outName, int outNameCap);

} // namespace savesync
} // namespace kmmo