# SoundBoard V4 firmware

Product firmware for the V4 PCB (ESP32-S3 on an Unexpected Maker ProS3).
Built from `../spec/v4/FirmwareSpec.md` in the phases of
`../spec/v4/ImplementationPlan.md`. Hardware facts and rules:
`../briefs/FirmwareBrief.md`.

## Build, flash, test

```bash
pio run -d firmware                       # build (FW_VERSION from git describe)
pio run -d firmware -t upload             # flash; then tap RESET on the board
pio device monitor -d firmware            # 115200, echo on; one command per line
pio test -d firmware -e native            # host unit tests (lib/sbcore)
```

A monitor must not hold the port during an upload. After an upload the chip
sits in download mode until RESET is tapped.

## Layout

```
platformio.ini        pros3 (board) and native (host tests) environments
src/                  the firmware (main.cpp, app/, config/, hal/, display/, diag/, power/, util/)
lib/sbcore/           pure logic shared with the host tests: Config, settings table,
                      JSON loader/saver/validation, levels, migration
lib/Adafruit_ST77xx/  vendored screen driver (registry copy minus its SD dependency)
test/test_config/     native tests for the configuration core
tools/version.py      pre-build: embeds FW_VERSION
examples/             config.annalise.json (spec §13.6) and its one-line `merge` form
```

## Console

`?` help, `s` status, `get <path>`, `set <path> <value>`, `merge <json>`,
`save`, `dump`, `factory`, `log [n]`, `loglevel <level>`, `reboot`, `crash`,
`wdt`. A value for `set` is JSON when it parses (`60`, `true`,
`[1,3,3,3]`, `{"offHoldMs":1200}`) and text otherwise (`green`); string and
enum settings always take text. `set` and `merge` change the running
configuration only; `save` writes `config.json` and the flash mirror.

Inputs (Phase 2): `1`-`4` press a pad, released after 100 ms
(`2 1500` holds P2 for 1500 ms), `+` / `-` an attendant click, `l` toggles
the four pad deltas at 1 Hz, `c` recalibrates the pads (hands off; the screen
says so). `s` prints the button states, the dim timer and a touch block:
phase, shield mode, thresholds, and per pad the channel, role, baseline, raw
reading and delta. Every pad event and button command is logged
(`PadDown P1 (ch2) +9.8% [press 3]`, `PadUp`, `PadStuck`, `command: OFF`).
Baselines live in NVS `sb-cal/touch`, tagged with the pad order and shield
settings they were taken under; `factory` clears them so the next boot
calibrates. The first touch readings after the driver starts are garbage
(tests/02: a saturated 0x3FFFFF), so the boot check and the detector skip
samples that are far from the stored baseline.

Audio (Phase 3): `b` toggles the on-board speakers, `bt` the Bluetooth
speaker enable (a rail cycle on enable, `AT+POWER_OFF` on disable), `sounds`
lists the cache (state, length, format per file), `AT+...` goes to the KCX
unchanged. `s` adds the rail and I2S state, what is playing, underruns, the
streaming ring's starved count, the KCX banner/link state, the cache fill,
the amp state and the scope pin (IO18: HIGH at `PadDown`, LOW when the first
frames reach the DAC; the log prints the same latency in ms). A simulated
press (`2`) plays the entry's sound like a real one; a missing file plays the
two-tone. Sounds go in `/sounds/` on the card in the canonical format;
`tools/convert_sounds.py` (repo root) converts a folder of recordings and
checks them against a `config.json`.

Levels and presses (Phase 4): a console press is a full press. With
Annalise's configuration `1` is the level pad (next level, wrap, the click
cue), `2`-`4` run the entry on the current level: a sound, or on level 3
Louder / Mute / Quieter (the click at the new master volume; Mute stops the
sound and shows MUTE, the next volume change un-mutes with a click). `++`
is a quick tap of both buttons (level 1), `h+` / `h-` a long hold (next /
previous level with the attendant cues). Repeat while held needs
`set press.repeatWhileHeld true` and a long press (`2 5000`): the sound
repeats `press.repeatDelayMs` after it ends while the pad stays down; the
level pad repeats with `press.levelRepeatWhileHeld`. The board returns to
level 1 after `levelChange.returnToFirstAfterS` without input (console lines
count as input, so stop typing). `s` adds a `press:` line (last press,
repeat state, the return timer) and the log prints `level view redrawn N ms
after the change` for the §11.4 budget.

Screen: the views draw into a 64 KB canvas in PSRAM and the display pushes
the rows each region touched as one block (a level change redraws in ~35 ms;
`loglevel debug` prints every region's draw and push time). A slice that
leaves regions dirty continues on the next 5 ms tick. The startup cue waits
until `_startup.wav` is cached (3 s cap) so the file plays, not the built-in
arpeggio.

Power (Phase 5): `sleep` enters SLEEP at once (the USB port disappears;
any pad or button wakes it; a touch wake plays that pad's sound), `off`
enters OFF (a button held about half a second wakes it, a shorter tap sends
it straight back to OFF without lighting the screen; with USB present the
screen shows the charge every `power.chargeCheckMin` for 5 s). `batt`
prints the battery line (voltage, percentage, USB, the method and K);
`batt 4.12` calibrates K against a meter reading on VBAT and stores the
pull-down method in NVS `sb-cal`; `batt clear` returns to the design
divider; `batt fake 3.6` makes every sample read 3.6 V (RAM only, `batt
fake off` or a reset ends it) so the low-battery row can be tried with a
full battery, and `batt fake 3.6 nousb` also fakes USB absent so BATTERY
EMPTY → OFF runs without unplugging anything. On its first boot the firmware imports validation 03's `battcal/k`
so unit 1 needs no re-calibration. `s` adds the battery line and the sleep
/ off-return timers. The timeouts of §3.2 run from the last input: dim,
then SLEEP after `power.sleepAfterMin`; after an Off-wake with no pad press
the board goes back to OFF after `power.offReturnS`. Below
`power.lowBatteryWarnPct` the status row turns red; at
`power.shutdownPct` on battery the screen says BATTERY EMPTY and the board
switches off.

Vibration and jacks (Phase 6): `v` runs the current level's pattern at the
current strength (the log shows its length, and the `s` motor line reads
back the LEDC duty and the IO39 pad level, so a silent motor can be split
into chip, driver and motor); `j1`–`j4` close a jack for 1 s. A level change
plays the level pattern (count, pattern or the level's own array,
truncated at `vibration.maxPatternMs`, with the §9.4 caps: 2 s per "on",
50 ms gaps, 5 s of on-time in any 10 s); a return to level 1 gives a
single pulse. Presses close the pad's jack per `jacks.mode`, the entry's
`jack` and the level's `jacks` flag; a followed jack opens on release,
after `jacks.maxFollowMs`, on a stuck pad or on a level change. `s` adds a
motor line and the closed jacks. SLEEP waits for a running pattern; OFF and
FAULT stop the motor and open every relay at once.

Bluetooth speaker (Phase 8): with `bluetoothSpeaker.enabled` the KCX module
boots with the 5 V rail and relinks to its last speaker by itself; the
firmware only watches its lines (`POWER ON`, `MacAdd:…,Name:…` + `CON LAST`
on a link, `DISCONNECT`, `OK+STATUS:n`, `SCAN....`) and asks `AT+STATUS?`
every 60 s while unlinked. The module is a KCX_BT_RTX V1.4 and every command
needs CR LF (`docs/bluetooth-module.md`; `kcxcrlf` toggles it for the bench,
`pin <n>` dumps a GPIO's routing). The
bottom line says BT SPEAKER LINKED / BT SPEAKER LOST for 4 s at each change
and NO BT SPEAKER, dim, once the speaker has been enabled for 10 s without a
link (a wake or an enable restarts the 10 s). Pairing (`p`, later the menu
and the portal) sends `AT+PAIR` and shows PAIRING with a 60 s countdown bar
over the numeral and PAIR THE BT SPEAKER on the bottom line; a link report
that arrives after the start ends it with BT SPEAKER LINKED and the saved
cue, the timeout with NO BT SPEAKER FOUND. `pairwipe` is the only path that
sends `AT+DELVMLINK` (forget every saved speaker), then pairs. Disabling sends
`AT+POWER_OFF` and clears the speaker words without a BT SPEAKER LOST; enabling
cycles the rail once nothing is playing. After a wake the module relinks by
itself in about 4 s; sounds pressed before that play on the wired path only
(the wake-wait option of Draft 4 was dropped at CP-8: a link report is not
yet an audio stream, so the held-back sound was lost anyway). The Bluetooth
path also lags the wired and on-board outputs by 250-500 ms, so in practice
one output is used at a time. `s` prints a `bluetooth speaker:` line: module
alive / soft off, link, pairing state, the module's last line and a pending
rail cycle.

Bluetooth LE keyboard (Phase 7): the board advertises as `device.name`
(default `SoundBoard V4`) whenever `keyboard.enabled` and no host is
connected; pair from the host's Bluetooth settings ("Just Works", no
code). One host at a time, three bonds; a fourth new host is refused with
"TABLET MEMORY FULL" on the screen. A press sends its entry's `type` text, or its
`key` (`hold` while the pad is held, forced up after
`keyboard.holdKeysMaxMs`; `tap` for 30 ms), only while the host is *ready*
(encrypted and subscribed); otherwise nothing is sent, then or later. A held
key also goes up on a level change, a stuck pad, `kbd` off, SLEEP and OFF.
The bottom line says TABLET CONNECTED / TABLET LOST for 4 s at each change
and NO TABLET, dim, once the keyboard has been on for 10 s without a host
(§11.4: plain words, no tokens; SPEAKERS ON while the on-board speakers are
on). Console: `kbd`
toggles the keyboard in RAM, `kbdforget` wipes the bonds, `kbdtype <text>`
and `kbdkey <NAME> [ms]` send from the bench; `s` adds a keyboard line
(state, name, address, bonds, host, reports sent/undelivered, held key,
queue). The BLE init runs from the first app tick, after a latched wake
press is consumed, so it does not delay a wake-and-play.

To put Annalise's configuration on the card without opening the case, paste
the single line in `examples/config.annalise.oneline.txt` into the monitor,
then `save`.

## Bench commands (not in the spec; for the SD investigation of 2026-09-13)

`vbat` battery estimate (validation 03's unit-1 method); `sdcycle [ms]` gated
rail off, screen re-init, remount (recovers a hung card; reports whether the
card still had power); `sdtest [psram] [chunk]` 16 KB file write/verify through
FATFS; `sdraw <hz> [crc] [lib] [low] [dark] [n]` raw CMD24/CMD17 over n sectors
near the end of the partition (`low` = near the start of the data area, `dark`
= backlight off, `crc` = CRC on, `lib` = the core driver's deselect/status
sequence), restored afterwards; `sdclk <MHz>` remount at another clock;
`sdchunk <bytes>` bytes per card write. `sdraw` writes raw sectors: bench only.

### SD write failure of 2026-09-13: resolved, it was the card

Symptom: reads reliable at every clock; every write path (FATFS 512 B and 4 KB
writes, raw CMD24, validation 04's 512 KB write) corrupted data and the card
reset itself and hung, at 400 kHz as much as at 20 MHz, from internal RAM as
much as from PSRAM. The unmodified validation 04 build, which had passed on the
same card on 2026-09-09, failed the same way when re-flashed.

Cause: the original 14.6 GB SDHC card. It still reads fine and the Mac can still
write it in native SD mode, but its controller dies under SPI-mode programming
on this board. A different card (16 GB, reformatted FAT32/MBR as `SBV4` with
`diskutil eraseDisk FAT32 SBV4 MBRFormat /dev/diskN`) passes validation 04 in
full (write 220 KB/s verified, read ladder to 1000 KB/s at 20 MHz, rail-cycle
remount) and the product firmware (blank card gets the mirror written to it,
`sdtest 4096` OK at 20 MHz, `save` to card and mirror). The old card is out of
the product.

What was learned on the way, for rev B: before any change the scope showed
3V3_GATED at the screen collapsing to ~0.45 V for ~0.8 ms during write bursts.
Unit 1 now carries bodged 100 µF + 10 µF + 0.1 µF at the screen (on top of C6
100 µF) and 100 µF + 0.1 µF at the SD socket; the collapse disappeared but the
old card still failed identically, which is what pointed at the card. The board
as designed has only C6 on the rail, ~150 mm from the socket, so rev B should
carry 10 µF + 0.1 µF at the socket and at the screen regardless (or a dedicated
3.3 V regulator for SD/screen with real output capacitance). Side effect of the
added bulk: a hung card now survives board resets and short firmware rail cycles
(validation 04's 500 ms cycle could not remount the hung card), so a firmware
rail cycle that is meant to recover a card needs a discharge path.
