/**
 * uhf_scene_set_kill_password.c
 *
 * Scene: Set Kill Password
 *
 * Allows the user to set a new 32-bit Kill Password on the currently
 * selected tag using the Flipper's built-in hex byte editor.
 * Setting all bytes to 0x00 removes kill protection.
 *
 * Flow:
 *   Tag Menu → "Set Kill Password"
 *     → ByteInput (4-byte hex editor, pre-filled 00 00 00 00)
 *     → loading popup while worker writes Reserved bank word 0
 *     → result widget ("Updated!" or "Failed")
 *     → [Done] → returns to Tag Menu
 *
 * Protocol:
 *   CMD_WRITE_LABEL_DATA_STORAGE (0x49) targeting:
 *     bank       = RFU_BANK (0x00, Reserved)
 *     word addr  = 0x0000   (Kill Password at words 0–1)
 *     word count = 0x0002   (2 words = 4 bytes)
 *     data       = new 4-byte password
 *
 *   Current access password is assumed to be 0x00000000 (factory default).
 *   If the tag has a non-zero access password, this write will be rejected.
 */

#include "../uhf_app_i.h"

#define TAG         "UHFSetKillPW"
#define PW_BYTE_LEN 4

/* ── ByteInput callback ──────────────────────────────────────────────────────
 * Fires when the user presses OK in the byte editor.
 * ────────────────────────────────────────────────────────────────────────── */
static void uhf_set_kill_pw_byte_input_callback(void* ctx) {
    UHFApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventByteInputDone);
}

/* ── Widget button callback ────────────────────────────────────────────────── */
static void uhf_scene_set_kill_pw_widget_callback(
    GuiButtonType result,
    InputType type,
    void* ctx) {
    if(type == InputTypeShort) {
        UHFApp* app = ctx;
        view_dispatcher_send_custom_event(app->view_dispatcher, result);
    }
}

/* ── Worker callback ─────────────────────────────────────────────────────────
 * Posts UHFCustomEventWorkerExit on success, UHFCustomEventWorkerFail otherwise.
 * ────────────────────────────────────────────────────────────────────────── */
static void uhf_set_kill_pw_worker_callback(UHFWorkerEvent event, void* ctx) {
    UHFApp* app = ctx;
    if(event == UHFWorkerEventSuccess) {
        view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventWorkerExit);
    } else {
        view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventWorkerFail);
    }
}

/* ── Show result widget ──────────────────────────────────────────────────────
 * Displays "Kill Password Updated" on success or an error on failure.
 * ────────────────────────────────────────────────────────────────────────── */
static void show_kill_pw_result(UHFApp* app, bool success) {
    widget_reset(app->widget);

    if(success) {
        notification_message(app->notifications, &sequence_success);
        widget_add_string_element(
            app->widget, 64, 12, AlignCenter, AlignCenter, FontPrimary, "Kill Password");
        widget_add_string_element(
            app->widget, 64, 28, AlignCenter, AlignCenter, FontPrimary, "Updated!");
        widget_add_string_element(
            app->widget, 64, 44, AlignCenter, AlignCenter, FontSecondary, "Keep your password safe.");
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
        uhf_scene_set_kill_pw_widget_callback,
        app);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewWidget);
}

/* ── on_enter ────────────────────────────────────────────────────────────────
 * Pre-fills 00 00 00 00 and opens the Flipper hex byte editor.
 * ────────────────────────────────────────────────────────────────────────── */
void uhf_scene_set_kill_password_on_enter(void* ctx) {
    UHFApp* app = ctx;

    memset(app->byte_input_store, 0x00, PW_BYTE_LEN);

    byte_input_set_header_text(app->byte_input, "Kill Password");
    byte_input_set_result_callback(
        app->byte_input,
        uhf_set_kill_pw_byte_input_callback,
        NULL,
        app,
        app->byte_input_store,
        PW_BYTE_LEN);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewByteInput);
}

/* ── on_event ────────────────────────────────────────────────────────────────
 * State machine:
 *   ByteInputDone → copy bytes → start worker → popup
 *   WorkerExit    → success widget
 *   WorkerFail    → error widget
 *   GuiButtonTypeLeft (Done) → return to Tag Menu
 * ────────────────────────────────────────────────────────────────────────── */
bool uhf_scene_set_kill_password_on_event(void* ctx, SceneManagerEvent event) {
    UHFApp* app   = ctx;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {

        if(event.event == UHFCustomEventByteInputDone) {
            memcpy(app->worker->uhf_tag->security_password, app->byte_input_store, PW_BYTE_LEN);
            FURI_LOG_D(TAG, "Setting kill password: %02X%02X%02X%02X",
                app->byte_input_store[0], app->byte_input_store[1],
                app->byte_input_store[2], app->byte_input_store[3]);

            popup_reset(app->popup);
            popup_set_header(
                app->popup, "Setting Kill\nPassword...", 68, 30, AlignLeft, AlignTop);
            popup_set_icon(app->popup, 0, 3, &I_RFIDDolphinSend_97x61);
            view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewPopup);

            uhf_worker_start(
                app->worker,
                UHFWorkerStateSetKillPassword,
                uhf_set_kill_pw_worker_callback,
                app);
            uhf_blink_start(app);
            consumed = true;

        } else if(event.event == UHFCustomEventWorkerExit) {
            uhf_blink_stop(app);
            uhf_worker_stop(app->worker);
            show_kill_pw_result(app, true);
            consumed = true;

        } else if(event.event == UHFCustomEventWorkerFail) {
            uhf_blink_stop(app);
            uhf_worker_stop(app->worker);
            show_kill_pw_result(app, false);
            consumed = true;

        } else if(event.event == GuiButtonTypeLeft) {
            consumed = scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, UHFSceneTagMenu);
        }
    }

    return consumed;
}

/* ── on_exit ─────────────────────────────────────────────────────────────────
 * Stop worker and clean up views.
 * ────────────────────────────────────────────────────────────────────────── */
void uhf_scene_set_kill_password_on_exit(void* ctx) {
    UHFApp* app = ctx;
    uhf_worker_stop(app->worker);
    uhf_blink_stop(app);
    widget_reset(app->widget);
    popup_reset(app->popup);
}
