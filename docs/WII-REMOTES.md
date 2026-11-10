# Native Wii Remote input

HOTD2RE supports two separate Wii Remote connection paths.

- **Windows Bluetooth HID (default):** ordinary Windows Bluetooth pairing and DolphinBar Mode 4. This path does not require the passthrough DLL or libusbK.
- **USB Bluetooth passthrough (optional):** selected explicitly in Options > Wii Remotes. The game loads `wii_bt_passthrough.dll` from the same directory as `hotd2.exe` / `play.exe`. It also requires the matching-architecture `libusb-1.0.dll` in that directory; both are shipped together as optional runtime files. The host DLL owns a compatible USB HCI adapter through libusbK.

If the DLL is missing, its API version is incompatible, or no claimable USB Bluetooth HCI adapter is available, HOTD2RE falls back to Windows Bluetooth HID and displays the reason in the Wii Remote status page. The game input library itself does not link libusb and does not statically include the USB transport.

Both runtime DLLs must match the game executable architecture: x86 for a 32-bit executable or x64 for a 64-bit executable. With passthrough OFF, neither DLL is loaded and the CSR8510 is not claimed by HOTD2RE.

## Adapter setup

Use a dedicated USB Bluetooth adapter for passthrough. For the tested CSR8510 A10:

- USB VID:PID: `0A12:0001`
- Install the `libusbK` driver on that adapter with Zadig.
- Do **not** replace the driver for the Intel Bluetooth adapter used by Windows.
- Close other applications that are controlling the same CSR8510 while passthrough is active.

## Connect

1. Enable Wii Remote support in **Options > Wii Remotes** (or **Pause > Wii Remotes**).
2. Leave **USB Bluetooth passthrough OFF** for ordinary Windows Bluetooth / DolphinBar use, or turn it ON for the compatible USB HCI adapter.
3. Select **Connect remotes**.
4. Press the remote's red **SYNC** button during the search.

The direct USB path performs Bluetooth Classic inquiry, remote-name validation, baseband connection, legacy SYNC authentication, link encryption, HID L2CAP setup and HIDP report configuration. It currently supports up to two remotes. The 1+2 guest-pairing path remains available through Windows Bluetooth HID.

## DLL API and transport

The game loads the DLL dynamically and resolves a versioned C API. The boundary uses fixed-width C structures only; no C++ STL objects or CRT-allocated memory cross the DLL boundary. The DLL owns its libusb context, USB HCI interface and Bluetooth worker thread.

The DLL uses HCI commands on the USB control pipe, HCI events on the USB interrupt IN endpoint, ACL data on USB bulk IN/OUT endpoints, HID Control PSM `0x0011`, HID Interrupt PSM `0x0013`, and continuous report `0x33` for core buttons, accelerometer and extended IR. A powered infrared sensor bar is required for pointing.

## Pairing keys

When the remote sends `HCI Link Key Notification`, the passthrough runtime saves the key locally and restores known keys after HCI Reset. The file is:

```
%LOCALAPPDATA%\\HOTD2RE\\wii_link_keys.txt
```

It contains Bluetooth addresses and link keys in hexadecimal. Treat it as a credential: do not publish it or include it in bug reports. It is separate from `hotd2re_wii.ini`, which stores player bindings, calibration and the passthrough selection.

## Build and validation

Configure Windows with `-DHOTD2_WII_USB_PASSTHROUGH=ON` to build the optional DLL and low-level adapter probes. Build targets:

```powershell
cmake --build build --config Release --target hotd2 play wii_remote_test wii_bt_passthrough wii_bt_passthrough_api_test wii_bluetooth_probe wii_hci_probe
```

The passthrough DLL and its `libusb-1.0.dll` dependency are emitted alongside the game executables. Build Win32 for x86 executables/DLLs or x64 with `-DHOTD2_ALLOW_64BIT=ON`. Packaging admits the pair only when both PE architectures match and includes libusb's `COPYING` license. The API smoke test loads the DLL and checks its exports, version, disabled lifecycle and snapshot ABI without requiring a Bluetooth adapter or Wii Remote. This is not physical hardware validation.

The standalone diagnostic project `WIimoteDLL_Libusbk` remains a separate protocol diagnostic. Its demonstrated USB HCI, authentication, L2CAP and HIDP path informed the runtime transport implementation; the game now consumes that transport through an optional DLL. The in-game passthrough still requires physical validation with the CSR8510 and Wii Remote.

## Scope

The passthrough backend targets standard USB Bluetooth HCI adapters and Nintendo RVL-CNT-01 / RVL-CNT-01-TR remotes. MotionPlus, Nunchuk/Classic Controller extensions, speaker audio, Balance Board and vendor-specific HID extensions are not implemented. Windows Bluetooth HID remains available without the optional DLL.
