# INDI Vixen Wireless Unit

Experimental INDI telescope and guider driver for the Vixen Wireless Unit.
The current release is **0.1.137** and has been developed for an SXP2 mount,
Wireless Unit firmware 2.60, Raspberry Pi OS Trixie, and INDI 2.2.3.1
(public INDI API version 2.2.3).

## Features

- UDP discovery and connection to the Wireless Unit, retrying every five seconds until cancelled
- persistent Automatic discovery or Manual IP connection mode for routed networks
- equatorial coordinates, GoTo, Sync, tracking, Park, and Unpark
- manual N/S/E/W motion with `0.5x`, `1x`, `2x`, `4x`, `10x`, `20x`,
  `40x`, `100x`, `200x`, and `400x` rates
- pulse guiding and guide-rate configuration
- backlash, motor-power, polar-alignment, GoTo-speed, and meridian settings
- alignment-list display and deletion
- Wi-Fi Station and Access Point configuration
- explicit HOME initialization, plus an opt-in Assume HOME on Connect mode
- Simulation mode and automated protocol/driver tests

## Safety

The driver controls motorized astronomy equipment. Test with the mount
unloaded and attended before normal use. Keep cable clearance and mechanical
limits in view. Do not enable **Assume HOME** unless the mount has first been
placed physically at azimuth 270 degrees and altitude 0 degrees.

## Raspberry Pi OS Trixie build

Install the INDI 2.2.3.1 development package and the normal C++/CMake build
dependencies, then run:

```bash
./scripts/build-rpi-trixie.sh
sudo cmake --install build-rpi
```

The installed files are:

```text
/usr/bin/indi_vixen_wirelessunit
/usr/share/indi/indi_vixen_wirelessunit.xml
/usr/share/man/man1/indi_vixen_wirelessunit.1.gz
```

## Debian package

On Raspberry Pi OS Trixie:

```bash
./scripts/build-deb-trixie.sh
sudo apt install ./build/indi-vixen-wirelessunit_0.1.137-1_arm64.deb
```

## Protocol-only development build

The library and platform-independent tests can be built without libindi:

```bash
cmake -S . -B build-local \
  -DVIXEN_BUILD_INDI_DRIVER=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-local
ctest --test-dir build-local --output-on-failure
```

Building the actual driver requires INDI 2.2.3 or later. Simulation exercises
the driver without sending commands to physical equipment.

## License

Copyright (C) 2026 Satoshi Masuko `<wakeup1002@gmail.com>`.

Licensed under the GNU General Public License, version 3 or later. See
[COPYING](COPYING).
