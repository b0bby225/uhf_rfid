/**
 * uhf_scene_set_access_password.c
 *
 * Scene: Set / Reset Access Password
 *
 * Allows the user to set a new 32-bit Access Password on the currently
 * selected tag.  Entering "00000000" resets the password to the factory
 * default (no password).
 *
 * Flow:
 *   Tag Menu → "Set/Reset Access Password"
 *     → TextInput (enter 8 hex chars)
 *     → loading popup while worker writes Reserved bank word 2
 *     → result widget ("Updated!" or "Failed")
 *     → [Done] or [Back] → returns to Tag Menu
 *
 * Protocol:
 *   CMD_WRITE_LABEL_DATA_STORAGE (0x49) targeting:
 *     bank       = RFU_BANK (0x00, Reserved)
 *     word addr  = 0x0002   (Access Password at words 2–3)
 *     word count = 0x0002   (2 words = 4 bytes)
 *     data       = new 4-byte password
 *
 *   Current access password is assumed to be 0x00000000 (factory default).
 */

#include "../uhf_app_i.h"

#define TAG         "UHFSetAccessPW"
#define PW_BYTE_LEN 4

/* ── Widget button callback ──────────────────────────────────────────────────
 * Forwards GuiButtonType as a custom event (same pattern as read_tag_success).
 * ────────────────────────────────────────────────────────────────────────── */
static void uhf_scene_set_access_pw_widget_callback(
    GuiButtonType result,
    InputType type,
    void* ctx) {
    if(type == InputTypeShort) {
        UHFApp* app = ctx;
        view_dispatcher_send_custom_event(app->view_dispatcher, result);
    }
}

/* ── Worker callback ─────────────────────────────────────────────────────────
 * Called from the worker thread on success or any failure.
 * Posts UHFCustomEventWorkerExit (success) or UHFCustomEventWorkerFail.
 * ────────────────────────────────────────────────────────────────────────── */
static void uhf_set_access_pw_worker_callback(UHFWorkerEvent event, void* ctx) {
    UHFApp* app = ctx;
    if(event == UHFWorkerEventSuccess) {
        view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventWorkerExit);
    } else {
        view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventWorkerFail);
    }
}

/* ── ByteInput callback ──────────────────────────────────────────────────────
 * Fired when user confirms the password entry in the hex byte editor.
 * ────────────────────────────────────────────────────────────────────────── */
static void uhf_set_access_pw_byte_input_callback(void* ctx) {
    UHFApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventByteInputDone);
}

/* ── Show result widget ──────────────────────────────────────────────────────
 * Displays success ("Password Updated") or failure message with a Done button.
 * ────────────────────────────────────────────────────────────────────────── */
static void show_access_pw_result(UHFApp* app, bool success) {
    widget_reset(app->widget);

    if(success) {
        notification_message(app->notifications, &sequence_success);
        widget_add_string_element(
            app->widget, 64, 12, AlignCenter, AlignCenter, FontPrimary, "Access Password");
        widget_add_string_element(
            app->widget, 64, 28, AlignCenter, AlignCenter, FontPrimary, "Updated!");
        widget_add_string_element(
            app->widget, 64, 44, AlignCenter, AlignCenter, FontSecondary, "Tag is now protected.");
    } else {
        notification_message(app->notifications, &sequence_error);
        widget_add_string_element(
            app->widget, 64, 12, AlignCenter, AlignCenter, FontPrimary, "Update Failed");
        widget_add_string_element(
            app->widget, 64, 28, AlignCenter, AlignCenter, FontSecondary, "Tag not found or");
        widget_add_string_element(
            app->widget, 64, 38, AlignCenter, AlignCenter, FontSecondary, "wrong access password.");
    }

    widget_add_button_element(
        app->widget,
        GuiButtonTypeLeft,
        "Done",
        uhf_scene_set_access_pw_widget_callback,
        app);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewWidget);
}

/* ── on_enter ────────────────────────────────────────────────────────────────
 * Pre-fills 00 00 00 00 and opens the Flipper hex byte editor.
 * ────────────────────────────────────────────────────────────────────────── */
void uhf_scene_set_access_password_on_enter(void* ctx) {
    UHFApp* app = ctx;
    FURI_LOG_I(TAG, "scene_enter");

    memset(app->byte_input_store, 0x00, PW_BYTE_LEN);

    byte_input_set_header_text(app->byte_input, "Access Password");
    byte_input_set_result_callback(
        app->byte_input,
        uhf_set_access_pw_byte_input_callback,
        NULL,
        app,
        app->byte_input_store,
        PW_BYTE_LEN);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewByteInput);
}

/* ── on_event ────────────────────────────────────────────────────────────────
 * Handles the full state machine:
 *   TextEditDone → validate → start worker → show popup
 *   WorkerExit   → show success widget
 *   WorkerFail   → show error widget
 *   GuiButtonTypeLeft (Done) → return to Tag Menu
 * ────────────────────────────────────────────────────────────────────────── */
bool uhf_scene_set_access_password_on_event(void* ctx, SceneManagerEvent event) {
    UHFApp* app     = ctx;
    bool consumed   = false;

    if(event.type == SceneManagerEventTypeCustom) {

        if(event.event == UHFCustomEventByteInputDone) {
            /* Copy the 4 bytes from the byte editor directly — no parsing needed */
            memcpy(app->worker->uhf_tag->security_password, app->byte_input_store, PW_BYTE_LEN);
            FURI_LOG_D(TAG, "Setting access password: %02X%02X%02X%02X",
                app->byte_input_store[0], app->byte_input_store[1],
                app->byte_input_store[2], app->byte_input_store[3]);

            popup_reset(app->popup);
            popup_set_header(
                app->popup, "Setting Access\nPassword...", 68, 30, AlignLeft, AlignTop);
            popup_set_icon(app->popup, 0, 3, &I_RFIDDolphinSend_97x61);
            view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewPopup);

            uhf_worker_start(
                app->worker,
                UHFWorkerStateSetAccessPassword,
                uhf_set_access_pw_worker_callback,
                app);
            uhf_blink_start(app);
            consumed = true;

        } else if(event.event == UHFCustomEventWorkerExit) {
            /* Worker reported success */
            FURI_LOG_I(TAG, "worker result: SUCCESS");
            uhf_blink_stop(app);
            uhf_worker_stop(app->worker);
            show_access_pw_result(app, true);
            consumed = true;

        } else if(event.event == UHFCustomEventWorkerFail) {
            /* Worker reported failure (tag missing, wrong password, timeout) */
            FURI_LOG_E(TAG, "worker result: FAIL");
            uhf_blink_stop(app);
            uhf_worker_stop(app->worker);
            show_access_pw_result(app, false);
            consumed = true;

        } else if(event.event == GuiButtonTypeLeft) {
            /* "Done" button on result widget → return to Tag Menu */
            consumed = scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, UHFSceneTagMenu);
        }
    }

    return consumed;
}

/* ── on_exit ─────────────────────────────────────────────────────────────────
 * Stop any running worker and clean up all views.
 * ────────────────────────────────────────────────────────────────────────── */
void uhf_scene_set_access_password_on_exit(void* ctx) {
    UHFApp* app = ctx;
    uhf_worker_stop(app->worker);
    uhf_blink_stop(app);
    widget_reset(app->widget);
    popup_reset(app->popup);
}
