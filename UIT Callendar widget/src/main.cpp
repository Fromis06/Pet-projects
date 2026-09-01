#include "calendar.h"
#include "config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

namespace {

constexpr wchar_t kWindowClass[] = L"CalendarWidgetWindow";
constexpr wchar_t kNoteWindowClass[] = L"CalendarWidgetNoteWindow";
constexpr wchar_t kShowEventName[] = L"Local\\CalendarWidget.ShowExisting";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kRestoreMessage = WM_APP + 2;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT_PTR kTrimTimer = 2;
constexpr UINT_PTR kDesktopGuardTimer = 3;

enum CommandId : UINT {
    CmdToggleVisible = 100, CmdChooseIcs, CmdReload,
    CmdPreviousWeek, CmdNextWeek, CmdCurrentWeek, CmdAlwaysOnTop,
    CmdPinToDesktop, CmdAutoStart, CmdExit,
    CmdThemeDark = 200, CmdThemeLight,
    CmdBackdropClear = 220, CmdBackdropBlur, CmdBackdropAcrylic, CmdBackdropGlass,
    CmdOpacityBase = 300
};

enum AccentState {
    AccentDisabled = 0,
    AccentEnableBlurBehind = 3,
    AccentEnableAcrylicBlurBehind = 4
};

struct AccentPolicy {
    int state;
    int flags;
    DWORD gradientColor;
    int animationId;
};

struct WindowCompositionAttributeData {
    int attribute;
    void* data;
    SIZE_T size;
};

using SetWindowCompositionAttributeFn =
    BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);
using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);

constexpr wchar_t kStartupRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupValueName[] = L"CalendarWidget";

struct EventHitRegion {
    RECT bounds{};
    CalendarEvent event;
};

struct AppState {
    HWND window = nullptr;
    HWND noteWindow = nullptr;
    HWND desktopHost = nullptr;
    HINSTANCE instance = nullptr;
    HANDLE showEvent = nullptr;
    NOTIFYICONDATAW tray{};
    WidgetConfig config;
    CalendarParseResult calendar;
    std::vector<CalendarEvent> visibleEvents;
    std::vector<EventHitRegion> hitRegions;
    CalendarEvent noteEvent;
    int weekOffset = 0;
    HFONT titleFont = nullptr;
    HFONT dayFont = nullptr;
    HFONT eventFont = nullptr;
    HFONT roomFont = nullptr;
    UINT dpi = 96;
    FILETIME lastWriteTime{};
    bool hasWriteTime = false;
    bool shuttingDown = false;
    bool userVisible = true;
    bool internalVisibilityChange = false;
    bool dragging = false;
    POINT dragOffset{};
    std::int64_t lastToday = 0;
};

AppState app;

void RecreateFonts();
void ApplyRoundedRegion();
void ApplyBackdrop();

void HideEventNote() {
    if (app.noteWindow && IsWindowVisible(app.noteWindow)) {
        KillTimer(app.noteWindow, 1);
        ShowWindow(app.noteWindow, SW_HIDE);
    }
}

bool IsWindowCloaked(HWND window) {
    DWORD cloakReason = 0;
    return SUCCEEDED(DwmGetWindowAttribute(
               window, DWMWA_CLOAKED, &cloakReason, sizeof(cloakReason))) &&
           cloakReason != 0;
}

void ApplyDesktopPersistenceAttributes() {
    const BOOL enabled = TRUE;
    DwmSetWindowAttribute(app.window, DWMWA_DISALLOW_PEEK,
                          &enabled, sizeof(enabled));
    DwmSetWindowAttribute(app.window, DWMWA_EXCLUDED_FROM_PEEK,
                          &enabled, sizeof(enabled));
    const BOOL uncloaked = FALSE;
    DwmSetWindowAttribute(app.window, DWMWA_CLOAK,
                          &uncloaked, sizeof(uncloaked));
}

void RestoreWidgetVisibility() {
    if (!app.userVisible || app.shuttingDown || !app.window) return;
    if (IsWindowCloaked(app.window)) {
        app.internalVisibilityChange = true;
        const BOOL uncloaked = FALSE;
        DwmSetWindowAttribute(app.window, DWMWA_CLOAK,
                              &uncloaked, sizeof(uncloaked));
        ShowWindow(app.window, SW_HIDE);
        ShowWindow(app.window, SW_SHOWNOACTIVATE);
        app.internalVisibilityChange = false;
    }
    if (IsIconic(app.window)) ShowWindow(app.window, SW_RESTORE);
    if (!IsWindowVisible(app.window)) ShowWindow(app.window, SW_SHOWNOACTIVATE);
    SetWindowPos(app.window,
                 app.config.pinToDesktop
                     ? HWND_TOP
                     : (app.config.alwaysOnTop ? HWND_TOPMOST : HWND_TOP),
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ApplyBackdrop();
    RedrawWindow(app.window, nullptr, nullptr,
                 RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
}

BOOL CALLBACK FindDesktopHostCallback(HWND window, LPARAM parameter) {
    HWND shellView = FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!shellView) return TRUE;
    *reinterpret_cast<HWND*>(parameter) = window;
    return FALSE;
}

HWND FindDesktopHost() {
    if (HWND shell = GetShellWindow()) return shell;
    HWND host = nullptr;
    EnumWindows(FindDesktopHostCallback, reinterpret_cast<LPARAM>(&host));
    if (host) return host;
    return FindWindowW(L"Progman", nullptr);
}

bool ApplyDesktopPinning() {
    if (!app.window) return false;

    RECT screenRect{};
    GetWindowRect(app.window, &screenRect);
    const int width = screenRect.right - screenRect.left;
    const int height = screenRect.bottom - screenRect.top;
    app.internalVisibilityChange = true;

    if (app.config.pinToDesktop) {
        HWND desktopOwner = FindDesktopHost();
        if (!desktopOwner) {
            app.internalVisibilityChange = false;
            return false;
        }

        LONG_PTR style = GetWindowLongPtrW(app.window, GWL_STYLE);
        if ((style & WS_CHILD) != 0) {
            SetParent(app.window, nullptr);
            style = GetWindowLongPtrW(app.window, GWL_STYLE);
        }
        SetWindowLongPtrW(app.window, GWL_STYLE, (style & ~WS_CHILD) | WS_POPUP);
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previousOwner = SetWindowLongPtrW(
            app.window, GWLP_HWNDPARENT,
            reinterpret_cast<LONG_PTR>(desktopOwner));
        if (previousOwner == 0 && GetLastError() != ERROR_SUCCESS) {
            app.internalVisibilityChange = false;
            return false;
        }
        app.desktopHost = desktopOwner;
        SetWindowPos(app.window, HWND_TOP, screenRect.left, screenRect.top, width, height,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(app.window, GWLP_HWNDPARENT, 0);
        LONG_PTR style = GetWindowLongPtrW(app.window, GWL_STYLE);
        if ((style & WS_CHILD) != 0) {
            SetParent(app.window, nullptr);
            style = GetWindowLongPtrW(app.window, GWL_STYLE);
        }
        SetWindowLongPtrW(app.window, GWL_STYLE, (style & ~WS_CHILD) | WS_POPUP);
        app.desktopHost = nullptr;
        SetWindowPos(app.window,
                     app.config.alwaysOnTop ? HWND_TOPMOST : HWND_TOP,
                     screenRect.left, screenRect.top, width, height,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    app.internalVisibilityChange = false;
    app.dpi = GetDpiForWindow(app.window);
    RecreateFonts();
    ApplyRoundedRegion();
    ApplyBackdrop();
    RedrawWindow(app.window, nullptr, nullptr,
                 RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    return true;
}


COLORREF BackgroundColor() {
    if (app.config.backdrop == BackdropMode::Glass) {
        return app.config.theme == WidgetTheme::Dark ? RGB(22, 27, 36) : RGB(245, 249, 253);
    }
    return app.config.theme == WidgetTheme::Dark ? RGB(27, 29, 34) : RGB(246, 247, 249);
}
COLORREF HeaderColor() {
    if (app.config.backdrop == BackdropMode::Glass) {
        return app.config.theme == WidgetTheme::Dark ? RGB(29, 35, 46) : RGB(235, 243, 250);
    }
    return app.config.theme == WidgetTheme::Dark ? RGB(35, 38, 45) : RGB(232, 235, 240);
}
COLORREF PrimaryTextColor() {
    return app.config.theme == WidgetTheme::Dark ? RGB(242, 243, 245) : RGB(30, 32, 36);
}
COLORREF SecondaryTextColor() {
    return app.config.theme == WidgetTheme::Dark ? RGB(172, 177, 187) : RGB(89, 94, 104);
}
COLORREF SeparatorColor() {
    if (app.config.backdrop == BackdropMode::Glass) {
        return app.config.theme == WidgetTheme::Dark ? RGB(76, 85, 101) : RGB(188, 202, 216);
    }
    return app.config.theme == WidgetTheme::Dark ? RGB(59, 63, 73) : RGB(211, 215, 222);
}
COLORREF AccentColor() {
    return app.config.theme == WidgetTheme::Dark ? RGB(107, 157, 255) : RGB(45, 105, 215);
}

std::tm LocalTm(std::int64_t timestamp) {
    std::tm result{};
    const __time64_t native = static_cast<__time64_t>(timestamp);
    _localtime64_s(&result, &native);
    return result;
}

std::int64_t LocalTimestamp(std::tm value) {
    value.tm_isdst = -1;
    return static_cast<std::int64_t>(_mktime64(&value));
}

std::int64_t AddDays(std::int64_t timestamp, int days) {
    std::tm value = LocalTm(timestamp);
    value.tm_mday += days;
    return LocalTimestamp(value);
}

std::int64_t StartOfDay(std::int64_t timestamp) {
    std::tm value = LocalTm(timestamp);
    value.tm_hour = 0;
    value.tm_min = 0;
    value.tm_sec = 0;
    return LocalTimestamp(value);
}

std::int64_t StartOfCurrentWeek() {
    const std::int64_t now = static_cast<std::int64_t>(_time64(nullptr));
    std::tm value = LocalTm(now);
    value.tm_mday -= (value.tm_wday + 6) % 7;
    value.tm_hour = 0;
    value.tm_min = 0;
    value.tm_sec = 0;
    return AddDays(LocalTimestamp(value), app.weekOffset * 7);
}

bool ReadLastWriteTime(const std::wstring& path, FILETIME& output) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    output = data.ftLastWriteTime;
    return true;
}

std::wstring FormatDate(std::int64_t timestamp, const wchar_t* pattern) {
    const std::tm value = LocalTm(timestamp);
    SYSTEMTIME systemTime{};
    systemTime.wYear = static_cast<WORD>(value.tm_year + 1900);
    systemTime.wMonth = static_cast<WORD>(value.tm_mon + 1);
    systemTime.wDay = static_cast<WORD>(value.tm_mday);
    systemTime.wDayOfWeek = static_cast<WORD>(value.tm_wday);
    wchar_t buffer[128]{};
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &systemTime, pattern,
                        buffer, static_cast<int>(std::size(buffer)), nullptr)) {
        return buffer;
    }
    return L"";
}

std::wstring FormatTime(std::int64_t timestamp) {
    const std::tm value = LocalTm(timestamp);
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%02d:%02d", value.tm_hour, value.tm_min);
    return buffer;
}

void DeleteFonts() {
    if (app.titleFont) DeleteObject(app.titleFont);
    if (app.dayFont) DeleteObject(app.dayFont);
    if (app.eventFont) DeleteObject(app.eventFont);
    if (app.roomFont) DeleteObject(app.roomFont);
    app.titleFont = app.dayFont = app.eventFont = app.roomFont = nullptr;
}

HFONT MakeFont(int points, int weight) {
    return CreateFontW(-MulDiv(points, static_cast<int>(app.dpi), 72), 0, 0, 0,
                       weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void RecreateFonts() {
    DeleteFonts();
    app.titleFont = MakeFont(11, FW_SEMIBOLD);
    app.dayFont = MakeFont(10, FW_SEMIBOLD);
    app.eventFont = MakeFont(9, FW_NORMAL);
    app.roomFont = MakeFont(8, FW_NORMAL);
}

void ApplyRoundedRegion() {
    RECT rect{};
    GetClientRect(app.window, &rect);
    const int radius = MulDiv(18, static_cast<int>(app.dpi), 96);
    HRGN region = CreateRoundRectRgn(0, 0, rect.right + 1, rect.bottom + 1, radius, radius);
    if (!SetWindowRgn(app.window, region, TRUE)) DeleteObject(region);
}

void ApplyOpacity() {
    const int opacity = std::clamp(app.config.opacity, 0, 100);
    BYTE alpha = 255;
    if (app.config.backdrop != BackdropMode::Glass &&
        app.config.backdrop != BackdropMode::Acrylic) {
        const double normalized = static_cast<double>(opacity) / 100.0;
        alpha = static_cast<BYTE>(std::lround(std::sqrt(normalized) * 255.0));
    }
    SetLayeredWindowAttributes(app.window, 0, alpha, LWA_ALPHA);
}

DWORD BackdropTint() {
    const int opacity = std::clamp(app.config.opacity, 0, 100);
    const bool glass = app.config.backdrop == BackdropMode::Glass;
    const BYTE alpha = static_cast<BYTE>(glass
        ? 30 + opacity * 40 / 100
        : 70 + opacity * 165 / 100);
    const BYTE red = app.config.theme == WidgetTheme::Dark
        ? static_cast<BYTE>(glass ? 22 : 29)
        : static_cast<BYTE>(glass ? 244 : 244);
    const BYTE green = app.config.theme == WidgetTheme::Dark
        ? static_cast<BYTE>(glass ? 28 : 31)
        : static_cast<BYTE>(glass ? 248 : 247);
    const BYTE blue = app.config.theme == WidgetTheme::Dark
        ? static_cast<BYTE>(glass ? 38 : 34)
        : static_cast<BYTE>(glass ? 252 : 249);
    return (static_cast<DWORD>(alpha) << 24) |
           (static_cast<DWORD>(blue) << 16) |
           (static_cast<DWORD>(green) << 8) |
           red;
}

template <typename Function>
Function LoadUser32Function(const char* name) {
    FARPROC raw = GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(raw));
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

void ApplyBackdrop() {
    constexpr auto systemBackdropAttribute = static_cast<DWMWINDOWATTRIBUTE>(38);
    const int systemBackdrop = app.config.backdrop == BackdropMode::Glass ? 3 : 1;
    DwmSetWindowAttribute(app.window, systemBackdropAttribute,
                          &systemBackdrop, sizeof(systemBackdrop));
    const BOOL darkMode = app.config.theme == WidgetTheme::Dark;
    constexpr auto immersiveDarkModeAttribute = static_cast<DWMWINDOWATTRIBUTE>(20);
    DwmSetWindowAttribute(app.window, immersiveDarkModeAttribute,
                          &darkMode, sizeof(darkMode));
    MARGINS margins{};
    if (app.config.backdrop == BackdropMode::Glass) {
        margins = {-1, -1, -1, -1};
    }
    DwmExtendFrameIntoClientArea(app.window, &margins);

    auto setComposition = LoadUser32Function<SetWindowCompositionAttributeFn>(
        "SetWindowCompositionAttribute");
    if (setComposition) {
        AccentPolicy accent{};
        if (app.config.backdrop == BackdropMode::Clear) {
            accent.state = AccentDisabled;
        } else if (app.config.backdrop == BackdropMode::Blur) {
            accent.state = AccentEnableBlurBehind;
        } else {
            accent.state = AccentEnableAcrylicBlurBehind;
            accent.flags = 2;
            accent.gradientColor = BackdropTint();
        }
        WindowCompositionAttributeData data{19, &accent, sizeof(accent)};
        if (!setComposition(app.window, &data) &&
            (app.config.backdrop == BackdropMode::Acrylic ||
             app.config.backdrop == BackdropMode::Glass)) {
            accent.state = AccentEnableBlurBehind;
            setComposition(app.window, &data);
        }
    }
    ApplyOpacity();
    InvalidateRect(app.window, nullptr, TRUE);
}

void ApplyTopMost() {
    SetWindowPos(app.window,
                 app.config.pinToDesktop
                     ? HWND_TOP
                     : (app.config.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST),
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool ApplyAutoStart(bool enabled) {
    HKEY key = nullptr;
    const LSTATUS openResult = RegCreateKeyExW(
        HKEY_CURRENT_USER, kStartupRunKey, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (openResult != ERROR_SUCCESS) return false;

    bool success = false;
    if (enabled) {
        std::array<wchar_t, 32768> executablePath{};
        const DWORD length = GetModuleFileNameW(
            nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
        if (length > 0 && length < executablePath.size()) {
            const std::wstring command = L"\"" +
                std::wstring(executablePath.data(), length) + L"\"";
            const auto* bytes = reinterpret_cast<const BYTE*>(command.c_str());
            const DWORD byteCount = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
            success = RegSetValueExW(key, kStartupValueName, 0, REG_SZ,
                                     bytes, byteCount) == ERROR_SUCCESS;
        }
    } else {
        const LSTATUS result = RegDeleteValueW(key, kStartupValueName);
        success = result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);
    return success;
}

void TrimWorkingSet() {
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                             static_cast<SIZE_T>(-1));
}

void RefreshVisibleEvents() {
    HideEventNote();
    const std::int64_t start = StartOfCurrentWeek();
    app.visibleEvents = EventsInRange(app.calendar.events, start, AddDays(start, 7));
    InvalidateRect(app.window, nullptr, TRUE);
}

void ReloadCalendar() {
    if (app.config.icsPath.empty()) {
        app.calendar = {};
        app.visibleEvents.clear();
        app.hasWriteTime = false;
        InvalidateRect(app.window, nullptr, TRUE);
        return;
    }
    CalendarParseResult loaded = LoadIcsFile(app.config.icsPath);
    if (loaded.opened) app.calendar = std::move(loaded);
    app.hasWriteTime = ReadLastWriteTime(app.config.icsPath, app.lastWriteTime);
    RefreshVisibleEvents();
}

void SaveWindowPosition() {
    RECT rect{};
    if (GetWindowRect(app.window, &rect)) {
        app.config.x = rect.left;
        app.config.y = rect.top;
        app.config.width = rect.right - rect.left;
        app.config.height = rect.bottom - rect.top;
        SaveConfig(app.config);
    }
}

void AddTrayIcon() {
    app.tray = {};
    app.tray.cbSize = sizeof(app.tray);
    app.tray.hWnd = app.window;
    app.tray.uID = 1;
    app.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    app.tray.uCallbackMessage = kTrayMessage;
    app.tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(app.tray.szTip, L"Calendar Widget");
    Shell_NotifyIconW(NIM_ADD, &app.tray);
    app.tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &app.tray);
}

void AppendCheckedItem(HMENU menu, UINT id, const wchar_t* text, bool checked) {
    AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), id, text);
}

void ShowContextMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    HMENU theme = CreatePopupMenu();
    HMENU backdrop = CreatePopupMenu();
    HMENU opacity = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING, CmdToggleVisible,
                app.userVisible ? L"Ẩn widget" : L"Hiện widget");
    AppendMenuW(menu, MF_STRING, CmdChooseIcs, L"Chọn file ICS...");
    AppendMenuW(menu, MF_STRING, CmdReload, L"Tải lại lịch");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdPreviousWeek, L"Tuần trước");
    AppendMenuW(menu, MF_STRING, CmdCurrentWeek, L"Tuần hiện tại");
    AppendMenuW(menu, MF_STRING, CmdNextWeek, L"Tuần sau");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendCheckedItem(theme, CmdThemeDark, L"Dark", app.config.theme == WidgetTheme::Dark);
    AppendCheckedItem(theme, CmdThemeLight, L"Light", app.config.theme == WidgetTheme::Light);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), L"Theme");

    AppendCheckedItem(backdrop, CmdBackdropClear, L"Clear",
                      app.config.backdrop == BackdropMode::Clear);
    AppendCheckedItem(backdrop, CmdBackdropBlur, L"Blur",
                      app.config.backdrop == BackdropMode::Blur);
    AppendCheckedItem(backdrop, CmdBackdropAcrylic, L"Acrylic",
                      app.config.backdrop == BackdropMode::Acrylic);
    AppendCheckedItem(backdrop, CmdBackdropGlass, L"Glass",
                      app.config.backdrop == BackdropMode::Glass);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(backdrop), L"Hiệu ứng nền");

    for (int value = 0; value <= 100; value += 10) {
        const std::wstring label = std::to_wstring(value) + L"%";
        AppendCheckedItem(opacity, CmdOpacityBase + value / 10, label.c_str(),
                          app.config.opacity == value);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity),
                app.config.backdrop == BackdropMode::Glass
                    ? L"Độ đậm nền kính" : L"Opacity");
    AppendCheckedItem(menu, CmdPinToDesktop,
                      L"Ghim vào Desktop (không bị Win + D)", app.config.pinToDesktop);
    AppendCheckedItem(menu, CmdAlwaysOnTop,
                      L"Luôn ở trên ứng dụng (chế độ nổi)", app.config.alwaysOnTop);
    AppendCheckedItem(menu, CmdAutoStart,
                      L"Khởi động cùng Windows", app.config.autoStart);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdExit, L"Thoát");

    SetForegroundWindow(app.window);
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        point.x, point.y, 0, app.window, nullptr);
    if (command) PostMessageW(app.window, WM_COMMAND, command, 0);
    DestroyMenu(menu);
}

void ChooseIcsFile() {
    std::array<wchar_t, 32768> path{};
    if (!app.config.icsPath.empty()) {
        wcsncpy_s(path.data(), path.size(), app.config.icsPath.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = app.window;
    dialog.lpstrFilter = L"iCalendar (*.ics)\0*.ics\0Tất cả file\0*.*\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    dialog.lpstrDefExt = L"ics";
    if (GetOpenFileNameW(&dialog)) {
        app.config.icsPath = path.data();
        SaveConfig(app.config);
        ReloadCalendar();
        ShowWindow(app.window, SW_SHOWNOACTIVATE);
    }
}

void SetTheme(WidgetTheme value) {
    app.config.theme = value;
    SaveConfig(app.config);
    ApplyBackdrop();
}

void SetBackdrop(BackdropMode value) {
    app.config.backdrop = value;
    SaveConfig(app.config);
    ApplyBackdrop();
}

void SetOpacity(int value) {
    app.config.opacity = std::clamp(value, 0, 100);
    SaveConfig(app.config);
    ApplyBackdrop();
}

void HandleCommand(UINT command) {
    if (command >= CmdOpacityBase && command <= CmdOpacityBase + 10) {
        SetOpacity(static_cast<int>(command - CmdOpacityBase) * 10);
        return;
    }
    switch (command) {
    case CmdToggleVisible:
        HideEventNote();
        app.userVisible = !app.userVisible;
        if (app.userVisible) RestoreWidgetVisibility();
        else ShowWindow(app.window, SW_HIDE);
        break;
    case CmdChooseIcs: ChooseIcsFile(); break;
    case CmdReload: ReloadCalendar(); break;
    case CmdPreviousWeek: --app.weekOffset; RefreshVisibleEvents(); break;
    case CmdNextWeek: ++app.weekOffset; RefreshVisibleEvents(); break;
    case CmdCurrentWeek: app.weekOffset = 0; RefreshVisibleEvents(); break;
    case CmdAlwaysOnTop:
        app.config.alwaysOnTop = !app.config.alwaysOnTop;
        SaveConfig(app.config);
        ApplyTopMost();
        break;
    case CmdPinToDesktop: {
        const bool oldValue = app.config.pinToDesktop;
        app.config.pinToDesktop = !oldValue;
        if (!ApplyDesktopPinning()) {
            app.config.pinToDesktop = oldValue;
            ApplyDesktopPinning();
        }
        SaveConfig(app.config);
        ApplyTopMost();
        break;
    }
    case CmdAutoStart: {
        const bool desired = !app.config.autoStart;
        if (ApplyAutoStart(desired)) {
            app.config.autoStart = desired;
            SaveConfig(app.config);
        }
        break;
    }
    case CmdThemeDark: SetTheme(WidgetTheme::Dark); break;
    case CmdThemeLight: SetTheme(WidgetTheme::Light); break;
    case CmdBackdropClear: SetBackdrop(BackdropMode::Clear); break;
    case CmdBackdropBlur: SetBackdrop(BackdropMode::Blur); break;
    case CmdBackdropAcrylic: SetBackdrop(BackdropMode::Acrylic); break;
    case CmdBackdropGlass: SetBackdrop(BackdropMode::Glass); break;
    case CmdExit:
        app.shuttingDown = true;
        DestroyWindow(app.window);
        break;
    }
}

struct TimelineRange {
    int startMinutes = 8 * 60;
    int endMinutes = 18 * 60;
    int tickMinutes = 120;
};

int MinutesOfDay(std::int64_t timestamp) {
    const std::tm value = LocalTm(timestamp);
    return value.tm_hour * 60 + value.tm_min;
}

std::wstring FormatMinuteValue(int minutes) {
    minutes = std::clamp(minutes, 0, 1440);
    if (minutes == 1440) return L"24:00";
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%02d:%02d", (minutes / 60) % 24, minutes % 60);
    return buffer;
}

TimelineRange DetermineTimelineRange(std::int64_t weekStart, int gridHeight) {
    int earliest = 1440;
    int latest = 0;
    bool hasTimedEvent = false;
    for (int dayIndex = 0; dayIndex < 7; ++dayIndex) {
        const std::int64_t dayStart = AddDays(weekStart, dayIndex);
        const std::int64_t dayEnd = AddDays(dayStart, 1);
        for (const auto& event : app.visibleEvents) {
            if (event.allDay || event.start >= dayEnd || event.end <= dayStart) continue;
            earliest = std::min(earliest, event.start <= dayStart ? 0 : MinutesOfDay(event.start));
            latest = std::max(latest, event.end >= dayEnd ? 1440 : MinutesOfDay(event.end));
            hasTimedEvent = true;
        }
    }

    TimelineRange range;
    if (hasTimedEvent) {
        range.startMinutes = std::max(0, (earliest / 60) * 60);
        range.endMinutes = std::min(1440, ((latest + 59) / 60) * 60);
    }

    constexpr int minimumSpan = 6 * 60;
    if (range.endMinutes - range.startMinutes < minimumSpan) {
        const int center = (range.startMinutes + range.endMinutes) / 2;
        range.startMinutes = std::max(0, center - minimumSpan / 2);
        range.endMinutes = range.startMinutes + minimumSpan;
        if (range.endMinutes > 1440) {
            range.endMinutes = 1440;
            range.startMinutes = 1440 - minimumSpan;
        }
    }

    const int span = range.endMinutes - range.startMinutes;
    constexpr std::array<int, 6> candidates{30, 60, 120, 180, 240, 360};
    range.tickMinutes = candidates.back();
    for (const int candidate : candidates) {
        if (gridHeight * candidate / span >= 42) {
            range.tickMinutes = candidate;
            break;
        }
    }
    return range;
}

COLORREF EventColor(const CalendarEvent& event) {
    static constexpr std::array<COLORREF, 7> darkPalette{
        RGB(69, 106, 154), RGB(103, 78, 139), RGB(61, 121, 112),
        RGB(135, 94, 63), RGB(131, 73, 91), RGB(74, 96, 143), RGB(120, 111, 61)
    };
    static constexpr std::array<COLORREF, 7> lightPalette{
        RGB(177, 216, 242), RGB(216, 185, 230), RGB(179, 221, 205),
        RGB(244, 207, 168), RGB(238, 181, 194), RGB(193, 203, 237), RGB(237, 222, 161)
    };
    const std::wstring& key = event.uid.empty() ? event.title : event.uid;
    const std::size_t index = std::hash<std::wstring>{}(key) % darkPalette.size();
    return app.config.theme == WidgetTheme::Dark ? darkPalette[index] : lightPalette[index];
}

std::wstring CompactEventTitle(const std::wstring& title) {
    const std::size_t opening = title.rfind(L" (");
    if (opening == std::wstring::npos || title.empty() || title.back() != L')') {
        return title;
    }
    const std::wstring suffix = title.substr(opening + 2);
    const bool looksLikeCourseCode =
        suffix.find(L'.') != std::wstring::npos &&
        std::any_of(suffix.begin(), suffix.end(), [](wchar_t character) {
            return character >= L'0' && character <= L'9';
        });
    return looksLikeCourseCode ? title.substr(0, opening) : title;
}

void DrawEventBlock(HDC dc, const CalendarEvent& event, RECT rect) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const COLORREF fill = EventColor(event);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    const int radius = MulDiv(7, static_cast<int>(app.dpi), 96);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    const int roomHeight = event.location.empty()
        ? 0 : MulDiv(16, static_cast<int>(app.dpi), 96);
    SetTextColor(dc, app.config.theme == WidgetTheme::Dark
                         ? RGB(250, 250, 252) : RGB(31, 38, 45));
    SelectObject(dc, app.eventFont);
    const std::wstring title = CompactEventTitle(event.title);
    RECT textRect{rect.left + 5, rect.top + 4, rect.right - 4,
                  rect.bottom - roomHeight - 2};
    DrawTextW(dc, title.c_str(), -1, &textRect,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (!event.location.empty()) {
        SelectObject(dc, app.roomFont);
        RECT roomRect{rect.left + 5, rect.bottom - roomHeight - 1,
                      rect.right - 4, rect.bottom - 2};
        DrawTextW(dc, event.location.c_str(), -1, &roomRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

const EventHitRegion* HitTestEvent(POINT point) {
    for (auto it = app.hitRegions.rbegin(); it != app.hitRegions.rend(); ++it) {
        if (PtInRect(&it->bounds, point)) return &*it;
    }
    return nullptr;
}

LRESULT CALLBACK NoteWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(BackgroundColor());
        FillRect(dc, &client, background);
        DeleteObject(background);

        HPEN border = CreatePen(PS_SOLID, 1, SeparatorColor());
        HGDIOBJ oldPen = SelectObject(dc, border);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        const int radius = MulDiv(14, static_cast<int>(app.dpi), 96);
        RoundRect(dc, 0, 0, client.right, client.bottom, radius, radius);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(border);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, PrimaryTextColor());
        SelectObject(dc, app.titleFont);
        RECT titleRect{14, 11, client.right - 38, 54};
        DrawTextW(dc, app.noteEvent.title.c_str(), -1, &titleRect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT closeRect{client.right - 32, 8, client.right - 8, 32};
        DrawTextW(dc, L"×", -1, &closeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        HPEN separator = CreatePen(PS_SOLID, 1, SeparatorColor());
        oldPen = SelectObject(dc, separator);
        MoveToEx(dc, 14, 59, nullptr);
        LineTo(dc, client.right - 14, 59);
        SelectObject(dc, oldPen);
        DeleteObject(separator);

        SetTextColor(dc, SecondaryTextColor());
        SelectObject(dc, app.eventFont);
        std::wstring timeText = FormatDate(app.noteEvent.start, L"dddd, dd/MM/yyyy");
        if (app.noteEvent.allDay) timeText += L"  •  Cả ngày";
        else {
            timeText += L"  •  " + FormatTime(app.noteEvent.start) +
                        L"–" + FormatTime(app.noteEvent.end);
        }
        RECT timeRect{14, 68, client.right - 14, 89};
        DrawTextW(dc, timeText.c_str(), -1, &timeRect,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        std::wstring roomText = app.noteEvent.location.empty()
            ? L"Phòng: chưa có" : L"Phòng: " + app.noteEvent.location;
        RECT roomRect{14, 91, client.right - 14, 112};
        DrawTextW(dc, roomText.c_str(), -1, &roomRect,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        SetTextColor(dc, PrimaryTextColor());
        const std::wstring description = app.noteEvent.description.empty()
            ? L"Không có mô tả chi tiết." : app.noteEvent.description;
        RECT descriptionRect{14, 119, client.right - 14, client.bottom - 12};
        DrawTextW(dc, description.c_str(), -1, &descriptionRect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        HideEventNote();
        return 0;
    case WM_TIMER:
        HideEventNote();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowEventNote(const CalendarEvent& event, const RECT& eventRect) {
    app.noteEvent = event;
    if (!app.noteWindow) {
        app.noteWindow = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kNoteWindowClass, L"Chi tiết sự kiện", WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, app.instance, nullptr);
        if (!app.noteWindow) return;
    }

    POINT topLeft{eventRect.left, eventRect.top};
    POINT bottomRight{eventRect.right, eventRect.bottom};
    ClientToScreen(app.window, &topLeft);
    ClientToScreen(app.window, &bottomRight);

    const int noteWidth = MulDiv(340, static_cast<int>(app.dpi), 96);
    const int noteHeight = MulDiv(220, static_cast<int>(app.dpi), 96);
    RECT anchor{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(monitor, &monitorInfo);

    const int gap = MulDiv(8, static_cast<int>(app.dpi), 96);
    const int workLeft = static_cast<int>(monitorInfo.rcWork.left);
    const int workTop = static_cast<int>(monitorInfo.rcWork.top);
    const int workRight = static_cast<int>(monitorInfo.rcWork.right);
    const int workBottom = static_cast<int>(monitorInfo.rcWork.bottom);
    int x = bottomRight.x + gap;
    if (x + noteWidth > workRight) x = topLeft.x - noteWidth - gap;
    int y = topLeft.y;
    x = std::clamp(x, workLeft, std::max(workLeft, workRight - noteWidth));
    y = std::clamp(y, workTop, std::max(workTop, workBottom - noteHeight));

    HRGN region = CreateRoundRectRgn(
        0, 0, noteWidth + 1, noteHeight + 1,
        MulDiv(14, static_cast<int>(app.dpi), 96),
        MulDiv(14, static_cast<int>(app.dpi), 96));
    if (!SetWindowRgn(app.noteWindow, region, FALSE)) DeleteObject(region);

    SetWindowPos(app.noteWindow, HWND_TOPMOST, x, y, noteWidth, noteHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(app.noteWindow, nullptr, TRUE);
    KillTimer(app.noteWindow, 1);
    SetTimer(app.noteWindow, 1, 15000, nullptr);
}


void PaintHorizontalWidget(HDC target, const RECT& client) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    app.hitRegions.clear();

    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
    HBRUSH brush = CreateSolidBrush(
        app.config.backdrop == BackdropMode::Glass ? RGB(0, 0, 0) : BackgroundColor());
    FillRect(buffer, &client, brush);
    DeleteObject(brush);
    SetBkMode(buffer, TRANSPARENT);

    const int toolbarHeight = MulDiv(44, static_cast<int>(app.dpi), 96);
    const int dayHeaderHeight = MulDiv(38, static_cast<int>(app.dpi), 96);
    const int axisWidth = MulDiv(58, static_cast<int>(app.dpi), 96);
    const int rightPadding = MulDiv(8, static_cast<int>(app.dpi), 96);
    const int dayWidth = std::max(1, (width - axisWidth - rightPadding) / 7);
    const std::int64_t weekStart = StartOfCurrentWeek();
    const std::int64_t today = StartOfDay(static_cast<std::int64_t>(_time64(nullptr)));

    const bool hasAllDay = std::any_of(
        app.visibleEvents.begin(), app.visibleEvents.end(),
        [](const CalendarEvent& event) { return event.allDay; });
    const int allDayHeight = hasAllDay ? MulDiv(24, static_cast<int>(app.dpi), 96) : 0;
    const int gridTop = toolbarHeight + dayHeaderHeight + allDayHeight;
    const int gridBottom = height - MulDiv(16, static_cast<int>(app.dpi), 96);
    const int timelinePadding = MulDiv(24, static_cast<int>(app.dpi), 96);
    const int timelineTop = gridTop + timelinePadding;
    const int timelineBottom = gridBottom - timelinePadding;
    const int timelineHeight = std::max(1, timelineBottom - timelineTop);
    const TimelineRange timeline = DetermineTimelineRange(weekStart, timelineHeight);
    const int timelineSpan = timeline.endMinutes - timeline.startMinutes;

    RECT toolbar{0, 0, width, toolbarHeight};
    brush = CreateSolidBrush(
        app.config.backdrop == BackdropMode::Glass ? RGB(0, 0, 0) : HeaderColor());
    FillRect(buffer, &toolbar, brush);
    DeleteObject(brush);

    SetTextColor(buffer, PrimaryTextColor());
    SelectObject(buffer, app.titleFont);
    const std::wstring title = FormatDate(weekStart, L"dd/MM") + L" — " +
                               FormatDate(AddDays(weekStart, 6), L"dd/MM/yyyy");
    RECT titleRect{44, 0, width - 44, toolbarHeight};
    DrawTextW(buffer, title.c_str(), -1, &titleRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT leftArrow{8, 0, 42, toolbarHeight};
    RECT rightArrow{width - 42, 0, width - 8, toolbarHeight};
    DrawTextW(buffer, L"‹", -1, &leftArrow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(buffer, L"›", -1, &rightArrow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    HPEN gridPen = CreatePen(PS_SOLID, 1, SeparatorColor());
    HGDIOBJ oldPen = SelectObject(buffer, gridPen);

    for (int dayIndex = 0; dayIndex < 7; ++dayIndex) {
        const int left = axisWidth + dayIndex * dayWidth;
        const int right = dayIndex == 6 ? width - rightPadding : left + dayWidth;
        const std::int64_t dayStart = AddDays(weekStart, dayIndex);
        if (dayStart == today) {
            RECT highlight{left + 1, toolbarHeight, right, gridBottom};
            HBRUSH todayBrush = CreateSolidBrush(
                app.config.theme == WidgetTheme::Dark ? RGB(34, 43, 58) : RGB(232, 241, 255));
            FillRect(buffer, &highlight, todayBrush);
            DeleteObject(todayBrush);
        }
        if (dayIndex > 0) {
            MoveToEx(buffer, left, toolbarHeight, nullptr);
            LineTo(buffer, left, gridBottom);
        }
        SetTextColor(buffer, dayStart == today ? AccentColor() : PrimaryTextColor());
        SelectObject(buffer, app.dayFont);
        RECT dayRect{left + 4, toolbarHeight + 2, right - 4,
                     toolbarHeight + dayHeaderHeight};
        const std::wstring dayText = FormatDate(dayStart, L"ddd") + L"\n" +
                                     FormatDate(dayStart, L"dd/MM");
        DrawTextW(buffer, dayText.c_str(), -1, &dayRect,
                  DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }

    MoveToEx(buffer, axisWidth, toolbarHeight + dayHeaderHeight, nullptr);
    LineTo(buffer, width - rightPadding, toolbarHeight + dayHeaderHeight);
    MoveToEx(buffer, axisWidth, gridTop, nullptr);
    LineTo(buffer, width - rightPadding, gridTop);

    if (hasAllDay) {
        for (int dayIndex = 0; dayIndex < 7; ++dayIndex) {
            const std::int64_t dayStart = AddDays(weekStart, dayIndex);
            const std::int64_t dayEnd = AddDays(dayStart, 1);
            const CalendarEvent* first = nullptr;
            int count = 0;
            for (const auto& event : app.visibleEvents) {
                if (event.allDay && event.start < dayEnd && event.end > dayStart) {
                    if (!first) first = &event;
                    ++count;
                }
            }
            if (first) {
                CalendarEvent label = *first;
                if (count > 1) label.title += L"  +" + std::to_wstring(count - 1);
                const int left = axisWidth + dayIndex * dayWidth + 3;
                const int right = dayIndex == 6
                    ? width - rightPadding - 3
                    : axisWidth + (dayIndex + 1) * dayWidth - 3;
                const RECT eventRect{
                    left, toolbarHeight + dayHeaderHeight + 2, right, gridTop - 2};
                DrawEventBlock(buffer, label, eventRect);
                app.hitRegions.push_back(EventHitRegion{eventRect, *first});
            }
        }
    }

    SetTextColor(buffer, SecondaryTextColor());
    SelectObject(buffer, app.eventFont);
    const auto drawTimeMark = [&](int minute) {
        const int y = timelineTop +
            (minute - timeline.startMinutes) * timelineHeight / timelineSpan;
        RECT label{4, y - 9, axisWidth - 7, y + 9};
        const std::wstring text = FormatMinuteValue(minute);
        DrawTextW(buffer, text.c_str(), -1, &label,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        MoveToEx(buffer, axisWidth, y, nullptr);
        LineTo(buffer, width - rightPadding, y);
    };

    drawTimeMark(timeline.startMinutes);
    for (int minute = timeline.startMinutes + timeline.tickMinutes;
         minute < timeline.endMinutes;
         minute += timeline.tickMinutes) {
        drawTimeMark(minute);
    }
    drawTimeMark(timeline.endMinutes);

    for (int dayIndex = 0; dayIndex < 7; ++dayIndex) {
        const std::int64_t dayStart = AddDays(weekStart, dayIndex);
        const std::int64_t dayEnd = AddDays(dayStart, 1);
        const int left = axisWidth + dayIndex * dayWidth + 4;
        const int right = dayIndex == 6 ? width - rightPadding - 4
                                        : axisWidth + (dayIndex + 1) * dayWidth - 4;
        for (const auto& event : app.visibleEvents) {
            if (event.allDay || event.start >= dayEnd || event.end <= dayStart) continue;
            const int startMinute = event.start <= dayStart
                ? timeline.startMinutes : MinutesOfDay(event.start);
            const int endMinute = event.end >= dayEnd
                ? timeline.endMinutes : MinutesOfDay(event.end);
            if (endMinute <= timeline.startMinutes || startMinute >= timeline.endMinutes) continue;
            const int clippedStart = std::max(startMinute, timeline.startMinutes);
            const int clippedEnd = std::min(endMinute, timeline.endMinutes);
            int top = timelineTop +
                (clippedStart - timeline.startMinutes) * timelineHeight / timelineSpan + 2;
            int bottom = timelineTop +
                (clippedEnd - timeline.startMinutes) * timelineHeight / timelineSpan - 2;
            bottom = std::max(bottom, top + MulDiv(20, static_cast<int>(app.dpi), 96));
            bottom = std::min(bottom, timelineBottom);
            const RECT eventRect{left, top, right, bottom};
            DrawEventBlock(buffer, event, eventRect);
            app.hitRegions.push_back(EventHitRegion{eventRect, event});
        }
    }

    const std::int64_t now = static_cast<std::int64_t>(_time64(nullptr));
    const int currentMinute = MinutesOfDay(now);
    if (app.weekOffset == 0 && currentMinute >= timeline.startMinutes &&
        currentMinute <= timeline.endMinutes) {
        const int dayIndex = (LocalTm(now).tm_wday + 6) % 7;
        const int left = axisWidth + dayIndex * dayWidth;
        const int right = dayIndex == 6 ? width - rightPadding
                                        : axisWidth + (dayIndex + 1) * dayWidth;
        const int y = timelineTop +
            (currentMinute - timeline.startMinutes) * timelineHeight / timelineSpan;
        HPEN nowPen = CreatePen(PS_SOLID, 2, RGB(232, 78, 78));
        SelectObject(buffer, nowPen);
        MoveToEx(buffer, left, y, nullptr);
        LineTo(buffer, right, y);
        SelectObject(buffer, gridPen);
        DeleteObject(nowPen);
    }

    SetTextColor(buffer, SecondaryTextColor());
    SelectObject(buffer, app.eventFont);
    RECT messageRect{axisWidth + 20, gridTop, width - 20, gridBottom};
    if (app.config.icsPath.empty()) {
        DrawTextW(buffer, L"Click phải → Chọn file ICS để bắt đầu", -1, &messageRect,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    } else if (!app.calendar.opened) {
        SetTextColor(buffer, RGB(220, 80, 80));
        DrawTextW(buffer, L"Không thể đọc file ICS đã chọn.", -1, &messageRect,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    } else if (app.visibleEvents.empty()) {
        DrawTextW(buffer, L"Không có sự kiện trong tuần này", -1, &messageRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (app.config.backdrop == BackdropMode::Glass) {
        const COLORREF borderColor = app.config.theme == WidgetTheme::Dark
            ? RGB(126, 141, 164) : RGB(255, 255, 255);
        HPEN glassBorder = CreatePen(PS_SOLID, 1, borderColor);
        HGDIOBJ previousPen = SelectObject(buffer, glassBorder);
        HGDIOBJ previousBrush = SelectObject(buffer, GetStockObject(NULL_BRUSH));
        const int radius = MulDiv(18, static_cast<int>(app.dpi), 96);
        RoundRect(buffer, 1, 1, width - 1, height - 1, radius, radius);
        SelectObject(buffer, previousBrush);
        SelectObject(buffer, previousPen);
        DeleteObject(glassBorder);

        HPEN topHighlight = CreatePen(
            PS_SOLID, 1,
            app.config.theme == WidgetTheme::Dark ? RGB(172, 188, 211) : RGB(255, 255, 255));
        previousPen = SelectObject(buffer, topHighlight);
        MoveToEx(buffer, radius, 2, nullptr);
        LineTo(buffer, width - radius, 2);
        SelectObject(buffer, previousPen);
        DeleteObject(topHighlight);
    }

    SelectObject(buffer, oldPen);
    DeleteObject(gridPen);
    BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}



void CheckFileChange() {
    if (app.config.icsPath.empty()) return;
    FILETIME current{};
    if (!ReadLastWriteTime(app.config.icsPath, current)) return;
    if (!app.hasWriteTime || CompareFileTime(&current, &app.lastWriteTime) != 0) {
        ReloadCalendar();
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        app.window = window;
        app.dpi = GetDpiForWindow(window);
        RecreateFonts();
        ApplyRoundedRegion();
        ApplyBackdrop();
        ApplyDesktopPersistenceAttributes();
        AddTrayIcon();
        app.lastToday = StartOfDay(static_cast<std::int64_t>(_time64(nullptr)));
        SetTimer(window, kRefreshTimer, 5000, nullptr);
        SetTimer(window, kTrimTimer, 1500, nullptr);
        SetTimer(window, kDesktopGuardTimer, 500, nullptr);
        ReloadCalendar();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        PaintHorizontalWidget(dc, client);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        RECT client{};
        GetClientRect(window, &client);
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const POINT point{x, y};
        if (const EventHitRegion* hit = HitTestEvent(point)) {
            ShowEventNote(hit->event, hit->bounds);
            return 0;
        }
        HideEventNote();
        const int headerHeight = MulDiv(44, static_cast<int>(app.dpi), 96);
        if (y < headerHeight && x < 44) HandleCommand(CmdPreviousWeek);
        else if (y < headerHeight && x > client.right - 44) HandleCommand(CmdNextWeek);
        else {
            POINT cursor{};
            RECT windowRect{};
            GetCursorPos(&cursor);
            GetWindowRect(window, &windowRect);
            app.dragOffset = {cursor.x - windowRect.left, cursor.y - windowRect.top};
            app.dragging = true;
            SetCapture(window);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (app.dragging && (wParam & MK_LBUTTON)) {
            POINT cursor{};
            GetCursorPos(&cursor);
            POINT destination{cursor.x - app.dragOffset.x, cursor.y - app.dragOffset.y};
            if ((GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0 &&
                app.desktopHost) {
                ScreenToClient(app.desktopHost, &destination);
            }
            SetWindowPos(window, nullptr, destination.x, destination.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (app.dragging) {
            app.dragging = false;
            ReleaseCapture();
            SaveWindowPosition();
        }
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        app.dragging = false;
        return 0;
    case WM_RBUTTONUP: {
        HideEventNote();
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(window, &point);
        ShowContextMenu(point);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (GetKeyState(VK_CONTROL) < 0) {
            SetOpacity(app.config.opacity + (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1));
            return 0;
        }
        break;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window, &point);
        if (HitTestEvent(point)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE && app.userVisible) {
            PostMessageW(window, kRestoreMessage, 0, 0);
            return 0;
        }
        break;
    case WM_WINDOWPOSCHANGING:
        if (app.userVisible && !app.shuttingDown && !app.internalVisibilityChange) {
            auto* position = reinterpret_cast<WINDOWPOS*>(lParam);
            position->flags &= ~SWP_HIDEWINDOW;
        }
        return 0;
    case WM_EXITSIZEMOVE:
        SaveWindowPosition();
        return 0;
    case WM_DPICHANGED: {
        app.dpi = HIWORD(wParam);
        RecreateFonts();
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyRoundedRegion();
        return 0;
    }
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED && app.userVisible) {
            PostMessageW(window, kRestoreMessage, 0, 0);
            return 0;
        }
        ApplyRoundedRegion();
        return 0;
    case WM_TIMER:
        if (app.showEvent && WaitForSingleObject(app.showEvent, 0) == WAIT_OBJECT_0) {
            app.userVisible = true;
            RestoreWidgetVisibility();
        }
        if (wParam == kRefreshTimer) {
            CheckFileChange();
            const std::int64_t today = StartOfDay(static_cast<std::int64_t>(_time64(nullptr)));
            if (today != app.lastToday) {
                app.lastToday = today;
                RefreshVisibleEvents();
            }
            if (app.config.pinToDesktop && app.userVisible) {
                ApplyOpacity();
                RedrawWindow(window, nullptr, nullptr,
                             RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
            }
        } else if (wParam == kTrimTimer) {
            KillTimer(window, kTrimTimer);
            TrimWorkingSet();
        } else if (wParam == kDesktopGuardTimer && app.userVisible) {
            if (app.config.pinToDesktop &&
                (!app.desktopHost || !IsWindow(app.desktopHost))) {
                ApplyDesktopPinning();
            }
            if (IsIconic(window) || !IsWindowVisible(window) || IsWindowCloaked(window)) {
                PostMessageW(window, kRestoreMessage, 0, 0);
            }
        }
        return 0;
    case kRestoreMessage:
        RestoreWidgetVisibility();
        return 0;
    case WM_COMMAND:
        HandleCommand(LOWORD(wParam));
        return 0;
    case kTrayMessage:
        if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) {
            POINT point{};
            GetCursorPos(&point);
            ShowContextMenu(point);
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK || LOWORD(lParam) == NIN_SELECT) {
            HandleCommand(CmdToggleVisible);
        }
        return 0;
    case WM_CLOSE:
        if (!app.shuttingDown) {
            app.userVisible = false;
            HideEventNote();
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        break;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) {
            app.shuttingDown = true;
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, kRefreshTimer);
        KillTimer(window, kTrimTimer);
        KillTimer(window, kDesktopGuardTimer);
        HideEventNote();
        if (app.noteWindow) {
            DestroyWindow(app.noteWindow);
            app.noteWindow = nullptr;
        }
        SaveWindowPosition();
        Shell_NotifyIconW(NIM_DELETE, &app.tray);
        DeleteFonts();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void EnableDpiAwareness() {
    auto setAwareness = LoadUser32Function<SetProcessDpiAwarenessContextFn>(
        "SetProcessDpiAwarenessContext");
    if (setAwareness) setAwareness(reinterpret_cast<HANDLE>(-4));
    else SetProcessDPIAware();
}

RECT InitialWindowRect(const WidgetConfig& config) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT result{};
    result.left = config.x >= 0 ? config.x : work.right - config.width - 20;
    result.top = config.y >= 0 ? config.y : work.top + 20;
    result.right = result.left + config.width;
    result.bottom = result.top + config.height;

    HMONITOR monitor = MonitorFromRect(&result, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        result.left = std::clamp(result.left, info.rcWork.left,
                                 std::max(info.rcWork.left, info.rcWork.right - config.width));
        result.top = std::clamp(result.top, info.rcWork.top,
                                std::max(info.rcWork.top, info.rcWork.bottom - config.height));
        result.right = result.left + config.width;
        result.bottom = result.top + config.height;
    }
    return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    EnableDpiAwareness();
    HANDLE showEvent = CreateEventW(nullptr, FALSE, FALSE, kShowEventName);
    if (!showEvent) return 1;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\CalendarWidget.SingleInstance");
    if (!mutex) {
        CloseHandle(showEvent);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        SetEvent(showEvent);
        CloseHandle(mutex);
        CloseHandle(showEvent);
        return 0;
    }

    app.instance = instance;
    app.showEvent = showEvent;
    app.config = LoadConfig();
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && argumentCount > 1) {
        const std::filesystem::path inputPath(arguments[1]);
        if (inputPath.extension() == L".ics" || inputPath.extension() == L".ICS") {
            app.config.icsPath = inputPath.wstring();
        }
    }
    if (arguments) LocalFree(arguments);
    if (app.config.autoStart) ApplyAutoStart(true);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        CloseHandle(mutex);
        CloseHandle(showEvent);
        return 1;
    }

    WNDCLASSEXW noteClass{};
    noteClass.cbSize = sizeof(noteClass);
    noteClass.lpfnWndProc = NoteWindowProc;
    noteClass.hInstance = instance;
    noteClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    noteClass.lpszClassName = kNoteWindowClass;
    if (!RegisterClassExW(&noteClass)) {
        CloseHandle(mutex);
        CloseHandle(showEvent);
        return 1;
    }

    const RECT rect = InitialWindowRect(app.config);
    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED, kWindowClass, L"Calendar Widget",
        WS_POPUP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        CloseHandle(mutex);
        CloseHandle(showEvent);
        return 2;
    }

    if (app.config.pinToDesktop && !ApplyDesktopPinning()) {
        app.config.pinToDesktop = false;
        SaveConfig(app.config);
    }
    ApplyTopMost();

    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    TrimWorkingSet();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CloseHandle(mutex);
    CloseHandle(showEvent);
    return static_cast<int>(message.wParam);
}
