# JAMMA64 – Project Specification

## Project Overview

JAMMA64 is a firmware project for the **Raspberry Pi Pico 2 W** that converts standard **JAMMA arcade cabinet controls** into **Nintendo 64 controller signals**.

The system is designed primarily for use with the **Aleck64 arcade platform**, which uses standard N64 controller protocols but is installed in JAMMA cabinets with digital joysticks and buttons.

The firmware reads digital inputs from:

- 8-way arcade joysticks
- Up to 6 buttons per player
- Start buttons
- Coin, service, and test inputs

It then converts those inputs into:

- N64 controller button states
- Either D-pad or analog stick signals

All behavior is configurable through a **web interface** hosted directly on the Pico W.

---

## Hardware Context

### Cabinet Side

Standard JAMMA harness provides:

- Player 1:
  - Up, Down, Left, Right
  - Buttons 1–3 (JAMMA)
- Player 2:
  - Same as Player 1
- System:
  - Coin 1 / Coin 2
  - Service
  - Test
- Power:
  - +5V supply from JAMMA harness

### Additional Inputs

A **Capcom CPS-2 kick harness** provides:

- Buttons 4–6 for each player

### Output Side

The board connects to:

- Two Nintendo 64 controller ports
- Or directly to the Aleck64 PCB controller headers

Each port uses:

- Data line
- +3.3V
- Ground

---

## Core Functional Goals

### 1. Dual N64 Controller Emulation

The Pico must emulate:

- Player 1 N64 controller
- Player 2 N64 controller

Using:

- N64 serial protocol
- Accurate timing
- Low-latency responses

Both controllers must run simultaneously on one Pico 2 W.

---

### 2. Digital Joystick to Analog Stick Conversion

Each player uses a **single 8-way digital joystick**.

The firmware must support two modes:

#### D-pad Mode
- Digital switches map directly to:
  - N64 D-pad Up/Down/Left/Right

#### Analog Mode
- Same digital switches drive the analog stick.
- Directions convert to full-throw analog values:

Examples:

| Input | Analog Output |
|------|--------------|
| Up | Y = +max |
| Down | Y = −max |
| Left | X = −max |
| Right | X = +max |
| Up + Right | X = +max, Y = +max |

The **throw amount** must be adjustable via the web interface.

---

### 3. Configurable Button Mapping

All inputs must be mappable to N64 controls.

Possible input sources:

- P1/P2 buttons 1–6
- Start buttons
- Coin inputs
- Service and test switches

Possible N64 outputs:

- A
- B
- Z
- L
- R
- C-Up/Down/Left/Right
- D-pad
- Start

Mappings must be:

- Configurable in the web UI
- Saved as **per-game profiles**

---

### 4. Per-Game Profiles

The system must support multiple profiles.

Each profile contains:

- Button mappings
- Stick mode (analog or D-pad)
- Analog throw value

Profiles must:

- Be selectable from the web interface
- Be stored in flash memory
- Persist across power cycles

---

### 5. Web-Based Configuration Interface

The Pico W hosts a small HTTP server.

The interface must allow:

- Viewing current input state
- Changing stick mode
- Adjusting analog throw
- Remapping buttons
- Selecting profiles
- Saving settings to flash

The UI should:

- Be simple HTML/JS
- Work on phones and desktops
- Not require external servers

---

### 6. Wi-Fi Configuration via USB Drive

To avoid build-time credentials:

The Pico must present a **USB mass storage device**.

The user will:

1. Plug Pico into PC or phone.
2. See a drive named `JAMMA64`.
3. Create or edit:


The firmware must:

- Detect this file
- Parse credentials
- Save them to flash
- Reboot automatically

If no credentials are present:

- Device stays in USB configuration mode
- Web server is not started

---

## Software Architecture

The firmware is divided into modules.

---

### `main.c`

Responsibilities:

- System initialization
- Wi-Fi connection
- USB config mode
- Main loop
- Calling input and N64 routines

---

### `inputs.c`

Responsibilities:

- GPIO setup
- Internal pull-ups
- Reading all JAMMA inputs
- Debouncing
- Returning structured input state

---

### `profile.c`

Responsibilities:

- Profile storage
- Flash read/write
- Default profile
- Mapping tables
- Stick mode settings

---

### `web.c`

Responsibilities:

- HTTP server setup
- REST or CGI endpoints
- JSON responses
- Profile editing
- Mode switching

---

### `wifi_config.c`

Responsibilities:

- Parsing `wifi.txt`
- Validating credentials
- Saving to flash
- Loading credentials at boot

---

### `usb_msc.c`

Responsibilities:

- TinyUSB mass storage setup
- Presenting virtual disk
- Accepting `wifi.txt` writes
- Triggering credential parsing

---

## Technical Constraints

### Hardware

- Single Raspberry Pi Pico 2 W
- Powered from JAMMA +5V
- Must use internal pull-ups for switches

---

### Timing

N64 protocol requires:

- Microsecond-level timing
- Deterministic response

Preferred implementation:

- PIO-based serial engine
- Or cycle-accurate bit-banging

---

### Performance Targets

| Metric | Target |
|--------|--------|
| Input scan rate | ≥1 kHz |
| N64 response latency | <1 ms |
| Boot to Wi-Fi connected | <5 seconds |

---

## Safety and Electrical Goals

- No voltage from Pico should back-feed into JAMMA lines.
- All inputs must be:
  - Pulled up internally
  - Ground-switched through cabinet controls

N64 data lines must use:

- Proper logic levels
- Series resistors if required

---

## Development Milestones

### Phase 1 – Core Hardware
- GPIO input reading
- Basic serial debug output
- Single N64 controller output

### Phase 2 – Dual Controller
- Two N64 ports working simultaneously
- Stable timing

### Phase 3 – Web Interface
- Wi-Fi connection
- Basic status page
- Stick mode toggle

### Phase 4 – Profiles
- Button remapping
- Profile storage in flash
- Profile selection UI

### Phase 5 – USB Wi-Fi Setup
- Mass storage device
- `wifi.txt` parsing
- Automatic reboot after config

---

## Stretch Goals

Optional future features:

- mDNS hostname (e.g. `jamma64.local`)
- OTA firmware updates
- USB HID controller mode
- Input test screen with live indicators
- Auto-detect game and switch profiles

---

## Project Philosophy

This firmware should be:

- Deterministic
- Arcade-reliable
- Low-latency
- Easy to configure
- Minimal dependencies
- Fully self-contained
