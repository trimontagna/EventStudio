#include "app/platform.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

namespace evs::app {

namespace {
std::wstring widen(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}
std::vector<wchar_t> win32_filter(const char *filter) {
    std::wstring f = widen(filter);
    std::vector<wchar_t> out(f.begin(), f.end());
    for (auto &c : out)
        if (c == L'|')
            c = L'\0';
    out.push_back(L'\0');
    out.push_back(L'\0');
    return out;
}
} // namespace

std::filesystem::path open_file_dialog(const char *title, const char *filter) {
    wchar_t buffer[4096] = L"";
    auto f               = win32_filter(filter);
    auto t               = widen(title);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = f.data();
    ofn.lpstrFile   = buffer;
    ofn.nMaxFile    = static_cast<DWORD>(std::size(buffer));
    ofn.lpstrTitle  = t.c_str();
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::filesystem::path(buffer) : std::filesystem::path();
}

std::filesystem::path save_file_dialog(const char *title, const char *filter, const std::filesystem::path &initial,
                                       const char *default_extension) {
    wchar_t buffer[4096] = L"";
    const auto init      = initial.wstring();
    wcsncpy_s(buffer, init.c_str(), _TRUNCATE);
    auto f   = win32_filter(filter);
    auto t   = widen(title);
    auto ext = widen(default_extension[0] == '.' ? default_extension + 1 : default_extension);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = f.data();
    ofn.lpstrFile   = buffer;
    ofn.nMaxFile    = static_cast<DWORD>(std::size(buffer));
    ofn.lpstrTitle  = t.c_str();
    ofn.lpstrDefExt = ext.c_str();
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn) ? std::filesystem::path(buffer) : std::filesystem::path();
}

std::filesystem::path pick_folder_dialog(const char *title, const std::filesystem::path &initial) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::filesystem::path result;
    IFileOpenDialog *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(widen(title).c_str());
        std::error_code ec;
        auto start = initial;
        while (!start.empty() && !std::filesystem::is_directory(start, ec) && start.has_parent_path() &&
               start.parent_path() != start)
            start = start.parent_path();
        IShellItem *folder = nullptr;
        if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.wstring().c_str(), nullptr,
                                                                    IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return result;
}

std::string to_utf8(const std::filesystem::path &path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

std::filesystem::path from_utf8(const std::string &text) {
    return std::filesystem::u8path(text);
}

std::filesystem::path app_data_dir() {
    if (const wchar_t *local = _wgetenv(L"LOCALAPPDATA"); local && *local)
        return std::filesystem::path(local) / L"EventStudio";
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / L"EventStudio";
}

std::filesystem::path unique_path(const std::filesystem::path &p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
        return p;
    const std::wstring stem = p.stem().wstring(), ext = p.extension().wstring();
    for (int i = 1;; ++i) {
        auto candidate = p.parent_path() / (stem + L"_" + std::to_wstring(i) + ext);
        if (!std::filesystem::exists(candidate, ec))
            return candidate;
    }
}

std::filesystem::path default_output_path(const char *prefix, const char *extension) {
    std::filesystem::path dir;
    if (const char *profile = std::getenv("USERPROFILE"))
        dir = std::filesystem::path(profile) / "Videos" / "EventStudio";
    else
        dir = std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char name[128];
    std::snprintf(name, sizeof(name), "%s_%04d%02d%02d_%02d%02d%02d%s", prefix, tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, extension);
    return dir / name;
}

std::string human_bytes(double bytes) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int u               = 0;
    while (bytes >= 1000 && u < 4) {
        bytes /= 1000;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.2f %s", bytes, units[u]);
    return buf;
}

std::string human_rate(double per_second, const char *unit) {
    char buf[48];
    if (per_second >= 1e6)
        std::snprintf(buf, sizeof(buf), "%.2f M%s/s", per_second / 1e6, unit);
    else if (per_second >= 1e3)
        std::snprintf(buf, sizeof(buf), "%.1f k%s/s", per_second / 1e3, unit);
    else
        std::snprintf(buf, sizeof(buf), "%.0f %s/s", per_second, unit);
    return buf;
}

std::string format_time_us(long long us, bool micro) {
    const bool neg = us < 0;
    if (neg)
        us = -us;
    const long long ms = us / 1000, s = ms / 1000, m = s / 60, h = m / 60;
    char buf[48];
    if (micro)
        std::snprintf(buf, sizeof(buf), "%s%02lld:%02lld:%02lld.%06lld", neg ? "-" : "", h, m % 60, s % 60,
                      us % 1'000'000);
    else
        std::snprintf(buf, sizeof(buf), "%s%02lld:%02lld:%02lld.%03lld", neg ? "-" : "", h, m % 60, s % 60, ms % 1000);
    return buf;
}

void reveal_in_explorer(const std::filesystem::path &path) {
    const std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace evs::app
