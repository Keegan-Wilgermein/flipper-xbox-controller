#pragma once

#include <furi_ble/profile_interface.h>
#include <stdbool.h>
#include <stdint.h>

// Bluetooth LE profile that presents the Flipper as an Xbox Wireless Controller. Takes no parameters.
extern const FuriHalBleProfileTemplate* bridge_profile;

/// Starts (or stops) the extra advertisement that carries Appearance (gamepad).
void bridge_profile_beacon(bool on);

/// Sends input report 1 (16 bytes, without the report ID).
bool bridge_profile_send(FuriHalBleProfileBase* profile, const uint8_t* data, uint16_t len);

// Startup results of the last profile start, reported to the host in the status frame.
// Nothing that runs inside the profile callbacks may log: they run on the 1 KB Bluetooth thread.
typedef struct {
    uint8_t dis_ok;
    uint8_t hid_ok;
    uint8_t map_ok;
    uint8_t info_ok;
    uint16_t bt_stack_free;
} BridgeDiag;

extern BridgeDiag bridge_diag;
