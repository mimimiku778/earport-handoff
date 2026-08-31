# EarPort Handoff

GNOME AirPods integration with experimental Apple/Linux audio handoff, AirPods Max 2 support, battery status, noise control, and automatic pause/resume on wear detection.

[![CI](https://github.com/mimimiku778/earport-handoff/actions/workflows/ci.yml/badge.svg)](https://github.com/mimimiku778/earport-handoff/actions/workflows/ci.yml)
![GNOME 46–50](https://img.shields.io/badge/GNOME-46--50-blue)
![License](https://img.shields.io/badge/license-GPL--3.0--or--later-green)

This is an experimental fork of [Anoryth/EarPort](https://github.com/Anoryth/earport). It uses one integrated Apple Accessory Protocol (AAP) connection, so battery, ANC, wear detection, and handoff do not compete for the same Bluetooth channel.

![EarPort Extension](extension.png)

## What it does

- Claims the AirPods audio source when an MPRIS player starts on Linux.
- Pauses Linux players when an iPhone, iPad, or Mac takes the AirPods.
- Resumes only the players it paused when the Apple device releases them.
- Does not steal the AirPods during a call on another device.
- Delays handoff resume until the headphones are worn again.
- Shows battery state and controls ANC, Transparency, Adaptive mode, and Conversation Awareness where supported.
- Supports two paired AirPods and follows the most recently connected pair. Only one AAP control channel is active at a time.

## Supported models

| Model | Battery | Wear detection | ANC | Adaptive | Handoff |
|---|:---:|:---:|:---:|:---:|:---:|
| AirPods 1 / 2 | ✓ | ✓ | — | — | ✓ |
| AirPods 3 | ✓ | ✓ | — | — | ✓ |
| AirPods 4 | ✓ | ✓ | — | — | ✓ |
| AirPods 4 with ANC | ✓ | ✓ | ✓ | ✓ | ✓ |
| AirPods Pro | ✓ | ✓ | ✓ | — | ✓ |
| AirPods Pro 2 / 3 | ✓ | ✓ | ✓ | ✓ | ✓ |
| AirPods Max (Lightning / USB-C) | ✓ | ✓ | ✓ | — | ✓ |
| **AirPods Max 2 (A3454)** | ✓ | Experimental | ✓ | ✓ | ✓ |

AirPods 4 model numbers were already represented upstream and are now covered by tests. Max 2 support adds model `A3454` and protocol ID `0x2D20`. Max 2 keeps its raw two-slot AAP wear state; the single-sensor workaround is intentionally limited to first-generation AirPods Max.

## Ubuntu installation

### 1. Install dependencies

```bash
sudo apt update
sudo apt install -y \
  git meson ninja-build pkg-config \
  libglib2.0-dev libbluetooth-dev \
  pulseaudio-utils
```

PipeWire is supported through its PulseAudio compatibility service.

### 2. Make BlueZ identify as an Apple Bluetooth host

Back up the system configuration:

```bash
sudo cp /etc/bluetooth/main.conf /etc/bluetooth/main.conf.pre-earport
sudoedit /etc/bluetooth/main.conf
```

Add this line inside the existing `[General]` section (do not add a second `[General]` section):

```ini
DeviceID = bluetooth:004C:0000:0000
```

Restart Bluetooth:

```bash
sudo systemctl restart bluetooth
```

The DeviceID setting is system-wide. AirPods cache it at pairing time, so remove each AirPods pair from Ubuntu and pair it again after changing the setting.

### 3. Install EarPort Handoff

```bash
git clone https://github.com/mimimiku778/earport-handoff.git
cd earport-handoff
./install.sh
```

The installer places the daemon in `~/.local/bin`, installs a sandboxed systemd user service, and installs the GNOME extension. The daemon uses an unprivileged L2CAP `SOCK_SEQPACKET` connection and does not run as root or receive Linux capabilities.

On Wayland, log out and back in once so GNOME Shell loads the extension. Then connect either AirPods from GNOME Bluetooth settings.

Do not run [`xatuke/handoff`](https://github.com/xatuke/handoff) at the same time. Both programs would open the same proprietary AAP channel. If it was previously installed as a user service:

```bash
systemctl --user disable --now airpods-handoff.service
```

## Configuration

Handoff is enabled by default in `~/.config/earport/daemon.conf`:

```ini
[Settings]
ear_pause_mode=1
handoff_enabled=true
```

Set `handoff_enabled=false` and restart the service to retain EarPort features without automatic audio ownership:

```bash
systemctl --user restart earport-daemon.service
```

Ear-pause modes are `0` (off), `1` (pause when either side is removed), and `2` (pause only when both sides are removed).

## Current limitations

- This relies on reverse-engineered, private Apple protocols and may break after firmware or BlueZ changes.
- An AirPods Bluetooth link to Linux must already exist for EarPort to control AAP. It does not initiate a disconnected device connection solely from application playback.
- Playback detection is MPRIS-based. Browsers, Spotify, and common desktop players normally work; games and non-MPRIS applications may not trigger automatic claiming.
- Two paired AirPods are supported sequentially, not simultaneously. The newest connected device owns the single control channel.
- Max 2 model, ANC, and Adaptive capability handling are implemented and unit-tested; its AAP wear-event mapping still needs broader physical-device validation.
- Apple's complete account-aware automatic switching cannot be reproduced on Linux. This implements audio-source ownership behavior, not Apple Account integration.

## Troubleshooting

Check the service and logs:

```bash
systemctl --user status earport-daemon.service
journalctl --user -u earport-daemon.service -f
```

If BlueZ reports `br-connection-key-missing`, remove the affected AirPods from Bluetooth settings and pair it again after the DeviceID change.

If the Quick Settings item is absent on Wayland, confirm that the extension is enabled and then log out and back in:

```bash
gnome-extensions enable earport@anoryth.github.io
```

## Development

```bash
meson setup daemon/build daemon --buildtype=debug -Dwerror=true
meson compile -C daemon/build
meson test -C daemon/build --print-errorlogs
```

The daemon exposes `io.github.anoryth.EarPort` on the session bus. The existing D-Bus name and extension UUID are retained for compatibility with upstream EarPort.

See [CREDITS](CREDITS) for protocol and upstream acknowledgements.

## Uninstall

```bash
./install.sh --uninstall
```

The BlueZ DeviceID setting and Bluetooth pairings are system configuration and are intentionally not reverted by the uninstaller. Restore `/etc/bluetooth/main.conf.pre-earport` manually if desired.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). AirPods is a trademark of Apple Inc. This project is not affiliated with or endorsed by Apple.
