// Bluetooth LE profile that presents the Flipper as an Xbox Wireless Controller (model 1914)
// Battery + Device Information + HID + Microsoft's vendor service
//
// The HID service is a trimmed copy of Momentum's lib/ble_profile/extra_services/hid_service.c
//
// Calling the firmware's own copy from this profile crashed the Flipper on launch for an unknown reason
//
// Nothing here may log or format strings as profile callbacks run on the
// Bluetooth service thread, which has a 1 KB stack

#include "bridge_ble.h"
#include "config.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_ble/gatt.h>
#include <services/battery_service.h>
#include <extra_beacon.h>
#include <string.h>

// IMPORTANT: ble_gatt_characteristic_update() returns true on FAILURE (ie. result != success)

// STM32WB BLE stack constants (ble_defs.h is not part of the app SDK)
#define BR_UUID_TYPE_16          0x01
#define BR_UUID_TYPE_128         0x02
#define BR_PRIMARY_SERVICE       0x01
#define BR_PROP_READ             0x02
#define BR_PROP_WRITE_NO_RESP    0x04
#define BR_PROP_WRITE            0x08
#define BR_PROP_NOTIFY           0x10
#define BR_PERMISSION_NONE       0x00
// HID over GATT 1.0, section 6.1: every HID Service characteristic shall need an encrypted link
// (LE Security Mode 1, Level 2+).
#define BR_PERMISSION_ENCRYPTED  0x24 // ATTR_PERMISSION_ENCRY_READ | ATTR_PERMISSION_ENCRY_WRITE
#define BR_ACCESS_READ_WRITE     0x03
#define BR_DONT_NOTIFY_EVENTS    0x00
#define BR_NOTIFY_ATTRIBUTE_WRITE 0x01
#define BR_LEN_CONSTANT          0x00
#define BR_LEN_VARIABLE          0x01

#define BR_MAP_MAX    512
#define BR_REPORT_MAX 32

#define INPUT_REPORT_ID  0x01
#define OUTPUT_REPORT_ID 0x03 // rumble

// ---------------------------------------------------------------------------------------------

// The Xbox Wireless Controller's Bluetooth LE descriptor (model 1914),
// also in Mystfit/ESP32-BLE-CompositeHID (XboxDescriptors.h). 283 bytes: stock Momentum cuts GATT values at
// 255, so this needs the firmware patch (chunked writes); without it iOS gets a truncated map and ignores
// the device. Input report 1, 16 bytes: LX, LY, RX, RY (u16, centre 0x8000), LT, RT (10 bits in u16),
// hat (1-8, 0 = none), 15 buttons, then Share. Output report 3 is rumble
static const uint8_t report_map[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xc0, 0x09, 0x01,
    0xa1, 0x00, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x95, 0x02, 0x75,
    0x10, 0x81, 0x02, 0xc0, 0x05, 0x02, 0x09, 0xc5, 0x15, 0x00, 0x26, 0xff, 0x03, 0x95, 0x01, 0x75,
    0x0a, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03, 0x05, 0x02, 0x09,
    0xc4, 0x15, 0x00, 0x26, 0xff, 0x03, 0x95, 0x01, 0x75, 0x0a, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00,
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00,
    0x46, 0x3b, 0x01, 0x66, 0x14, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x75, 0x04, 0x95, 0x01,
    0x15, 0x00, 0x25, 0x00, 0x35, 0x00, 0x45, 0x00, 0x65, 0x00, 0x81, 0x03, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0f, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0f, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00,
    0x75, 0x01, 0x95, 0x01, 0x81, 0x03, 0x05, 0x0c, 0x0a, 0xb2, 0x00, 0x15, 0x00, 0x25, 0x01, 0x95,
    0x01, 0x75, 0x01, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03, 0x05,
    0x0f, 0x09, 0x21, 0x85, 0x03, 0xa1, 0x02, 0x09, 0x97, 0x15, 0x00, 0x25, 0x01, 0x75, 0x04, 0x95,
    0x01, 0x91, 0x02, 0x15, 0x00, 0x25, 0x00, 0x75, 0x04, 0x95, 0x01, 0x91, 0x03, 0x09, 0x70, 0x15,
    0x00, 0x25, 0x64, 0x75, 0x08, 0x95, 0x04, 0x91, 0x02, 0x09, 0x50, 0x66, 0x01, 0x10, 0x55, 0x0e,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02, 0x09, 0xa7, 0x15, 0x00, 0x26,
    0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02, 0x65, 0x00, 0x55, 0x00, 0x09, 0x7c, 0x15, 0x00,
    0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02, 0xc0, 0xc0,
};

static const uint8_t xbox_ms_table[318] = {
    0x01, 0x00, 0x41, 0xe6, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x84, 0x4e, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x43, 0x00, 0x00, 0x00, 0x11, 0x00, 0x9f, 0x07, 0x00, 0x00,
    0x20, 0x00, 0xaa, 0x4b, 0x02, 0x00, 0x21, 0x00, 0xb1, 0x09, 0x01, 0x00,
    0x22, 0x00, 0xa7, 0x44, 0x01, 0x00, 0x23, 0x00, 0x4d, 0x63, 0x00, 0x00,
    0x24, 0x00, 0x64, 0x13, 0x00, 0x00, 0x25, 0x00, 0xfe, 0x0d, 0x00, 0x00,
    0x26, 0x00, 0xad, 0x2b, 0x00, 0x00, 0x27, 0x00, 0xa7, 0x0c, 0x00, 0x00,
    0x28, 0x00, 0xaf, 0x0c, 0x00, 0x00, 0x29, 0x00, 0x3f, 0x3b, 0x00, 0x00,
    0x2a, 0x00, 0x1a, 0x0d, 0x00, 0x00, 0x2b, 0x00, 0x21, 0x00, 0x00, 0x00,
    0x2c, 0x00, 0x3a, 0xce, 0x03, 0x00, 0x2d, 0x00, 0xa2, 0x54, 0x00, 0x00,
    0x2e, 0x00, 0xf2, 0xaa, 0x00, 0x00, 0x2f, 0x00, 0x75, 0xf4, 0x00, 0x00,
    0x36, 0x00, 0x10, 0x00, 0x00, 0x00, 0x40, 0x00, 0x85, 0xfc, 0x00, 0x00,
    0x41, 0x00, 0xcd, 0xf1, 0x00, 0x00, 0x42, 0x00, 0xf4, 0x65, 0x03, 0x00,
    0x43, 0x00, 0x98, 0x61, 0x03, 0x00, 0x50, 0x00, 0x99, 0x6d, 0x0e, 0x00,
    0x51, 0x00, 0xbe, 0x24, 0x03, 0x00, 0x52, 0x00, 0xeb, 0x31, 0x03, 0x00,
    0x53, 0x00, 0xb9, 0xb9, 0x05, 0x00, 0x54, 0x00, 0xb3, 0xab, 0x02, 0x00,
    0x55, 0x00, 0x25, 0xba, 0x1f, 0x00, 0x56, 0x00, 0xfa, 0x1e, 0x03, 0x00,
    0x57, 0x00, 0x33, 0x50, 0x03, 0x00, 0x58, 0x00, 0xb6, 0xba, 0x00, 0x00,
    0x59, 0x00, 0x82, 0x8c, 0x00, 0x00, 0x60, 0x00, 0x0e, 0x64, 0x02, 0x00,
    0x64, 0x00, 0x59, 0x22, 0x61, 0x01, 0x61, 0x00, 0x48, 0xa2, 0x02, 0x00,
    0x65, 0x00, 0xf7, 0xc3, 0x56, 0x01, 0x62, 0x00, 0x82, 0xb4, 0x00, 0x00,
    0x66, 0x00, 0x9c, 0x0f, 0x1a, 0x00, 0x63, 0x00, 0x9d, 0x1e, 0x01, 0x00,
    0x67, 0x00, 0x79, 0xea, 0x21, 0x00, 0xc1, 0x00, 0xf9, 0x04, 0x00, 0x00,
    0xc3, 0x00, 0x0a, 0x00, 0x00, 0x00, 0xc4, 0x00, 0x03, 0x00, 0x00, 0x00,
    0xc5, 0x00, 0xdc, 0x54, 0x00, 0x00, 0xc6, 0x00, 0x0c, 0x55, 0x00, 0x00,
    0xc7, 0x00, 0x02, 0x00, 0x00, 0x00,
};
static const uint8_t xbox_ms_info[32] = {
    0x00, 0x62, 0x45, 0xb6, 0x9e, 0x52, 0xea, 0x00, 0x0b, 0xa5, 0x27, 0x6d,
    0x18, 0x42, 0x74, 0x00, 0x63, 0x6b, 0x20, 0x30, 0x00, 0x00, 0x00, 0x00,
    0x23, 0x00, 0x00, 0x00, 0xa0, 0xa1, 0x02, 0x20,
};
static const uint8_t xbox_ms_uuid[3][16] = {
    {0x0d, 0xd4, 0x98, 0x32, 0x95, 0xa7, 0x83, 0x9c, 0x4f, 0x4c, 0x60, 0x5f, 0x01, 0x00, 0x00, 0x00},
    {0x0d, 0xd4, 0x98, 0x32, 0x95, 0xa7, 0x83, 0x9c, 0x4f, 0x4c, 0x60, 0x5f, 0x02, 0x00, 0x00, 0x00},
    {0x0d, 0xd4, 0x98, 0x32, 0x95, 0xa7, 0x83, 0x9c, 0x4f, 0x4c, 0x60, 0x5f, 0x03, 0x00, 0x00, 0x00},
};
static const uint8_t xbox_ms_write_uuid[16] = {
    0x0d, 0xd4, 0x98, 0x32, 0x95, 0xa7, 0x83, 0x9c, 0x4f, 0x4c, 0x60, 0x5f, 0x04, 0x00, 0x00, 0x00};

BridgeDiag bridge_diag;

// ---------------------------------------------------------------------------------------------
// HID service (vendored)

typedef struct {
    const void* ptr;
    uint16_t len;
} DataWrap;

static bool map_cb(const void* ctx, const uint8_t** data, uint16_t* len) {
    const DataWrap* w = ctx;
    if(data && w) {
        *data = w->ptr;
        *len = w->len;
    } else {
        *len = BR_MAP_MAX;
    }
    return false;
}

static bool report_cb(const void* ctx, const uint8_t** data, uint16_t* len) {
    const DataWrap* w = ctx;
    if(data && w) {
        *data = w->ptr;
        *len = w->len;
    } else {
        *len = BR_REPORT_MAX;
    }
    return false;
}

typedef struct {
    uint8_t id;
    uint8_t type; // 1 = input, 2 = output
} ReportRef;

static bool ref_cb(const void* ctx, const uint8_t** data, uint16_t* len) {
    *len = sizeof(ReportRef);
    if(data) *data = ctx;
    return false;
}

static const BleGattCharacteristicParams hid_chars[] = {
    {.name = "Protocol Mode",
     .data_prop_type = FlipperGattCharacteristicDataFixed,
     .data.fixed.length = 1,
     .uuid.Char_UUID_16 = 0x2A4E,
     .uuid_type = BR_UUID_TYPE_16,
     .char_properties = BR_PROP_READ | BR_PROP_WRITE_NO_RESP,
     .security_permissions = BR_PERMISSION_ENCRYPTED,
     .gatt_evt_mask = BR_NOTIFY_ATTRIBUTE_WRITE,
     .is_variable = BR_LEN_CONSTANT},
    {.name = "Report Map",
     .data_prop_type = FlipperGattCharacteristicDataCallback,
     .data.callback.fn = map_cb,
     .uuid.Char_UUID_16 = 0x2A4B,
     .uuid_type = BR_UUID_TYPE_16,
     .char_properties = BR_PROP_READ,
     .security_permissions = BR_PERMISSION_ENCRYPTED,
     .gatt_evt_mask = BR_DONT_NOTIFY_EVENTS,
     .is_variable = BR_LEN_VARIABLE},
    {.name = "HID Information",
     .data_prop_type = FlipperGattCharacteristicDataFixed,
     .data.fixed.length = 4,
     .uuid.Char_UUID_16 = 0x2A4A,
     .uuid_type = BR_UUID_TYPE_16,
     .char_properties = BR_PROP_READ,
     .security_permissions = BR_PERMISSION_ENCRYPTED,
     .gatt_evt_mask = BR_DONT_NOTIFY_EVENTS,
     .is_variable = BR_LEN_CONSTANT},
    {.name = "HID Control Point",
     .data_prop_type = FlipperGattCharacteristicDataFixed,
     .data.fixed.length = 1,
     .uuid.Char_UUID_16 = 0x2A4C,
     .uuid_type = BR_UUID_TYPE_16,
     .char_properties = BR_PROP_WRITE_NO_RESP,
     .security_permissions = BR_PERMISSION_ENCRYPTED,
     .gatt_evt_mask = BR_NOTIFY_ATTRIBUTE_WRITE,
     .is_variable = BR_LEN_CONSTANT},
};
#define HID_CHAR_COUNT (sizeof(hid_chars) / sizeof(hid_chars[0]))

typedef struct {
    FuriHalBleProfileBase base;
    BleServiceBattery* battery;
    uint16_t dis_handle;
    BleGattCharacteristicInstance dis[4];
    uint8_t dis_count;
    uint16_t ms_handle;
    BleGattCharacteristicInstance ms[3];
    uint8_t pnp[7];
    uint16_t hid_handle;
    BleGattCharacteristicInstance hid[HID_CHAR_COUNT];
    BleGattCharacteristicInstance report;
    ReportRef ref;
    // Writable output report (rumble). Real pads have one, and iOS's drivers may write to it on connect.
    BleGattCharacteristicInstance out_report;
    ReportRef out_ref;
} BridgeProfile;
_Static_assert(offsetof(BridgeProfile, base) == 0, "profile base must come first");

static void dis_start(BridgeProfile* p) {
    p->pnp[0] = 0x02; // vendor ID source: USB-IF
    p->pnp[1] = XBOX_VID & 0xFF;
    p->pnp[2] = XBOX_VID >> 8;
    p->pnp[3] = XBOX_PID & 0xFF;
    p->pnp[4] = XBOX_PID >> 8;
    p->pnp[5] = XBOX_VERSION & 0xFF;
    p->pnp[6] = XBOX_VERSION >> 8;
    // Static and filled field by field (no compound literals), so nothing large lands on the
    // 1 KB Bluetooth service thread stack. Order as on the real controller, which has no Model string.
    static BleGattCharacteristicParams c[4];
    memset(c, 0, sizeof(c));
    c[0].name = "Manufacturer";
    c[0].uuid.Char_UUID_16 = 0x2A29;
    c[0].data.fixed.ptr = (const uint8_t*)XBOX_MANUFACTURER;
    c[0].data.fixed.length = strlen(XBOX_MANUFACTURER);
    c[1].name = "PnP ID";
    c[1].uuid.Char_UUID_16 = 0x2A50;
    c[1].data.fixed.ptr = p->pnp;
    c[1].data.fixed.length = sizeof(p->pnp);
    c[2].name = "Firmware";
    c[2].uuid.Char_UUID_16 = 0x2A26;
    c[2].data.fixed.ptr = (const uint8_t*)XBOX_FIRMWARE;
    c[2].data.fixed.length = strlen(XBOX_FIRMWARE);
    c[3].name = "Serial";
    c[3].uuid.Char_UUID_16 = 0x2A25;
    c[3].data.fixed.ptr = (const uint8_t*)XBOX_SERIAL;
    c[3].data.fixed.length = strlen(XBOX_SERIAL);
    const uint8_t n = 4;
    uint16_t uuid = 0x180A;
    if(!ble_gatt_service_add(BR_UUID_TYPE_16, (Service_UUID_t*)&uuid, BR_PRIMARY_SERVICE, 1 + 2 * n, &p->dis_handle)) {
        p->dis_handle = 0;
        return;
    }
    bridge_diag.dis_ok = 1;
    for(uint8_t i = 0; i < n; i++) {
        c[i].data_prop_type = FlipperGattCharacteristicDataFixed;
        c[i].uuid_type = BR_UUID_TYPE_16;
        c[i].char_properties = BR_PROP_READ;
        // Readable before pairing, as on a real controller (the spec only says "should" encrypt these).
        c[i].security_permissions = BR_PERMISSION_NONE;
        c[i].gatt_evt_mask = BR_DONT_NOTIFY_EVENTS;
        c[i].is_variable = BR_LEN_CONSTANT;
        ble_gatt_characteristic_init(p->dis_handle, &c[i], &p->dis[i]);
        ble_gatt_characteristic_update(p->dis_handle, &p->dis[i], NULL);
    }
    p->dis_count = n;
}

/// Microsoft's GATT service, as on a real Xbox controller.
static void ms_start(BridgeProfile* p) {
    if(!ble_gatt_service_add(BR_UUID_TYPE_128, (const Service_UUID_t*)xbox_ms_uuid[0], BR_PRIMARY_SERVICE, 1 + 2 * 3, &p->ms_handle)) {
        p->ms_handle = 0;
        return;
    }
    static BleGattCharacteristicParams c[3];
    memset(c, 0, sizeof(c));
    c[0].name = "MS table";
    memcpy(c[0].uuid.Char_UUID_128, xbox_ms_uuid[1], 16);
    c[0].data.fixed.ptr = xbox_ms_table;
    c[0].data.fixed.length = sizeof(xbox_ms_table);
    c[0].char_properties = BR_PROP_READ;
    c[1].name = "MS info";
    memcpy(c[1].uuid.Char_UUID_128, xbox_ms_uuid[2], 16);
    c[1].data.fixed.ptr = xbox_ms_info;
    c[1].data.fixed.length = sizeof(xbox_ms_info);
    c[1].char_properties = BR_PROP_READ;
    static const uint8_t blank[20];
    c[2].name = "MS write";
    memcpy(c[2].uuid.Char_UUID_128, xbox_ms_write_uuid, 16);
    c[2].data.fixed.ptr = blank;
    c[2].data.fixed.length = sizeof(blank);
    c[2].char_properties = BR_PROP_WRITE;
    for(uint8_t i = 0; i < 3; i++) {
        c[i].data_prop_type = FlipperGattCharacteristicDataFixed;
        c[i].uuid_type = BR_UUID_TYPE_128;
        c[i].security_permissions = BR_PERMISSION_NONE;
        c[i].gatt_evt_mask = BR_DONT_NOTIFY_EVENTS;
        c[i].is_variable = i == 2 ? BR_LEN_VARIABLE : BR_LEN_CONSTANT;
        ble_gatt_characteristic_init(p->ms_handle, &c[i], &p->ms[i]);
        if(i < 2) ble_gatt_characteristic_update(p->ms_handle, &p->ms[i], NULL);
    }
}

static void hid_start(BridgeProfile* p) {
    uint16_t uuid = 0x1812;
    uint8_t records = 1 + 2 * HID_CHAR_COUNT + 4 + 4; // + input and output reports with their descriptors
    if(!ble_gatt_service_add(BR_UUID_TYPE_16, (Service_UUID_t*)&uuid, BR_PRIMARY_SERVICE, records, &p->hid_handle)) {
        p->hid_handle = 0;
        return;
    }
    bridge_diag.hid_ok = 1;
    ble_gatt_characteristic_init(p->hid_handle, &hid_chars[0], &p->hid[0]);
    static const uint8_t protocol_mode = 1; // report protocol
    ble_gatt_characteristic_update(p->hid_handle, &p->hid[0], &protocol_mode);

    static BleGattCharacteristicDescriptorParams ref_desc = {
        .uuid_type = BR_UUID_TYPE_16,
        .uuid.Char_UUID_16 = 0x2908,
        .max_length = sizeof(ReportRef),
        .data_callback.fn = ref_cb,
        .security_permissions = BR_PERMISSION_ENCRYPTED,
        .access_permissions = BR_ACCESS_READ_WRITE,
        .gatt_evt_mask = BR_DONT_NOTIFY_EVENTS,
        .is_variable = BR_LEN_CONSTANT,
    };
    static BleGattCharacteristicParams report = {
        .name = "Report",
        .data_prop_type = FlipperGattCharacteristicDataCallback,
        .data.callback.fn = report_cb,
        .uuid.Char_UUID_16 = 0x2A4D,
        .uuid_type = BR_UUID_TYPE_16,
        .char_properties = BR_PROP_READ | BR_PROP_NOTIFY,
        .security_permissions = BR_PERMISSION_ENCRYPTED,
        .gatt_evt_mask = BR_DONT_NOTIFY_EVENTS,
        .is_variable = BR_LEN_VARIABLE,
        .descriptor_params = &ref_desc,
    };
    // Only the reports the descriptor declares: a real Xbox pad has no spare report characteristics.
    p->ref = (ReportRef){.id = INPUT_REPORT_ID, .type = 1}; // 1 = input
    ref_desc.data_callback.context = &p->ref;
    ble_gatt_characteristic_init(p->hid_handle, &report, &p->report);

    static const uint8_t zeros[8];
    static BleGattCharacteristicDescriptorParams out_desc = {
        .uuid_type = BR_UUID_TYPE_16,
        .uuid.Char_UUID_16 = 0x2908,
        .max_length = sizeof(ReportRef),
        .data_callback.fn = ref_cb,
        .security_permissions = BR_PERMISSION_ENCRYPTED,
        .access_permissions = BR_ACCESS_READ_WRITE,
        .gatt_evt_mask = BR_DONT_NOTIFY_EVENTS,
        .is_variable = BR_LEN_CONSTANT,
    };
    static BleGattCharacteristicParams out = {
        .name = "Report",
        .data_prop_type = FlipperGattCharacteristicDataFixed,
        .data.fixed.ptr = zeros,
        .data.fixed.length = sizeof(zeros),
        .uuid.Char_UUID_16 = 0x2A4D,
        .uuid_type = BR_UUID_TYPE_16,
        .char_properties = BR_PROP_READ | BR_PROP_WRITE | BR_PROP_WRITE_NO_RESP,
        .security_permissions = BR_PERMISSION_ENCRYPTED,
        .gatt_evt_mask = BR_DONT_NOTIFY_EVENTS, // the stack accepts writes; rumble is ignored
        .is_variable = BR_LEN_VARIABLE,
        .descriptor_params = &out_desc,
    };
    p->out_ref = (ReportRef){.id = OUTPUT_REPORT_ID, .type = 2}; // 2 = output
    out_desc.data_callback.context = &p->out_ref;
    ble_gatt_characteristic_init(p->hid_handle, &out, &p->out_report);

    for(size_t i = 1; i < HID_CHAR_COUNT; i++) {
        ble_gatt_characteristic_init(p->hid_handle, &hid_chars[i], &p->hid[i]);
    }

    static DataWrap map;
    map = (DataWrap){report_map, sizeof(report_map)};
    bridge_diag.map_ok = !ble_gatt_characteristic_update(p->hid_handle, &p->hid[1], &map);
    static const uint8_t info[4] = {0x11, 0x01, 0x00, 0x02}; // HID 1.11, no country, normally connectable
    bridge_diag.info_ok = !ble_gatt_characteristic_update(p->hid_handle, &p->hid[2], info);
}

// ---------------------------------------------------------------------------------------------
// Profile

static FuriHalBleProfileBase* profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    BridgeProfile* p = malloc(sizeof(BridgeProfile));
    memset(p, 0, sizeof(*p));
    memset(&bridge_diag, 0, sizeof(bridge_diag));
    p->base.config = bridge_profile;
    p->battery = ble_svc_battery_start(true);
    // Our own small DIS. The firmware's one formats strings with snprintf, and this runs on the
    // Bluetooth service thread, which only has 1 KB of stack.
    dis_start(p);
    hid_start(p);
    ms_start(p);
    // Bytes of stack left on the Bluetooth service thread (1 KB total). Reported to the host.
    bridge_diag.bt_stack_free = (uint16_t)furi_thread_get_stack_space(furi_thread_get_current_id());
    return &p->base;
}

static void profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base && base->config == bridge_profile);
    BridgeProfile* p = (BridgeProfile*)base;
    if(p->hid_handle) {
        for(size_t i = 0; i < HID_CHAR_COUNT; i++) ble_gatt_characteristic_delete(p->hid_handle, &p->hid[i]);
        ble_gatt_characteristic_delete(p->hid_handle, &p->report);
        ble_gatt_characteristic_delete(p->hid_handle, &p->out_report);
        ble_gatt_service_delete(p->hid_handle);
    }
    if(p->ms_handle) {
        for(size_t i = 0; i < 3; i++) ble_gatt_characteristic_delete(p->ms_handle, &p->ms[i]);
        ble_gatt_service_delete(p->ms_handle);
    }
    if(p->dis_handle) {
        for(size_t i = 0; i < p->dis_count; i++) ble_gatt_characteristic_delete(p->dis_handle, &p->dis[i]);
        ble_gatt_service_delete(p->dis_handle);
    }
    ble_svc_battery_stop(p->battery);
    free(p);
}

static uint8_t current_mac[GAP_MAC_ADDR_SIZE]; // the address the profile advertises with

// A real Xbox controller advertises Microsoft's company ID (0x0006) with one zero byte.
static const uint8_t xbox_mfg_data[] = {0x06, 0x00, 0x00};

static void profile_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    memset(config, 0, sizeof(GapConfig));
    config->adv_service.UUID_Type = BR_UUID_TYPE_16;
    config->adv_service.Service_UUID_16 = 0x1812;
    config->appearance_char = 0x03C4; // gamepad
    config->bonding_mode = true;
    config->pairing_method = GapPairingPinCodeVerifyYesNo; // confirm the code on the Flipper
    config->conn_param.conn_int_min = 0x06; // 7.5 ms
    config->conn_param.conn_int_max = 0x24; // 45 ms
    config->conn_param.slave_latency = 0;
    config->conn_param.supervisor_timeout = 0;

    // An address of its own, so the phone keeps this pairing apart from the Flipper's others.
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->mac_address[2] += 3;
    config->mac_address[0] ^= 0x88;
    config->mac_address[1] ^= BRIDGE_MAC_GENERATION;
    memcpy(current_mac, config->mac_address, sizeof(current_mac));

    // The firmware puts this in the scan response, wrapped as manufacturer-specific data.
    memcpy(config->mfg_data, xbox_mfg_data, sizeof(xbox_mfg_data));
    config->mfg_data_len = sizeof(xbox_mfg_data);

    // adv_name[0] is the AD type byte; the rest is the visible name (20 chars max, the firmware patch
    // sets the full name after connecting).
    config->adv_name[0] = furi_hal_version_get_ble_local_device_name_ptr()[0];
    strlcpy(config->adv_name + 1, XBOX_NAME, sizeof(config->adv_name) - 1);
}

static const FuriHalBleProfileTemplate template = {
    .start = profile_start,
    .stop = profile_stop,
    .get_gap_config = profile_gap_config,
};

const FuriHalBleProfileTemplate* bridge_profile = &template;

bool bridge_profile_send(FuriHalBleProfileBase* base, const uint8_t* data, uint16_t len) {
    if(!base || base->config != bridge_profile) return false;
    BridgeProfile* p = (BridgeProfile*)base;
    if(!p->hid_handle || len > BR_REPORT_MAX) return false;
    DataWrap w = {data, len};
    return !ble_gatt_characteristic_update(p->hid_handle, &p->report, &w);
}

// The firmware never puts Appearance in the advertisement, but a real Xbox controller does (0x03C4, gamepad),
// and hosts use it to class the device. So a second, non-connectable advertisement from the same address
// carries it (with the manufacturer data); hosts merge advertisements per address.
void bridge_profile_beacon(bool on) {
    if(furi_hal_bt_extra_beacon_is_active()) furi_hal_bt_extra_beacon_stop();
    if(!on) return;
    GapExtraBeaconConfig config = {
        .min_adv_interval_ms = 100,
        .max_adv_interval_ms = 150,
        .adv_channel_map = GapAdvChannelMapAll,
        .adv_power_level = GapAdvPowerLevel_0dBm,
        .address_type = GapAddressTypePublic,
    };
    memcpy(config.address, current_mac, sizeof(config.address));
    static const uint8_t data[] = {
        0x02, 0x01, 0x06, // flags: LE general discoverable, no BR/EDR
        0x03, 0x19, 0xC4, 0x03, // appearance: gamepad
        0x03, 0x03, 0x12, 0x18, // complete 16-bit service UUIDs: HID
        0x04, 0xFF, 0x06, 0x00, 0x00, // manufacturer data: Microsoft
    };
    if(furi_hal_bt_extra_beacon_set_config(&config) && furi_hal_bt_extra_beacon_set_data(data, sizeof(data))) {
        furi_hal_bt_extra_beacon_start();
    }
}
