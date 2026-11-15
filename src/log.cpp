#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
constexpr LONGLONG kMaxLogBytes = 1024 * 1024;

// Folders the log never shows, as the log's %ls formats them: the game folder and the user's profile folder
// (C:\Users\<name>). Players send this log with bug reports, and a Windows user name is often a real name, so every
// line shows <game> and %USERPROFILE% instead. The game folder is checked first: it may be inside the profile.
struct Hidden {
    char path[MAX_PATH * 3];
    int len;
    const char* tag;
};
Hidden g_hidden[2] = {{{}, 0, "<game>"}, {{}, 0, "%USERPROFILE%"}};

void SetHidden(Hidden& h, const wchar_t* path)
{
    int r = snprintf(h.path, sizeof(h.path), "%ls", path);  // same conversion as the log's %ls
    h.len = r > 3 && r < static_cast<int>(sizeof(h.path)) ? r : 0;
}

char Fold(char c)
{
    if (c == '/') return '\\';
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Whether line[i..] starts with folder h as a whole folder: case and / vs \ don't matter, and the next character
// must end the name (so a profile folder named Al does not match inside one named Alice).
bool FolderAt(const char* line, int i, int n, const Hidden& h)
{
    if (h.len <= 0 || n - i < h.len) return false;
    for (int k = 0; k < h.len; ++k)
        if (Fold(line[i + k]) != Fold(h.path[k])) return false;
    if (i + h.len == n) return true;
    const unsigned char next = static_cast<unsigned char>(line[i + h.len]);
    const bool nameChar = (next >= '0' && next <= '9') || (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                          next == '_' || next == '-' || next == '.' || next == '~' || next >= 0x80;
    return !nameChar;
}

// Replaces the hidden folders in line[0..n) in place; returns the new length (at most size).
int HideFolders(char* line, int n, int size)
{
    char out[1024];
    const int cap = size < static_cast<int>(sizeof(out)) ? size : static_cast<int>(sizeof(out));
    int o = 0;
    for (int i = 0; i < n && o < cap;) {
        const Hidden* hit = nullptr;
        for (const Hidden& h : g_hidden)
            if (FolderAt(line, i, n, h)) { hit = &h; break; }
        if (hit) {
            for (const char* t = hit->tag; *t && o < cap; ++t) out[o++] = *t;
            i += hit->len;
        } else {
            out[o++] = line[i++];
        }
    }
    std::memcpy(line, out, o);
    return o;
}
}

void LogInit(const wchar_t* moduleDir)
{
    SetHidden(g_hidden[0], moduleDir);
    wchar_t profile[MAX_PATH];
    DWORD k = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (k > 3 && k < MAX_PATH) SetHidden(g_hidden[1], profile);

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\ds3_mods.log", moduleDir);
    // Append, so a relaunch to check a purchase does not wipe the session that made it. Start over past 1 MB.
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER size{};
    if (GetFileSizeEx(g_file, &size) && size.QuadPart > kMaxLogBytes)
        SetEndOfFile(g_file);  // The file pointer is still at 0, so this empties it.
    SetFilePointer(g_file, 0, nullptr, FILE_END);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char header[64];
    int n = sprintf_s(header, "\r\n===== session %04u-%02u-%02u =====\r\n", t.wYear, t.wMonth, t.wDay);
    DWORD written;
    WriteFile(g_file, header, static_cast<DWORD>(n), &written, nullptr);
}

void Log(const char* fmt, ...)
{
    if (g_file == INVALID_HANDLE_VALUE) return;

    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = sprintf_s(line, "%02u:%02u:%02u.%03u [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                      GetCurrentThreadId());

    va_list args;
    va_start(args, fmt);
    int m = vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
    va_end(args);
    n = (m < 0) ? n : (n + m < static_cast<int>(sizeof(line)) - 2 ? n + m : static_cast<int>(sizeof(line)) - 3);
    n = HideFolders(line, n, static_cast<int>(sizeof(line)) - 2);
    line[n++] = '\r';
    line[n++] = '\n';

    AcquireSRWLockExclusive(&g_lock);
    DWORD written;
    WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
    FlushFileBuffers(g_file);
    ReleaseSRWLockExclusive(&g_lock);
}
