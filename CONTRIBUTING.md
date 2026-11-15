# Contributing to the DS3 Mod Loader

Thanks for wanting to help! This project exists to make Dead Space 3 easier to mod, so every fix, finding and
idea helps the next person. You don't need to be an expert, and you don't need to ask before you start.

I made this with a lot of help from AI (Claude and Codex), so some things in the code or the guide may be wrong.
If you find one, you're probably right: please tell me, or fix it.

## Report a bug or a mistake

Open an issue and say:

- what you did, what you expected, and what happened instead;
- the loader's version and the versions of the add-ons you had on (the Mods panel shows them under each switch);
- and attach `ds3_mods.log` from the game folder, plus an add-on's own log from its `plugins` folder if it was
  involved.

The logs hold no account details, and `ds3_mods.log` writes your folders as `<game>` and `%USERPROFILE%`. Still,
read a log before you post it.

A mistake in the guide (`sdk/README.md`) is a bug too. Report it the same way.

## Share what you found in the game

Found where a number lives in memory, or what a game function does? Add it to
[`sdk/GAME_NOTES.md`](sdk/GAME_NOTES.md) in the same table style, with a short note on how you found it and how
sure you are. A best guess is fine if you say it's a guess.

To show what you found, use the memory inspector's `save` command (see "Finding things in memory" in
`sdk/README.md`). It hides login data and your Windows user name. Never post a raw memory dump, and read the saved
file before you share it.

## Send a fix or an improvement

1. Fork the repo and make a branch for your change. New to this? GitHub's guide
   [Contributing to a project](https://docs.github.com/en/get-started/exploring-projects-on-github/contributing-to-a-project)
   walks you through it.
2. Make your change. Small pull requests are easiest to check, so one fix per pull request is best.
3. If you changed code, build it (see "Building from source" in [`README.md`](README.md)) and test it in the game.
4. Open a pull request and say what you changed and how you tested it, for example "Steam version, 1920x1080,
   keyboard and mouse, the Seal Store on".

Fixing a typo or a sentence in the guide? You can do that straight on GitHub with the pencil button, no build
needed.

Using AI to help you is fine. Just say so in the pull request, and test what you send.

## Share your own mod

Mods don't have to live in this repo. Put yours in your own repo or wherever you like, and open an issue to tell
me about it. I'd love to see what people make. `sdk/README.md`, "Releasing and updating your mod", explains how to
package it so players can update it easily.

## The rules

These keep the project safe and legal for everyone, and pull requests that break them can't be accepted:

- **Never touch paid DLC, entitlements, ownership, logins or the game's copy protection (DRM)**, not even to look
  around.
- **No game files** in the repo: no game assets, no copies of `deadspace3.exe`, no decompiled game code. Addresses,
  byte patterns and your own notes about them are fine.
- **No personal data**: no account details, login tokens, real names or full paths in code, logs or examples.
- Keep the text plain and short, for players and beginner modders.

By sending a pull request you agree that your change is shared under the project's MIT licence
([`LICENSE.txt`](LICENSE.txt)).

Thanks!
RHFlyer
