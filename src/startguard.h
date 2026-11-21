#pragma once

// Safe start. Before any mod is loaded, the loader writes a small journal (ds3_mods_start.ini next to the DLL) that
// says "loading" and what is being loaded. Once the game has kept drawing frames for a while it says "ok". A start
// that finds "loading" left by a game that is gone knows the last start did not finish, and loads no mods at all, so
// the player can reach the Mods panel and switch off whatever they added last. Nobody's switch is changed, and nothing
// is blamed: a crash, a power cut and a player who killed the game look the same.
//
// Limits: a crash after the start counted as finished (300 frames over 20 s) is not noticed; for that there is
// [mods] safe_start=1. Without a journal that can be written, basic mods are not loaded at all, and mods with a
// mod.ini load unguarded as they did before 1.1.0.

enum class StartKind {
    Normal,     // load mods as usual (after StartGuardCommit, or StartGuardNoMods when there is nothing to load)
    Safe,       // the last start did not finish, or [mods] safe_start=1 asked for it: load nothing this time
    OtherGame,  // another game process is still starting and owns the journal: leave it alone
};

// Decides how this start goes. modsIni: ds3_mods.ini, for [mods] safe_start (set back to 0 once used). On Normal it
// keeps a lock other game processes wait on, until StartGuardCommit or StartGuardNoMods.
StartKind StartGuardBegin(const wchar_t* gameDir, const wchar_t* modsIni);
// Why a Safe start is one: true = the player asked with safe_start=1.
bool StartGuardAsked();
// A Safe start that could not be written down (a read-only file): the next start will be a safe start too.
bool StartGuardStuck();

// A Normal start: name everything about to be loaded (key such as "basic:Foo.asi" or "mod:hello_mod", and the
// file's SHA-256 or ""), then commit. Commit writes the journal and reads it back; false = it could not be saved.
void StartGuardAdd(const wchar_t* key, const char* sha256);
bool StartGuardCommit();
// A Normal start with nothing to load: nothing is written.
void StartGuardNoMods();
// Whether this key had this SHA-256 at the last start that finished (false: new or changed since).
bool StartGuardKnown(const wchar_t* key, const char* sha256);

// Once per drawn frame: after enough frames over enough time, this start counts as finished.
void StartGuardFrame();
// At a clean exit before that.
void StartGuardExit();
