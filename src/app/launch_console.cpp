#include "app/launch_console.h"

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace evs::app::launch_console {
namespace {

HWND g_window   = nullptr; // conhost window, or the Windows Terminal window owning the console
bool g_terminal = false;   // hosted by Windows Terminal
std::atomic<bool> g_keep_hidden{false};

std::wstring lower(std::wstring s) {
    for (auto &c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Command line of another process (handle with PROCESS_QUERY_LIMITED_INFORMATION), empty if unknown.
std::wstring command_line(HANDLE process) {
    struct UnicodeString {
        USHORT length, maximum_length;
        PWSTR buffer;
    };
    using Query = LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query =
        reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query)
        return {};
    constexpr ULONG process_command_line_information = 60;
    ULONG size = 0;
    query(process, process_command_line_information, nullptr, 0, &size);
    if (size < sizeof(UnicodeString))
        return {};
    std::vector<unsigned char> buf(size);
    if (query(process, process_command_line_information, buf.data(), size, &size) != 0)
        return {};
    const auto *us = reinterpret_cast<const UnicodeString *>(buf.data());
    return us->buffer ? std::wstring(us->buffer, us->length / sizeof(wchar_t)) : std::wstring{};
}

std::vector<std::wstring> arguments(const std::wstring &cmdline) {
    std::wstring rest = cmdline;
    const size_t first = rest.find_first_not_of(L" \t");
    if (first == std::wstring::npos)
        return {};
    rest = rest.substr(first);
    if (rest[0] == L'"') { // quoted program path
        const size_t end = rest.find(L'"', 1);
        rest             = end == std::wstring::npos ? std::wstring{} : rest.substr(end + 1);
    } else {
        const size_t end = rest.find_first_of(L" \t");
        rest             = end == std::wstring::npos ? std::wstring{} : rest.substr(end);
    }
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && std::iswspace(rest[i]))
            ++i;
        const size_t j = i;
        while (i < rest.size() && !std::iswspace(rest[i]))
            ++i;
        if (i > j)
            out.push_back(lower(rest.substr(j, i - j)));
    }
    return out;
}

// A shell a person types in (it stays after EventStudio). cmd /c and powershell -Command / -File without -NoExit
// run something and exit: the double-clicked batch file, helpers it started.
bool is_terminal(const std::wstring &exe, const std::wstring &cmdline) {
    static const wchar_t *shells[] = {L"cmd.exe",  L"powershell.exe", L"pwsh.exe", L"bash.exe",
                                      L"sh.exe",   L"wsl.exe",        L"code.exe"};
    bool shell = false;
    for (const wchar_t *s : shells)
        shell = shell || exe == s;
    if (!shell)
        return false;
    const auto args = arguments(cmdline);
    if (exe == L"cmd.exe") {
        for (const auto &a : args) {
            if (a.rfind(L"/c", 0) == 0)
                return false;
            if (a.rfind(L"/k", 0) == 0 || a.rfind(L"/", 0) != 0)
                return true;
        }
        return true;
    }
    if (exe == L"powershell.exe" || exe == L"pwsh.exe") {
        for (const auto &a : args) {
            if (a.rfind(L"-noe", 0) == 0 || a.rfind(L"/noe", 0) == 0)
                return true;
            const std::wstring p = a.substr(a.find_first_not_of(L"-/") == std::wstring::npos ? a.size()
                                                                                            : a.find_first_not_of(L"-/"));
            if (p == L"c" || p == L"f" || p == L"file" || p == L"e" || p == L"ec" || p == L"enc" ||
                p == L"encodedcommand" || p.rfind(L"com", 0) == 0)
                return false; // what follows is the command, not options
        }
        return true;
    }
    return true;
}

// True when every process on the console is known and none is a terminal.
bool console_is_ours() {
    std::vector<DWORD> pids(64);
    const DWORD n = GetConsoleProcessList(pids.data(), static_cast<DWORD>(pids.size()));
    if (n == 0 || n > pids.size())
        return false;
    for (DWORD k = 0; k < n; ++k) {
        if (pids[k] == GetCurrentProcessId())
            continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[k]);
        if (!h)
            return false; // unknown process on the console: leave it alone
        wchar_t path[1024];
        DWORD size   = 1024;
        const bool ok = QueryFullProcessImageNameW(h, 0, path, &size) != 0;
        bool terminal = true;
        if (ok) {
            std::wstring exe = lower(path);
            exe              = exe.substr(exe.find_last_of(L'\\') + 1);
            terminal         = is_terminal(exe, command_line(h));
        }
        CloseHandle(h);
        if (terminal)
            return false;
    }
    return true;
}

} // namespace

void hide() {
    for (int i = 1; i < __argc; ++i)
        if (std::string(__argv[i]) == "--console")
            return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return; // started without a console (Explorer, shortcut to the exe)
    HWND con = GetConsoleWindow();
    if (!con || !console_is_ours()) {
        FreeConsole();
        return;
    }
    HWND owner = GetAncestor(con, GA_ROOTOWNER);
    wchar_t cls[256] = {};
    if (owner && GetClassNameW(owner, cls, 256) && std::wstring(cls) == L"CASCADIA_HOSTING_WINDOW_CLASS") {
        g_window   = owner;
        g_terminal = true;
    } else {
        g_window = con;
    }
    ShowWindow(g_window, SW_HIDE);
    // Windows Terminal shows its window a moment after creating it: hide it again for 3 s
    g_keep_hidden = true;
    std::thread([] {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (g_keep_hidden && std::chrono::steady_clock::now() < end) {
            if (IsWindowVisible(g_window))
                ShowWindow(g_window, SW_HIDE);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }).detach();
}

void close() {
    if (!g_window)
        return;
    g_keep_hidden = false;
    FreeConsole();
    if (g_terminal)
        PostMessageW(g_window, WM_CLOSE, 0, 0);
    g_window = nullptr;
}

} // namespace evs::app::launch_console

#else

namespace evs::app::launch_console {
void hide() {}
void close() {}
} // namespace evs::app::launch_console

#endif
