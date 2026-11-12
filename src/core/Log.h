#pragma once

#include <string>

namespace logx {

void init(const std::wstring& path);
const wchar_t* path();
void write(const char* level, const char* fmt, ...);
void close();
// Last lines still in memory, for the crash report. Safe if the log lock is held.
int copyRecent(char* dst, int cap);

}  // namespace logx

#define LOG_INFO(...) ::logx::write("INFO", __VA_ARGS__)
#define LOG_WARN(...) ::logx::write("WARN", __VA_ARGS__)
#define LOG_ERR(...)  ::logx::write("ERR ", __VA_ARGS__)
