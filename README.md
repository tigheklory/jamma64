# JAMMA64

Convert JAMMA arcade controls to Nintendo 64 controllers using a Raspberry Pi
Pico 2 W.

Features
- Dual N64 controller emulation
- Digital-to-analog stick conversion
- Web-based configuration
- Wi-Fi setup via USB mass storage
- Selectable N64 backend:
  - `builtin` (current in-tree backend)
  - `joybus` (integration of `JonnyHaystack/joybus-pio`)

Hardware
- Raspberry Pi Pico 2 W
- JAMMA harness
- N64 controller ports

Build
```bash
mkdir build
cd build
cmake -DPICO_BOARD=pico2_w ..
ninja
```

Build with joybus backend:
```bash
cmake -S . -B build -G Ninja \
  -DPICO_BOARD=pico2_w \
  -DJAMMA64_N64_BACKEND=joybus \
  -DJAMMA64_JOYBUS_PIO_DIR=/path/to/joybus-pio
cmake --build build --target jamma64.uf2 -j4
```

## Third-party acknowledgment

This project includes optional integration with:
- `joybus-pio` by JonnyHaystack:
  https://github.com/JonnyHaystack/joybus-pio

`joybus-pio` is licensed under LGPL-3.0 (or later). See
`THIRD_PARTY_NOTICES.md` for attribution and licensing details.
