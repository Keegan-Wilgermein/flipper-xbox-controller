// Xbox Bridge: receives Xbox controller input reports from a host computer on USB CDC channel 1
// and sends them to a phone over Bluetooth LE as an Xbox Wireless Controller. Names and IDs are in config.h.
//
// Frames (both directions): 0xA5, type, length, payload[length]
//   Host -> Flipper:  0x02 ping     [anything]          -> 0x82 pong [same bytes, tick u32]
//                     0x03 status   []                  -> 0x81 status
//                     0x04 quit     []
//                     0x10 report   input report 1, 16 bytes (no report ID byte)
//   Flipper -> Host:  0x81 status   [bt status, reports sent u32, errors u32, dis/hid/map/info ok, stack free u16]

#include "bridge_ble.h"
#include "config.h"

#include <bt/bt_service/bt.h>
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb_cdc.h>
#include <gui/gui.h>
#include <string.h>

#define BRIDGE_CDC BRIDGE_CDC_CHANNEL
#define KEYS_PATH  APP_DATA_PATH(BRIDGE_KEYS_FILE)

typedef enum {
    FlagReceive = 1 << 0,
    FlagQuit = 1 << 1,
    FlagSendStatus = 1 << 2,
} WorkerFlag;

// Button presses (and quit requests from the host) for the main thread.
typedef struct {
    InputEvent input;
} BridgeEvent;

typedef struct {
    Gui* gui;
    ViewPort* view;
    FuriMessageQueue* input;
    FuriThread* worker;
    FuriMutex* lock;

    Bt* bt;
    FuriHalBleProfileBase* profile;
    BtStatus bt_status;
    FuriHalUsbInterface* usb_prev;

    uint32_t reports;
    uint32_t errors;
    uint32_t frames;

    uint8_t rx[300];
    size_t rx_len;
} Bridge;

static void cdc_rx(void* ctx) {
    Bridge* b = ctx;
    furi_thread_flags_set(furi_thread_get_id(b->worker), FlagReceive);
}

static CdcCallbacks cdc_callbacks = {
    .tx_ep_callback = NULL,
    .rx_ep_callback = cdc_rx,
    .state_callback = NULL,
    .ctrl_line_callback = NULL,
    .config_callback = NULL,
};

static void send_frame(uint8_t type, const uint8_t* payload, uint8_t len) {
    uint8_t buf[CDC_DATA_SZ];
    if(len > CDC_DATA_SZ - 3) len = CDC_DATA_SZ - 3;
    buf[0] = 0xA5;
    buf[1] = type;
    buf[2] = len;
    if(len) memcpy(buf + 3, payload, len);
    furi_hal_cdc_send(BRIDGE_CDC, buf, len + 3);
}

static void send_status(Bridge* b) {
    uint8_t p[15];
    p[0] = (uint8_t)b->bt_status;
    memcpy(p + 1, &b->reports, 4);
    memcpy(p + 5, &b->errors, 4);
    memcpy(p + 9, &bridge_diag, 6);
    send_frame(0x81, p, sizeof p);
}

static void bt_status_changed(BtStatus status, void* ctx) {
    Bridge* b = ctx;
    b->bt_status = status;
    furi_thread_flags_set(furi_thread_get_id(b->worker), FlagSendStatus);
    view_port_update(b->view);
}

static void start_profile(Bridge* b) {
    bt_disconnect(b->bt);
    furi_delay_ms(200);
    b->profile = bt_profile_start(b->bt, bridge_profile, NULL);
    if(!b->profile) b->errors++;
    bridge_profile_beacon(b->profile != NULL);
    // The advertised name stops at 20 characters; the name hosts read after connecting can be the real one
    // (firmware patch).
    if(b->profile) furi_hal_bt_set_device_name(XBOX_NAME);
}

static void handle_frame(Bridge* b, uint8_t type, const uint8_t* p, uint8_t len) {
    b->frames++;
    if(type == 0x10) {
        furi_mutex_acquire(b->lock, FuriWaitForever);
        bool ok = bridge_profile_send(b->profile, p, len);
        furi_mutex_release(b->lock);
        if(ok) b->reports++;
        else b->errors++;
    } else if(type == 0x02) {
        uint8_t out[64];
        uint8_t n = MIN(len, 56);
        memcpy(out, p, n);
        uint32_t tick = furi_get_tick();
        memcpy(out + n, &tick, 4);
        send_frame(0x82, out, n + 4);
    } else if(type == 0x03) {
        send_status(b);
    } else if(type == 0x04) {
        // Quit, so the host can relaunch a new build without a button press.
        BridgeEvent e = {.input = {.type = InputTypeShort, .key = InputKeyBack}};
        furi_message_queue_put(b->input, &e, 0);
    }
}

static void parse(Bridge* b) {
    size_t i = 0;
    while(i < b->rx_len) {
        if(b->rx[i] != 0xA5) { i++; continue; }
        if(b->rx_len - i < 3) break;
        uint8_t len = b->rx[i + 2];
        if(b->rx_len - i < 3u + len) break;
        handle_frame(b, b->rx[i + 1], b->rx + i + 3, len);
        i += 3 + len;
    }
    memmove(b->rx, b->rx + i, b->rx_len - i);
    b->rx_len -= i;
}

static int32_t worker(void* ctx) {
    Bridge* b = ctx;
    while(true) {
        uint32_t f = furi_thread_flags_wait(FlagReceive | FlagQuit | FlagSendStatus, FuriFlagWaitAny, 500);
        if(f == (uint32_t)FuriFlagErrorTimeout) continue;
        if(f & FuriFlagError) continue;
        if(f & FlagQuit) break;
        if(f & FlagSendStatus) send_status(b);
        if(f & FlagReceive) {
            int32_t n;
            do {
                if(b->rx_len >= sizeof(b->rx) - CDC_DATA_SZ) b->rx_len = 0; // overflow: drop
                n = furi_hal_cdc_receive(BRIDGE_CDC, b->rx + b->rx_len, CDC_DATA_SZ);
                if(n > 0) b->rx_len += n;
            } while(n == CDC_DATA_SZ);
            parse(b);
        }
    }
    return 0;
}

static void draw(Canvas* c, void* ctx) {
    Bridge* b = ctx;
    static const char* status[] = {"Unavailable", "Off", "Waiting for phone", "Connected"};
    char line[40];
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 11, BRIDGE_TITLE);
    canvas_set_font(c, FontSecondary);
    snprintf(line, sizeof line, "BT: %s", b->bt_status <= BtStatusConnected ? status[b->bt_status] : "?");
    canvas_draw_str(c, 2, 30, line);
    snprintf(line, sizeof line, "Sent %lu  Err %lu", (unsigned long)b->reports, (unsigned long)b->errors);
    canvas_draw_str(c, 2, 44, line);
}

static void on_input(InputEvent* in, void* ctx) {
    BridgeEvent e = {.input = *in};
    furi_message_queue_put(((Bridge*)ctx)->input, &e, 0);
}

int32_t xbox_bridge_app(void* arg) {
    UNUSED(arg);
    Bridge* b = malloc(sizeof(Bridge));
    memset(b, 0, sizeof(*b));
    b->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    b->input = furi_message_queue_alloc(8, sizeof(BridgeEvent));

    b->view = view_port_alloc();
    view_port_draw_callback_set(b->view, draw, b);
    view_port_input_callback_set(b->view, on_input, b);
    b->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(b->gui, b->view, GuiLayerFullscreen);

    // USB: keep the CLI on channel 0, take channel 1 for the host.
    b->usb_prev = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    furi_hal_usb_set_config(&usb_cdc_dual, NULL);

    b->worker = furi_thread_alloc_ex("BridgeWorker", 3072, worker, b);
    furi_thread_start(b->worker);
    furi_hal_cdc_set_callbacks(BRIDGE_CDC, &cdc_callbacks, b);

    // Bluetooth: own key storage so pairing the bridge never touches the Flipper's normal pairing.
    b->bt = furi_record_open(RECORD_BT);
    bt_set_status_changed_callback(b->bt, bt_status_changed, b);
    bt_keys_storage_set_storage_path(b->bt, KEYS_PATH);
    furi_mutex_acquire(b->lock, FuriWaitForever);
    start_profile(b);
    furi_mutex_release(b->lock);

    BridgeEvent e;
    while(true) {
        if(furi_message_queue_get(b->input, &e, FuriWaitForever) != FuriStatusOk) continue;
        if(e.input.type == InputTypeShort && e.input.key == InputKeyBack) {
            break;
        }
    }

    furi_hal_cdc_set_callbacks(BRIDGE_CDC, NULL, NULL);
    furi_thread_flags_set(furi_thread_get_id(b->worker), FlagQuit);
    furi_thread_join(b->worker);
    furi_thread_free(b->worker);

    bt_set_status_changed_callback(b->bt, NULL, NULL);
    bridge_profile_beacon(false);
    bt_disconnect(b->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_default_path(b->bt);
    furi_check(bt_profile_restore_default(b->bt));
    // Give the Bluetooth service time to finish with this app's profile code before it is unloaded.
    furi_delay_ms(500);
    furi_record_close(RECORD_BT);

    furi_hal_usb_set_config(b->usb_prev, NULL);

    gui_remove_view_port(b->gui, b->view);
    view_port_free(b->view);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(b->input);
    furi_mutex_free(b->lock);
    free(b);
    return 0;
}
