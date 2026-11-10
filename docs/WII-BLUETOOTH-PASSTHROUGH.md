# USB Bluetooth passthrough (development)

The passthrough backend is being developed independently from the existing Windows HID backend. The first hardware milestone validates USB access before implementing Bluetooth host behavior.

## CSR8510 probe

On Windows, install the libusbK driver for the dedicated CSR8510 adapter with Zadig. Do not replace the driver for the Intel Bluetooth device used by Windows.

Configure CMake with -DHOTD2_WII_USB_PASSTHROUGH=ON, then build the probe target (for example: cmake --build build --config Release --target wii_bluetooth_probe). Run it from the output directory:

    .\wii_bluetooth_probe.exe

The default target is USB\VID_0A12&PID_0001. A different adapter can be inspected with:

    .\wii_bluetooth_probe.exe 0xVID 0xPID

The probe enumerates matching devices, prints USB interface classes and endpoint layouts, opens the adapter, and attempts to claim/release a standard Bluetooth HCI interface. It does not send HCI commands, pair a remote, change Windows Bluetooth settings, or keep the adapter claimed after exit.

A successful result validates only the USB driver/transport boundary. It does not yet mean Wii Remote pairing or input is implemented.

## Architecture constraints

- Keep the existing Windows HID backend intact and selectable.
- Use the USB adapter exclusively while passthrough is active; do not share it with the Windows Bluetooth stack.
- Keep the Intel Bluetooth radio available to Windows.
- Normalize remote reports into the existing input::wii::Remote / Snapshot path.
- Do not port Dolphin's IOS emulation. Implement only the host-side Bluetooth behavior required by Wii Remotes.
- Treat Dolphin's adapter and controller compatibility as the target, not a CSR-only hard-coded implementation.
