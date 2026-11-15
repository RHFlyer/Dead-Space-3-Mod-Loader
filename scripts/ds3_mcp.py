"""Read-only MCP server for inspecting a running deadspace3.exe.

Tools: ds3_status, ds3_read, ds3_list, ds3_snapshot, ds3_diff, ds3_search, ds3_save (writes a filtered .txt). Usable two ways: in a console
(`py -3 -I ds3_mcp.py`, then type `help`), or from an AI assistant that speaks MCP (it starts the script itself).
Never writes to the process and never calls into it. EA auth tokens live in the environment blocks and the LSX
buffers: the environment blocks are found again before every command and never read, token-looking searches are
refused, and every byte this server returns goes through redact() first, run on a window 1 KB wider than the
request on each side (a token-like run that reaches past that window is blanked whatever it holds). The filter is a
best effort (it blanks what looks like a token or a secret), not a proof: don't paste raw dumps where others see them.
Stdlib only. Run as `py -3 -I ds3_mcp.py`; speaks MCP (JSON-RPC 2.0, one message per line) on stdin/stdout.
"""
import ctypes
import datetime
import ctypes.wintypes as wt
import json
import os
import re
import shlex
import struct
import sys
import time

EXE = "deadspace3.exe"
IMAGE_BASE = 0x400000

# Layout (game version: the current Steam release of Dead Space 3).
PLAYER_MANAGER = 0x011D3B78   # FUN_00436170: *(*(mgr + 4) + i*4) -> slot; slot vtable +0xC returns the player
PLAYER_FLAGS = 0x594          # bit 13 picks the resource block; badges 0x20000 (parts) / 0x80000 (circuits)
RESOURCE_BLOCK, RESOURCE_BLOCK_ALT = 0xE3C, 0xEA4
RESOURCES = 0x48              # 6 uint32 in the block (FUN_006ced10)
RESOURCE_NAMES = ["tungsten", "semiconductor", "scrap", "somatic gel", "transducer", "seals"]
INVENTORY_LIST = 0xF10        # entries ptr, count at +4; 0x28 bytes: GUID, flags, count
DISCOVERED_LIST = 0xFBC       # FUN_005b0b40: entries ptr +4, count +8; 0x9DC bytes: GUID, "new" byte +0x10

MAX_READ = 0x4000
SEARCH_MAX_HITS = 500         # search stops counting here (each hit is checked through the filter)
SEARCH_MAX_CHECKS = 1500      # ... or after checking this many candidates, accepted or not
SEARCH_MAX_SECONDS = 15.0     # ... or after this long
READ_MARGIN = 0x400           # bytes read on each side of a request so the token filter sees all of a token
MAX_SNAPSHOT = 0x1000000
MAX_SNAPSHOTS = 8
CHUNK = 0x400000

PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010
MEM_COMMIT, PAGE_NOACCESS, PAGE_GUARD = 0x1000, 0x01, 0x100

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t),
                ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.GetDriveTypeW.argtypes = [wt.LPCWSTR]
k32.GetDriveTypeW.restype = wt.UINT
k32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.Process32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
ntdll.NtQueryInformationProcess.argtypes = [wt.HANDLE, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                            ctypes.POINTER(ctypes.c_ulong)]


class ToolError(Exception):
    pass


# ---------------------------------------------------------------- redaction

_TOKEN_CHARS = rb"[A-Za-z0-9+/=_\-]"
_TOKENISH = [re.compile(_TOKEN_CHARS + rb"{20,}"), re.compile(rb"(?:" + _TOKEN_CHARS + rb"\x00){20,}")]
_HEXRUN = [re.compile(rb"[0-9A-Fa-f]{32,}"), re.compile(rb"(?:[0-9A-Fa-f]\x00){32,}")]
_KEYWORDS = ["token", "authorization", "authcode", "auth_code", "bearer", "password", "secret", "cookie",
             "sessionkey", "session_key", "x-auth"]
_KEYWORD_RES = [re.compile(re.escape(k.encode()), re.I) for k in _KEYWORDS] + \
               [re.compile(b"".join(re.escape(bytes([c])) + rb"\x00" for c in k.encode()), re.I) for k in _KEYWORDS]
_KEYWORD_SPAN = 512  # bytes after a keyword that are blanked too (the value usually follows it)
_LONG_RUN = 48       # a token-like run this many characters long is blanked even without digits and capitals


def _user_names():
    """USERNAME and the profile folder's own name (they can differ). A user name is often a real name, so it is blanked
    like login data, in the hex and the text."""
    return sorted(n for n in {os.environ.get("USERNAME", ""),
                              os.path.basename(os.environ.get("USERPROFILE", "").rstrip("\\/"))} if n)


def _profile_parent():
    """The name of the folder that holds the profile, usually 'Users' ('' for a profile at a drive root)."""
    return os.path.basename(os.path.dirname(os.environ.get("USERPROFILE", "").rstrip("\\/")))


def _profile_paths():
    """The profile folder's full path, and the home folder's if Windows names another one (it can be on a server)."""
    paths = {os.environ.get("USERPROFILE", ""), os.environ.get("HOMEDRIVE", "") + os.environ.get("HOMEPATH", ""),
             os.environ.get("HOMESHARE", "")}
    return sorted(x.rstrip("\\/") for x in paths if x.strip("\\/"))


def _name_alts(name):
    """The name in its case forms (some change length, like German ss); matched with re.I on top of them."""
    forms = {name, name.lower(), name.upper(), name.casefold()}
    return "|".join(re.escape(f) for f in sorted(forms, key=len, reverse=True))


# What may follow a user name that is a folder in a path: a separator, the end of the string (NUL, line end, a space or a
# quote) or the end of what is read.
_PATH_END = r"(?=[\\/\x00\s\"'<>|;,]|\Z)"
# Folders that only a user profile has: a name right before one of them is the profile, wherever it is (redirected).
_PROFILE_SUBFOLDERS = r"Documents|My Documents|AppData|Desktop|Downloads|Saved Games|OneDrive|Pictures|Videos|" \
                      r"Music|Favorites|Contacts|Links|Searches|NTUSER\.DAT"


def _user_rules():
    r"""Regexes (on text) whose group 'n' is a user name to hide, as (kind, regex); kind is 'path', 'start' or 'word'.
    path: the name as the profile folder: after \Users\ or the profile's own parent (any drive), at the end of the
          profile's full path (a drive root like C:\Jo, a server share), or before a profile-only folder. Any length.
    start: the name at the very start of what is read, before a separator (the rest of the path is out of sight).
    word: the name on its own (3+ letters), Unicode letters and digits counting as part of a word."""
    names = _user_names()
    rules = []
    parents = sorted({"Users", _profile_parent()} - {""}, key=str.casefold)
    for name in names:
        n = "(?P<n>" + _name_alts(name) + ")"
        for parent in parents:
            rules.append(("path", r"(?<=[\\/]" + re.escape(parent) + r"[\\/])" + n + _PATH_END))
        rules.append(("path", r"(?<=[\\/])" + n + r"(?=[\\/](?:" + _PROFILE_SUBFOLDERS + ")" + _PATH_END + ")"))
        rules.append(("start", r"\A" + n + r"(?=[\\/])"))
        if len(name) >= 3:
            rules.append(("word", r"(?<![^\W_])" + n + r"(?![^\W_])"))
    folded = {x.casefold() for x in names}
    for path in _profile_paths():
        parts = [x for x in re.split(r"[\\/]+", path) if x]
        if len(parts) >= 2 and parts[-1].casefold() in folded:
            rules.append(("path", r"[\\/]+".join(re.escape(x) for x in parts[:-1]) + r"[\\/]+(?P<n>" +
                          _name_alts(parts[-1]) + ")" + _PATH_END))
    return [(kind, re.compile(rx, re.I)) for kind, rx in rules]


_USER_RULES = _user_rules()


class _Utf8Offsets:
    """Letter index -> byte offset in UTF-8 text decoded with surrogateescape. Matches come in rising order within a
    regex, so it counts on from the last answer instead of from the start (linear over many matches)."""

    def __init__(self, text):
        self.text, self.i, self.b = text, 0, 0

    def __call__(self, i):
        if i < self.i:
            self.i = self.b = 0
        self.b += len(self.text[self.i:i].encode("utf-8", "surrogateescape"))
        self.i = i
        return self.b


def _text_views(buf):
    """buf as text four ways, each with a function from a letter's index to its byte offset: one byte per letter;
    UTF-8 (bytes that are not valid UTF-8 become one stand-in letter each, so the text maps back exactly); and UTF-16
    code units at even and at odd offsets, taken one at a time."""
    yield buf.decode("latin-1"), lambda i: i
    if not buf.isascii():  # all-ASCII UTF-8 is the same as the one-byte view
        text = bytes(buf).decode("utf-8", "surrogateescape")
        yield text, _Utf8Offsets(text)
    view = memoryview(buf)
    for a in (0, 1):
        n = (len(buf) - a) // 2
        if n > 0:
            yield "".join(map(chr, view[a:a + 2 * n].cast("H"))), lambda i, a=a: a + 2 * i


def _user_name_spans(buf):
    """(start, end) in bytes of each user name to blank, in every view of buf."""
    spans = []
    for text, at in _text_views(buf):
        for _kind, rx in _USER_RULES:
            spans += [(at(m.start("n")), at(m.end("n"))) for m in rx.finditer(text)]
    return spans

# E-mail addresses (an account's e-mail can be in game memory), ASCII and UTF-16. Found from each '@' with a bounded
# look to each side, so the time stays linear even over long runs of letters.
_EMAIL_LOCAL = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._%+-")
_EMAIL_LOCAL_MAX = 64     # characters before the '@' (the longest local part an address may have)
_EMAIL_DOMAIN_MAX = 255   # characters after it
_EMAIL_DOMAIN = [re.compile(rb"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+"),
                 re.compile(rb"(?:[A-Za-z0-9-]\x00)+(?:\.\x00(?:[A-Za-z0-9-]\x00)+)+")]


def _email_spans(buf):
    """(start, end) of each e-mail address in buf, ASCII (width 1) and UTF-16 (width 2)."""
    spans = []
    for w, at, domain in ((1, b"@", _EMAIL_DOMAIN[0]), (2, b"@\x00", _EMAIL_DOMAIN[1])):
        i = buf.find(at)
        while i >= 0:
            start = i
            for _ in range(_EMAIL_LOCAL_MAX):
                j = start - w
                if j < 0 or buf[j] not in _EMAIL_LOCAL or (w == 2 and buf[j + 1] != 0):
                    break
                start = j
            if start < i:
                m = domain.match(buf, i + w, min(len(buf), i + w + _EMAIL_DOMAIN_MAX * w))
                if m:
                    spans.append((start, m.end()))
            i = buf.find(at, i + 1)
    return spans


def _mixed(run):
    text = run.replace(b"\x00", b"")
    return (re.search(rb"[0-9]", text) and re.search(rb"[A-Z]", text) and re.search(rb"[a-z]", text)) is not None


def redact(buf):
    """Blank anything that could be a credential, in place. Returns the number of bytes blanked.
    A token-like run counts if it mixes digits and both cases, or is _LONG_RUN+ characters long, or touches either end of
    buf (then part of it is out of sight and could hold the digits and capitals)."""
    spans = []
    for width, rx in zip((1, 2), _TOKENISH):
        spans += [m.span() for m in rx.finditer(buf)
                  if _mixed(m.group()) or (m.end() - m.start()) // width >= _LONG_RUN
                  or m.start() == 0 or m.end() == len(buf)]
    for rx in _HEXRUN:
        spans += [m.span() for m in rx.finditer(buf)]
    for rx in _KEYWORD_RES:
        spans += [(m.start(), min(len(buf), m.end() + _KEYWORD_SPAN)) for m in rx.finditer(buf)]
    spans += _user_name_spans(buf)
    spans += _email_spans(buf)
    blanked = 0
    for s, e in spans:
        for i in range(s, e):
            if buf[i] != 0x2A:
                buf[i] = 0x2A
                blanked += 1
    return blanked


SEARCH_REFUSED = re.compile(r"(?i)token|auth|bearer|passw|secret|cookie|session|lsx|nucleus|persona|access|"
                            r"refresh|login|credential|email|@")


# ---------------------------------------------------------------- process access

def find_pid():
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    if snap in (None, wt.HANDLE(-1).value):
        return None
    try:
        e = PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            if e.szExeFile.lower() == EXE:
                return e.th32ProcessID
            ok = k32.Process32NextW(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    return None


class Game:
    def __init__(self, pid):
        self.pid = pid
        self.h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.h:
            raise ToolError(f"OpenProcess({pid}) failed, error {ctypes.get_last_error()}")
        self.deny = self._environment_blocks()

    def refresh_deny(self):
        """Find the environment blocks again (the game may have moved one) and keep the old ranges too. Raises if
        they can't be found, so no command reads without the guard."""
        self.deny = sorted(set(self.deny) | set(self._environment_blocks()))

    def close(self):
        k32.CloseHandle(self.h)

    def folder(self):
        """The game's folder (where deadspace3.exe is), or None."""
        buf = ctypes.create_unicode_buffer(1024)
        size = wt.DWORD(len(buf))
        if k32.QueryFullProcessImageNameW(self.h, 0, buf, ctypes.byref(size)):
            return os.path.dirname(buf.value)
        return None

    def _raw(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        if k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)) and got.value == size:
            return buf.raw
        return None

    def _environment_blocks(self):
        """Address ranges of the 32-bit and 64-bit environment blocks (PEB -> ProcessParameters -> Environment).
        Both must be found (the game is a 32-bit program on 64-bit Windows, so it has both); else nothing is read."""
        deny = []
        peb32 = ctypes.c_size_t()
        if ntdll.NtQueryInformationProcess(self.h, 26, ctypes.byref(peb32), ctypes.sizeof(peb32), None) == 0 \
                and peb32.value:
            params = self._raw(peb32.value + 0x10, 4)
            p = struct.unpack("<I", params)[0] if params else 0
            env, size = (self._raw(p + 0x48, 4), self._raw(p + 0x290, 4)) if p else (None, None)
            e = struct.unpack("<I", env)[0] if env and size else 0
            if e:
                deny.append((e, struct.unpack("<I", size)[0] or 0x10000))
        if len(deny) != 1:
            raise ToolError("could not locate the game's 32-bit environment block; refusing to read without that guard")
        pbi = (ctypes.c_size_t * 6)()
        if ntdll.NtQueryInformationProcess(self.h, 0, pbi, ctypes.sizeof(pbi), None) == 0 and pbi[1]:
            params = self._raw(pbi[1] + 0x20, 8)
            p = struct.unpack("<Q", params)[0] if params else 0
            env, size = (self._raw(p + 0x80, 8), self._raw(p + 0x3F0, 8)) if p else (None, None)
            e = struct.unpack("<Q", env)[0] if env and size else 0
            if e:
                deny.append((e, struct.unpack("<Q", size)[0] or 0x10000))
        if len(deny) != 2:
            raise ToolError("could not locate the game's 64-bit environment block; refusing to read without that guard")
        # Whole pages, with a margin: the block may grow when the game sets variables.
        return [(base & ~0xFFF, ((base + size + 0x10000) | 0xFFF) + 1) for base, size in deny]

    def _plain(self, addr, size):
        """Bytes at addr, NOT redacted (only read() and u32() may call this). Unreadable or denied bytes come back
        as 0 and are listed in `holes`."""
        out = bytearray(size)
        holes = []
        pos = 0
        while pos < size:
            n = min(0x1000 - ((addr + pos) & 0xFFF), size - pos)
            a = addr + pos
            if any(lo <= a < hi for lo, hi in self.deny):
                holes.append((pos, pos + n, "environment"))
            else:
                chunk = self._raw(a, n)
                if chunk is None:
                    holes.append((pos, pos + n, "unreadable"))
                else:
                    out[pos:pos + n] = chunk
            pos += n
        return out, holes

    def read(self, addr, size):
        """Bytes at addr, redacted. Unreadable or denied bytes come back as 0 and are listed in `holes`.
        The filter runs on a wider window (READ_MARGIN bytes each side), so a short read, or one that starts or
        ends inside a token, is blanked like a read of the whole token; a run longer than that window is blanked
        whatever it holds."""
        lo = max(0, addr - READ_MARGIN)
        hi = min(0x100000000, addr + size + READ_MARGIN)
        buf, holes = self._plain(lo, hi - lo)
        off = addr - lo
        before = bytes(buf[off:off + size])
        redact(buf)
        out = buf[off:off + size]
        blanked = sum(1 for a, b in zip(before, out) if a != b)
        mine = [(max(s, off) - off, min(e, off + size) - off, why) for s, e, why in holes if s < off + size and e > off]
        return out, blanked, _merge(mine)

    def u32(self, addr):
        """A 4-byte value, through the same guard and filter as read(): refused if any of its bytes is unreadable,
        in an environment block, or blanked by the filter."""
        b, blanked, holes = self.read(addr, 4)
        if holes:
            raise ToolError(f"cannot read 0x{addr:08X} ({holes[0][2]})")
        if blanked:
            raise ToolError(f"0x{addr:08X} is inside data the filter hides")
        return struct.unpack("<I", bytes(b))[0]

    def regions(self):
        addr, m = 0, MBI()
        while addr < 0x100000000 and k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(m),
                                                         ctypes.sizeof(m)):
            size = m.RegionSize
            if m.State == MEM_COMMIT and not m.Protect & PAGE_GUARD and m.Protect != PAGE_NOACCESS:
                yield addr, size, {0x1000000: "image", 0x40000: "mapped"}.get(m.Type, "private")
            addr += size

    def region_of(self, addr):
        m = MBI()
        if not k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(m), ctypes.sizeof(m)) \
                or m.State != MEM_COMMIT:
            return None
        return {0x1000000: "image", 0x40000: "mapped"}.get(m.Type, "heap")


def _merge(holes):
    merged = []
    for s, e, why in holes:
        if merged and merged[-1][1] == s and merged[-1][2] == why:
            merged[-1] = (merged[-1][0], e, why)
        else:
            merged.append((s, e, why))
    return merged


_game = None


def game():
    global _game
    pid = find_pid()
    if pid is None:
        if _game:
            _game.close()
            _game = None
        raise ToolError(f"{EXE} is not running")
    if _game is None or _game.pid != pid:
        if _game:
            _game.close()
            SNAPSHOTS.clear()
        _game = Game(pid)
    else:
        _game.refresh_deny()
    return _game


# ---------------------------------------------------------------- player

def player_count(g):
    """Used slots in the player manager (+0xC; capacity +0x8 is 16). 0 on the main menu, 1 in single player."""
    mgr = g.u32(PLAYER_MANAGER)
    return g.u32(mgr + 0xC) if mgr else 0


def resolve_player(g, index=0):
    """(player address or None, how it was found). Index 0 is what the game's FUN_00436170(0) and the mod use; which
    slot is the local player in co-op is not verified yet."""
    mgr = g.u32(PLAYER_MANAGER)
    if not mgr:
        return None, "player manager is null (main menu or loading?)"
    count = g.u32(mgr + 0xC)
    if index >= count:
        return None, f"player slot {index} is empty ({count} in use)"
    slot = g.u32(g.u32(mgr + 4) + 4 * index)
    if not slot:
        return None, f"player slot {index} is null"
    p, how = _decode_getter(g, slot)
    # FUN_00a961f0(p, 1): bit 1 of the 64-bit type mask at +0x150 (FUN_00436170 returns 0 without it).
    if not (g.u32(p + 0x150) >> 1) & 1:
        how += ", type bit missing (the game would not treat this as a player)"
    return p, how


def _decode_getter(g, slot):
    fn = g.u32(g.u32(slot) + 0xC)
    code = bytes(g.read(fn, 16)[0])
    # Simple getters only; anything else is reported so the decoder can be extended.
    if code[:2] == b"\x8B\x41" and code[3:4] == b"\xC3":
        return g.u32(slot + code[2]), f"slot 0x{slot:08X}, getter mov eax,[ecx+0x{code[2]:X}]"
    if code[:2] == b"\x8B\x81" and code[6:7] == b"\xC3":
        off = struct.unpack("<I", code[2:6])[0]
        return g.u32(slot + off), f"slot 0x{slot:08X}, getter mov eax,[ecx+0x{off:X}]"
    if code[:2] == b"\x8D\x41" and code[3:4] == b"\xC3":
        return slot + code[2], f"slot 0x{slot:08X}, getter lea eax,[ecx+0x{code[2]:X}]"
    if code[:2] == b"\x8D\x81" and code[6:7] == b"\xC3":  # seen live: lea eax,[ecx-0x3A0] (slot is a base inside the player)
        off = struct.unpack("<i", code[2:6])[0]
        return (slot + off) & 0xFFFFFFFF, f"slot 0x{slot:08X}, getter lea eax,[ecx{off:+#x}]"
    if code[:3] == b"\x8B\xC1\xC3":
        return slot, f"slot 0x{slot:08X}, getter returns the slot"
    raise ToolError(f"unknown player getter at 0x{fn:08X} (slot 0x{slot:08X}); this game build may differ")


def need_player(g, index=0):
    p, how = resolve_player(g, index)
    if not p:
        raise ToolError(how)
    return p


def resource_block(g, p):
    alt = (g.u32(p + PLAYER_FLAGS) >> 13) & 1
    return p + (RESOURCE_BLOCK_ALT if alt else RESOURCE_BLOCK)


# ---------------------------------------------------------------- address expressions

_EXPR_TOKEN = re.compile(r"\s*(0x[0-9A-Fa-f]+|\d+|[A-Za-z_]\w*|[\[\]+\-])")


def evaluate(g, text):
    """'0x011D3B78', 'player+0xFBC', '[[0x011D3B78]+4]' ([x] reads a uint32 at x). Numbers need 0x for hex."""
    tokens, pos = [], 0
    while pos < len(text.rstrip()):
        m = _EXPR_TOKEN.match(text, pos)
        if not m:
            raise ToolError(f"cannot parse address at: {text[pos:]!r}")
        tokens.append(m.group(1))
        pos = m.end()
    # Player names take an optional slot index: player = player0, resources1 = the second player's resources.
    names = {"image": lambda i: IMAGE_BASE, "player": lambda i: need_player(g, i),
             "resources": lambda i: resource_block(g, need_player(g, i)) + RESOURCES,
             "inventory": lambda i: need_player(g, i) + INVENTORY_LIST,
             "discovered": lambda i: need_player(g, i) + DISCOVERED_LIST}

    def term(i):
        t = tokens[i] if i < len(tokens) else None
        if t is None:
            raise ToolError("address ends early")
        if t == "[":
            v, i = expr(i + 1)
            if i >= len(tokens) or tokens[i] != "]":
                raise ToolError("missing ]")
            return g.u32(v), i + 1
        if t.lower().startswith("0x"):
            return int(t, 16), i + 1
        if t.isdigit():
            return int(t), i + 1
        m = re.fullmatch(r"([a-z]+?)(\d?)", t.lower())
        if m and m.group(1) in names and not (m.group(1) == "image" and m.group(2)):
            return names[m.group(1)](int(m.group(2) or 0)), i + 1
        raise ToolError(f"unknown name {t!r}; known: {', '.join(names)} (player names take a slot, e.g. player1)")

    def expr(i):
        v, i = term(i)
        while i < len(tokens) and tokens[i] in "+-":
            op = tokens[i]
            w, i = term(i + 1)
            v = v + w if op == "+" else v - w
        return v & 0xFFFFFFFF, i

    v, i = expr(0)
    if i != len(tokens):
        raise ToolError(f"unexpected {tokens[i]!r}")
    return v


# ---------------------------------------------------------------- formatting

def fmt_guid(b):
    return "-".join(f"{x:08X}" for x in struct.unpack("<4I", b[:16]))


_NAMES = None


def guid_names():
    """GUID -> 'Tier: stats' from CIRCUITS.md (circuits only; parts are not catalogued yet). It is looked for in the
    repo's sdk folder (this file in scripts/) and next to the sdk (this file in the download's sdk/tools/)."""
    global _NAMES
    if _NAMES is None:
        _NAMES = {}
        here = os.path.dirname(os.path.abspath(__file__))
        for path in (os.path.join(here, "..", "sdk", "CIRCUITS.md"), os.path.join(here, "..", "CIRCUITS.md"),
                     os.path.join(here, "CIRCUITS.md")):
            try:
                with open(path, encoding="utf-8") as f:
                    for line in f:
                        m = re.match(r"\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|\s*`([0-9A-F-]{35})`", line)
                        if m:
                            _NAMES[m.group(3)] = f"{m.group(1)}: {m.group(2)}"
                break
            except OSError:
                continue
    return _NAMES


def fmt_float(v):
    f = struct.unpack("<f", struct.pack("<I", v))[0]
    if v and 1e-4 <= abs(f) <= 1e7:
        return f"{f:g}"
    return ""


def hexdump(addr, data, holes):
    bad = set()
    for s, e, _ in holes:
        bad.update(range(s, e))
    lines = []
    for off in range(0, len(data), 16):
        row = range(off, min(off + 16, len(data)))
        hx = " ".join("??" if i in bad else f"{data[i]:02X}" for i in row)
        asc = "".join(" " if i in bad else (chr(data[i]) if 0x20 <= data[i] < 0x7F else ".") for i in row)
        lines.append(f"{addr + off:08X}  {hx:<47}  {asc}")
    return lines


def dwords(g, addr, data, holes):
    bad = set()
    for s, e, _ in holes:
        bad.update(range(s, e))
    lines = []
    for off in range(0, len(data) - 3, 4):
        if off in bad:
            lines.append(f"+{off:04X}  ????????")
            continue
        v = struct.unpack_from("<I", data, off)[0]
        note = []
        if v >= 0x10000:
            kind = g.region_of(v)
            if kind:
                note.append(f"-> {kind}")
        f = fmt_float(v)
        if f and not note:
            note.append(f"f={f}")
        signed = struct.unpack_from("<i", data, off)[0]
        lines.append(f"+{off:04X}  {v:08X}  {signed:>11}  {' '.join(note)}".rstrip())
    return lines


def footer(blanked, holes):
    notes = []
    if blanked:
        notes.append(f"{blanked} byte(s) blanked by the filter (login data, e-mail addresses or your user name, "
                     "shown as '*')")
    for s, e, why in holes:
        notes.append(f"+{s:X}..+{e:X} {why}")
    return notes


# ---------------------------------------------------------------- tools

def t_status(args):
    g = game()
    out = [f"{EXE} pid {g.pid}"]
    try:
        count = player_count(g)
    except ToolError as e:
        count = 0
        out.append(f"player manager: {e}")
    out.append(f"players: {count}" + ("  (slot 0 is the one the mod treats as local)" if count > 1 else ""))
    for index in range(max(count, 1)):
        try:
            p, how = resolve_player(g, index)
        except ToolError as e:
            p, how = None, str(e)
        if not p:
            out.append(f"player{index}: none ({how})")
            continue
        out += [f"player{index}: 0x{p:08X} ({how})"] + ["  " + line for line in player_summary(g, p)]
    return "\n".join(out)


def player_summary(g, p):
    out = []
    flags = g.u32(p + PLAYER_FLAGS)
    block = resource_block(g, p)
    res = [g.u32(block + RESOURCES + 4 * i) for i in range(6)]
    out.append(f"flags +0x594: 0x{flags:08X}  (block {'alt +0xEA4' if flags >> 13 & 1 else '+0xE3C'}"
               f"{', new-parts badge' if flags & 0x20000 else ''}{', new-circuits badge' if flags & 0x80000 else ''})")
    out.append(f"resources @0x{block + RESOURCES:08X}: "
               + ", ".join(f"[{i}] {n} {v}" for i, (n, v) in enumerate(zip(RESOURCE_NAMES, res))))
    for name, count_at in (("inventory", INVENTORY_LIST + 4), ("discovered", DISCOVERED_LIST + 8)):
        try:
            out.append(f"{name}: {g.u32(p + count_at)} entries")
        except ToolError as e:
            out.append(f"{name}: {e}")
    return out


def t_read(args):
    g = game()
    addr = evaluate(g, str(args.get("address", "")))
    size = int(args.get("size", 0x100))
    if not 0 < size <= MAX_READ:
        raise ToolError(f"size must be 1..0x{MAX_READ:X}")
    data, blanked, holes = g.read(addr, size)
    fmt = args.get("format", "hex")
    head = [f"0x{addr:08X} ({g.region_of(addr) or 'not committed'}), 0x{size:X} bytes"]
    body = dwords(g, addr, data, holes) if fmt == "dwords" else hexdump(addr, data, holes)
    return "\n".join(head + body + footer(blanked, holes))


def t_list(args):
    g = game()
    p = need_player(g, int(args.get("player", 0)))
    which = args.get("list", "discovered")
    limit = int(args.get("limit", 200))
    only_new = bool(args.get("only_new", False))
    names = guid_names()
    if which == "discovered":
        base, count_at, stride = g.u32(p + DISCOVERED_LIST + 4), p + DISCOVERED_LIST + 8, 0x9DC
    elif which == "inventory":
        base, count_at, stride = g.u32(p + INVENTORY_LIST), p + INVENTORY_LIST + 4, 0x28
    else:
        raise ToolError("list must be 'discovered' or 'inventory'")
    count = g.u32(count_at)
    if count > 100000:
        raise ToolError(f"implausible count {count} at 0x{count_at:08X}")
    out = [f"{which}: {count} entries at 0x{base:08X}"]
    shown = 0
    for i in range(count):
        rec, _, holes = g.read(base + i * stride, 0x28 if which == "inventory" else 0x14)
        if holes:
            out.append(f"#{i}: unreadable")
            continue
        guid = fmt_guid(rec)
        if which == "discovered":
            new = rec[0x10]
            if only_new and not new:
                continue
            extra = "  NEW" if new else ""
        else:
            extra = "  " + " ".join(f"{v:08X}" for v in struct.unpack_from("<6I", rec, 0x10))
        out.append(f"#{i} {guid}{extra}  {names.get(guid, '')}".rstrip())
        shown += 1
        if shown >= limit:
            out.append(f"... stopped at limit {limit}")
            break
    return "\n".join(out)


SNAPSHOTS = {}


def t_snapshot(args):
    g = game()
    name = str(args.get("name", "a"))
    addr = evaluate(g, str(args.get("address", "player")))
    size = int(args.get("size", 0x2000))
    if not 0 < size <= MAX_SNAPSHOT:
        raise ToolError(f"size must be 1..0x{MAX_SNAPSHOT:X}")
    data, blanked, holes = g.read(addr, size)
    SNAPSHOTS[name] = (addr, bytes(data), holes)
    while len(SNAPSHOTS) > MAX_SNAPSHOTS:
        SNAPSHOTS.pop(next(iter(SNAPSHOTS)))
    return f"snapshot {shown(name)}: 0x{addr:08X}, 0x{size:X} bytes" + "".join("\n" + n for n in footer(blanked, holes))


def t_diff(args):
    g = game()
    before = str(args.get("before", "a"))
    if before not in SNAPSHOTS:
        raise ToolError(f"no snapshot {shown(before)}; have: {', '.join(map(shown, SNAPSHOTS)) or 'none'}")
    addr, old, old_holes = SNAPSHOTS[before]
    after = args.get("after")
    if after:
        if after not in SNAPSHOTS:
            raise ToolError(f"no snapshot {shown(after)}")
        addr2, new, new_holes = SNAPSHOTS[after]
        if addr2 != addr or len(new) != len(old):
            raise ToolError("snapshots cover different ranges")
        label = shown(after)
    else:
        data, _, new_holes = g.read(addr, len(old))
        new, label = bytes(data), "live"
    limit = int(args.get("limit", 120))
    skip_floats = bool(args.get("skip_floats", False))
    bad = set()
    for s, e, _ in old_holes + new_holes:
        bad.update(range(s, e))
    changes = []
    for off in range(0, len(old) - 3, 4):
        if off in bad or old[off:off + 4] == new[off:off + 4]:
            continue
        a, b = struct.unpack_from("<I", old, off)[0], struct.unpack_from("<I", new, off)[0]
        fa, fb = fmt_float(a), fmt_float(b)
        if skip_floats and fa and fb:
            continue
        sa, sb = struct.unpack_from("<i", old, off)[0], struct.unpack_from("<i", new, off)[0]
        line = f"+{off:05X} @{addr + off:08X}  {a:08X} -> {b:08X}  ({sa} -> {sb}, {sb - sa:+})"
        if fa and fb:
            line += f"  f {fa} -> {fb}"
        changes.append(line)
    out = [f"{shown(before)} -> {label}: {len(changes)} changed dword(s) in 0x{len(old):X} bytes from 0x{addr:08X}"]
    out += changes[:limit]
    if len(changes) > limit:
        out.append(f"... {len(changes) - limit} more (raise limit or narrow the range)")
    return "\n".join(out)


def t_search(args):
    g = game()
    kind = args.get("kind", "text")
    value = str(args.get("value", ""))
    limit = int(args.get("limit", 20))
    images_only = bool(args.get("image_only", False))
    if kind == "text":
        if len(value) < 3:
            raise ToolError("text must be at least 3 characters")
        if SEARCH_REFUSED.search(value):
            raise ToolError("refused: that text could lead to account or auth data")
        needles = [(value.encode("latin-1", "replace"), "a"), (value.encode("utf-16-le"), "u")]
    elif kind == "hex":
        needles = [(bytes.fromhex(value.replace(" ", "")), "x")]
    elif kind == "u32":
        needles = [(struct.pack("<I", int(value, 0) & 0xFFFFFFFF), "d")]
    else:
        raise ToolError("kind must be text, hex or u32")
    if not needles[0][0]:
        raise ToolError("empty value")
    longest = max(len(n) for n, _ in needles)
    hits, total, stopped, checked, why = [], 0, False, 0, ""
    t0 = time.monotonic()
    # Raw bytes are only searched here, never shown. Each hit is then checked through read() (the filter with its
    # margin) and counted only if the filter leaves it unchanged, so the count can't test guesses at a token.
    # Checking only the hits keeps a search of the whole game to seconds; it stops after SEARCH_MAX_HITS.
    for base, size, kind_name in g.regions():
        if stopped:
            break
        if images_only and kind_name != "image":
            continue
        if any(lo < base + size and base < hi for lo, hi in g.deny):
            continue
        for start in range(0, size, CHUNK):
            if not stopped and time.monotonic() - t0 > SEARCH_MAX_SECONDS:
                why, stopped = f"stopped after {SEARCH_MAX_SECONDS:.0f} seconds", True
            if stopped:
                break
            n = min(CHUNK + longest - 1, size - start)
            data = g._raw(base + start, n)
            if data is None:
                continue
            for needle, tag in needles:
                i = data.find(needle)
                while i >= 0 and i < CHUNK:
                    addr = base + start + i
                    checked += 1
                    if bytes(g.read(addr, len(needle))[0]) == needle:
                        total += 1
                        if len(hits) < limit:
                            hits.append((addr, tag, kind_name))
                    if total >= SEARCH_MAX_HITS:
                        why = f"stopped after {SEARCH_MAX_HITS} hits"
                    elif checked >= SEARCH_MAX_CHECKS:
                        why = f"stopped after checking {SEARCH_MAX_CHECKS} places"
                    elif time.monotonic() - t0 > SEARCH_MAX_SECONDS:
                        why = f"stopped after {SEARCH_MAX_SECONDS:.0f} seconds"
                    if why:
                        stopped = True
                        break
                    i = data.find(needle, i + 1)
                if stopped:
                    break
    count = f"{total}+" if stopped else f"{total}"
    out = [f"{count} hit(s) for {kind} {shown(value)}" + (" (image only)" if images_only else "")]
    # A search for text with the user name in it shows that text in every hit: there the name is hidden wherever it is.
    names = [n for n in _user_names() if kind == "text" and re.search(_name_alts(n), value, re.I)]
    for a, tag, kind_name in hits:
        ctx, _, _ = g.read(max(0, a - 16), 80)
        step = 2 if tag == "u" else 1
        preview = "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in ctx[::step] if step == 1 or c)
        for n in names:
            preview = re.sub(_name_alts(n), "<user>", preview, flags=re.I)
        out.append(f"0x{a:08X} [{tag} {kind_name}] {preview}")
    if total > len(hits):
        out.append(f"... {total - len(hits)} more")
    if stopped:
        out.append(f"{why}, so the count is incomplete: search for a rarer value, or add  image_only=true")
    return "\n".join(out)


# ---------------------------------------------------------------- saving a safe-to-share file

SAVE_COMMANDS = ("status", "read", "list", "diff", "search")
SAVE_HEADER = "Dead Space 3 memory inspector output (read-only)"  # overwrite=true replaces only a file whose first
#                                                                   line is this
_RESERVED = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"} | \
            {f"{d}{i}" for d in ("COM", "LPT") for i in list("0123456789") + ["\u00b9", "\u00b2", "\u00b3"]}


def _folder_re(path):
    """A path as a whole folder: any case, / or \\ between parts, not followed by more of a name."""
    parts = [re.escape(x) for x in re.split(r"[\\/]+", path.rstrip("\\/")) if x]
    return re.compile(r"[\\/]+".join(parts) + r"(?![\w.~-])", re.I) if len(parts) > 1 else None


def _merged(spans, reverse=False):
    """Overlapping (start, end) spans joined into one."""
    out = []
    for s, e in sorted(spans):
        if out and s < out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], e))
        else:
            out.append((s, e))
    return out[::-1] if reverse else out


def hide_personal(text, game_dir=None):
    """The game folder as <game>, the user's profile folder as %USERPROFILE%, the Windows user name on its own as
    <user> (like the loader's log), and e-mail addresses as <e-mail>. The game folder goes first: it may be inside
    the profile."""
    for path, tag in ((game_dir, "<game>"), (os.environ.get("USERPROFILE"), "%USERPROFILE%")):
        rx = _folder_re(path) if path else None
        if rx:
            text = rx.sub(lambda _m, t=tag: t, text)
    spans = [m.span("n") for kind, rx in _USER_RULES if kind in ("path", "start") for m in rx.finditer(text)]
    for name in _user_names():  # on its own: not part of a longer word, a file name or a short name like NAME~1
        if len(name) >= 3:
            spans += [m.span() for m in re.finditer(r"(?<![\w.~-])(?:" + _name_alts(name) + r")(?![\w~-])", text, re.I)]
    for s, e in _merged(spans, reverse=True):
        text = text[:s] + "<user>" + text[e:]
    # E-mail addresses: the same linear search as the byte filter (one byte per character, so the spans line up).
    for s, e in sorted(_email_spans(text.encode("latin-1", "replace")), reverse=True):
        text = text[:s] + "<e-mail>" + text[e:]
    return text


def hide_typed(text, game_dir=None):
    """Text the user typed (a search value, a snapshot name, a command line), or a message that repeats it: like
    hide_personal, and the user name as a whole word at any length, since a short name typed on its own is still the
    name."""
    text = hide_personal(text, game_dir)
    for name in _user_names():
        text = re.sub(r"(?<![^\W_])(?:" + _name_alts(name) + r")(?![^\W_])", "<user>", text, flags=re.I)
    return text


def shown(value):
    """A typed value as it is printed back: quoted, with the user name hidden."""
    return repr(hide_typed(str(value)))


def _game_dir():
    """The game folder for hiding it in messages, or None if the game is not running."""
    try:
        return game().folder()
    except Exception:
        return None


def save_path(name):
    """Where save writes: a bare file name goes next to this script; a path is used as given. Always a .txt file on
    this computer, in a folder that exists."""
    name = name.strip().strip('"')
    if not name:
        raise ToolError("give a file name, e.g.  save notes.txt read player 0x40")
    if name.startswith(("\\\\", "//")):
        raise ToolError("save writes only to this computer, not to a network path")
    if ":" in name[2:] or (":" in name[:2] and not (len(name) > 1 and name[1] == ":" and name[0].isalpha())):
        raise ToolError("a file name can't have ':' in it (only after a drive letter, as in C:\\)")
    if not name.lower().endswith(".txt"):
        name += ".txt"
    if os.path.basename(name) == name:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    else:
        path = os.path.abspath(name)
    if path.startswith("\\\\"):
        raise ToolError("save writes only to this computer, not to a network path")
    stem = os.path.basename(path).split(".")[0].rstrip(" .").upper()
    if stem in _RESERVED:
        raise ToolError(f"{shown(os.path.basename(path))} is a name Windows keeps for devices; pick another")
    if not os.path.isdir(os.path.dirname(path)):
        raise ToolError("that folder does not exist")
    root = os.path.splitdrive(path)[0] + "\\"
    if _drive_type(root) == DRIVE_REMOTE or os.path.realpath(os.path.dirname(path)).startswith("\\\\"):
        raise ToolError("save writes only to this computer, not to a network drive")
    return path


DRIVE_REMOTE = 4


def _drive_type(root):
    return k32.GetDriveTypeW(root)


def t_save(args):
    """Runs one read-only command and writes its output, already filtered, to a text file to share."""
    path = save_path(str(args.get("file", "")))
    words = args.get("command", "")
    words = [str(w) for w in words] if isinstance(words, list) else shlex.split(str(words))
    if not words or words[0].lower().removeprefix("ds3_") not in SAVE_COMMANDS:
        raise ToolError("save takes one of: " + ", ".join(SAVE_COMMANDS) + ", e.g.  save notes.txt read player 0x40")
    g = game()
    text = call_tool(words)  # the same filtered output the command prints; raises if the command fails
    game_dir = g.folder()
    when = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    body = "\n".join([
        SAVE_HEADER,
        f"Command: {hide_typed(' '.join(words), game_dir)}",
        f"Saved: {when}",
        "Login data, e-mail addresses and your user name are blanked as a best effort ('*'); folder paths show",
        "as <game> and %USERPROFILE%. Read this file before you post it.",
        "",
        text,
        "",
    ])
    body = hide_personal(body, game_dir)
    if args.get("overwrite") and os.path.exists(path):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                first = f.readline().strip()
        except OSError as e:
            raise ToolError(f"could not read the file to replace: {e.strerror}")
        if first != SAVE_HEADER:
            raise ToolError(f"{hide_typed(path, game_dir)} does not start with the save header, so it is not "
                            "replaced; pick another name")
    try:
        with open(path, "w" if args.get("overwrite") else "x", encoding="utf-8", newline="\r\n") as f:
            f.write(body)
    except FileExistsError:
        raise ToolError(f"{hide_typed(path, game_dir)} already exists; add  overwrite=true  to replace it (only a "
                        "file whose first line is the save header is replaced)")
    except OSError as e:
        raise ToolError(f"could not write the file: {e.strerror}")
    return f"saved to {hide_typed(path, game_dir)} ({body.count(chr(10))} lines). Share this file, not a screenshot."


TOOLS = {
    "ds3_status": (t_status, "Is Dead Space 3 running; every player slot (co-op: 2) with address, flags, the 6 "
                             "resources (incl. Ration Seals) and list sizes. Start here.", {}),
    "ds3_read": (t_read, "Read up to 0x4000 bytes of game memory (read-only, credential-filtered). Address is an "
                         "expression: hex needs 0x; names player, resources, inventory, discovered (add a slot for "
                         "co-op: player1, resources1), image; [x] dereferences a uint32. e.g. 'player+0x594'.",
                 {"address": {"type": "string"}, "size": {"type": "integer", "description": "bytes, default 0x100"},
                  "format": {"type": "string", "enum": ["hex", "dwords"],
                             "description": "dwords: one line per uint32 with int, float and pointer hints"}}),
    "ds3_list": (t_list, "List the player's discovered list (circuits/parts the save knows; NEW flag) or "
                         "inventory entries, with circuit names from sdk/CIRCUITS.md.",
                 {"list": {"type": "string", "enum": ["discovered", "inventory"]},
                  "player": {"type": "integer", "description": "slot, default 0"},
                  "only_new": {"type": "boolean"}, "limit": {"type": "integer"}}),
    "ds3_snapshot": (t_snapshot, "Save a named copy of a memory range (default: 0x2000 bytes at player) to diff "
                                 "after the user does something in game. Kept in memory only.",
                     {"name": {"type": "string"}, "address": {"type": "string"},
                      "size": {"type": "integer", "description": "bytes, max 16 MB"}}),
    "ds3_diff": (t_diff, "Changed uint32s between a snapshot and live memory (or a second snapshot).",
                 {"before": {"type": "string"}, "after": {"type": "string"}, "limit": {"type": "integer"},
                  "skip_floats": {"type": "boolean",
                                  "description": "hide float-to-float changes (position, camera noise)"}}),
    "ds3_search": (t_search, "Find text (ASCII and UTF-16), hex bytes or a uint32 value in the game's memory. "
                             "Auth-related text is refused.",
                   {"kind": {"type": "string", "enum": ["text", "hex", "u32"]}, "value": {"type": "string"},
                    "limit": {"type": "integer"}, "image_only": {"type": "boolean"}}),
    "ds3_save": (t_save, "Run one of status, read, list, diff or search and write its output, already filtered, to "
                         "a .txt file the user can share (a bare name goes next to this script). Folder paths and "
                         "the Windows user name are hidden. overwrite=true replaces only a file whose first line is the "
                         "save header.",
                 {"file": {"type": "string"}, "command": {"type": "string",
                                                          "description": "e.g. 'read player 0x40 dwords'"},
                  "overwrite": {"type": "boolean"}}),
}


# ---------------------------------------------------------------- MCP over stdio

def tool_list():
    return [{"name": n, "description": d,
             "inputSchema": {"type": "object", "properties": props, "additionalProperties": False}}
            for n, (_, d, props) in TOOLS.items()]


def handle(msg):
    method, params = msg.get("method"), msg.get("params") or {}
    if method == "initialize":
        return {"protocolVersion": params.get("protocolVersion", "2025-06-18"),
                "capabilities": {"tools": {}}, "serverInfo": {"name": "ds3", "version": "0.1.0"}}
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": tool_list()}
    if method == "tools/call":
        name = params.get("name")
        if name not in TOOLS:
            raise LookupError(f"unknown tool {shown(name)}")
        try:
            text, err = hide_personal(TOOLS[name][0](params.get("arguments") or {}), _game_dir()), False
        except ToolError as e:
            text, err = hide_typed(str(e), _game_dir()), True
        except (ValueError, OverflowError) as e:
            text, err = hide_typed(f"bad argument: {e}", _game_dir()), True
        return {"content": [{"type": "text", "text": text}], "isError": err}
    raise LookupError(f"method not found: {shown(method)}")


# ---------------------------------------------------------------- plain console use (no AI assistant needed)

CONSOLE_HELP = """Commands (the game must be running; nothing is ever written to it):
  status                                 is the game running; player address, resources, list sizes
  read <address> [size] [format]         e.g.  read player+0x594 0x40 dwords      (format: hex or dwords)
  list [discovered|inventory]            the player's lists; add  only_new=true  to see NEW items only
  snapshot <name> [address] [size]       remember a memory range now ...
  diff <before> [after]                  ... and show what changed since (do something in game in between);
                                         add  skip_floats=true  to hide decimals (camera, position) that change
  search <text|hex|u32> <value>          e.g.  search u32 1500     or     search text "Plasma Cutter"
  save <file> <command>                  write a command's output to a .txt file to share, e.g.
                                         save tungsten.txt diff before skip_floats=true
                                         (next to this script; overwrite=true  after the name replaces an earlier saved file)
  help, quit
To ask for help, share a saved file, not a screenshot or a pasted dump: login data is blanked (best effort), and
folder paths and your Windows user name are hidden.
Any option can also be given as name=value, e.g.  read player size=0x20.
Addresses are expressions: player, resources, inventory, discovered, image, hex like 0x011D3B78,
+ and -, and [x] to follow a pointer, e.g.  [player+0xF10]."""


def _convert(value, schema):
    kind = schema.get("type")
    if kind == "integer":
        return int(value, 0)
    if kind == "boolean":
        if value.lower() in ("1", "true", "yes", "on"):
            return True
        if value.lower() in ("0", "false", "no", "off"):
            return False
        raise ValueError(f"{value!r} is not true or false")
    return value


def call_tool(words):
    """One command as words, e.g. ['read', 'player+0x594', '0x40']: its output, or ToolError / ValueError."""
    name = words[0].lower()
    tool = name if name.startswith("ds3_") else "ds3_" + name
    if tool not in TOOLS:
        raise ToolError(f"unknown command {words[0]!r}; type  help")
    fn, _, props = TOOLS[tool]
    if tool == "ds3_save":  # save <file> [overwrite=true] <command ...>
        rest = list(words[1:])
        args = {"file": rest.pop(0) if rest else ""}
        if rest and rest[0].lower() in ("overwrite=true", "overwrite=1", "overwrite=yes"):
            args["overwrite"] = True
            rest.pop(0)
        args["command"] = rest
        return fn(args)
    order, args = list(props), {}
    for word in words[1:]:
        key, eq, value = word.partition("=")
        if eq and key in props:
            args[key] = _convert(value, props[key])
            continue
        key = next((k for k in order if k not in args), None)
        if key is None:
            raise ToolError(f"too many values for {name}; type  help")
        args[key] = _convert(word, props[key])
    return fn(args)


def run_command(words):
    """One console command, e.g. ['read', 'player+0x594', '0x40']. Returns the text to print."""
    if not words:
        return ""
    if words[0].lower() in ("help", "?"):
        return CONSOLE_HELP
    try:
        return hide_personal(call_tool(words), _game_dir())
    except ToolError as e:
        return hide_typed(f"error: {e}", _game_dir())
    except (ValueError, OverflowError) as e:
        return hide_typed(f"bad value: {e}", _game_dir())


def console():
    print("Dead Space 3 memory inspector (read-only). Type  help  for commands,  quit  to leave.")
    while True:
        try:
            line = input("ds3> ")
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if line.strip().lower() in ("quit", "exit", "q"):
            return
        try:
            words = shlex.split(line)  # so  search text "two words"  is one value
        except ValueError:
            words = line.split()
        print(run_command(words))


def main():
    # py -3 -I ds3_mcp.py status          one command, then exit
    # py -3 -I ds3_mcp.py                 in a console window: type commands (snapshots last until you quit)
    # started by an AI assistant (MCP)    stdin is not a console, so it speaks MCP as before
    if len(sys.argv) > 1 and sys.argv[1] != "--mcp":
        print(run_command(sys.argv[1:]))
        return
    if len(sys.argv) == 1 and sys.stdin.isatty():
        console()
        return
    for line in sys.stdin:
        if not line.strip():
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "id" not in msg:
            continue  # notification
        try:
            reply = {"jsonrpc": "2.0", "id": msg["id"], "result": handle(msg)}
        except LookupError as e:
            reply = {"jsonrpc": "2.0", "id": msg["id"], "error": {"code": -32601, "message": str(e)}}
        except Exception as e:  # keep the server alive whatever a tool does
            reply = {"jsonrpc": "2.0", "id": msg["id"], "error": {"code": -32603, "message": hide_typed(repr(e))}}
        sys.stdout.write(json.dumps(reply) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
