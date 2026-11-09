# Dual Cable Link

In gpSP the "Cable Link" mode runs **every GBA of the link on every
console**: the player's own GBA, and hidden copies of the other players'.
The GBAs of a console are joined by a link cable in memory, and only the
buttons travel over the network. A game's link traffic never waits for the
WiFi, however many transfers it makes per frame (Zelda: Four Swords, Street
Fighter, ...). The price is a few frames of input delay (2 on a local
network with 2 players, 3 with 3 or 4), chosen automatically from the round
trip times, and the CPU of the other GBAs (they run on the other CPU core).

The **Cable Link players** core option (2, 3 or 4, 2 by default) says how
many GBAs are on the cable; every console must have the same value. Only
that many GBAs are loaded: with 2, each console runs 2. The hidden GBAs read
the ROM from the player's one (no extra copy of it in memory, except the
first megabyte).

Every other mode, and Cable Link when the consoles cannot use the dual
link, works exactly as before.

## Settings that must match

Each console runs a copy of the others' GBAs, so all of them must be the
very same machine. If anything here differs, the consoles say **"Cable
Link: games or settings differ, using the real time cable"** and play over
the real time cable, which is much slower (every transfer waits for the
network):

| what | where | note |
|---|---|---|
| the game | the ROM file | the very same dump (same CRC): not Europe on one console and USA or Virtual Console on another |
| BIOS | core option **BIOS** and `system/gba_bios.bin` | with **Auto select** a console uses `gba_bios.bin` if it is in its system folder, and the built-in BIOS if not: either copy the same `gba_bios.bin` to every console, or choose **Builtin BIOS** on all of them |
| Dynarec | core option **Dynamic Recompiler** | on (or off) everywhere |
| Sound rate | core option **Sound Output Rate (Hz)** | the same everywhere (the game reads the sound timing) |
| RTC, Rumble | core options **RTC support**, **Rumble support** | the same everywhere |
| Cable Link players | core option **Cable Link players** | the same everywhere, or the message is "the consoles have a different number of players" |
| the core | `gpsp_plus_libretro.so` | the same build on every console (Miyoo Mini, Android, ...) |

Options that only change the picture or the controls (colour correction,
frame mixing, sprite limit, frameskip, turbo) can differ.

The **Boot mode** option can differ too: the restart that starts a dual
session always boots through the BIOS (the one the BIOS option gives),
on every console, and so does a reset while linked. When the session ends
the boot mode option counts again. Every other mode, and the real time
cable, follow the options as always.

**Dynamic Recompiler** and **Sound Output Rate** changed in the menu while
the consoles are linked (or waiting for the players) do not apply at once:
the copies of this GBA on the other consoles could not follow, and the link
would break ("the consoles went out of sync"). The core says "the change
will be applied when the link ends" and applies it then. (For the next
session it must be the same on every console again, see the table.)

When the dual link is up every console shows **"Cable Link: both GBAs
connected (N frames of delay)"** (or "3 GBAs", "4 GBAs"). A local WiFi gives
2 or 3 frames; netplay through a relay server (tunnel) works but adds the
relay's delay (about 7 frames with a 80 ms ping).

## How a session goes

Every console selects **Cable Link** (or **Automatic**, which picks Cable
Link for every game without a Wireless Adapter, Pokemon or Advance Wars mode
of its own) and the same number of players, and starts a netplay session
(host and clients) as usual.

1. Each client compares ROM, BIOS, link protocol version, number of
   players and the settings that change the machine with the host. If anything differs, the
   real time cable is used instead, for everybody, with a message (and each
   client restarts its game then, as games only look for the cable at boot).
   (Which game it is, and the other GBAs, are worked out when the session
   starts: the host does it while it waits for the clients.)
2. Until all the players are there the games go on, and every console shows
   "waiting for players (2/4)" and so on each time one comes.
3. With everybody there, the host measures the round trip to each client
   (input delay = one way + 1.6 frames, rounded up, at least 2; with 3 or 4
   players a frame more, as the buttons between two clients go through the
   host).
4. Each console freezes its GBA between two frames and sends it (savestate
   and save memory, 544 KB, run length coded: after the restart it is mostly
   empty) to every other one. Every game restarts first, once; each player
   keeps their own save. The host is player 1, the clients follow in the
   order they came.
5. From then on only the buttons are sent. Every 2 seconds the consoles
   compare all the GBAs; if they differ in two checks in a row the link is
   cut with a message. (Consoles of different kinds, such as a Miyoo Mini
   and an Android phone, can differ for a moment in CPU flags that nothing
   reads; a GBA that really went another way stays different.)
6. Every console shows the same frame at the same time (screen and sound go
   together): with the buttons each one says how far ahead it is, and one
   that is ahead holds a frame (a frame of silence) when it gets more than
   5/8 of a frame ahead, at most once a second. Each client keeps with the
   host and the host with each client. Consoles never run at exactly the
   same speed, so this happens now and then.
7. If the WiFi has a bad moment and a console has to wait for another's
   buttons, it says so, and that one sends its buttons a frame earlier from
   then on (up to 4 frames more). After a calm minute it takes a frame off
   again (never below the delay of step 3); if that was too soon, it waits
   twice as long the next time (up to 10 minutes).

A reset while linked goes with the buttons: every console restarts every
game in the same frame, the cable still plugged in.

While linked, savestates cannot be loaded, cheats are off and fast forward
is disabled: all of them would make the copies of a GBA different. When a
console leaves, each player's GBA carries on alone.

## Different games, and programs sent through the cable

Two different games (Metroid Zero Mission and Metroid Fusion, Ruby and
Sapphire...) play over the real time cable. Some of them send a program
to the other GBA with the BIOS (MultiBoot): Zero Mission's Fusion link
sends one to a Fusion console started with **Start+Select held** while the
BIOS boots (boot mode **Boot to BIOS**), and single cartridge multiplayer
works the same way.

The built-in BIOS does this too now (`bios/multiboot`: SWI 25h, the sender,
and the Start+Select boot, the receiver, in the free end of the built-in
BIOS, patched in when it is loaded), word for word as the official BIOS
does it on the cable, so any mix of consoles with `gba_bios.bin` and
without it works. Before, a console on the built-in BIOS never sent the
program (its game waited for ever, "Linking...") and never took one (it
started its own cartridge), and the other console sat in its BIOS waiting.

A console that is a client of the netplay session also sends its cable
status again now and then: RetroArch netplay 8 drops what a client sends
before it is a player, and the host would otherwise go on without it.

## Code

| file | what |
|---|---|
| `dual.h` | the design notes and the interfaces |
| `serial_dual.c` | one end of the cable (SIOCNT/RCNT/SIO data, timing; multiplayer for 2-4, normal mode between players 1 and 2); in every instance |
| `dual.c` | the cable between the ends, the hidden instances, network protocol, input lockstep; instance A only |
| `Makefile` | builds the hidden instances: the core again with `-DGPSP_INSTANCE_B`, partially linked, copied with every global renamed `gbab_*`, `gbac_*`, `gbad_*` |

Hooks in the core, all inactive unless `serial_mode == SERIAL_MODE_DUAL_CABLE`:
`update_gba` (time sync, end of frame), `serial.c` (register writes,
scheduling), `input.c` (buttons), `cheats.c`, the RTC (read in UTC from the
savestate clock, so that every console agrees), the ROM loading of the hidden
GBAs (`gba_memory.c`), and `libretro.c`.

## Build

    make platform=miyoomini

gives `gpsp_plus_libretro.so` (no LTO here: every GBA must be the very same
machine code).

Android (every ABI, in `libs/<abi>/libretro.so`; rename it to
`gpsp_plus_libretro.so`), run from the `jni` folder:

    cd jni && ndk-build

On ARM64 the JIT cache of every GBA must be within 128 MB of the core's
code. The GBA the player sees maps it when the core starts, with room for
the hidden GBAs behind it (`memmap.c`): mapped later, when a session starts,
there is often no room left (Android fills that space with the allocator's
blocks) and a hidden GBA without its cache would run on the interpreter, out
of step with its copy on the other console. If one still ends up without it,
the session uses the real time cable instead.

## Test without consoles

`tools/dualtest` runs 2 to 4 consoles with RetroArch in one process, joined
by a network in memory (the clients only talk to the host, which passes
their packets on, as RetroArch does), on the real arm32 build under
`qemu-arm`:

    make -C tools/dualtest
    tools/dualtest/run.sh $PWD/tools/dualtest/linktest.gba 1200 5 0
    PLAYERS=4 tools/dualtest/run.sh /path/to/game.gba 2400 5 600

Arguments: ROM, frames, one-way latency (ms), frames each player plays alone
first. `PLAYERS=n` sets the consoles and their players option (`PLAYERS1=n`
gives console 1 another value), `JOIN_GAP=n` the frames between two clients
joining, `LATE_CONNECT=n` the frames the host hears about each client late,
`NETPLAY8=1` drops what a client sends before it is a player (as RetroArch
netplay 8 does; with `LATE_CONNECT` that is a promotion n frames late).

`PACE=1` runs each console on its own thread at 59.73 frames a second and
prints how long the setup took, how far apart the screens are and how many
frames were repeated (held by the time sync, or waits); with it
`SKEW_PPM=n` makes client k's clock k times n ppm faster, `BW_KBPS=n` limits
the network bandwidth, `JITTER_MS=n` adds a random delay to each packet and
`SPIKE_MS=n SPIKE_EVERY=k` delays one packet in k by n ms (the ones behind
it wait).

`KEYS0`..`KEYS3` script each player's buttons and `SHOTS=dir` with
`SHOT_AT`/`SHOT_EVERY` saves screenshots, to drive a game into its
multiplayer mode; `RESET_AT=n` presses reset on the host. `linktest.gba`
(built from `linktest.c`) makes tens of multiplayer transfers per frame and
logs the four words of each; the harness checks that every GBA logged the
same words, with a word from each player. `run.sh debug ...` links a core
whose cable prints what it does; `DUAL_FAULT=1` with it gives a hidden GBA
other buttons once, to check that the agreement check catches it.
`SERIAL=auto` (or any other value of the option) changes the link mode,
`ROM2=...` gives client 1 another game (the real time cable must take over).
`BIOSDIR=dir` uses the official BIOS (`dir/gba_bios.bin`; `BUILTIN1=1` keeps
console 1 on the built-in one), `BOOT=bios` boots
through it (hold Start+Select there with `KEYS1` to boot multiboot), `SAV0=file`
(`SAV1`...) loads a console's save memory and `SAVEOUT=dir` writes them
at the end.

Consoles of different kinds (Miyoo Mini: ARM32 dynarec, Android: ARM64)
run copies of each other's GBA, so every dynarec must give the very same
machine, cycle for cycle. `tools/dualtest/dettest.c` runs a core alone
with scripted buttons and prints a hash of the machine every frame: build
it for each CPU, run the same game and buttons on each core (`qemu-arm`,
`qemu-aarch64`, native) and compare the outputs. (The interpreter has other
timings: the dynarec setting must match, see the table above.)
