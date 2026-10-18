#pragma once

// ------------------------------------------------------------------ The app

// Editable constants for the Flipper app

// Title on the Flipper's screen
#define BRIDGE_TITLE "Xbox Bridge"

// USB serial channel the host talks to (channel 0 stays the Flipper's CLI)
#define BRIDGE_CDC_CHANNEL 1

// Pairing keys, in the app's data folder, apart from the Flipper's normal pairings
#define BRIDGE_KEYS_FILE ".bridge.keys"

// Mixed into the Bluetooth address. Change it for a fresh identity: iOS caches a device's services per
// address, so a changed GATT table under the same address can be ignored until the phone forgets it
#define BRIDGE_MAC_GENERATION 0x5C

// ------------------------------------------------------------------ Xbox Wireless Controller

// Don't touch these or the controller may not pair corrctly

// Copied from a real Xbox Series controller, model 1914. The serial is made up in the same format
#define XBOX_NAME         "Xbox Wireless Controller" // Will show up as 'Xbox Wireless Contro' until connected
#define XBOX_MANUFACTURER "Microsoft"
#define XBOX_VID          0x045E
#define XBOX_PID          0x0B20
#define XBOX_VERSION      0x0517
#define XBOX_FIRMWARE     "5.17.3202.0"
#define XBOX_SERIAL       "03600185440000"
