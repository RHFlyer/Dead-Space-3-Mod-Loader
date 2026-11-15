#pragma once

// Append-only log file next to the proxy DLL (ds3_mods.log). Safe to call from any thread.
void LogInit(const wchar_t* moduleDir);
void Log(const char* fmt, ...);
