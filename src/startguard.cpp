// Safe start. See startguard.h.

#include "startguard.h"

#include "log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>

namespace {

constexpr ULONGLONG kGoodAfterMs = 20000;  // frames drawn over this long after the mods loaded: the start finished
constexpr int kGoodAfterFrames = 300;
constexpr int kMaxEntries = 40;
constexpr ULONGLONG kRetryMs = 5000;  // a failed "ok" write is tried again this much later, a few times
constexpr int kMaxTries = 6;
constexpr DWORD kLockWaitMs = 10000;

wchar_t g_journal[MAX_PATH] = {};  // empty: no journal (the path does not fit)
bool g_asked = false;
bool g_stuck = false;  // a safe start that could not be written down: the next start will be one too
bool g_owner = false;  // this process wrote "loading" and may write what follows
bool g_done = false;
ULONGLONG g_committedAt = 0;
ULONGLONG g_retryAt = 0;
int g_tries = 0;
int g_frames = 0;
// Held from StartGuardBegin until this process has written its decision, so two game processes starting together
// cannot both read "ok" and both write "loading".
HANDLE g_lock = nullptr;

struct Entry {
    wchar_t key[160];
    char sha[65];
};
Entry g_entries[kMaxEntries] = {};
int g_entryCount = 0;

void Get(const wchar_t* section, const wchar_t* key, wchar_t* out, DWORD size)
{
    out[0] = L'\0';
    if (g_journal[0]) GetPrivateProfileStringW(section, key, L"", out, size, g_journal);
}

bool Put(const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    return g_journal[0] && WritePrivateProfileStringW(section, key, value, g_journal) != FALSE;
}

// Whether the journal now says `state`, read back from the file.
bool StateIs(const wchar_t* state)
{
    wchar_t now[24];
    Get(L"start", L"state", now, 24);
    return _wcsicmp(now, state) == 0;
}

// One lock per game folder, by a hash of its path (a mutex name may not hold backslashes).
void Lock(const wchar_t* gameDir)
{
    unsigned hash = 2166136261u;
    for (const wchar_t* p = gameDir; *p; ++p) hash = (hash ^ static_cast<unsigned>(towlower(*p))) * 16777619u;
    wchar_t name[64];
    swprintf_s(name, L"Local\\DS3ModLoaderStart-%08X", hash);
    g_lock = CreateMutexW(nullptr, FALSE, name);
    if (!g_lock) return;
    const DWORD r = WaitForSingleObject(g_lock, kLockWaitMs);
    if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {  // another game holds it far too long: go on without it
        CloseHandle(g_lock);
        g_lock = nullptr;
    }
}

void Unlock()
{
    if (!g_lock) return;
    ReleaseMutex(g_lock);
    CloseHandle(g_lock);
    g_lock = nullptr;
}

// Whether process `pid` is still running and is this same program (a process id is reused after its owner ends).
bool SameGameRunning(DWORD pid)
{
    if (!pid || pid == GetCurrentProcessId()) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    bool same = false;
    DWORD code = 0;
    if (GetExitCodeProcess(p, &code) && code == STILL_ACTIVE) {
        wchar_t theirs[MAX_PATH], ours[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, theirs, &n) && GetModuleFileNameW(nullptr, ours, MAX_PATH))
            same = _wcsicmp(theirs, ours) == 0;
    }
    CloseHandle(p);
    return same;
}

bool OwnsJournal()
{
    if (!g_owner) return false;
    wchar_t pid[24];
    Get(L"start", L"pid", pid, 24);
    return static_cast<DWORD>(wcstoul(pid, nullptr, 10)) == GetCurrentProcessId();
}

}  // namespace

StartKind StartGuardBegin(const wchar_t* gameDir, const wchar_t* modsIni)
{
    // A game folder whose path leaves no room for the file name: no journal. Basic mods are then not loaded
    // (StartGuardCommit fails), and nothing here may stop the game from starting.
    if (_snwprintf_s(g_journal, _TRUNCATE, L"%s\\ds3_mods_start.ini", gameDir) < 0) {
        g_journal[0] = L'\0';
        Log("safe start: the game folder's path is too long for ds3_mods_start.ini; there is no start journal");
    }
    Lock(gameDir);
    wchar_t state[24], pidText[24], mine[24];
    Get(L"start", L"state", state, 24);
    Get(L"start", L"pid", pidText, 24);
    const DWORD pid = static_cast<DWORD>(wcstoul(pidText, nullptr, 10));
    swprintf_s(mine, L"%lu", GetCurrentProcessId());

    const bool unfinished = _wcsicmp(state, L"loading") == 0;
    if (unfinished && SameGameRunning(pid)) {
        Log("safe start: another Dead Space 3 (process %lu) is still starting and owns ds3_mods_start.ini; this one "
            "leaves it alone", pid);
        Unlock();
        return StartKind::OtherGame;
    }
    g_asked = GetPrivateProfileIntW(L"mods", L"safe_start", 0, modsIni) != 0;
    if (!unfinished && !g_asked) return StartKind::Normal;  // the lock stays until the commit (or StartGuardNoMods)

    if (g_asked) {
        Log("safe start: [mods] safe_start=1 asked for it. No mod is loaded and no patch is written this time");
        // One start only. If the settings file can't be written, the next start asks again: say so.
        WritePrivateProfileStringW(L"mods", L"safe_start", L"0", modsIni);
        if (GetPrivateProfileIntW(L"mods", L"safe_start", 0, modsIni) != 0) {
            g_stuck = true;
            Log("safe start: safe_start could not be set back to 0 (is ds3_mods.ini read-only?). Every start will be a "
                "safe start until it is 0 again: set it by hand");
        }
    } else {
        Log("safe start: the last start did not finish (ds3_mods_start.ini still said \"loading\"). No mod is loaded "
            "and no patch is written this time. If it happens again, switch off the mod you added or updated last");
    }
    // This start counts as finished, so the next one loads the mods again. What the last good start loaded is kept.
    if (unfinished) {
        Put(L"start", L"pid", mine);
        if (!Put(L"start", L"state", L"safe") || !StateIs(L"safe")) {
            g_stuck = true;
            Log("safe start: ds3_mods_start.ini could not be changed (is it, or the game folder, read-only?). Every "
                "start will be a safe start until it can be written: delete that file or make it writable");
        }
    }
    Unlock();
    return StartKind::Safe;
}

bool StartGuardAsked()
{
    return g_asked;
}

bool StartGuardStuck()
{
    return g_stuck;
}

void StartGuardAdd(const wchar_t* key, const char* sha256)
{
    if (g_entryCount >= kMaxEntries) return;
    Entry& e = g_entries[g_entryCount++];
    wcsncpy_s(e.key, key, _TRUNCATE);
    strncpy_s(e.sha, sha256 ? sha256 : "", _TRUNCATE);
}

void StartGuardNoMods()
{
    Unlock();
}

bool StartGuardCommit()
{
    // [this] in one write (key=value strings, each ended by a 0, the list by another), then the process id, and the
    // state last: until "loading" is there the journal still describes the start before.
    static wchar_t block[kMaxEntries * 240 + 2];
    size_t at = 0;
    for (int i = 0; i < g_entryCount; ++i) {
        const int n = _snwprintf_s(block + at, _countof(block) - at - 1, _TRUNCATE, L"%s=%hs", g_entries[i].key,
                                   g_entries[i].sha[0] ? g_entries[i].sha : "-");
        if (n < 0) break;
        at += static_cast<size_t>(n) + 1;
    }
    block[at] = L'\0';
    if (at == 0) block[1] = L'\0';
    wchar_t mine[24];
    swprintf_s(mine, L"%lu", GetCurrentProcessId());
    const bool wrote = g_journal[0] && WritePrivateProfileSectionW(L"this", block, g_journal) &&
                       Put(L"start", L"pid", mine) && Put(L"start", L"state", L"loading");
    const bool saved = wrote && StateIs(L"loading");
    if (saved) {
        g_owner = true;
        g_committedAt = GetTickCount64();
    } else {
        Log("safe start: ds3_mods_start.ini could not be written (is it, or the game folder, read-only?), so a start "
            "that does not finish could not be noticed");
    }
    Unlock();  // a second game that was waiting now reads "loading" with this process's id
    return saved;
}

bool StartGuardKnown(const wchar_t* key, const char* sha256)
{
    wchar_t was[80], now[80];
    Get(L"last_good", key, was, 80);
    swprintf_s(now, L"%hs", sha256 && sha256[0] ? sha256 : "-");
    return was[0] && _wcsicmp(was, now) == 0;
}

void StartGuardFrame()
{
    if (g_done || !g_owner) return;
    ++g_frames;
    const ULONGLONG now = GetTickCount64();
    if (g_frames < kGoodAfterFrames || now - g_committedAt < kGoodAfterMs || now < g_retryAt) return;
    if (!OwnsJournal()) {
        g_done = true;
        Log("safe start: ds3_mods_start.ini now belongs to another game process; left alone");
        return;
    }
    static wchar_t block[kMaxEntries * 240 + 2];
    const DWORD n = GetPrivateProfileSectionW(L"this", block, _countof(block), g_journal);
    if (n == 0) block[0] = block[1] = L'\0';
    WritePrivateProfileSectionW(L"last_good", block, g_journal);
    // Finished only once "ok" is really in the file: until then the next start would be a safe start.
    if (Put(L"start", L"state", L"ok") && StateIs(L"ok")) {
        g_done = true;
        Log("safe start: this start finished (the game drew %d frames over %llu s); noted in ds3_mods_start.ini",
            g_frames, (now - g_committedAt) / 1000);
        return;
    }
    g_retryAt = now + kRetryMs;
    if (++g_tries >= kMaxTries) {
        g_retryAt = ~0ull;  // no more tries from the frame tick; a clean exit still tries to write "closed"
        Log("safe start: this start finished, but that could not be written to ds3_mods_start.ini (%d tries). Unless "
            "the game exits cleanly and the file can be written then, the next start will be a safe start", g_tries);
    }
}

void StartGuardExit()
{
    // Under the loader lock: one write, no waiting, no retries.
    if (g_done || !OwnsJournal()) return;
    g_done = true;
    Put(L"start", L"state", L"closed");  // a clean exit: no safe start next time, and not a good start either
}
