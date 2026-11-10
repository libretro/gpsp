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

## Settings

Each console runs a copy of the others' GBAs, and each copy must be the very
same machine as the GBA it copies. The consoles work that out between them
when the session starts:

| what | what happens if it differs |
|---|---|
| the game | the dual cable is still used when every console has the other games in its ROM folder; otherwise (or with **Cable Link: different games** set to **Real time cable** on the host) the **real time cable** is used, see below |
| BIOS (core option **BIOS** and `system/gba_bios.bin`) | every GBA uses the **built-in BIOS** for the session (*"the consoles have different BIOS, every GBA uses the built-in one"*); a console that had to switch keeps it until the game is loaded again |
| Dynarec, Sound rate, RTC, Rumble | nothing: each copy of a GBA runs with the settings of the console it belongs to |
| Cable Link players | the clients take the host's number (*"Cable Link: N players, as the host has it"*) |
| the core | the same build is needed on every console (Miyoo Mini, Android, ...) |

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
will be applied when the link ends" and applies it then.

When the dual link is up every console shows **"Cable Link: both GBAs
connected (N frames of delay)"** (or "3 GBAs", "4 GBAs"). A local WiFi gives
2 or 3 frames; netplay through a relay server (tunnel) works but adds the
relay's delay (about 7 frames with a 80 ms ping).

## Different games

Two different games (Ruby and Sapphire, two editions of Battle Network, the
Europe and USA versions of one game, or a game and its Virtual Console dump:
any two ROMs that are not byte for byte the same) go on the **dual cable**
too when every console has them, with nothing to set up (most people never
open the core options): each console looks for the other players' games in
the folder of its own ROM and runs them too. Every screen then goes at full
speed; the start takes a little longer (the other games are read from the
card) and each one needs memory (as much as its ROM: the
copies of the same game share it). A game is found by its whole contents,
never by its name:

1. `system/gpsp_plus_roms.txt`, an index of the games found before (a line
   per game: hash, size, quick hash, path), written by the core;
2. the file with the other player's file name, in the same folder;
3. the folder: the 16 `.gba`/`.bin` files named most like the other
   player's file (or like this console's own: another region or edition),
   then the others for half a second at most. Only the files of the right
   size are opened, and only their header (one sector) is read; the ones
   with the right header have their first and last 32 KB compared, and the
   one that matches is loaded and checked whole (a game and its Virtual
   Console dump can share all of that). (Asking for the size of each file of
   a folder with thousands of games took 30 seconds on a Miyoo Mini, FAT32,
   the game frozen: hence the order and the limit. A game with quite another
   name in a huge folder may not be found: the real time cable is used.)

If a console does not have the game (or has no room for it, Miyoo Mini Plus:
128 MB in all) every console uses the real time cable (*"a console does not
have the other game"*, *"not enough memory for the other game"*).

The core option **Cable Link: different games** (the host's value counts) is
**Automatic** by default, which is all of the above. **Real time cable**
never looks for the other games: different games start right away on the
real time cable (*"Cable Link: different games, using the real time
cable"*).

## How a session goes

Every console selects **Cable Link** (or **Automatic**, which picks Cable
Link for every game without a Wireless Adapter, Pokemon or Advance Wars mode
of its own) and the same number of players, and starts a netplay session
(host and clients) as usual.

1. Each client tells the host its game, BIOS and settings, and takes the
   host's number of players. Different games that the dual cable cannot
   take (see above) make everybody use the real time cable, with a message
   (and each client restarts its game then, as games only look for the cable at boot). Another version of the
   link (another build of the core) cannot be talked to at all, by either
   cable: the consoles say "the other console has another version of this
   core" and play on unplugged. (Which game it is, and the
   other GBAs, are worked out when the session starts: the host does it
   while it waits for the clients.)
2. Until all the players are there the games go on, and every console shows
   "waiting for players (2/4)" and so on each time one comes.
3. With everybody there, the host tells everybody each player's game and
   settings, and each console gets its copies of the other GBAs ready (with
   their owners' settings, and their games if they differ). Then the host
   measures the round trip to each client
   (input delay = one way + 1.6 frames, rounded up, at least 2; with 3 or 4
   players a frame more, as the buttons between two clients go through the
   host).
4. Each console freezes its GBA between two frames and sends it (savestate
   and save memory, 544 KB, run length coded: after the restart it is mostly
   empty) to every other one. Every game restarts first, once; each player
   keeps their own save. The host is player 1, the clients follow in the
   order they came.
5. From then on only the buttons are sent. Every 2 seconds the consoles
   compare all the GBAs (a hash of all the memory a game can see, 400 KB
   per GBA, a word at a time); if they differ in two checks in a row the
   link is cut with a message. (Consoles of different kinds, such as a Miyoo Mini
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

The GBAs of a console wait for each other all the time, for microseconds.
Should one ever wait for whole seconds (5), the other is not coming: rather
than hanging the frontend for good, the cable is cut, every GBA finishes
its frame alone and the session stops with a message.

While linked, savestates cannot be loaded, cheats are off and fast forward
is disabled: all of them would make the copies of a GBA different. When a
console leaves, each player's GBA carries on alone.

## Different games, and programs sent through the cable

Two different games (Metroid Zero Mission and Metroid Fusion...) play over
the real time cable when the dual cable cannot take them (see above). Some of them send a program
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

## The real time cable

Over the real time cable each transfer needs the other console's word, a
network round trip away. A child sends its next word as soon as its game
left it alone for a moment after a transfer, once that has been the right
one many times in a row, and says now and then that it still holds; when
the parent starts the next transfer with it already there, and recent
enough (the child said it held up to a frame and a half before), it takes
it (and says so: both ends agree on what the wire had) and does not wait at
all. Games that make a transfer every frame or so (menus, races) then run
at full speed: Mario Kart Super Circuit, Europe against USA, 8 ms each way:
60 frames a second in menus and races (it was about 40 before). A word the
game changes in that last frame and a half goes one transfer late, as a
word loaded a moment too late on a real cable (Mario Kart does it once or
twice per session, between screens, and does not mind).

A burst of transfers where each word answers the one before, as in a
handshake screen (Mario Kart's "Multi-Pak Link" screen: about 9 a frame, the
child echoing the parent), still costs a round trip each: 17 frames a second
with 8 ms each way, 34 with 3 ms. Nothing but the other game itself could
tell what it answers (a guess that is wrong now and then made Mario Kart
lose the link later on): that is what the dual cable is for.

A WiFi hiccup (home WiFi often holds packets for 100 ms or more now and
then, power saving) stops the child until the parent's clock gets through;
it then runs an extra frame now and then until it is back where it should
be, instead of staying behind for good (the parent would wait for all of
that the next time it needs its word). A parent that does not wait can be
frames ahead: the child still gives its game the time the parent had
between two transfers, even more than a frame (Mario Kart between screens),
or the game has no time to load its word.

While the network holds everything back, both ends wait: up to a second.
Giving up earlier loses the link: the game then gets a console that is not
there, or (a child that stops following the parent's clock) runs on alone
with no transfers. With a 400 ms stall now and then, Mario Kart lost the
link every time when the child gave up after 250 ms, and never with a
second. A console that really stopped (paused, in the menu) freezes the
other one for that second, once.

A wait spins for the first millisecond (the answer is usually that close on
a wired or simulated network), then takes naps of 0.1 ms: a CPU core
spinning through every round trip was most of what this cable cost. (Where
such a nap takes milliseconds, a system with coarse timers, it notices and
goes on spinning: the Miyoo Mini's kernel has no high resolution timers,
any sleep takes 10 to 20 ms there.) Both consoles of a session on one PC, Mario Kart, 8 ms
each way: 69% of a core before, 29% now, at the same frames a second.

Two Android handhelds over a home WiFi with such hiccups (ping 3-240 ms):
about 45 frames a second in menus and races, the old core and this one
alike (the hiccups set the pace), 46 on the "Multi-Pak Link" screen (41
before), the link holds every time.

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
`CORE1=file` gives client 1 another build of the core (two versions of the
link that meet: each new one says so, the games go on unplugged).
`OTHER=1` sets **Cable Link: different games** to Automatic (the harness defaults to the real time cable, unlike the core).
`LINKLOG=dir` saves each console's link test log (`dir/cN.link`) and
`LINKLOG_RESET=n` empties them at frame n of each console (the host's fills
up with lone transfers before the clients are there): with the real time
cable the logs cover other stretches of time, so compare them by the
parent's word of each transfer, which counts up.
`NET=listen:PORT` / `NET=IP:PORT` (with `PACE=1`) runs only the host or only
client 1, the other end being another process over TCP, on another device
(built with the Android NDK it runs from `adb shell` in `/data/local/tmp`):
the real time cable over a real network. `KEYS1` still names client 1's
buttons on its own end; `KEEPALIVE_US=n` adds an empty packet every n µs.
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
