#include "net.h"

#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstring>
#include <cstdlib>

#pragma comment(lib, "ws2_32.lib")

namespace kmmo {
namespace net {

namespace {
bool g_wsInit = false;

void EnsureWsInit() {
    if (!g_wsInit) {
        WSADATA wsa = {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
            g_wsInit = true;
        } else {
            log::Error("net: WSAStartup failed");
        }
    }
}
} // namespace

struct Session {
    SOCKET sock = INVALID_SOCKET;
};

Session* SessionStart(const char* host, int port, size_t timeoutMs) {
    EnsureWsInit();

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        log::Error("net: socket() failed err=%d", WSAGetLastError());
        return nullptr;
    }

    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host, &addr.sin_addr);

    int r = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        log::Error("net: connect failed err=%d", WSAGetLastError());
        closesocket(s);
        return nullptr;
    }

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    timeval tv = {};
    tv.tv_sec = static_cast<long>(timeoutMs / 1000);
    tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
    r = select(0, nullptr, &wset, nullptr, &tv);
    if (r <= 0) {
        log::Error("net: connect timeout to %s:%d", host, port);
        closesocket(s);
        return nullptr;
    }
    int soErr = 0;
    int soLen = sizeof(soErr);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soLen);
    if (soErr != 0) {
        log::Error("net: connect so_error=%d", soErr);
        closesocket(s);
        return nullptr;
    }

    u_long block = 0;
    ioctlsocket(s, FIONBIO, &block);

    Session* ses = static_cast<Session*>(malloc(sizeof(Session)));
    ses->sock = s;
    log::Info("net: connected to %s:%d", host, port);
    return ses;
}

void SessionStop(Session* s) {
    if (!s) return;
    if (s->sock != INVALID_SOCKET) {
        closesocket(s->sock);
        s->sock = INVALID_SOCKET;
    }
    free(s);
    log::Info("net: session closed");
}

// Read one CRLF/LF-terminated line. Returns true on a full line. On failure,
// *wasClosed is set when the peer closed the socket (as opposed to a timeout
// or other error), so callers can treat a dead connection differently.
bool RecvLine(SOCKET sock, char* out, size_t cap, size_t timeoutMs,
              bool* wasClosed) {
    if (wasClosed) *wasClosed = false;
    size_t pos = 0;
    fd_set rset;
    timeval tv;

    while (pos + 1 < cap) {
        FD_ZERO(&rset);
        FD_SET(sock, &rset);
        tv.tv_sec = static_cast<long>(timeoutMs / 1000);
        tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
        int r = select(0, &rset, nullptr, nullptr, &tv);
        if (r <= 0) return false;
        char c = 0;
        int n = recv(sock, &c, 1, 0);
        if (n == 0) {
            if (wasClosed) *wasClosed = true;
            return false;
        }
        if (n < 0) return false;
        if (c == '\n') {
            out[pos] = '\0';
            return true;
        }
        if (c != '\r') {
            out[pos++] = c;
        }
    }
    out[cap - 1] = '\0';
    return true;
}

Reply SessionRequest(Session* s, const char* cmd, size_t timeoutMs) {
    Reply reply = {};
    reply.result = Result::Closed;
    if (!s || s->sock == INVALID_SOCKET) {
        reply.result = Result::ConnectError;
        return reply;
    }

    size_t len = strlen(cmd);
    int sent = send(s->sock, cmd, static_cast<int>(len), 0);
    if (sent <= 0) {
        reply.result = Result::SendError;
        return reply;
    }

    bool closed = false;
    if (!RecvLine(s->sock, reply.line, sizeof(reply.line), timeoutMs, &closed)) {
        reply.result = (closed ? Result::Closed : Result::Timeout);
        return reply;
    }
    reply.result = Result::Ok;
    return reply;
}

Reply SessionDrain(Session* s, size_t timeoutMs) {
    Reply reply = {};
    reply.result = Result::Closed;
    if (!s || s->sock == INVALID_SOCKET) {
        return reply;
    }
    bool closed = false;
    if (!RecvLine(s->sock, reply.line, sizeof(reply.line), timeoutMs, &closed)) {
        reply.result = (closed ? Result::Closed : Result::Timeout);
        return reply;
    }
    reply.result = Result::Ok;
    return reply;
}

Result SessionSendLine(Session* s, const char* cmd, size_t) {
    if (!s || s->sock == INVALID_SOCKET) return Result::ConnectError;
    size_t len = strlen(cmd);
    int sent = send(s->sock, cmd, static_cast<int>(len), 0);
    if (sent <= 0) {
        return Result::SendError;
    }
    return Result::Ok;
}

Reply SessionRecvLine(Session* s, size_t timeoutMs) {
    Reply reply = {};
    reply.result = Result::Closed;
    if (!s || s->sock == INVALID_SOCKET) {
        return reply;
    }
    bool closed = false;
    if (!RecvLine(s->sock, reply.line, sizeof(reply.line), timeoutMs, &closed)) {
        reply.result = (closed ? Result::Closed : Result::Timeout);
        return reply;
    }
    reply.result = Result::Ok;
    return reply;
}

Result SessionSendBytes(Session* s, const void* data, size_t len) {
    if (!s || s->sock == INVALID_SOCKET) return Result::ConnectError;
    const char* p = static_cast<const char*>(data);
    size_t off = 0;
    while (off < len) {
        int sent = send(s->sock, p + off, static_cast<int>(len - off), 0);
        if (sent <= 0) return Result::SendError;
        off += static_cast<size_t>(sent);
    }
    return Result::Ok;
}

Result SessionRecvBytes(Session* s, void* out, size_t len, size_t timeoutMs) {
    if (!s || s->sock == INVALID_SOCKET) return Result::ConnectError;
    char* p = static_cast<char*>(out);
    size_t off = 0;
    fd_set rset;
    timeval tv;
    while (off < len) {
        FD_ZERO(&rset);
        FD_SET(s->sock, &rset);
        tv.tv_sec = static_cast<long>(timeoutMs / 1000);
        tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
        int r = select(0, &rset, nullptr, nullptr, &tv);
        if (r <= 0) return Result::Timeout;
        int n = recv(s->sock, p + off, static_cast<int>(len - off), 0);
        if (n == 0) return Result::Closed;
        if (n < 0) return Result::RecvError;
        off += static_cast<size_t>(n);
    }
    return Result::Ok;
}

} // namespace net
} // namespace kmmo