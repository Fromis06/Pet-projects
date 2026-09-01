#pragma once

#include <string>

enum class WidgetTheme {
    Dark = 0,
    Light = 1
};

enum class BackdropMode {
    Clear = 0,
    Blur = 1,
    Acrylic = 2,
    Glass = 3
};

struct WidgetConfig {
    int x = -1;
    int y = -1;
    int width = 960;
    int height = 480;
    int opacity = 90;
    WidgetTheme theme = WidgetTheme::Dark;
    BackdropMode backdrop = BackdropMode::Glass;
    bool alwaysOnTop = true;
    bool pinToDesktop = true;
    bool autoStart = true;
    std::wstring icsPath;
};

WidgetConfig LoadConfig();
void SaveConfig(const WidgetConfig& config);
std::wstring ConfigFilePath();
