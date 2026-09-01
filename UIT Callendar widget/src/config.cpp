#include "config.h"

#include <algorithm>
#include <filesystem>

#include <windows.h>
#include <shlobj.h>

namespace {

int ReadInt(const std::wstring& path, const wchar_t* key, int fallback) {
    return GetPrivateProfileIntW(L"Widget", key, fallback, path.c_str());
}

std::wstring ReadString(const std::wstring& path, const wchar_t* key) {
    wchar_t buffer[32768]{};
    GetPrivateProfileStringW(L"Widget", key, L"", buffer,
                             static_cast<DWORD>(std::size(buffer)), path.c_str());
    return buffer;
}

void WriteInt(const std::wstring& path, const wchar_t* key, int value) {
    const std::wstring text = std::to_wstring(value);
    WritePrivateProfileStringW(L"Widget", key, text.c_str(), path.c_str());
}

} // namespace

std::wstring ConfigFilePath() {
    wchar_t base[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, base))) {
        std::filesystem::path directory = std::filesystem::path(base) / L"CalendarWidget";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return (directory / L"config.ini").wstring();
    }
    return L"CalendarWidget.ini";
}

WidgetConfig LoadConfig() {
    WidgetConfig config;
    const std::wstring path = ConfigFilePath();
    const int layoutVersion = ReadInt(path, L"LayoutVersion", 0);
    config.x = ReadInt(path, L"X", config.x);
    config.y = ReadInt(path, L"Y", config.y);
    config.width = std::clamp(ReadInt(path, L"Width", config.width), 680, 1800);
    config.height = std::clamp(ReadInt(path, L"Height", config.height), 280, 900);
    if (layoutVersion < 2) {
        config.x = -1;
        config.y = -1;
        config.width = 960;
        config.height = 480;
    } else if (layoutVersion < 3) {
        config.height = 480;
    }
    config.opacity = std::clamp(ReadInt(path, L"Opacity", config.opacity), 0, 100);
    config.theme = ReadInt(path, L"Theme", 0) == 1 ? WidgetTheme::Light : WidgetTheme::Dark;

    const int backdrop = std::clamp(ReadInt(path, L"Backdrop", 3), 0, 3);
    config.backdrop = ReadInt(path, L"MaterialVersion", 0) < 1
        ? BackdropMode::Glass
        : static_cast<BackdropMode>(backdrop);
    config.alwaysOnTop = ReadInt(path, L"AlwaysOnTop", 1) != 0;
    config.pinToDesktop = ReadInt(path, L"PinToDesktop", 1) != 0;
    config.autoStart = ReadInt(path, L"AutoStart", 1) != 0;
    config.icsPath = ReadString(path, L"IcsPath");
    return config;
}

void SaveConfig(const WidgetConfig& config) {
    const std::wstring path = ConfigFilePath();
    WriteInt(path, L"LayoutVersion", 3);
    WriteInt(path, L"X", config.x);
    WriteInt(path, L"Y", config.y);
    WriteInt(path, L"Width", config.width);
    WriteInt(path, L"Height", config.height);
    WriteInt(path, L"Opacity", config.opacity);
    WriteInt(path, L"Theme", static_cast<int>(config.theme));
    WriteInt(path, L"Backdrop", static_cast<int>(config.backdrop));
    WriteInt(path, L"MaterialVersion", 1);
    WriteInt(path, L"AlwaysOnTop", config.alwaysOnTop ? 1 : 0);
    WriteInt(path, L"PinToDesktop", config.pinToDesktop ? 1 : 0);
    WriteInt(path, L"AutoStart", config.autoStart ? 1 : 0);
    WritePrivateProfileStringW(L"Widget", L"IcsPath", config.icsPath.c_str(), path.c_str());
}
