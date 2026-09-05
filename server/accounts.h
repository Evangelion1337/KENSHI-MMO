#pragma once

#include <cstddef>
#include <cstdint>

namespace kmmo {

// Account record on disk: username;salt;hash[;spawnX;spawnY;spawnZ]
// The trailing spawn triplet is a legacy fallback: world saves now live in
// per-account folders under saves/<username>/current.save (the server-side
// save for that player). The client's enter-world flow must use ONLY this
// server state (local saves are ignored). Missing fields default to the
// standard wanderer spawn.
struct Account {
    char user[64];
    char salt[33];
    char hash[65]; // sha256(salt + password)
    int spawnX = 0;
    int spawnY = 0;
    int spawnZ = 0;
    char squad[64] = {}; // the in-world squad this account owns ('' = none yet)
};

enum class AccountResult {
    Ok,          // success
    Exists,      // register target already exists
    NotFound,    // login: no such user
    BadPassword, // login: wrong password
};

namespace accounts {

// Load the account file into memory. Returns count loaded.
int Load(const char* path);

// Register a new account. On Ok, persists the store. The account's squad is
// left empty; it is claimed later when the account enters the world.
AccountResult Register(const char* user, const char* password);

// Authenticate. On Ok, fills out the stored account (including server save).
AccountResult Login(const char* user, const char* password, Account& out);

// True if the user exists.
bool HasUser(const char* user);

// Fetch the server-side spawn save for a user. Returns false if unknown.
// Reads saves/<username>/current.save first, then the legacy triplet.
bool GetSpawn(const char* user, int& x, int& y, int& z);

// Persist a character's world position as the server-side save for the user.
// Writes saves/<username>/current.save (creating the per-account folder).
bool SavePos(const char* user, int x, int y, int z);

// ---- Full save-file blob storage (saves/<username>/save.blob) ----

// Size of the stored save blob for a user. Returns false if none exists.
bool GetSaveSize(const char* user, int64_t& size);

// Read a slice of the stored save blob. Returns bytes read, or -1 on error.
// off/len must be within [0, GetSaveSize).
int64_t ReadSave(const char* user, int64_t off, void* buf, int64_t len);

// Write a slice of the save blob (resizing as needed). Returns bytes written
// or -1 on error. Call WriteSaveBegin first to reset the blob.
bool WriteSaveBegin(const char* user);

// Append/slice write into the blob at the given offset.
int64_t WriteSave(const char* user, int64_t off, const void* buf, int64_t len);

// Mark the blob complete with the final total size.
void WriteSaveEnd(const char* user, int64_t size);

// Wipe the stored save blob for a user.
void ClearSave(const char* user);

// The blob key for the single shared authority world. All clients download and
// upload through this key so everyone plays the SAME world (co-op visibility).
// Login/registration stay per-account; only the world blob is shared.
const char* SharedBlobUser();

// If no shared world blob exists yet, seed it from the largest legacy
// per-account blob so an existing world becomes the authority world.
void SeedSharedBlobIfMissing(const char* baseDir);

// ---- Squad ownership ----

// Remember which in-world squad this account controls. Returns false if the
// account doesn't exist.
bool SetSquad(const char* user, const char* squad);

// Fetch the squad name an account controls. Returns false if none known.
bool GetSquad(const char* user, char* out, int cap);

} // namespace accounts
} // namespace kmmo