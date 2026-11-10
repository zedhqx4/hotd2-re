# USB Bluetooth passthrough (development)

The passthrough backend is being developed independently from the existing Windows HID backend. The first hardware milestone validates USB access; the next validates basic HCI command/event exchange.

## CSR8510 USB interface probe

On Windows, install the libusbK driver for the dedicated CSR8510 adapter with Zadig. Do not replace the driver for the Intel Bluetooth device used by Windows.

Configure CMake with `-DHOTD2_WII_USB_PASSTHROUGH=ON`, then build the USB probe:

    cmake --build build --config Release --target wii_bluetooth_probe

Run it from the output directory:

    .\wii_bluetooth_probe.exe

The default target is `USB\VID_0A12&PID_0001`. A different adapter can be inspected with:

    .\wii_bluetooth_probe.exe 0xVID 0xPID

The probe enumerates matching devices, prints USB interface classes and endpoint layouts, opens the adapter, and attempts to claim/release a standard Bluetooth HCI interface. It does not send HCI commands, pair a remote, change Windows Bluetooth settings, or keep the adapter claimed after exit.

## HCI command/event probe

Build the separate HCI probe:

    cmake --build build --config Release --target wii_hci_probe

Run:

    .\wii_hci_probe.exe

The initial probe sends these standard HCI commands and waits for their matching events:

- `HCI Reset` (`0x0C03`)
- `Read Local Version Information` (`0x1001`)
- `Read BD_ADDR` (`0x1009`)

USB Bluetooth transport details matter: HCI commands are sent over the default USB control pipe; HCI events arrive on the interrupt IN endpoint; ACL data uses the bulk IN/OUT endpoints. The probe deliberately does not send ACL data, scan, pair, or connect to a Wii Remote. It releases the claimed interface before exit.

A successful result validates basic controller command/event exchange only. It does not yet mean Wii Remote discovery, pairing, HIDP, or input is implemented.

## Architecture constraints

- Keep the existing Windows HID backend intact and selectable.
- Use the USB adapter exclusively while passthrough is active; do not share it with the Windows Bluetooth stack.
- Keep the Intel Bluetooth radio available to Windows.
- Normalize remote reports into the existing input::wii::Remote / Snapshot path.
- Do not port Dolphin's IOS emulation. Implement only the host-side Bluetooth behavior required by Wii Remotes.
- Treat Dolphin's adapter and controller compatibility as the target, not a CSR-only hard-coded implementation.
