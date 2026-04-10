#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <notification/notification_messages.h>

#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <gui/modules/loading.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>

#include <input/input.h>

/* Bundled QR code library — not in the public SDK, shipped within this FAP */
#include "lib/qrcode/qrcode.h"

#include "uhf_app.h"
#include "uhf_worker.h"
#include "uhf_device.h"
#include "scenes/uhf_scene.h"

#include <storage/storage.h>
#include <lib/toolbox/path.h>
#include <toolbox/path.h>
#include <flipper_format/flipper_format.h>

#include <uhf_rfid_icons.h>

#define UHF_TEXT_STORE_SIZE 128

enum UHFCustomEvent {
    // Reserve first 100 events for button types and indexes, starting from 0
    UHFCustomEventReserved = 100,

    UHFCustomEventVerifyDone,
    UHFCustomEventViewExit,
    UHFCustomEventWorkerExit,
    UHFCustomEventWorkerFail,       /* worker reported failure (tag missing, wrong pw, etc.) */
    UHFCustomEventByteInputDone,
    UHFCustomEventTextInputDone,
    UHFCustomEventTextEditDone,     /* security scenes: hex password confirmed in TextInput */
    UHFCustomEventBulkTagFound,     /* bulk scan: new unique tag written to SD              */
    UHFCustomEventWorkerSuccess,    /* settings scene: worker op completed successfully      */
    UHFCustomEventBruteForceProgress, /* brute force: progress tick (every BF_LOG_INT att.) */
};

typedef enum {
    EventTypeTick,
    EventTypeKey,
} EventType;

struct UHFApp {
    UHFWorker* worker;
    ViewDispatcher* view_dispatcher;
    Gui* gui;
    NotificationApp* notifications;
    SceneManager* scene_manager;
    // Storage* storage;
    UHFDevice* uhf_device;
    char text_store[UHF_TEXT_STORE_SIZE + 1];
    FuriString* text_box_store;
    // Bulk scan session state (valid only while UHFSceneBulkScan is active)
    uint32_t  bulk_scan_count;    /* running unique-tag count                          */
    bool      bulk_scan_saved;    /* true = keep SD file on exit; false = delete       */
    Storage*  bulk_scan_storage;  /* storage record open for the scan session          */
    File*     bulk_scan_file;     /* /ext/uhf_bulk_scan.txt, open during scan          */
    // Settings scene state
    uint8_t   settings_write_target; /* 0=EPC 1=TID 2=User 3=RFU while WriteBank is pending */
    // Brute force scene state
    uint32_t  bf_start_pw;           /* password value when brute force scene was entered    */
    // Settings scene — variable item list + persistent indices
    VariableItemList* variable_item_list;
    uint8_t settings_baud_idx;    /* index into BAUD_VALUES[]   */
    uint8_t settings_power_idx;   /* index into POWER_VALUES[]  */
    uint8_t settings_region_idx;  /* index into REGION_CODES[]  */
    // Common Views
    Submenu* submenu;
    Popup* popup;
    Loading* loading;
    ByteInput* byte_input;
    uint8_t    byte_input_store[12]; /* buffer for byte editor: max 12 bytes (96-bit EPC/TID) */
    TextInput* text_input;
    Widget* widget;
    // QR Code scene state
    View*    view_qr;          /* custom canvas view for QR rendering      */
    QRCode   qr_code;          /* QR code descriptor (version, size, …)    */
    uint8_t* qr_modules;       /* heap-allocated bit matrix; NULL=inactive */
    char     qr_epc_str[130];  /* uppercase hex EPC string, NUL-terminated */
    uint8_t  qr_epc_str_len;   /* number of hex characters (not bytes)     */
    bool     qr_encoded;       /* true when qr_code + qr_modules are valid */
};

typedef enum {
    UHFViewMenu,
    UHFViewPopup,
    UHFViewLoading,
    UHFViewByteInput,
    UHFViewTextInput,
    UHFViewWidget,
    UHFViewTagQr,             /* custom canvas view for Show EPC as QR Code */
    UHFViewVariableItemList,  /* settings: inline left/right value selection */
} UHFView;

UHFApp* uhf_app_alloc();

void uhf_text_store_set(UHFApp* uhf, const char* text, ...);

void uhf_text_store_clear(UHFApp* uhf);

void uhf_blink_start(UHFApp* uhf);

void uhf_blink_stop(UHFApp* uhf);

void uhf_show_loading_popup(void* context, bool show);

/** Check if memory is set to pattern
 *
 * @warning    zero size will return false
 *
 * @param[in]  data     Pointer to the byte array
 * @param[in]  pattern  The pattern
 * @param[in]  size     The byte array size
 *
 * @return     True if memory is set to pattern, false otherwise
 */
bool uhf_is_memset(const uint8_t* data, const uint8_t pattern, size_t size);

char* convertToHexString(const uint8_t* array, size_t length);

bool uhf_save_read_data(UHFResponseData* uhf_response_data, Storage* storage, const char* filename);

/* QR view callbacks – defined in scenes/uhf_scene_tag_qr.c */
void uhf_scene_tag_qr_draw_callback(Canvas* canvas, void* ctx);
bool uhf_scene_tag_qr_input_callback(InputEvent* event, void* ctx);
