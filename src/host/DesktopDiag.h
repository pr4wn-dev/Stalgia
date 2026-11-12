#pragma once

#include <windows.h>

// Read-only diagnostics. --diag must not send 0x052C or otherwise mutate Explorer.
void logOsVersion();
void logMonitorAndDpiInfo();
void logDesktopWindowTree();
void describeWindow(HWND hwnd, char* out, int outChars);
HWND findWindowByClass(const wchar_t* className);
DWORD windowsBuildNumber();
bool progmanIsRaisedDesktop(HWND progman);
