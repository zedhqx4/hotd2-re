# USB Bluetooth passthrough (optional DLL)

This document is retained as a compatibility link for older development notes.

The USB Bluetooth HCI backend is **not statically linked into `hotd2.exe` or `play.exe`**. It is built as the optional `wii_bt_passthrough.dll`, alongside its dynamically linked `libusb-1.0.dll` dependency. Both are loaded dynamically only when **Options > Wii Remotes > USB Bluetooth passthrough** is enabled.

The default connection path remains Windows Bluetooth HID, including normal Windows-paired Wii Remotes and DolphinBar Mode 4. Users without a compatible USB HCI adapter do not need the DLL and should leave passthrough OFF.

For current adapter setup, runtime behavior, API, build targets, x86/x64 requirements and validation status, see [WII-REMOTES.md](WII-REMOTES.md).

Important hardware notes remain unchanged for passthrough users:

- Tested adapter: CSR8510 A10, USB VID:PID `0A12:0001`.
- Install libusbK only on the dedicated passthrough adapter with Zadig.
- Do not replace the driver for the Intel Bluetooth adapter used by Windows.
- The DLL and game executable must have the same architecture.
- The CI DLL/API smoke test is not a substitute for physical validation with the CSR8510 and a Wii Remote.
