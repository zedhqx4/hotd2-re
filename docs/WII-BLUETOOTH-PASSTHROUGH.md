# USB Bluetooth passthrough

This page is retained as a compatibility link for older development notes. The current setup, adapter-selection rules, configuration keys, driver requirements, DLL API and validation status are documented in [WII-REMOTES.md](WII-REMOTES.md).

The passthrough backend is optional. Windows Bluetooth HID and DolphinBar Mode 4 remain the default path. No particular Bluetooth adapter model is hard-coded as a requirement; use `usb_vid` and `usb_pid` in `hotd2re_wii.ini` to select a claimable USB HCI adapter, or leave both at `-1` for automatic selection.
