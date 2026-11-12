#pragma once

// Copy this exe to %LOCALAPPDATA%\Programs\Stalgia\Stalgia.exe, write a desktop
// shortcut, and launch the installed copy. Does not turn on Start with Windows.
// Returns a process exit code.
int runInstall();

// Ask a running instance to exit, remove the Run key and shortcuts, restore
// icons and the classic wallpaper, and delete the per-user install folder.
int runUninstall();
