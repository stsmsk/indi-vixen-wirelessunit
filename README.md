# INDI Vixen Wireless Unit

Experimental INDI telescope and guider driver for the Vixen Wireless Unit.
The current release is **0.1.138** and has been developed for an SXP2 mount,
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
sudo apt install ./build/indi-vixen-wirelessunit_0.1.138-1_arm64.deb
```

## Raspberry Pi deployment and log capture

The complete Pi-side test workflow can be run with one command. It builds and
tests the Debian package, installs it, starts a verbose INDI server, captures
Wireless Unit UDP traffic, and writes system snapshots before and after the
test:

```bash
./scripts/deploy-and-capture-rpi.sh --interface wlan0
```

Use `any` (the default) instead of a specific interface when traffic may pass
through either an Access Point or Station interface. Configure KStars/Ekos to
connect to the Raspberry Pi on port 7624, perform the test, and press Ctrl+C in
the script's terminal. The script then stops only the processes that it
started and saves the readable INDI and packet logs, the raw PCAP, and system
information under `log/run-TIMESTAMP/`.

An existing INDI server is never stopped automatically. If port 7624 is in
use, stop that server yourself or select an isolated port, for example:

```bash
./scripts/deploy-and-capture-rpi.sh --server-port 7625 --duration 300
```

Run `./scripts/deploy-and-capture-rpi.sh --help` for build, installation,
capture, and log-directory options. The script requires `tcpdump` when packet
capture is enabled.

### Run the complete workflow remotely

From the source tree on a Mac or another development computer, the following
command transfers a clean source copy to a newly created directory on the Pi,
runs the complete workflow there, and retrieves its logs afterward:

```bash
./scripts/remote-deploy-and-capture-rpi.sh \
  --host masuko@trixie.local -- --interface any
```

Arguments after `--` are passed to the Pi-side script. Each invocation uses a
new directory below `~/build/codex/indi-vixen-wirelessunit-runs/`; existing Pi
sources and logs are not removed. When run directly in a terminal, the script
automatically permits the Raspberry Pi sudo password prompt. Password input is
not displayed while typing. Non-interactive execution requires passwordless or
cached remote sudo; `--non-interactive` can be used to require that behavior
explicitly.

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
