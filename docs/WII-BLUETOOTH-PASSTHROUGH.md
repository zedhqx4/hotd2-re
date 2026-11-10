# USB Bluetooth passthrough for native Wii Remotes

HOTD2RE now has a direct USB HCI host backend for Wii Remotes. It feeds the existing
`input::wii::Remote` / `Snapshot` input path; it does not emulate the Wii's IOS Bluetooth
device and does not copy Dolphin's implementation.

The backend is selected automatically when HOTD2RE can open and claim a standard USB
Bluetooth HCI interface through libusb. If no claimable USB HCI adapter is found, HOTD2RE
keeps using its existing Windows Bluetooth/HID backend (including DolphinBar Mode 4).

## Adapter setup on Windows

Use a dedicated USB Bluetooth adapter for passthrough. For the tested CSR8510 A10:

- USB VID:PID: `0A12:0001`
- Install the `libusbK` driver on that adapter with Zadig.
- Do **not** replace the driver for the Intel Bluetooth adapter used by Windows.
- Close other applications that are controlling the same CSR8510 while HOTD2RE is using it.

The CSR8510 is then exclusively owned by HOTD2RE while Wii Remote support is enabled.
Windows Bluetooth will no longer use that particular adapter; the separate Intel radio
remains available to Windows.

## Connect

1. Enable Wii Remote support in **Options > Wii Remotes** (or **Pause > Wii Remotes**).
2. Select **Connect remotes**.
3. Press the remote's red **SYNC** button during the search.

The USB backend performs Bluetooth Classic inquiry, remote-name validation, baseband
connection, legacy SYNC authentication, link encryption, HID L2CAP setup and HIDP report
configuration. It currently supports up to two remotes. For this direct USB backend, use
the red SYNC button; the 1+2 guest-pairing path remains available through the Windows HID
backend.

## Transport and timing

The host backend uses:

- HCI commands on the USB control pipe.
- HCI events on the USB interrupt IN endpoint.
- ACL data on the USB bulk IN/OUT endpoints.
- HID Control (PSM `0x0011`) and HID Interrupt (PSM `0x0013`) L2CAP channels.
- Continuous HID report `0x33` for core buttons, accelerometer and extended IR.
- The existing HOTD2RE report decoder and four-point aim calibration.

For compatibility with the tested CSR8510 family, the backend requests HCI QoS
`GUARANTEED` using the same parameters as Dolphin. It also requests the Wii's Bluetooth
link policy and 5 ms Sniff interval (200 Hz report cadence). If a controller rejects
either timing request, those requests are best-effort and basic HID input can still work.

IR initialization writes the Wii Remote's sensor configuration registers before selecting
report `0x33`. A powered infrared sensor bar is required for pointing.

## Pairing keys

When the remote sends `HCI Link Key Notification`, HOTD2RE saves the key locally and
restores known keys after HCI Reset. The file is:

```text
%LOCALAPPDATA%\HOTD2RE\wii_link_keys.txt
```

It contains Bluetooth addresses and link keys in hexadecimal. Treat it as a credential:
do not publish it or include it in bug reports. It is separate from `hotd2re_wii.ini`,
which stores player bindings and aim calibration.

## Build and diagnostics

The runtime backend and probes are included when configuring Windows builds with:

```powershell
cmake -S . -B build -A Win32 -DHOTD2_WII_USB_PASSTHROUGH=ON
cmake --build build --config Release --target hotd2 hotd2re wii_remote_test wii_bluetooth_probe wii_hci_probe
```

For the supported x64 port build, add `-DHOTD2_ALLOW_64BIT=ON` and use `-A x64`.
The `wii_bluetooth_probe` and `wii_hci_probe` utilities only inspect the adapter and HCI
command/event transport; they do not pair a remote.

## Current scope and validation

The current USB backend targets standard USB Bluetooth HCI adapters and Nintendo
RVL-CNT-01 / RVL-CNT-01-TR remotes. It supports core buttons, accelerometer and IR
pointing for two players. MotionPlus, Nunchuk/Classic Controller extensions, speaker
audio, Balance Board and vendor-specific HID extensions are not implemented in this
backend. The existing Windows HID backend remains available as the fallback.

The standalone USB diagnostic has already completed real hardware authentication,
encryption, L2CAP setup and received core-button reports from a CSR8510 A10. The new
HOTD2RE runtime backend has a separate compile/test gate; it still requires its own
physical in-game validation before being called hardware-verified.
