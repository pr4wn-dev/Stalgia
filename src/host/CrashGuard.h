#pragma once

// Top-level crash path: restore icons and wallpaper, then write a minidump
// and the last log lines under %APPDATA%\Stalgia\crash.
namespace CrashGuard {
void install();
}
