# Native Wii Remote controls

Open **Options > Wii Remotes** or **Pause > Wii Remotes**. Wii Remote buttons and IR
pointing feed the game's native input system; no mouse-mapping utility is required.

## Connect

HOTD2RE supports two Windows connection paths:

- **Dedicated USB Bluetooth adapter (direct passthrough):** install the `libusbK` driver
  on the dedicated adapter (the tested CSR8510 A10 is `0A12:0001`) using Zadig. Enable
  Wii Remote support, select **Connect remotes**, and press the red **SYNC** button.
  HOTD2RE automatically selects a claimable standard USB HCI adapter and owns it while
  support is enabled. Keep the Intel Bluetooth radio's Windows driver untouched. The
  direct USB backend currently uses red SYNC pairing; 1+2 pairing is not yet implemented
  on this path.
- **Windows Bluetooth/HID:** leave the adapter on its normal Windows driver, then use
  **Connect remotes** and the SYNC or 1+2 procedure supported by the Windows backend.
  A Mayflash DolphinBar works in **Mode 4**; sync the bar and remote first.

When the dedicated libusb adapter is unavailable or cannot be claimed, HOTD2RE falls
back to its existing Windows Bluetooth/HID backend. Do not pair a remote through Windows
Settings while the dedicated libusb adapter is being used by HOTD2RE. Close other
applications controlling the same radio.

Connected remotes are assigned Player 1 and Player 2, with matching player LEDs.
**Swap players** exchanges their assignments and profiles. A lost connection keeps its
slot reserved until it reconnects or is explicitly disconnected in setup. Online play
uses the first local input slot for the local player, including the guest.

## Configure and calibrate

Select **Player 1 setup** or **Player 2 setup**. Defaults are **B: fire**, **A: reload**,
**+: confirm**, and **HOME: pause**. Use the D-pad, keyboard, or mouse to navigate. Click
a binding or use left/right to change it; duplicate bindings swap instead of assigning
two actions to one button.

Select **Calibrate aim**, stand at your normal playing position, and aim at each of the
four crosses. Press **B** once at each cross. Keep both ends of the sensor bar visible.
**Esc** cancels without replacing your previous calibration. Calibrate again after moving
the bar, changing display size, or changing playing position. Aim smoothing can be Off,
Light, or Strong. The setup screen shows tracking, battery level, button presses, and the
live aim position.

Player bindings and aim calibration are saved beside the game in `hotd2re_wii.ini`.
For the direct USB backend, Bluetooth link keys are stored separately in
`%LOCALAPPDATA%\HOTD2RE\wii_link_keys.txt`. That file contains credentials; do not share
it. Turn off **Wii Remote support** to release the USB adapter and return to regular inputs.

## Compatibility and validation

The Windows HID backend targets Nintendo RVL-CNT-01 and RVL-CNT-01-TR HID devices,
including the interfaces presented by DolphinBar Mode 4. The direct USB backend targets
standard USB Bluetooth HCI adapters, including the tested CSR8510 A10, and currently
supports up to two remotes with core buttons, accelerometer and IR pointing. It requests
QoS Guaranteed and the Wii's 5 ms Sniff mode to target the expected 200 Hz input cadence.

Nunchuk/Classic Controller extensions, MotionPlus, speaker audio, Balance Board and
third-party extension protocols are not implemented in the direct USB backend. Native
pointing requires an infrared sensor bar.

Automated tests cover HID reports, malformed packets, four-point calibration, button
mappings, short taps, disconnect/reconnect slot retention, mixed mouse/remote input,
setup menus and saved preferences. The standalone USB diagnostic has verified physical
CSR8510 authentication, encryption, L2CAP and core-button reports; the new game-integrated
USB backend still requires its own physical in-game validation.

Protocol references: [Wii Remote reports and Bluetooth timing](https://wiibrew.org/wiki/Wiimote).
