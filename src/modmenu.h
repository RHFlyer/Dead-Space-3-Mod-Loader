#pragma once

struct Config;

// The Mods panel: every [modules] switch and the options of each part, drawn over the paused game by the overlay.
// Opened from the pause menu ([mods_menu] key / pad_button while the pause screen is up); closed with Esc / B.
// It also lists each drop-in mod (mods.h): its [modules] switch and its manifest's settings. Changes are written at
// once, to ds3_mods.ini or to the mod's own ini, and applied live where the part (or mod) allows it; the rest are
// marked "applies after restart". the add-ons' state files and [debug] are never shown or touched.
// Needs the overlay and input hooks (InstallOverlay, InstallInput) first.
void InstallModMenu(const Config& config);
