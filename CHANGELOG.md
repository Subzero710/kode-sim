# Changelog

What changes from one kode-sim release to the next. The versions follow
[SemVer](https://semver.org): until 1.0 a minor release (0.2, 0.3…) may
change how kode-sim is used, and says so here.

## [Unreleased]

### Fixed

- **The USB host takes nothing from a Kode Dot that restarted until its new
  boot raises its pull-up.** A sector of a read under way when the firmware
  restarted could reach the computer after the restart, and the new boot's
  enumeration took it for its own data: "a bad command status wrapper". It
  showed on a slower machine (GitHub's Linux runner).

## [0.2.0] - 2026-11-13

kode-sim is open source (GPL-3.0-or-later), and a window of its own. On
esp-emu 1.0.0, Espressif's first stable release, the sound leaves kemu: the
firmware's own I2S driver runs on esp-emu's I2S, and kode-sim is the
ES8311's end of the data pads.

### Added

- **A window of its own.** `kode-sim run` opens the Kode Dot in Chrome or
  Edge in app mode, with a profile in the cache: no tabs or address bar, the
  sound and the microphone without a click or a prompt, and closing the
  window stops the simulator. `$KODE_SIM_BROWSER` names another browser;
  without one, the page opens as a tab, as before.
- **The window shows only the Kode Dot.** "Ajustes" slides out the
  controls and the console. The sound starts as the page connects, where
  the browser allows it, and the Mac's microphone is on unless it was
  turned off, which the page remembers.
- **Open source:** the GPL-3.0 licence, `NOTICE.md`, issue forms (a bug, a
  feature, and "I want to work on this", so that work on a simulator happens
  once, here), a pull request template, and Dependabot. The README says
  what works, what waits for Espressif and what is planned; the details are
  in `docs/GUIDE.md`.
- `kode-sim --version`.

- The USB-OTG port. A TinyUSB app (SD Reader, BadUSB...) runs unchanged over a
  device controller kemu models (`dcd_*`), with a simulated computer on the
  cable that enumerates it, mounts a mass-storage device's FAT volume (list,
  copy in, read back, eject, pull the cable) and types a keyboard's reports as
  text, on a Spanish or a US layout. On the page: "Ordenador (USB)".
- The IMU (QMI8658A) and the magnetometer (QMC5883P) as chips, posed from the
  page: pitch, roll, heading, a magnet and a shake, sent as a `MOTION` frame
  (0x8D). A compass, a level and a metal detector run on them.
- `?embed=1`: the page inside another page (an iframe) - the device takes the
  room the frame gives it, the side panel is one row (Más / Reset / Encender,
  with the rest behind Más), and the page tells its parent it is in use
  (`postMessage({kodeSim: "activity"})`, at most every 10 s) so a host can
  stop a simulator nobody touches.

### Fixed

- **A kode-sdk updated in place no longer breaks the next build.** The copy
  kode-sim builds in kept the dependencies.lock solved against the old
  kode-sdk, which could point at a component that is gone (kode-sdk 0.1.1
  dropped `rfal`): every build then failed in CMake. The lock is solved
  again whenever the kode-sdk's components change.
- **The tests run against the newest kode-sdk and the official KodeOS**
  (kodediy/KodeOS, its default branch), brought up to date at every
  session; they were left on a first clone of kode-sdk and on kode-dot-os,
  which stopped in October.
- **`kode-sim install` speaks KodeOS's console:** `install src="terminal"
  dst=app run=on` with the image's project name, so an older copy of the
  app gives its place first (kode-dot-os's `app_install` is gone).
- **`review` and the tests know a boot by kode-sdk's or esp-emu's line**
  too: KodeOS keeps ESP-IDF's "Project name:" quiet.
- **KodeOS's factory test (`CONFIG_KODE_TEST_DOT`) is off in the
  simulator,** as the radio it needs is.

### Changed

- **The I2S's word clock stays off its pad in the simulator.** esp-emu 1.0.0
  drives WS on GPIO 37 and sends kode-sim every edge over `--cosim-tcp`,
  some 96,000 messages a second of emulated time while anything plays,
  which kode-sim only dropped. kemu (`kemu_i2s.c`) configures the channels
  as asked but with WS unrouted; the sound and the microphone are unchanged
  and the GPIO 37 messages are gone (esp-emulator#52).
- **esp-emu 1.0.0** (was 1.0.0-rc2), installed by kode-sim on its first run
  as before. The emulator suite passes on it unchanged.
- **The sound is esp-emu's I2S.** `kemu_audio` is gone: kdrv_es8311's I2S
  channels and esp_codec_dev's data interface run as on the board. kode-sim
  claims the codec's data pads (DSDIN 38, ASDOUT 33) over `--cosim-tcp`: the
  speaker reaches the page mono at the DAC's volume, as before, and silence
  is no longer sent; the microphone is fed 5 ms ahead of emulated time with
  the Mac's microphone, the speaker through the case (-20 dB, 11 ms late)
  and the ADC's floor. kodeOS's audio self-test passes on it.
- **The Mac's microphone is a switch on the page,** "Micrófono del Mac",
  for every page at once. The firmware opens its receiver with every sound,
  so it can no longer say when it records; off, the Kode Dot hears its own
  speaker and its floor, which is what kodeOS's audio self-test needs. A
  reset leaves the switch as it was; the board going off turns it off.

## [0.1.0] - 2026-11-07

The first release: the base to build on. A Kode Dot on the Mac, in Rust, on
Espressif's esp-emu 1.0.0-rc2 and its virtual parts. What esp-emu cannot do
yet - the display, sound, the microSD and the C5 - kemu does inside the
firmware; [docs/ROADMAP.md](docs/ROADMAP.md) says what each next release
waits for.

### The simulator

- `kode-sim run`: builds kodeOS or a kode-sdk app for the simulator, boots
  it in esp-emu and opens the Kode Dot in the browser. On the page: the
  410×502 glass with touch, the six buttons, the RGB LED, the haptic, the
  speaker and the microphone through the Mac, the battery slider and the USB
  switch, the microSD slot, the console, and Reset and power.
- The board's chips are answered in-process over esp-emu's `--cosim-tcp`,
  as the kode-sdk drivers expect them: TCA6408A (five buttons, the touch
  and IMU IRQs, the panel's power), CST820, KTD2026, AW86233, BQ25896 and
  CW2217 (one battery), ES8311. TOP is GPIO35.
- The console is the terminal, the page and a serial port, so kode-dot-os's
  `kcli.py` works on it as on the board.
- A network for apps that ask for WiFi (the radio's signals over the P4's
  emulated Ethernet), `--forward` for inbound ports, `--no-wifi`,
  `--no-internet`.
- The microSD is a card image on the Mac, kept across runs (`--sd`,
  `--no-sd`, "SD insertada" on the page).
- The P4's RTC is set from the Mac's at every boot.
- `--exit-on-crash` for scripts and CI, `--save-state` to keep the flash,
  `--port` (0 lets the Mac pick).

### Apps

- `kode-sim build` (`--json`, `--board`, `--sdk`): builds in a copy in the
  cache and never touches the project.
- `kode-sim install`: into the running kodeOS over its console, as kcli.py
  does, or `--mailbox` into app_pending and a reset, as an Arduino upload
  does.
- `kode-sim review`: an app put through a scenario in a Kode Dot of its own,
  headless, with a verdict, the console log and screenshots (for
  apps.kode.diy).

### Fixed before the release, from a full review

- **Boots:** a boot after a reset that cut a frame short is always seen.
  The decoder starts again on a reset, and on a whole HELLO inside a cut
  frame.
- **Shutdown:** stopping kode-sim leaves no esp-emu running, also while a
  page is still booting one, or while a review runs (SIGTERM, SIGINT,
  SIGHUP). `review` then exits 128 + the signal and removes its temporary
  files.
- **Installs:**
  - only one runs at a time, even after the page that asked for it leaves;
  - a mailbox upload resets the chip only after its write was taken;
  - `--mailbox` spares kodeOS's intent sector (at most 0x5FF000 bytes);
  - an app too big for the factory slot is refused at build time, with
    both sizes.
- **The card and the saved flash:** a card image, and a project's
  `--save-state` flash, are in one simulator at a time. A write the image
  refuses is reported on the console.
- **The SD protocol:** every read carries a tag, so a late answer to a read
  kemu gave up on is never taken as the next read's data.
- **The console's serial port:** writes whole lines or drops them and
  counts them (`api/stats`).
- **A closed terminal:** no longer panics kode-sim.
- **The page:**
  - a stuck page is bounded in memory and reconnects to the whole picture;
  - a page that joins late gets the microphone's state and the cleared
    screen;
  - a reconnect never throws;
  - a tap and then a hold is one press, on a button and on the glass;
  - a microphone that arrives after a MIC_STOP is stopped, and one audio
    unlock at a time, so the microphone always finds its worklet;
  - the console draws once per animation frame.
- **Uploads:** the page's upload checks its Origin before reading the body.
- **kemu, in the firmware:**
  - TOP stays a button with WiFi up: the emulated EMAC's TXD0/TXD1 moved
    from GPIO34/35 to 41/42;
  - a frame from the Mac is handed on as soon as its last byte is in, not
    20 ms later: installing a 1.5 MB app takes 2.5 s instead of 15.6 s;
  - a flip compares only the rows kdrv_display hands over (502 → 193 rows
    on average, about half the cycles per frame);
  - the panel says vsync at every refresh, so the way home from an app
    keeps the glass lit;
  - the speaker crosses the wire mono, half the bytes.
- **Builds:**
  - an unchanged project builds again without configuring again or solving
    its dependencies again;
  - two builds of one project take turns instead of building over each
    other;
  - nested `build/` folders are source;
  - a missing ESP-IDF says what was looked for and how to install it.
- **`review`:**
  - Python's patterns work (lookarounds, backreferences, a stray `{`);
  - holds and taps are timed on the emulated clock, across a chip reset
    too;
  - times out of range and screenshot names outside the out folder are
    refused with exit 2, not a panic.
- **Tests:**
  - a test whose page closed fails instead of passing vacuously;
  - a simulator that never comes up is stopped;
  - the tests use the esp-emu the binary pins.

### Known limitations

- The display, sound, the microSD and the C5 are kemu's, not esp-emu's, so
  a binary built for the board boots with working chips but no screen.
  `kode-sim build` links kemu in.
- WiFi is the P4's Ethernet behind the radio's first four requests. Scans,
  joins, the vault, BLE and IR are not there.
- The IMU, magnetometer, NFC, 125 kHz RFID and USB-OTG are absent.
- kemu's link to the Mac is UART1, which esp-emu bridges, so an app's
  second expansion UART fails in the simulator only.
- Made for the Mac (Apple silicon); the host suites also run on Linux.

[0.1.0]: https://github.com/kodediy/kode-sim/releases/tag/v0.1.0
