# Third-Party Notices

This project can be built with an optional N64 backend that integrates code
from the following project:

- Project: `joybus-pio`
- Repository: https://github.com/JonnyHaystack/joybus-pio
- Author: JonnyHaystack and contributors
- License: GNU Lesser General Public License v3.0 or later (LGPL-3.0-or-later)

## How it is used here

When `JAMMA64_N64_BACKEND=joybus` is selected, this project builds against
`joybus-pio` source files from a local checkout provided via
`JAMMA64_JOYBUS_PIO_DIR`.

## License text location

The `joybus-pio` license text is provided in that upstream repository in its
`LICENSE` file.
