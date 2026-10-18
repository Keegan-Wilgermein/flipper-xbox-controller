# flipper-xbox-controller

A Flipper Zero that iOS accepts as a real **Xbox Wireless Controller** over Bluetooth LE. Games see a normal
controller: both sticks, triggers, d-pad and every button, all at once. A computer feeds it input reports over
USB, so anything that can write to a serial port can play an iPhone game with a controller.

## What's here

| Path | What it is |
|---|---|
| `bridge/` | The Flipper app (FAP). Takes controller reports over USB, sends them over Bluetooth as an Xbox controller |
| `bridge/config.h` | Every name and ID the bridge presents, in one place |
| `firmware/momentum-mntm-012-long-gatt.patch` | Small patch to Momentum firmware that the Xbox identity needs (see below) |
| `host/xbox_pad.py` | Minimal sender: presses buttons and moves sticks. Standard library only |
| `host/XboxPad.swift` | The same report builder in Swift |
| `tools/` | Mac tools used to reverse-engineer the real controller: raw HID reports, GameController events, BLE advertisements |

## How the clone works

iOS only hands a Bluetooth controller to the GameController framework when it looks like a
controller it knows. Matching the PnP ID and HID descriptor isn't enough. The bridge copies a real
**model 1914** pad (Xbox Series, firmware 5.17.3202.0):

- **PnP ID** `045E:0B20`, version `0x0517`; Device Information with the real firmware string, a made-up serial in
  the same format, and no Model string (the real pad has none).
- **HID report map**: the real 283-byte descriptor (identical to the model 1914 descriptor in
  [Mystfit/ESP32-BLE-CompositeHID](https://github.com/Mystfit/ESP32-BLE-CompositeHID)'s `XboxDescriptors.h`),
  input report 1 and rumble output report 3. HID characteristics need an encrypted link.
- **Microsoft's vendor service** `00000001-5F60-4C4F-9C83-A7953298D40D` with the values read from the real pad:
  characteristic 2 (318 bytes), 3 (32 bytes) and 4 (write).
- **Advertisement**: manufacturer data `06 00 00` (Microsoft), Appearance `0x03C4` (gamepad) in an extra beacon,
  and the full name "Xbox Wireless Controller" in the GAP Device Name characteristic.

### Why the firmware patch

Stock Flipper firmware can't do two of those things:

1. **GATT values over 255 bytes are cut short.** `aci_gatt_update_char_value` takes a one-byte length, so the
   283-byte report map arrived as 27 bytes and iOS ignored the device. The patch writes long values in 240-byte
   chunks with `aci_gatt_update_char_value_ext`.
2. **Names are capped at 20 characters** ("Xbox Wireless Contr"). The patch reserves 32 bytes for the GAP Device
   Name and adds `furi_hal_bt_set_device_name()` (API 87.2), which the bridge calls after its profile starts.

## Build and install

Needs a Flipper Zero, a Mac or Linux machine, and an iPhone (tested on an iPhone 16 Pro).

Patched firmware:

```bash
git clone --branch mntm-012 https://github.com/Next-Flip/Momentum-Firmware.git momentum
cd momentum
git apply ../flipper-xbox-controller/firmware/momentum-mntm-012-long-gatt.patch
./fbt updater_package    # flash dist/f7-C/flipper-z-f7-update-mntm-012.tgz with qFlipper
```

The app, built with [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) against the patched SDK (the stock
SDK lacks `furi_hal_bt_set_device_name`, so the build and editor both fail without it):

```bash
pip install ufbt
ufbt update --local=../momentum/dist/f7-C/flipper-z-f7-sdk-mntm-012.zip
cd bridge
ufbt vscode_dist         # editor config that finds the Flipper headers
ufbt launch              # build, copy to the Flipper and open it
```

If qFlipper's update fails with `FS R/W error [2-0]`, put the Flipper in DFU mode and flash
`dist/f7-C/flipper-z-f7-full-mntm-012.dfu` with `dfu-util` instead.

Then open **Apps → Bluetooth → Xbox Bridge**, and:

```bash
python3 host/xbox_pad.py demo
```

Pair **Xbox Wireless Controller** from the iPhone's Bluetooth settings. Settings → General → Game Controller
appears once iOS has accepted it.

## USB protocol

The bridge listens on the Flipper's second USB serial channel. Frames both ways: `0xA5, type, length, payload`.

| Type | Direction | Payload |
|---|---|---|
| `0x02` | → Flipper | ping, any bytes. Answered with `0x82`: same bytes + tick |
| `0x03` | → Flipper | status request. Answered with `0x81`: Bluetooth state, reports sent (u32), errors (u32), startup checks |
| `0x04` | → Flipper | quit |
| `0x10` | → Flipper | input report 1 below, without its report ID |

### Xbox input report 1 (16 bytes, little-endian)

| Bytes | Field |
|---|---|
| 0–7 | LX, LY, RX, RY: u16, `0x8000` centre, Y down (`0x0000` up, `0xFFFF` down) |
| 8–11 | LT, RT: 10 bits in a u16 (`0x03FF` full) |
| 12 | Hat: 1 up, then clockwise to 8 up-left; 0 released |
| 13–14 | Buttons (u16): A 0, B 1, X 3, Y 4, LB 6, RB 7, View 10, Menu 11, Xbox 12, L3 13, R3 14 |
| 15 | Share: bit 0 |

## Known limits

- **View, Menu, L3, R3 and Share don't reach third-party apps' GameController handlers** from the clone (the test
  app's watcher never sees them), but games do get them. Remapping them in iOS's controller settings to other buttons
  proved they arrive. Cause unknown.
- The Xbox button opens Game Center, like on a real pad.
- A normal Momentum update replaces the patched firmware.
- Only tested with one iPhone and one borrowed real controller.

## Licence

GPL-3.0, like the Momentum firmware the bridge and patch are built on. The descriptor and service values were read
from a real controller; thanks to Mystfit's ESP32-BLE-CompositeHID for the descriptor that confirmed them.
