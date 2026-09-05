#pragma once

#include <cstddef>

namespace kmmo {

// The MMO session: after a successful login, hold the connection open and
// drive the "enter world -> spawn as wanderer" flow on a worker thread.
namespace session {

// Deliver a successfully-authenticated login. Spawns the enter-world worker.
void OnLoginOk(const char* username, const char* serverHost, int serverPort);

// If a login session is being held open, detach/stop it.
void OnLogout();

// Textual status of the current session (for the panel status line).
const char* Status();

} // namespace session
} // namespace kmmo