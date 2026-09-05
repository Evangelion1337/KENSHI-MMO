#pragma once

#include <cstddef>

namespace kmmo {
namespace net {

constexpr int kProtoPort = 25565;

enum class Result {
    Ok,
    ConnectError,
    SendError,
    RecvError,
    Timeout,
    Closed,
};

struct Reply {
    Result result;
    char line[512];
};

// A persistent TCP session. Created by SessionStart, torn down by SessionStop.
// Request is not safe to call from multiple threads on the same session.
struct Session;

// Open a connection. Returns nullptr on failure.
Session* SessionStart(const char* host, int port, size_t timeoutMs);

// Send one command line and await one reply line.
Reply SessionRequest(Session* s, const char* cmd, size_t timeoutMs);

// Close and free the session.
void SessionStop(Session* s);

// Reads a reply line from the socket without sending (used to consume
// multi-line welcome bursts).
Reply SessionDrain(Session* s, size_t timeoutMs);

// Send a raw command line (no reply read). Ok on success.
Result SessionSendLine(Session* s, const char* cmd, size_t timeoutMs);

// Read one line from the session (server-initiated, e.g. QUICKSAVE).
Reply SessionRecvLine(Session* s, size_t timeoutMs);

// Send len raw bytes (loops until all of it is written).
Result SessionSendBytes(Session* s, const void* data, size_t len);

// Receive exactly len raw bytes or fail. Used for binary save-blob chunks.
Result SessionRecvBytes(Session* s, void* out, size_t len, size_t timeoutMs);

} // namespace net
} // namespace kmmo