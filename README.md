JAMMA64
Convert JAMMA arcade controls to Nintendo 64 controllers
using a Raspberry Pi Pico 2 W.

Features
- Dual N64 controller emulation
- Digital-to-analog stick conversion
- Web-based configuration
- Wi-Fi setup via USB mass storage

Hardware
- Raspberry Pi Pico 2 W
- JAMMA harness
- N64 controller ports

Build
mkdir build
cd build
cmake -DPICO_BOARD=pico2_w ..
ninja
