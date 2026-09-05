#include "panel.h"

#include "net.h"
#include "log.h"
#include "session.h"
#include "version.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "comctl32.lib")

namespace kmmo {

namespace {

const UINT WM_KMMO_STATUS = WM_APP + 1;
const UINT WM_KMMO_EXIT = WM_APP + 2;
const UINT WM_KMMO_NETDONE = WM_APP + 3;

const int IDC_USER = 1001;
const int IDC_PASS = 1002;
const int IDC_STATUS = 1003;
const int IDC_LOGIN = 1004;
const int IDC_REGISTER = 1005;
const int IDC_SERVER = 1006;

struct PanelState {
    HWND hwnd = nullptr;
    HWND hUser = nullptr;
    HWND hPass = nullptr;
    HWND hStatus = nullptr;
    HWND hServer = nullptr;
    char host[128] = KMMO_SERVER_HOST;
    int port = 25565;
    volatile bool running = false;
    HANDLE thread = nullptr;
};

PanelState g_panel;
CRITICAL_SECTION g_netLock;   // guards net::Request
bool g_netLockInit = false;

void SetStatus(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (g_panel.hStatus) {
        SetWindowTextA(g_panel.hStatus, buf);
    }
}

// Runs on a worker thread so the panel window never blocks.
struct NetJob {
    char cmd[256];
    bool login;             // true if an OK reply should trigger world entry
    bool registerJob;       // true if this is a REGISTER (may chain a login)
    char user[128];         // the username for world-session login
    char pass[128];         // kept for the post-register auto-login
};

void QueueNet(bool isLogin, bool isRegister, const char* user, const char* pass,
              const char* fmt, ...) {
    NetJob* job = static_cast<NetJob*>(malloc(sizeof(NetJob)));
    job->login = isLogin;
    job->registerJob = isRegister;
    strncpy(job->user, user ? user : "", sizeof(job->user) - 1);
    strncpy(job->pass, pass ? pass : "", sizeof(job->pass) - 1);
    va_list args;
    va_start(args, fmt);
    vsnprintf(job->cmd, sizeof(job->cmd), fmt, args);
    va_end(args);
    CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        NetJob* job = static_cast<NetJob*>(param);
        const char* host = g_panel.host;
        int port = g_panel.port;

        log::Info("panel: connecting to %s:%d for cmd='%.32s'", host, port, job->cmd);
        EnterCriticalSection(&g_netLock);
        net::Session* s = net::SessionStart(host, port, 5000);
        if (!s) {
            LeaveCriticalSection(&g_netLock);
            SetStatus("Net error (unreachable %s:%d)", host, port);
            log::Error("panel: connect failed for cmd='%.32s'", job->cmd);
            free(job);
            return 0;
        }
        log::Info("panel: connected, draining greeting");
        net::SessionDrain(s, 5000);
        log::Info("panel: sending '%s'", job->cmd);
        net::Reply r = net::SessionRequest(s, job->cmd, 6000);
        LeaveCriticalSection(&g_netLock);

        log::Info("panel: reply '%s' (result=%d)", r.line, (int)r.result);

        char status[300];
        if (r.result == net::Result::Ok) {
            snprintf(status, sizeof(status), "%s", r.line);
            if (job->registerJob && strncmp(r.line, "OK REGISTERED", 13) == 0) {
                // Account created: drop the user straight into the world.
                SetStatus("Registered. Logging in...");
                QueueNet(true, false, job->user, job->pass,
                         "LOGIN %s %s\r\n", job->user, job->pass);
            } else if (job->login && strncmp(r.line, "OK LOGIN_OK", 11) == 0) {
                SetStatus("Login OK. Entering world...");
                session::OnLoginOk(job->user, host, port);
            } else if (job->login && strncmp(r.line, "ERR", 3) == 0) {
                session::OnLogout();
            }
        } else {
            snprintf(status, sizeof(status), "Net error (unreachable %s:%d)", host, port);
        }
        net::SessionStop(s);
        if (g_panel.hwnd) {
            PostMessageA(g_panel.hwnd, WM_KMMO_NETDONE, 0, 0);
        }
        SetStatus(status);
        free(job);
        return 0;
    }, job, 0, nullptr);
}

LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        break;
    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        if (id == IDC_LOGIN || id == IDC_REGISTER) {
            char user[128] = {}, pass[128] = {};
            GetWindowTextA(g_panel.hUser, user, sizeof(user));
            GetWindowTextA(g_panel.hPass, pass, sizeof(pass));
            if (user[0] == '\0' || pass[0] == '\0') {
                SetStatus("Enter a username and password.");
                break;
            }
            if (id == IDC_LOGIN) {
                QueueNet(true, false, user, pass, "LOGIN %s %s\r\n", user, pass);
                SetStatus("Logging in...");
            } else {
                QueueNet(false, true, user, pass, "REGISTER %s %s\r\n", user, pass);
                SetStatus("Registering...");
            }
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(220, 220, 220));
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_KMMO_NETDONE: {
        // status already set by worker
        break;
    }
    case WM_CLOSE:
    case WM_KMMO_EXIT:
        DestroyWindow(hwnd);
        g_panel.hwnd = nullptr;
        g_panel.running = false;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

DWORD WINAPI PanelThread(LPVOID) {
    if (!g_netLockInit) {
        InitializeCriticalSection(&g_netLock);
        g_netLockInit = true;
    }

    HINSTANCE hInst = GetModuleHandleA("KenshiMMO.dll");

    WNDCLASSA wc = {};
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW);
    wc.lpszClassName = "KenshiMMOPanel";
    RegisterClassA(&wc);

    int x = GetSystemMetrics(SM_CXSCREEN) / 2 - 175;
    int y = GetSystemMetrics(SM_CYSCREEN) / 2 - 120;

    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "KenshiMMOPanel", "Kenshi MMO - Login",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, 350, 260, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        g_panel.running = false;
        return 1;
    }
    g_panel.hwnd = hwnd;

    InitCommonControls();

    CreateWindowExA(0, "STATIC", "Server:",
                    WS_CHILD | WS_VISIBLE, 15, 12, 60, 20, hwnd, nullptr, hInst, nullptr);
    g_panel.hServer = CreateWindowExA(0, "EDIT", g_panel.host,
                                      WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                      80, 10, 250, 22, hwnd, reinterpret_cast<HMENU>(IDC_SERVER),
                                      hInst, nullptr);

    CreateWindowExA(0, "STATIC", "Username:",
                    WS_CHILD | WS_VISIBLE, 15, 48, 60, 20, hwnd, nullptr, hInst, nullptr);
    g_panel.hUser = CreateWindowExA(0, "EDIT", "",
                                    WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                    80, 46, 250, 22, hwnd, reinterpret_cast<HMENU>(IDC_USER),
                                    hInst, nullptr);

    CreateWindowExA(0, "STATIC", "Password:",
                    WS_CHILD | WS_VISIBLE, 15, 84, 60, 20, hwnd, nullptr, hInst, nullptr);
    g_panel.hPass = CreateWindowExA(0, "EDIT", "",
                                    WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
                                    80, 82, 250, 22, hwnd, reinterpret_cast<HMENU>(IDC_PASS),
                                    hInst, nullptr);

    CreateWindowExA(0, "BUTTON", "Register",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 80, 132, 120, 30,
                    hwnd, reinterpret_cast<HMENU>(IDC_REGISTER), hInst, nullptr);
    CreateWindowExA(0, "BUTTON", "Login",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, 132, 120, 30,
                    hwnd, reinterpret_cast<HMENU>(IDC_LOGIN), hInst, nullptr);

    g_panel.hStatus = CreateWindowExA(0, "STATIC",
                                      "Connect to the Kenshi MMO server.",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      15, 178, 315, 40, hwnd, reinterpret_cast<HMENU>(IDC_STATUS),
                                      hInst, nullptr);

    SetStatus("Ready.");

    MSG msg;
    while (g_panel.running && GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

} // namespace

void PanelStart(const char* serverHost, int serverPort) {
    if (g_panel.running) return;
    strncpy(g_panel.host, serverHost, sizeof(g_panel.host) - 1);
    g_panel.port = serverPort;
    g_panel.running = true;
    g_panel.thread = CreateThread(nullptr, 0, PanelThread, nullptr, 0, nullptr);
    log::Info("panel: started (server %s:%d)", serverHost, serverPort);
}

void PanelStop() {
    if (!g_panel.running) return;
    g_panel.running = false;
    if (g_panel.hwnd) {
        PostMessageA(g_panel.hwnd, WM_KMMO_EXIT, 0, 0);
    }
    if (g_panel.thread) {
        WaitForSingleObject(g_panel.thread, 3000);
        g_panel.thread = nullptr;
    }
    log::Info("panel: stopped");
}

} // namespace kmmo