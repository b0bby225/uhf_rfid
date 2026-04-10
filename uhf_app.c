#include "uhf_app_i.h"

/* ── convertToHexString ────────────────────────────────────────────────────
 * Converts a byte array to an uppercase space-separated hex string.
 * Always returns a heap-allocated, null-terminated string.
 * Caller must free() the result.
 *
 * Bug fixes vs original:
 *   1. Returned string literal " " when length==0; free() of a literal crashes.
 *      Now returns a heap-allocated single space string.
 *   2. malloc(str_len) did not include the null terminator; the string was
 *      not null-terminated, causing out-of-bounds reads downstream.
 *      Now allocates str_len+1 and explicitly sets the terminator.
 * ──────────────────────────────────────────────────────────────────────── */
char* convertToHexString(const uint8_t* array, size_t length) {
    if(array == NULL || length == 0) {
        /* Heap-allocate so the caller can unconditionally call free(). */
        char* empty = (char*)malloc(2);
        if(empty) {
            empty[0] = ' ';
            empty[1] = '\0';
        }
        return empty;
    }

    FuriString* temp_str = furi_string_alloc();

    for(size_t i = 0; i < length; i++) {
        furi_string_cat_printf(temp_str, "%02X ", array[i]);
    }
    const char* furi_str = furi_string_get_cstr(temp_str);

    size_t str_len = strlen(furi_str);
    char* str = (char*)malloc(str_len + 1); /* +1 for null terminator */
    if(str) {
        memcpy(str, furi_str, str_len);
        str[str_len] = '\0'; /* null-terminate */
    }

    furi_string_free(temp_str);
    return str;
}

bool uhf_custom_event_callback(void* ctx, uint32_t event) {
    furi_assert(ctx);
    UHFApp* uhf_app = ctx;
    return scene_manager_handle_custom_event(uhf_app->scene_manager, event);
}

bool uhf_back_event_callback(void* ctx) {
    furi_assert(ctx);
    UHFApp* uhf_app = ctx;
    return scene_manager_handle_back_event(uhf_app->scene_manager);
}

void uhf_tick_event_callback(void* ctx) {
    furi_assert(ctx);
    UHFApp* uhf_app = ctx;
    scene_manager_handle_tick_event(uhf_app->scene_manager);
}

UHFApp* uhf_alloc() {
    UHFApp* uhf_app = (UHFApp*)malloc(sizeof(UHFApp));
    uhf_app->view_dispatcher = view_dispatcher_alloc();
    uhf_app->scene_manager = scene_manager_alloc(&uhf_scene_handlers, uhf_app);
    view_dispatcher_set_event_callback_context(uhf_app->view_dispatcher, uhf_app);
    view_dispatcher_set_custom_event_callback(uhf_app->view_dispatcher, uhf_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        uhf_app->view_dispatcher, uhf_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        uhf_app->view_dispatcher, uhf_tick_event_callback, 100);

    // Open GUI record
    uhf_app->gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(
        uhf_app->view_dispatcher, uhf_app->gui, ViewDispatcherTypeFullscreen);

    // Worker
    uhf_app->worker = uhf_worker_alloc();

    // Device
    uhf_app->uhf_device = uhf_device_alloc();

    UHFTag* uhf_tag = uhf_tag_alloc();
    // Both worker and device hold a pointer to the same tag object
    uhf_app->worker->uhf_tag = uhf_tag;
    uhf_app->uhf_device->uhf_tag = uhf_tag;

    // Open Notification record
    uhf_app->notifications = furi_record_open(RECORD_NOTIFICATION);

    // Submenu
    uhf_app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        uhf_app->view_dispatcher, UHFViewMenu, submenu_get_view(uhf_app->submenu));

    // Popup
    uhf_app->popup = popup_alloc();
    view_dispatcher_add_view(
        uhf_app->view_dispatcher, UHFViewPopup, popup_get_view(uhf_app->popup));

    // Loading
    uhf_app->loading = loading_alloc();
    view_dispatcher_add_view(
        uhf_app->view_dispatcher, UHFViewLoading, loading_get_view(uhf_app->loading));

    // Byte Input (hex editor for kill/access password entry)
    uhf_app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        uhf_app->view_dispatcher, UHFViewByteInput, byte_input_get_view(uhf_app->byte_input));

    // Text Input
    uhf_app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        uhf_app->view_dispatcher, UHFViewTextInput, text_input_get_view(uhf_app->text_input));

    // Custom Widget
    uhf_app->widget = widget_alloc();
    view_dispatcher_add_view(
        uhf_app->view_dispatcher, UHFViewWidget, widget_get_view(uhf_app->widget));

    // QR code view (custom canvas; callbacks defined in uhf_scene_tag_qr.c)
    // The draw callback receives the VIEW MODEL (not context), so we allocate a
    // model that stores the UHFApp* so the draw callback can reach app state.
    uhf_app->view_qr = view_alloc();
    view_allocate_model(uhf_app->view_qr, ViewModelTypeLockFree, sizeof(UHFApp*));
    *(UHFApp**)view_get_model(uhf_app->view_qr) = uhf_app;
    view_set_draw_callback(uhf_app->view_qr, uhf_scene_tag_qr_draw_callback);
    view_set_input_callback(uhf_app->view_qr, uhf_scene_tag_qr_input_callback);
    view_set_context(uhf_app->view_qr, uhf_app);
    view_dispatcher_add_view(uhf_app->view_dispatcher, UHFViewTagQr, uhf_app->view_qr);

    // Variable item list (settings scene)
    uhf_app->variable_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        uhf_app->view_dispatcher,
        UHFViewVariableItemList,
        variable_item_list_get_view(uhf_app->variable_item_list));
    uhf_app->settings_baud_idx   = 0;
    uhf_app->settings_power_idx  = 0;
    uhf_app->settings_region_idx = 0;

    // QR module buffer starts NULL; allocated on demand in uhf_scene_tag_qr_on_enter
    uhf_app->qr_modules = NULL;
    uhf_app->qr_encoded = false;

    return uhf_app;
}

void uhf_free(UHFApp* uhf_app) {
    furi_assert(uhf_app);

    // Submenu
    view_dispatcher_remove_view(uhf_app->view_dispatcher, UHFViewMenu);
    submenu_free(uhf_app->submenu);

    // Popup
    view_dispatcher_remove_view(uhf_app->view_dispatcher, UHFViewPopup);
    popup_free(uhf_app->popup);

    // Loading
    view_dispatcher_remove_view(uhf_app->view_dispatcher, UHFViewLoading);
    loading_free(uhf_app->loading);

    // ByteInput
    view_dispatcher_remove_view(uhf_app->view_dispatcher, UHFViewByteInput);
    byte_input_free(uhf_app->byte_input);

    // TextInput
    view_dispatcher_remove_view(uhf_app->view_dispatcher, UHFViewTextInput);
    text_input_free(uhf_app->text_input);

    // Custom Widget
    view_dispatcher_remove_view(uhf_app->view_dispatcher, UHFViewWidget);
    widget_free(uhf_app->widget);

    // Variable item list
    view_dispatcher_remove_view(uhf_app->view_dispatcher, UHFViewVariableItemList);
    variable_item_list_free(uhf_app->variable_item_list);

    // QR code view
    view_dispatcher_remove_view(uhf_app->view_dispatcher, UHFViewTagQr);
    view_free(uhf_app->view_qr);
    if(uhf_app->qr_modules) {
        free(uhf_app->qr_modules);
        uhf_app->qr_modules = NULL;
    }

    /* Cache tag pointer BEFORE freeing worker.
     * Original code did uhf_worker_free() then uhf_tag_free(worker->uhf_tag),
     * which is a use-after-free because worker is already freed. */
    UHFTag* uhf_tag = uhf_app->worker->uhf_tag;
    uhf_worker_stop(uhf_app->worker);
    uhf_worker_free(uhf_app->worker);
    uhf_tag_free(uhf_tag);

    // View Dispatcher
    view_dispatcher_free(uhf_app->view_dispatcher);

    // Scene Manager
    scene_manager_free(uhf_app->scene_manager);

    // GUI
    furi_record_close(RECORD_GUI);
    uhf_app->gui = NULL;

    // UHFDevice
    uhf_device_free(uhf_app->uhf_device);

    // Notifications
    furi_record_close(RECORD_NOTIFICATION);
    uhf_app->notifications = NULL;

    free(uhf_app);
}

static const NotificationSequence uhf_sequence_blink_start_cyan = {
    &message_blink_start_10,
    &message_blink_set_color_cyan,
    &message_do_not_reset,
    NULL,
};

static const NotificationSequence uhf_sequence_blink_stop = {
    &message_blink_stop,
    NULL,
};

void uhf_blink_start(UHFApp* uhf_app) {
    notification_message(uhf_app->notifications, &uhf_sequence_blink_start_cyan);
}

void uhf_blink_stop(UHFApp* uhf_app) {
    notification_message(uhf_app->notifications, &uhf_sequence_blink_stop);
}

void uhf_show_loading_popup(void* ctx, bool show) {
    UHFApp* uhf_app = ctx;
    if(show) {
        view_dispatcher_switch_to_view(uhf_app->view_dispatcher, UHFViewLoading);
    }
}

int32_t uhf_app_main(void* ctx) {
    UNUSED(ctx);
    UHFApp* uhf_app = uhf_alloc();

    // Enable 5V pin for YRM100 module power
    furi_hal_power_enable_otg();

    scene_manager_next_scene(uhf_app->scene_manager, UHFSceneVerify);
    view_dispatcher_run(uhf_app->view_dispatcher);

    // Disable 5V pin
    furi_hal_power_disable_otg();

    uhf_free(uhf_app);
    return 0;
}
