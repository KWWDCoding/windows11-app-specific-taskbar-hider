// ==WindhawkMod==
// @id              windows11-app-specific-taskbar-hider
// @name            Windows 11 App Specific Taskbar Hider
// @description     Hides the taskbar only while a selected app is foreground; restores on focus loss or exit
// @version         1.3
// @author          KWWDCoding
// @github          https://github.com/kwwdcoding/windows11-app-specific-taskbar-hider
// @homepage        https://www.kwwd.co.uk
// @include         explorer.exe
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Windows 11 App Specific Taskbar Hider

Hides the primary taskbar while a configured application is the foreground
window. Switching to another app, closing the target app, or disabling the mod
restores a taskbar hidden by the mod without activating it.

## Settings

- **Hide taskbar while a target app is foreground:** enables hiding.
- **Target processes:** executable filenames, matched case-insensitively.
  Add `dota2.exe`, for example. Paths are not supported.

## Changes in 1.3

- Always require foreground focus. Alt+Tab releases taskbar hiding even if the
  target process remains running. The old `requireFocus` setting is no longer
  exposed or read; a previously saved false value no longer enables background
  hiding. Existing `hideTaskbar` and `targetProcesses` settings remain supported.
- Remove ShowWindow, SetWindowPos, and SetForegroundWindow hooks. This mod no
  longer blocks shell visibility, positioning, or activation API calls.
- Remove work-area changes and settings broadcasts. Version 1.2 expanded the
  work area but only read it on restoration rather than restoring its value.
- Queue visibility changes asynchronously and restore with SW_SHOWNOACTIVATE.
- Stop and join the worker before unloading; release its event, thread handle,
  and settings lock only after the worker has finished.

## Behavior and limitations

The worker checks foreground state every 100 ms. Restoration is asynchronous,
so this interval is not a strict latency guarantee. Edge hover can briefly
reveal the taskbar; absolute hover suppression is intentionally removed.

Only the primary Shell_TrayWnd is managed. Secondary-monitor taskbars are not
changed. A taskbar already hidden when observed is not claimed by the mod.
The mod does not alter Windows auto-hide preferences, input mappings, mouse
capture, cursor clipping, or the desktop work area. Game exit clears its active
hiding state; the worker remains idle/polling for future target applications
until the mod is disabled or unloaded.

If an older version left the desktop or input in an abnormal state, disable it
and restart Windows before testing. This version cannot undo arbitrary state
left by an earlier mod, a game, a mouse utility, or a graphics driver.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hideTaskbar: true
  $name: Hide taskbar while a target app is foreground
- targetProcesses: [Resolve.exe, Photoshop.exe, FL64.exe, InDesign.exe]
  $name: Target processes
  $description: Executable filenames. Switching away always restores the taskbar.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <string>
#include <vector>

namespace {

constexpr DWORD kPollIntervalMs = 100;
constexpr int kMaxTargetProcesses = 256;

// Settings callbacks publish a complete snapshot under this lock. Only the
// worker changes taskbar visibility and owns g_hiddenTaskbar.
CRITICAL_SECTION g_lock;
bool g_enabled = false;
std::vector<std::wstring> g_targets;
HANDLE g_stop = nullptr;
HANDLE g_worker = nullptr;
// Owned exclusively by the worker; never accessed by settings callbacks.
HWND g_hiddenTaskbar = nullptr;

void LoadSettings() {
    bool enabled = Wh_GetIntSetting(L"hideTaskbar") != 0;
    std::vector<std::wstring> targets;
    for (int i = 0; i < kMaxTargetProcesses; ++i) {
        PCWSTR value = Wh_GetStringSetting(L"targetProcesses[%d]", i);
        bool endOfList = !value || !*value;
        if (!endOfList) {
            targets.emplace_back(value);
        }
        if (value) {
            Wh_FreeStringSetting(value);
        }
        if (endOfList) {
            break;
        }
    }
    EnterCriticalSection(&g_lock);
    g_enabled = enabled;
    g_targets.swap(targets);
    LeaveCriticalSection(&g_lock);
}

bool IsTargetForeground(HWND foreground) {
    if (!foreground || IsIconic(foreground)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    if (!pid || pid == GetCurrentProcessId()) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false; // Fail open: restore the taskbar.
    WCHAR path[32768];
    DWORD length = ARRAYSIZE(path);
    bool found = false;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) {
        std::wstring full(path, length);
        const auto separator = full.find_last_of(L"\\/");
        const std::wstring name = separator == std::wstring::npos
            ? full : full.substr(separator + 1);
        EnterCriticalSection(&g_lock);
        if (g_enabled) {
            for (const auto& target : g_targets) {
                if (_wcsicmp(name.c_str(), target.c_str()) == 0) {
                    found = true;
                    break;
                }
            }
        }
        LeaveCriticalSection(&g_lock);
    }
    CloseHandle(process);
    return found;
}

void RestoreTaskbar() {
    if (g_hiddenTaskbar) {
        if (IsWindow(g_hiddenTaskbar)) {
            // Queue to the shell's UI thread; never activate or take focus.
            if (!ShowWindowAsync(g_hiddenTaskbar, SW_SHOWNOACTIVATE)) return;
        }
        g_hiddenTaskbar = nullptr;
    }
}

void UpdateTaskbarVisibility() {
    HWND foreground = GetForegroundWindow();
    bool hide = IsTargetForeground(foreground);
    // Recheck after querying the process: focus may have changed meanwhile.
    if (!hide || foreground != GetForegroundWindow()) {
        RestoreTaskbar();
        return;
    }
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (g_hiddenTaskbar && g_hiddenTaskbar != taskbar) RestoreTaskbar();
    if (!taskbar || !IsWindowVisible(taskbar)) return;
    if (foreground != GetForegroundWindow()) {
        RestoreTaskbar();
        return;
    }
    if (ShowWindowAsync(taskbar, SW_HIDE)) g_hiddenTaskbar = taskbar;
}

DWORD WINAPI Worker(void*) {
    while (WaitForSingleObject(g_stop, kPollIntervalMs) == WAIT_TIMEOUT) {
        UpdateTaskbarVisibility();
    }
    RestoreTaskbar();
    return 0;
}

void StopWorker() {
    if (!g_worker) return;
    SetEvent(g_stop);
    // The worker uses asynchronous window operations, so it never waits for
    // Explorer's UI thread while that thread may be unloading this mod.
    WaitForSingleObject(g_worker, INFINITE);
    CloseHandle(g_worker);
    g_worker = nullptr;
}

}  // namespace

BOOL Wh_ModInit() {
    InitializeCriticalSection(&g_lock);
    LoadSettings();
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop) g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_worker) {
        if (g_stop) CloseHandle(g_stop);
        g_stop = nullptr;
        DeleteCriticalSection(&g_lock);
        return FALSE;
    }
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModBeforeUninit() {
    StopWorker();
}

void Wh_ModUninit() {
    StopWorker();
    if (g_stop) CloseHandle(g_stop);
    g_stop = nullptr;
    DeleteCriticalSection(&g_lock);
}
