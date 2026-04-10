/**
 * uhf_scene_settings.c
 *
 * Scene: Settings (inline variable item list)
 *
 * Each adjustable option is shown as a row with its current value.
 * Left / Right arrows cycle through the values for that row.
 * OK applies the highlighted selection (Baudrate / Power / Region)
 * or opens the hex TextInput for Write operations.
 *
 * Items:
 *   Baudrate  [< 230400 >]  — Left/Right selects; OK sends to module
 *   Power     [< 30 dBm >]  — Left/Right selects; OK sends to module
 *   Region    [<    US   >] — Left/Right selects; OK sends to module
 *   Write EPC              — OK opens hex text input
 *   Write TID              — OK opens hex text input
 *   Write User             — OK opens hex text input
 *   Write RFU              — OK opens hex text input
 */

#include "../uhf_app_i.h"
#include <string.h>
#include <stdlib.h>

#define TAG "UHFSettings"

/* ── Sub-states ─────────────────────────────────────────────────────────────── */
typedef enum {
    SettingsSubMain     = 0,
    SettingsSubWriting  = 1,
    SettingsSubApplying = 2,
} SettingsSubState;

typedef enum {
    SettingsWriteEPC  = 0,
    SettingsWriteTID  = 1,
    SettingsWriteUser = 2,
    SettingsWriteRFU  = 3,
} SettingsWriteTarget;

/* ── Item indices — must match the order in show_settings_vil() ─────────────── */
typedef enum {
    SettingsItemBaudrate  = 0,
    SettingsItemPower     = 1,
    SettingsItemRegion    = 2,
    SettingsItemWriteEPC  = 3,
    SettingsItemWriteTID  = 4,
    SettingsItemWriteUser = 5,
    SettingsItemWriteRFU  = 6,
} SettingsItem;

/* ── Option tables ──────────────────────────────────────────────────────────── */
static const uint32_t BAUD_VALUES[]       = {9600, 19200, 38400, 57600, 115200, 230400};
static const char* const BAUD_LABELS[]    = {"9600", "19200", "38400", "57600", "115200", "230400"};
#define BAUD_COUNT  (sizeof(BAUD_VALUES) / sizeof(BAUD_VALUES[0]))

static const uint32_t POWER_VALUES[]      = {500, 1000, 1500, 2000, 2500, 3000};
static const char* const POWER_LABELS[]   = {"5 dBm", "10 dBm", "15 dBm", "20 dBm", "25 dBm", "30 dBm"};
#define POWER_COUNT (sizeof(POWER_VALUES) / sizeof(POWER_VALUES[0]))

static const uint8_t REGION_CODES[]       = {1, 2, 3, 4, 6};
static const char* const REGION_LABELS[]  = {"China 920MHz", "US", "EU", "China 840MHz", "Korea"};
#define REGION_COUNT (sizeof(REGION_CODES) / sizeof(REGION_CODES[0]))

/* ── Value-change callbacks (Left / Right on a row) ─────────────────────────── */
static void baud_change_cb(VariableItem* item) {
    UHFApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, BAUD_LABELS[idx]);
    app->settings_baud_idx = idx;
}

static void power_change_cb(VariableItem* item) {
    UHFApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, POWER_LABELS[idx]);
    app->settings_power_idx = idx;
}

static void region_change_cb(VariableItem* item) {
    UHFApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, REGION_LABELS[idx]);
    app->settings_region_idx = idx;
}

/* ── Enter callback (OK pressed on a row) ───────────────────────────────────── */
static void settings_vil_enter_cb(void* ctx, uint32_t index) {
    UHFApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

/* ── ByteInput done callback ─────────────────────────────────────────────────── */
static void settings_byte_input_callback(void* ctx) {
    UHFApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventByteInputDone);
}

/* Byte counts per write target (EPC=12, TID=12, User=8, RFU=4) */
static const uint8_t WRITE_BYTE_COUNTS[] = {12, 12, 8, 4};

/* ── Worker callback ─────────────────────────────────────────────────────────── */
static void settings_worker_callback(UHFWorkerEvent event, void* ctx) {
    UHFApp* app = ctx;
    uint32_t ev = (event == UHFWorkerEventSuccess)
                      ? UHFCustomEventWorkerSuccess
                      : UHFCustomEventWorkerFail;
    view_dispatcher_send_custom_event(app->view_dispatcher, ev);
}

/* ── Popup timeout callback ──────────────────────────────────────────────────── */
static void settings_popup_callback(void* ctx) {
    UHFApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventViewExit);
}


/* ── Build and display the variable item list ───────────────────────────────── */
static void show_settings_vil(UHFApp* app) {
    variable_item_list_reset(app->variable_item_list);

    VariableItem* item;

    /* Baudrate — pre-select current index */
    item = variable_item_list_add(
        app->variable_item_list, "Baudrate", BAUD_COUNT, baud_change_cb, app);
    variable_item_set_current_value_index(item, app->settings_baud_idx);
    variable_item_set_current_value_text(item, BAUD_LABELS[app->settings_baud_idx]);

    /* Power */
    item = variable_item_list_add(
        app->variable_item_list, "Power", POWER_COUNT, power_change_cb, app);
    variable_item_set_current_value_index(item, app->settings_power_idx);
    variable_item_set_current_value_text(item, POWER_LABELS[app->settings_power_idx]);

    /* Region */
    item = variable_item_list_add(
        app->variable_item_list, "Region", REGION_COUNT, region_change_cb, app);
    variable_item_set_current_value_index(item, app->settings_region_idx);
    variable_item_set_current_value_text(item, REGION_LABELS[app->settings_region_idx]);

    /* Write items — no value cycling; OK opens TextInput */
    variable_item_list_add(app->variable_item_list, "Write EPC",  0, NULL, app);
    variable_item_list_add(app->variable_item_list, "Write TID",  0, NULL, app);
    variable_item_list_add(app->variable_item_list, "Write User", 0, NULL, app);
    variable_item_list_add(app->variable_item_list, "Write RFU",  0, NULL, app);

    variable_item_list_set_enter_callback(app->variable_item_list, settings_vil_enter_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewVariableItemList);
    scene_manager_set_scene_state(app->scene_manager, UHFSceneSettings, SettingsSubMain);
}

/* ── Open ByteInput (hex editor) for a Write operation ──────────────────────── */
static void show_write_input(UHFApp* app, SettingsWriteTarget target) {
    app->settings_write_target = (uint8_t)target;

    static const char* const headers[] = {"Write EPC", "Write TID", "Write User", "Write RFU"};
    uint8_t byte_count = WRITE_BYTE_COUNTS[target];

    memset(app->byte_input_store, 0x00, byte_count);

    byte_input_set_header_text(app->byte_input, headers[target]);
    byte_input_set_result_callback(
        app->byte_input,
        settings_byte_input_callback,
        NULL,
        app,
        app->byte_input_store,
        byte_count);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewByteInput);
    scene_manager_set_scene_state(app->scene_manager, UHFSceneSettings, SettingsSubWriting);
}

/* ── Start a module op — show "Applying…" popup and launch worker ───────────── */
static void start_module_op(UHFApp* app, const char* msg, UHFWorkerState state) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "Settings", 64, 5, AlignCenter, AlignTop);
    popup_set_text(app->popup, msg, 64, 36, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewPopup);
    scene_manager_set_scene_state(app->scene_manager, UHFSceneSettings, SettingsSubApplying);
    uhf_worker_start(app->worker, state, settings_worker_callback, app);
}

/* ── Show a timed result popup, then auto-return to settings list ───────────── */
static void show_result(UHFApp* app, const char* msg) {
    popup_reset(app->popup);
    popup_set_header(app->popup, "Settings", 64, 5, AlignCenter, AlignTop);
    popup_set_text(app->popup, msg, 64, 36, AlignCenter, AlignCenter);
    popup_set_callback(app->popup, settings_popup_callback);
    popup_set_context(app->popup, app);
    popup_set_timeout(app->popup, 1500);
    popup_enable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewPopup);
}

/* ── on_enter ────────────────────────────────────────────────────────────────── */
void uhf_scene_settings_on_enter(void* ctx) {
    UHFApp* app = ctx;

    /* Sync the displayed baud rate to the worker's actual current baud */
    app->settings_baud_idx = 0;
    for(uint8_t i = 0; i < (uint8_t)BAUD_COUNT; i++) {
        if(BAUD_VALUES[i] == app->worker->current_baud) {
            app->settings_baud_idx = i;
            break;
        }
    }

    show_settings_vil(app);
}

/* ── on_event ────────────────────────────────────────────────────────────────── */
bool uhf_scene_settings_on_event(void* ctx, SceneManagerEvent event) {
    UHFApp* app = ctx;
    bool consumed = false;
    uint32_t state = scene_manager_get_scene_state(app->scene_manager, UHFSceneSettings);

    /* ── Back ──────────────────────────────────────────────────────────────────── */
    if(event.type == SceneManagerEventTypeBack) {
        if(state == SettingsSubMain) {
            /* fall through — scene manager pops to Start */
        } else if(state == SettingsSubApplying) {
            uhf_worker_stop(app->worker);
            show_settings_vil(app);
            consumed = true;
        } else {
            /* TextInput Back → return to settings list */
            show_settings_vil(app);
            consumed = true;
        }
    }

    /* ── Custom events ─────────────────────────────────────────────────────────── */
    if(event.type == SceneManagerEventTypeCustom) {

        if(state == SettingsSubMain) {
            /* OK was pressed on a row — apply or open TextInput */
            switch((SettingsItem)event.event) {
                case SettingsItemBaudrate:
                    app->worker->setting_u32 = BAUD_VALUES[app->settings_baud_idx];
                    start_module_op(app, "Setting baudrate...", UHFWorkerStateSetBaudrate);
                    consumed = true;
                    break;
                case SettingsItemPower:
                    app->worker->setting_u32 = POWER_VALUES[app->settings_power_idx];
                    start_module_op(app, "Setting power...", UHFWorkerStateSetPower);
                    consumed = true;
                    break;
                case SettingsItemRegion:
                    app->worker->setting_u8 = REGION_CODES[app->settings_region_idx];
                    start_module_op(app, "Setting region...", UHFWorkerStateSetRegion);
                    consumed = true;
                    break;
                case SettingsItemWriteEPC:
                    show_write_input(app, SettingsWriteEPC);
                    consumed = true;
                    break;
                case SettingsItemWriteTID:
                    show_write_input(app, SettingsWriteTID);
                    consumed = true;
                    break;
                case SettingsItemWriteUser:
                    show_write_input(app, SettingsWriteUser);
                    consumed = true;
                    break;
                case SettingsItemWriteRFU:
                    show_write_input(app, SettingsWriteRFU);
                    consumed = true;
                    break;
                default:
                    break;
            }

        } else if(state == SettingsSubWriting) {
            if(event.event == UHFCustomEventByteInputDone) {
                UHFTag* tag = app->worker->uhf_tag;
                SettingsWriteTarget target = (SettingsWriteTarget)app->settings_write_target;
                uint8_t byte_count = WRITE_BYTE_COUNTS[target];

                switch(target) {
                    case SettingsWriteEPC:
                        uhf_tag_set_epc(tag, app->byte_input_store, byte_count);
                        app->worker->setting_u8 = 1; /* EPC_BANK */
                        break;
                    case SettingsWriteTID:
                        uhf_tag_set_tid(tag, app->byte_input_store, byte_count);
                        app->worker->setting_u8 = 2; /* TID_BANK */
                        break;
                    case SettingsWriteUser:
                        uhf_tag_set_user(tag, app->byte_input_store, byte_count);
                        app->worker->setting_u8 = 3; /* USER_BANK */
                        break;
                    case SettingsWriteRFU:
                        memcpy(tag->security_password, app->byte_input_store, byte_count);
                        app->worker->setting_u8 = 0; /* RFU_BANK */
                        break;
                }
                start_module_op(app, "Present tag...", UHFWorkerStateWriteBank);
                consumed = true;
            }

        } else if(state == SettingsSubApplying) {
            if(event.event == UHFCustomEventWorkerSuccess) {
                uhf_worker_stop(app->worker);
                show_result(app, "Done!");
                consumed = true;
            } else if(event.event == UHFCustomEventWorkerFail) {
                uhf_worker_stop(app->worker);
                show_result(app, "Failed!");
                consumed = true;
            } else if(event.event == UHFCustomEventViewExit) {
                show_settings_vil(app);
                consumed = true;
            }
        }
    }

    return consumed;
}

/* ── on_exit ─────────────────────────────────────────────────────────────────── */
void uhf_scene_settings_on_exit(void* ctx) {
    UHFApp* app = ctx;
    uhf_worker_stop(app->worker);
    variable_item_list_reset(app->variable_item_list);
    popup_reset(app->popup);
}
