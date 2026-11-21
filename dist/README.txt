DS3 Mod Loader v{VERSION}
=========================

A mod loader for the Steam version of Dead Space 3: a single proxy DLL (dinput8.dll), loaded by the game at start,
that runs drop-in mods from the plugins folder and gives you a panel in the pause menu to switch them on and change
their settings. The loader does not change how the game plays by itself: every feature is a mod. No game files are
changed: delete the files below and the game is as it was.

What it does
  - Other people's .asi mods: drop the file into plugins\ and switch it on in the Mods panel. No setup file is
    needed (see "Other people's .asi mods" below).
  - The Mods panel: pause the game and choose "mods" (or press F11, or Back on a pad). Every mod's switch and
    the settings it lists are there (a mod's settings only while it is loaded). Changes are saved at once.
  - Drop-in mods: folders in plugins\. New mods are off until you switch them on in the Mods panel. The loader
    comes with none. All of these are in the add-ons download (ds3-mod-loader-addons-v{VERSION}.zip), one
    folder each, with a READ ME FIRST.txt that says what each one does:
      RawInputMouseFix .... 1:1 mouse aim without the game's acceleration and camera smoothing. Based on the Raw
                            Input Mouse Fix by slopkhayzo (MIT licence).
      SealStore ........... the Ration Seal Store: spend Ration Seals in the game's store again, on resource packs,
                            circuit packs and schematics. Bought with seals only, never money.
      AutoPickup .......... collects dropped items all around you without an Ammo Sweeper on your weapon, and
                            lists what you just picked up in the bottom-right corner
      ResourceCounter ..... your six resources in a corner of the screen
      MouseSliderRange .... a wider range for the game's mouse sensitivity slider (a patch mod: no code). No
                            effect while the Raw Input Mouse Fix's 1:1 aim is active.
      HelloMod ............ the smallest example mod: a hotkey that says hello
  - The source code of every add-on above, the Ration Seal Store included, with the mod SDK, for people who want
    to make their own mods, is a separate download (ds3-mod-loader-source-v{VERSION}.zip): the guide is
    sdk\README.md in it. Unzip it anywhere; the game doesn't need it.

Install
  1. BACK UP YOUR SAVES first: copy the folder  Documents\EA Games\Dead Space 3  somewhere safe.
  2. Unzip everything in this zip into the game folder, next to deadspace3.exe (Steam: right-click Dead Space 3 >
     Manage > Browse local files). That adds dinput8.dll, ds3_mods.ini, a plugins folder for add-ons and the
     "DS3 Mod Loader docs" folder this README is in.
  3. Start the game.

  Add-ons: unzip the add-ons download anywhere and read its READ ME FIRST.txt. Copy the folder of each mod you
  want into plugins\ in the game folder (e.g. plugins\SealStore\). Then switch it on in the Mods panel and restart
  the game: a newly switched-on mod starts working after a restart. To remove one, delete its folder. For 1:1
  mouse aim, copy RawInputMouseFix and switch on "Raw Input Mouse Fix". Its speed follows the game's own Mouse
  Sensitivity slider (settings > controls); at 50 it runs at 0.50.

Other people's .asi mods (basic mods)
  Most mods made for an "ASI loader" work with this loader too. No other loader is needed.
  1. Close the game. Copy the mod's .asi file, and any files that came with it, into plugins\ in the game folder
     (or into a folder of its own inside plugins\).
  2. Start the game, pause and choose "mods". The mod is listed under "Basic mods", switched off.
  3. Switch it on and restart the game.
  A basic mod has no settings in the Mods panel: change its own ini file the way its maker describes.
  A mod that comes as a .dll is not listed. If its maker says it is an ASI mod, rename the file to end in .asi.
  If a mod is not listed, ds3_mods.log in the game folder may say why (for example a file or folder name with
  special characters: rename it). The limits: the .asi must be directly in plugins\ or one folder deep; file
  and folder names in plain letters, digits and simple punctuation, up to 63 characters each; up to 8 .asi
  files in one subfolder; 32 mods in all.
  The loader loads a basic mod as it is and can't check what it does. Switching one on means trusting it, so only
  use mods from people you trust.
  Coming from Ultimate ASI Loader? It is usually also a file called dinput8.dll in the game folder, so this
  loader's dinput8.dll takes its place: let Windows replace the file. Then move your .asi files (from the game
  folder or its scripts folder) into plugins\ and switch them on in the Mods panel. If the Mods panel then says
  "Basic mods (left to ...)", Ultimate ASI Loader is still installed under the file name shown there: delete
  that file from the game folder.

If the game does not start any more (safe start)
  When a start does not finish, for example because a mod crashed the game, the next start is a safe start: no
  mod is loaded, and a notice on screen says so. Pause, choose "mods", switch off the mod you added or updated
  last, and restart the game. Mods that were on but not loaded are marked there. Nothing is switched off for you.
  You can also ask for a safe start yourself: set  safe_start=1  under [mods] in ds3_mods.ini (add both lines if
  they are not there). It is used once. Or delete the mod's file or folder from plugins\.
  What it can't do: it only notices a start that fails early: roughly the first 20 seconds after the mods are
  loaded (the game must also have drawn 300 frames). For a crash later on, use safe_start=1 or remove the mod.
  It also needs to write a small file in the game folder (ds3_mods_start.ini): if the folder is read-only,
  basic mods are not loaded at all, and the Mods panel says so on their rows.

Updating
  Close the game first.
  - An add-on: copy the new version's folder over the old one in plugins\ and let Windows replace the files. Your
    on/off switch and its settings are kept: the switch is in ds3_mods.ini, and the add-ons download never holds
    your settings. CHANGELOG.txt in the add-on's folder says what changed.
  - The loader itself (for example 1.0.0 to this version): ds3_mods.ini holds your on/off switches, and the new
    zip has a fresh one. So first copy your ds3_mods.ini somewhere else. Unzip the new zip into the game folder
    and let Windows replace the files. Then copy your ds3_mods.ini back. If you replaced it anyway: your
    mods are switched off and the panel's own settings (text size, keys) are back to their defaults. Each mod's
    own settings and your saves are kept. For a later version, read "Updating" in that version's README first.
  The Mods panel shows each mod's version under its switch. If a mod's row says "needs loader" and a version, that
  mod was made for a newer DS3 Mod Loader: install the newer loader.

Uninstall
  Delete dinput8.dll, ds3_mods.ini, ds3_mods.log, ds3_mods_start.ini, the plugins folder and the "DS3 Mod Loader
  docs" folder from the game folder, and ds3_seals_state.ini if you used the Ration Seal Store.

Good to know
  - What you buy or unlock stays on the save you did it on.
  - Leave any [debug] setting in a mod's ini at 0; they are for testing only.
  - Co-op has NOT been tested. If you play co-op with it, note what happens and send back ds3_mods.log.
  - If anything goes wrong, look in ds3_mods.log in the game folder (and an add-on's own log in its plugins
    folder, e.g. plugins\SealStore\seal_store.log), and send it along with your report. They hold no account
    details, and the loader's log writes the game folder as <game> and your user folder as %USERPROFILE%, so your
    Windows user name is not in it.
  - This mod never touches paid DLC, entitlements, ownership or the game's DRM, and mods made with its SDK must not
    either. A DLL mod is a program: install mods from other people only if you trust them.

Known issues
  - Tested at 1920x1080 with keyboard and mouse. Other resolutions, ultrawide screens and a controller on the
    pause menu's "mods" button have not been tested yet; please report what you see.
  - Co-op is untested (see above).
  - Ultimate ASI Loader together with this loader is not supported: use one of the two. If both are installed,
    this loader leaves .asi files without a mod.ini to the other one, and the Mods panel says so.
  - Updating the loader by unzipping the new zip over the old one replaces ds3_mods.ini, which holds your
    switches: every mod is then off until you switch it on again in the Mods panel, and the panel's own settings
    (text size, keys) are back to their defaults. Each mod's own settings and your saves are kept. To keep your
    switches, copy ds3_mods.ini away first and back afterwards (see "Updating").
  - A basic mod that has to load at the very start of the game (some graphics injectors) may not work: basic mods
    are loaded when the game sets up its input, a moment later.
  - The Auto Pickup add-on is meant to leave quest items, whole weapons and the Spare Parts Box for you to pick up
    yourself. That has not been seen in game yet; if it picks one of them up, please send ds3_mods.log.

Licence
  MIT (LICENSE.txt). Third-party parts and their licences: THIRD-PARTY-NOTICES.txt. What changed: CHANGELOG.txt.
  All are in this folder, with SHA256SUMS.txt: a checksum for every other file in this zip, by its path from
  the game folder. The add-ons download has its own list in "DS3 Mod Loader add-ons" (paths from that folder), and the
  source download in "Source code" (paths from that folder).
