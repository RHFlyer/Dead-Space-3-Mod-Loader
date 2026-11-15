DS3 Mod Loader v{VERSION}
=========================

A mod loader for the Steam version of Dead Space 3: a single proxy DLL (dinput8.dll), loaded by the game at start,
that runs drop-in mods from the plugins folder and gives you a panel in the pause menu to switch them on and change
their settings. The loader does not change how the game plays by itself: every feature is a mod. No game files are
changed: delete the files below and the game is as it was.

What it does
  - The Mods panel: pause the game and choose "mods" (or press F11, or Back on a pad). Every mod's switch and
    the settings it lists are there. Changes are saved at once.
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

Updating
  Close the game first.
  - An add-on: copy the new version's folder over the old one in plugins\ and let Windows replace the files. Your
    on/off switch and its settings are kept: the switch is in ds3_mods.ini, and the add-ons download never holds
    your settings. CHANGELOG.txt in the add-on's folder says what changed.
  - The loader itself: read "Updating" in the README of the new version first; it says how ds3_mods.ini is kept.
  The Mods panel shows each mod's version under its switch. If a mod's row says "needs loader" and a version, that
  mod was made for a newer DS3 Mod Loader: install the newer loader.

Uninstall
  Delete dinput8.dll, ds3_mods.ini, ds3_mods.log, the plugins folder and the "DS3 Mod Loader docs" folder from
  the game folder, and ds3_seals_state.ini if you used the Ration Seal Store.

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
  - The Auto Pickup add-on is meant to leave quest items, whole weapons and the Spare Parts Box for you to pick up
    yourself. That has not been seen in game yet; if it picks one of them up, please send ds3_mods.log.

Licence
  MIT (LICENSE.txt). Third-party parts and their licences: THIRD-PARTY-NOTICES.txt. What changed: CHANGELOG.txt.
  All are in this folder, with SHA256SUMS.txt: a checksum for every other file in this zip, by its path from
  the game folder. The add-ons download has its own list in "DS3 Mod Loader add-ons" (paths from that folder), and the
  source download in "Source code" (paths from that folder).
