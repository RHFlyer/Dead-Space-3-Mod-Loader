<#
.SYNOPSIS
  Builds the release zips for players and modders, locally. It never uploads or publishes anything.

.DESCRIPTION
  Builds the proxy and the sample mods (Win32 Release), then stages and zips, in release\:

  <Name>-v<Version>.zip, the main download, laid out so that unzipping all of it into the game folder is tidy:
    dinput8.dll, ds3_mods.ini                        the DS3 Mod Loader and its default settings
    plugins\README.txt                               where add-ons go (a text file keeps the folder in every unzip
                                                     tool; the loader ignores loose files in plugins\)
    DS3 Mod Loader docs\                            README.txt and CHANGELOG.txt for players; LICENSE.txt (MIT) and
                                                     THIRD-PARTY-NOTICES.txt (MinHook, BSD 2-clause, compiled into
                                                     dinput8.dll, and the Raw Input Mouse Fix add-on's origin, MIT);
                                                     SHA256SUMS.txt, a checksum of every other file in the zip, by its path
                                                     from the game folder

  <Name>-addons-v<Version>.zip, every optional mod in one download: one folder, DS3 Mod Loader add-ons\, with
  READ ME FIRST.txt (what each mod does, who made it, how to install, update and remove one), SHA256SUMS.txt, and
  one folder per mod (SealStore, AutoPickup, RawInputMouseFix, ResourceCounter, MouseSliderRange, HelloMod) with
  mod.ini, the built DLL, README.txt, CHANGELOG.txt and LICENSE.txt (the mouse fix also
  LICENSE-RawInputMouseFix.txt, slopkhayzo's MIT licence). Nothing installs by itself: the player copies the mods
  they want into plugins\. No mod folder holds a settings file, so copying a new version over the old one keeps
  the player's settings (sdk\README.md, "Releasing and updating your mod").

  <Name>-source-v<Version>.zip, for mod makers: one folder, Source code\, with README.txt, LICENSE.txt,
  SHA256SUMS.txt and sdk\: ds3mod.h, README.md (the guide), CMakeLists.txt, GAME_NOTES.md, CIRCUITS.md, the source
  of every add-on (an allowlist: tests and experiments stay out) and tools\ds3_mcp.py (the memory inspector).
  Players don't need it. The loader's own source is the repository itself.

  <Name>-v<Version>-SHA256SUMS.txt: the checksum of every zip, for the download page.

  Checks before zipping: every DLL is 32-bit; no log, state file (ds3_seals_state.ini), old settings file
  (ds3_seals.ini), save, dump, test or build folder is in it; every shipped settings ini has each [debug] switch at 0.
  For each add-on: its mod.ini has id=, a three-number version= and a min_loader= no newer than this loader; its
  CHANGELOG.txt starts with that version. The add-ons zip holds no settings file.
  Publishing the zips is up to you.

.EXAMPLE
  From the repo folder, in PowerShell:
  & .\scripts\package-release.ps1
  & .\scripts\package-release.ps1 -SkipBuild -Name ds3-mods-test
#>
param(
    [string]$Name = 'ds3-mod-loader',
    [string]$Version = '',          # default: DS3_PROXY_VERSION in src\version.h
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'

if (-not $Version) {
    $text = Get-Content -Raw -LiteralPath (Join-Path $repo 'src\version.h')
    if ($text -notmatch 'DS3_PROXY_VERSION "([^"]+)"') { throw 'DS3_PROXY_VERSION not found in src\version.h' }
    $Version = $Matches[1]
}
# What of sdk\ ships in the source download: everything git tracks under these paths, nothing else under sdk\.
$sdkRelease = @('sdk/ds3mod.h', 'sdk/README.md', 'sdk/CMakeLists.txt', 'sdk/GAME_NOTES.md', 'sdk/CIRCUITS.md',
                'sdk/hello_mod', 'sdk/resource_counter', 'sdk/mouse_slider_range',
                'sdk/seal_store', 'sdk/auto_pickup', 'sdk/raw_mouse')
$docsDir = 'DS3 Mod Loader docs'
$addonsDir = 'DS3 Mod Loader add-ons'
$sourceDir = 'Source code'

# The optional add-ons: plugins folder name, its source folder under sdk\, built files (from build\plugins\<Mod>\),
# files of its source folder that ship as they are (Extra), and what READ ME FIRST.txt says about it (About; For is
# 'players' or 'makers'). Page: the mod's own repo, once it has a published release: READ ME FIRST.txt and the mod's
# README.txt then say it can also be downloaded there. Set it only when <Page>/releases/latest opens signed out.
$addons = @(
    @{ Folder = 'RawInputMouseFix'; Source = 'sdk\raw_mouse';          Built = @('DS3RawMouse.asi'); For = 'players';
       # Based on slopkhayzo's fix (MIT): their licence ships next to the project's.
       Extra = @('LICENSE-RawInputMouseFix.txt');
       Credit = 'Based on the Raw Input Mouse Fix by slopkhayzo (MIT, LICENSE-RawInputMouseFix.txt):
https://github.com/slopkhayzo/DeadSpace3-RawInputMouseFix';
       About = '1:1 mouse aim, without the game''s mouse acceleration and camera smoothing. The game''s own Mouse
Sensitivity slider (settings > controls) still sets the speed: at 50 the fix runs at 0.50. Its other settings
are in the Mods panel.' },
    @{ Folder = 'SealStore';        Source = 'sdk\seal_store';         Built = @('SealStore.asi'); For = 'players';
       About = 'The Ration Seal Store: spend Ration Seals in the game''s store again, on resource packs, circuit
packs and schematics. Bought with seals only, never money. Its pity counts are kept in ds3_seals_state.ini in
the game folder.' },
    @{ Folder = 'AutoPickup';       Source = 'sdk\auto_pickup';        Built = @('AutoPickup.asi'); For = 'players';
       About = 'Collects dropped items all around you without an Ammo Sweeper on your weapon, and lists what you
just picked up in the bottom-right corner.' },
    @{ Folder = 'ResourceCounter';  Source = 'sdk\resource_counter';   Built = @('ResourceCounter.asi'); For = 'players';
       About = 'Your six resources in a corner of the screen. Also the sample mod makers copy to start their own.' },
    @{ Folder = 'MouseSliderRange'; Source = 'sdk\mouse_slider_range'; Built = @(); For = 'players';
       About = 'A wider range for the game''s Mouse Sensitivity slider, so the whole slider is faster or slower. A
patch mod: no code, only its mod.ini. No effect while the Raw Input Mouse Fix''s 1:1 aim is active.' },
    @{ Folder = 'HelloMod';         Source = 'sdk\hello_mod';          Built = @('HelloMod.asi'); For = 'makers';
       About = 'The smallest example mod: press F12 and it says hello on screen. Only useful if you want to see
how a mod is made.' }
)
# Mods with their own page that are no longer in the add-ons download: @{ Name = '...'; Page = 'https://...' }.
# They stay listed under "More mods by RHFlyer" in READ ME FIRST.txt.
$moreMods = @()

# ---- Build ----
if (-not $SkipBuild) {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) { $cmake = 'C:\Program Files\CMake\bin\cmake.exe' }
    & $cmake -S $repo -B $build -A Win32 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
    & $cmake --build $build --config Release
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

# ---- Helpers ----
$releaseDir = Join-Path $repo 'release'
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

function New-Stage([string]$stageName) {
    $stage = Join-Path $releaseDir $stageName
    if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
    $zip = "$stage.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -Force -LiteralPath $zip }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    return $stage
}

function Put([string]$stage, [string]$from, [string]$to) {
    if (-not (Test-Path -LiteralPath $from -PathType Leaf)) { throw "missing: $from" }
    $dest = Join-Path $stage $to
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    Copy-Item -LiteralPath $from -Destination $dest
}

# A text file with Windows line endings, so Notepad on any Windows shows it right.
function Get-TrackedFiles([string[]]$paths) {
    Push-Location $repo
    try {
        $tracked = & git ls-files -- $paths
        if ($LASTEXITCODE -ne 0 -or -not $tracked) { throw 'git ls-files failed (commit the sdk folder first)' }
    } finally { Pop-Location }
    return $tracked
}

function Put-Text([string]$stage, [string]$text, [string]$to) {
    $dest = Join-Path $stage $to
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    [IO.File]::WriteAllText($dest, $text.Replace("`r`n", "`n").Replace("`n", "`r`n"))
}

function Test-Stage([string]$stage) {
    $files = Get-ChildItem -Recurse -File -LiteralPath $stage
    $bad = $files | Where-Object {
        $_.Name -match '\.(log|sav|dmp|bin|exe|pdb|ilk|obj)$' -or $_.Name -ieq 'ds3_seals_state.ini' -or $_.Name -ieq 'ds3_seals.ini' -or
        $_.FullName -match '\\(build|tests|dumps)\\'
    }
    if ($bad) { throw "refusing to package: $($bad.FullName -join ', ')" }
    # Every settings ini that ships (not the manifests) must have its [debug] switches at 0.
    foreach ($ini in $files | Where-Object { $_.Extension -eq '.ini' -and $_.Name -ne 'mod.ini' }) {
        $inDebug = $false
        foreach ($line in Get-Content -LiteralPath $ini.FullName) {
            if ($line -match '^\s*\[(.+)\]') { $inDebug = $Matches[1] -ieq 'debug'; continue }
            if ($inDebug -and $line -match '^\s*([^;=\s][^=]*)=\s*(\S+)' -and $Matches[2] -ne '0') {
                throw "$($ini.Name): [debug] $($Matches[1].Trim()) is $($Matches[2]), not 0"
            }
        }
    }
    foreach ($dll in $files | Where-Object { $_.Extension -in '.dll', '.asi' }) {
        $bytes = [IO.File]::ReadAllBytes($dll.FullName)
        $pe = [BitConverter]::ToInt32($bytes, 0x3C)
        $machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
        if ($machine -ne 0x14C) { throw "$($dll.Name) is not a 32-bit DLL (machine 0x$('{0:X}' -f $machine))" }
    }
}

# Zips a folder with / in entry names (what every unzip tool expects) and one fixed date on every entry, so the zip
# doesn't carry the packer's local time or timezone, and the same files always make the same zip.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zipDate = [DateTimeOffset]::new(2026, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
function Write-Zip([string]$stage, [string]$zip) {
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem -Recurse -File -LiteralPath $stage | Sort-Object FullName) {
            $entry = $archive.CreateEntry($f.FullName.Substring($stage.Length + 1).Replace('\', '/'),
                                          [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $zipDate
            $out = $entry.Open()
            $in = [IO.File]::OpenRead($f.FullName)
            try { $in.CopyTo($out) } finally { $in.Dispose(); $out.Dispose() }
        }
    } finally { $archive.Dispose() }
}

# Writes SHA256SUMS.txt in the stage, zips it, and returns the zip's file name and hash. $sumsDir: the folder inside
# the stage that holds every file and whose paths the list uses (an add-on's own folder, sdk\; '' = the top).
# $sumsAt: where the list goes, inside $sumsDir ('' = $sumsDir itself; the main zip puts it in its docs folder).
function Close-Stage([string]$stage, [string]$sumsDir = '', [string]$sumsAt = '') {
    Test-Stage $stage
    $base = if ($sumsDir) { Join-Path $stage $sumsDir } else { $stage }
    $outside = Get-ChildItem -Recurse -File -LiteralPath $stage | Where-Object { -not $_.FullName.StartsWith("$base\") }
    if ($outside) { throw "files outside ${sumsDir}: $($outside.FullName -join ', ')" }
    $sums = foreach ($f in Get-ChildItem -Recurse -File -LiteralPath $base | Sort-Object FullName) {
        $rel = $f.FullName.Substring($base.Length + 1).Replace('\', '/')   # as the zip names it
        '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash.ToLower(), $rel
    }
    # \n line ends, so sha256sum -c works on Linux, Steam Deck and Git Bash too.
    $sumsFile = if ($sumsAt) { Join-Path $base "$sumsAt\SHA256SUMS.txt" } else { Join-Path $base 'SHA256SUMS.txt' }
    [IO.File]::WriteAllText($sumsFile, (($sums -join "`n") + "`n"))
    $zip = "$stage.zip"
    Write-Zip $stage $zip
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLower()
    $count = (Get-ChildItem -Recurse -File -LiteralPath $stage).Count
    Write-Host "Made $zip ($count files, $([math]::Round((Get-Item -LiteralPath $zip).Length / 1KB)) KB)"
    return [pscustomobject]@{ Name = (Split-Path -Leaf $zip); Hash = $hash }
}

function Read-Manifest([string]$path) {
    $m = @{}
    $inMod = $false
    foreach ($line in Get-Content -LiteralPath $path) {
        if ($line -match '^\s*\[(.+)\]') { $inMod = $Matches[1] -ieq 'mod'; continue }
        if ($inMod -and $line -match '^\s*([a-z_]+)\s*=\s*(.*)$') { $m[$Matches[1]] = $Matches[2].Trim() }
    }
    foreach ($k in 'name', 'id', 'version', 'min_loader', 'description') {
        if (-not $m[$k]) { throw "$path has no $k= in [mod]" }
    }
    # The update standard (sdk\README.md, "Releasing and updating your mod").
    if ($m.version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$') { throw "${path}: version=$($m.version) is not three numbers like 1.2.0" }
    if ($m.min_loader -notmatch '^\d+\.\d+\.\d+$') { throw "${path}: min_loader=$($m.min_loader) is not three numbers like 1.0.0" }
    # The loader keeps 159 characters of a description (src\mods.h); the Mods panel would cut a longer one mid-word.
    if ($m.description.Length -gt 159) { throw "${path}: description= is $($m.description.Length) characters; the Mods panel shows 159" }
    return $m
}

# The newest entry of a mod's CHANGELOG.txt (its first line that starts with a digit) must be for this version.
function Test-Changelog([string]$path, [string]$version) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "missing: $path (one entry per version, newest first)" }
    $first = Get-Content -LiteralPath $path | Where-Object { $_ -match '^\d' } | Select-Object -First 1
    if (-not $first -or $first -notmatch "^$([regex]::Escape($version))(\s|$)") {
        throw "${path}: the newest entry is '$first', not $version"
    }
}

$licence = Join-Path $repo 'LICENSE.txt'
$results = @()

# ---- Every mod.ini is checked first, so a bad one stops the run before any zip is written ----
foreach ($a in $addons) {
    $src = Join-Path $repo $a.Source
    $m = Read-Manifest (Join-Path $src 'mod.ini')
    if ([version]$m.min_loader -gt [version]($Version -replace '-.*$', '')) {
        throw "$($a.Folder): min_loader=$($m.min_loader) is newer than this loader ($Version)"
    }
    Test-Changelog (Join-Path $src 'CHANGELOG.txt') $m.version
}

# ---- Main zip ----
$stage = New-Stage "$Name-v$Version"
Put $stage (Join-Path $build 'Release\dinput8.dll') 'dinput8.dll'
Put-Text $stage (Get-Content -Raw -LiteralPath (Join-Path $repo 'dist\ds3_mods.ini')) 'ds3_mods.ini'
# The loader is only the framework: every mod, the mouse fix included, is in the add-ons download, and the source is
# a download for mod makers. The plugins folder ships with one text file, so every unzip tool creates it.
Put-Text $stage @"
Add-ons go here, one folder each (for example plugins\SealStore\). Copy a mod's folder here from the add-ons
download ($Name-addons-v$Version.zip, folder $addonsDir), then switch it on in the Mods panel (pause, "mods")
and restart the game. To remove an add-on, delete its folder.

Other people's .asi mods go here too: put the .asi file (and the files that came with it) in this folder, or in
a folder of its own inside it. It shows up in the Mods panel under "Basic mods", switched off. Switch it on and
restart the game. More in "DS3 Mod Loader docs\README.txt".

This text file is only here so the folder exists; the loader ignores it.
"@ 'plugins\README.txt'
Put-Text $stage (Get-Content -Raw -LiteralPath (Join-Path $repo 'dist\README.txt')).Replace('{VERSION}', $Version) "$docsDir\README.txt"
Put-Text $stage (Get-Content -Raw -LiteralPath (Join-Path $repo 'dist\CHANGELOG.txt')) "$docsDir\CHANGELOG.txt"
# The binary's licences ship with it: MIT for the loader, MinHook's BSD notice for the code compiled into it.
Put-Text $stage (Get-Content -Raw -LiteralPath $licence) "$docsDir\LICENSE.txt"
Put-Text $stage (Get-Content -Raw -LiteralPath (Join-Path $repo 'THIRD-PARTY-NOTICES.txt')) "$docsDir\THIRD-PARTY-NOTICES.txt"
$results += Close-Stage $stage '' $docsDir

# ---- Add-ons zip: every mod in one folder, each in its own folder, with READ ME FIRST.txt ----
$stage = New-Stage "$Name-addons-v$Version"
$index = @()
$pages = @($moreMods | ForEach-Object { [pscustomobject]$_ })
foreach ($a in $addons) {
    $src = Join-Path $repo $a.Source
    $m = Read-Manifest (Join-Path $src 'mod.ini')
    $dir = "$addonsDir\$($a.Folder)"
    Put $stage (Join-Path $src 'mod.ini') "$dir\mod.ini"
    foreach ($b in $a.Built) { Put $stage (Join-Path $build "plugins\$($a.Folder)\$b") "$dir\$b" }
    foreach ($e in @($a.Extra | Where-Object { $_ })) { Put-Text $stage (Get-Content -Raw -LiteralPath (Join-Path $src $e)) "$dir\$e" }
    Put-Text $stage (Get-Content -Raw -LiteralPath (Join-Path $src 'CHANGELOG.txt')) "$dir\CHANGELOG.txt"
    $title = "$($m.name) $($m.version), an optional add-on for the DS3 Mod Loader"
    $credit = if ($a.Credit) { "`n$($a.Credit)`n" } else { '' }
    $where = if ($a.Page) {
        "Its own page: $($a.Page)/releases/latest`n" +
        "There you'll find it as a download of its own, its newer versions, its source and where to report bugs. This`n" +
        "download has version $($m.version); if the page has a newer one, unzip it into the game folder: it lands on this`n" +
        "copy and keeps your settings. MIT licence (LICENSE.txt)."
    } else {
        "Its source is in the source download, under $sourceDir\sdk\$(Split-Path -Leaf $a.Source)\. MIT licence`n(LICENSE.txt)."
    }
    $readme = @"
$title
$('=' * $title.Length)

$($m.description)
$credit
Needs the DS3 Mod Loader (dinput8.dll) $($m.min_loader) or later, installed in the game folder.

Install: close the game and copy this folder, $($a.Folder), into the plugins folder in the game folder (next to
deadspace3.exe), so that you get plugins\$($a.Folder)\mod.ini. Start the game, pause, choose "mods" and switch
"$($m.name)" on (new mods start off), then restart the game: a newly switched-on mod starts working after a
restart.

Update: close the game, copy the new version's $($a.Folder) folder over the old one in plugins, and let Windows
replace the files. Your on/off switch and your settings are kept: the download never holds them. CHANGELOG.txt
says what changed.

Remove: delete plugins\$($a.Folder).

$where
"@
    Put-Text $stage $readme "$dir\README.txt"
    Put-Text $stage (Get-Content -Raw -LiteralPath $licence) "$dir\LICENSE.txt"
    $by = if ($m.author) { ", by $($m.author)" } else { '' }
    $pageLines = if ($a.Page) {
        "    Also on its own page, where its updates come out first, with its source and bug reports:`n" +
        "    $($a.Page)/releases/latest`n"
    } else { '' }
    $index += [pscustomobject]@{ For = $a.For; Text = "  $($m.name) $($m.version): folder $($a.Folder)$by`n" +
        (($a.About -split "`r?`n" | ForEach-Object { "    $_" }) -join "`n") + "`n" + $pageLines }
    if ($a.Page) { $pages += [pscustomobject]@{ Name = $m.name; Page = $a.Page } }
}
# The player's settings are never in the add-ons zip: a new version copied over the old one must not reset them.
$settings = Get-ChildItem -Recurse -File -LiteralPath $stage -Filter '*.ini' | Where-Object { $_.Name -ne 'mod.ini' }
if ($settings) { throw "the add-ons zip must not hold a settings file: $($settings.FullName -join ', ')" }
$title = "DS3 Mod Loader ${Version}: add-ons"
$forPlayers = ($index | Where-Object For -eq 'players' | ForEach-Object Text) -join "`n"
$forMakers = ($index | Where-Object For -eq 'makers' | ForEach-Object Text) -join "`n"
if ($pages) {
    $width = ($pages | ForEach-Object { $_.Name.Length } | Measure-Object -Maximum).Maximum + 4
    $forMakers += "`nMore mods by RHFlyer`n  Each has its own page. Every one needs the DS3 Mod Loader, like the mods above.`n" +
        (($pages | Sort-Object Name | ForEach-Object { "  $($_.Name.PadRight($width))$($_.Page)/releases/latest" }) -join "`n") + "`n"
}
$readFirst = @"
$title
$('=' * $title.Length)

Optional mods for the DS3 Mod Loader. Each folder here is one mod. Pick the ones you want: nothing is installed
until you copy a folder into the game's plugins folder, and every mod starts switched off. (This download is a
collection to copy from. A single mod from another maker usually comes as a zip you unzip straight into the
game folder: see its README.)

You need the DS3 Mod Loader $Version in the game folder first: the main download, $Name-v$Version.zip.

Install a mod
  1. Close the game.
  2. Copy the mod's folder (for example SealStore) into the plugins folder in the game folder, next to
     deadspace3.exe, so that you get plugins\SealStore\mod.ini. Copy the mod folders one by one: don't copy
     this whole "$addonsDir" folder into plugins, and don't unzip this download into plugins. The
     mods would then be one folder too deep, and the loader would find none of them.
  3. Start the game, pause, choose "mods" (or press F11), switch the mod on, and restart the game: a newly
     switched-on mod starts working after a restart.

Update a mod: close the game, copy the new version's folder over the old one in plugins, and let Windows replace
the files. Your on/off switch and settings are kept: no folder here holds them. Each folder's CHANGELOG.txt says
what changed.

Remove a mod: delete its folder from plugins.

For players
$forPlayers
For mod makers
$forMakers
Each folder has its own README.txt and CHANGELOG.txt. The source of every mod is in the source download,
$Name-source-v$Version.zip. MIT licence (LICENSE.txt in each folder). SHA256SUMS.txt lists a checksum for every
other file here.
"@
Put-Text $stage $readFirst "$addonsDir\READ ME FIRST.txt"
$results += Close-Stage $stage $addonsDir

# ---- Source zip, for mod makers: the guide, the API, the tools and the source of every add-on ----
$stage = New-Stage "$Name-source-v$Version"
foreach ($f in Get-TrackedFiles $sdkRelease) { Put $stage (Join-Path $repo $f) ("$sourceDir\" + ($f -replace '/', '\')) }
Put $stage (Join-Path $repo 'scripts\ds3_mcp.py') "$sourceDir\sdk\tools\ds3_mcp.py"   # the memory inspector (sdk\README.md)
$title = "DS3 Mod Loader ${Version}: source code of the mods, and the SDK"
$readme = @"
$title
$('=' * $title.Length)

For making mods. Players don't need this download: the game only needs the loader and the add-ons they want.
Unzip it anywhere, for example in Documents; you get one folder, $sourceDir\, with sdk\ in it. Start with
sdk\README.md, the guide. The loader's own source is the project's repository.

In sdk\:

  ds3mod.h              the whole API for DLL mods, with a comment on every call
  README.md             the guide: patch mods, your first DLL mod, building, releasing and updating a mod
  GAME_NOTES.md         known game addresses; CIRCUITS.md: the game's circuit list
  tools\ds3_mcp.py      a read-only memory inspector for the running game
  hello_mod\            the smallest DLL mod: a hotkey that greets you, and Mods-panel settings
  resource_counter\     the sample to copy: your six resources in a corner of the screen
  mouse_slider_range\   a patch mod: no code at all, only bytes in its mod.ini
  auto_pickup\          Auto Pickup: five hooks that change how the game collects items, settings that apply at
                        once, and a pickup list drawn with its own font and fading boxes.
  raw_mouse\            the Raw Input Mouse Fix, based on slopkhayzo's fix (MIT, LICENSE-RawInputMouseFix.txt): an
                        input mod. Raw mouse input from the game's window, two hooks (one a naked assembly gate
                        that keeps the game's registers), polling from the frame callback, and hot reload.
  seal_store\           the Ration Seal Store: hooks, store offers sold for Ration Seals (API 3), pity counts,
                        its own log and settings. The biggest example; read the others first.

To build (README.md, "Build the samples", has the tools and every step), in sdk\:

  cmake -S . -B build -A Win32
  cmake --build build --config Release

Each built mod lands in build\plugins\<Mod>\, ready to copy into the game's plugins folder. The built add-ons are
also a download of their own, $Name-addons-v$Version.zip. MIT licence (LICENSE.txt).
"@
Put-Text $stage $readme "$sourceDir\README.txt"
Put-Text $stage (Get-Content -Raw -LiteralPath $licence) "$sourceDir\LICENSE.txt"
$results += Close-Stage $stage $sourceDir

# ---- Summary ----
$sumsFile = Join-Path $releaseDir "$Name-v$Version-SHA256SUMS.txt"
[IO.File]::WriteAllText($sumsFile, ((($results | ForEach-Object { '{0}  {1}' -f $_.Hash, $_.Name }) -join "`n") + "`n"))
Write-Output ''
$results | ForEach-Object { Write-Output ('SHA256 {0}  {1}' -f $_.Hash, $_.Name) }
Write-Output "Checksums in $sumsFile"
Write-Output 'Nothing was uploaded. Test the zips in a clean copy of the game folder before you share them.'
