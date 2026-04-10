/**
 * uhf_scene_kill_tag.c
 *
 * Scene: Kill Tag (permanent destruction)
 *
 * Permanently and irreversibly disables the currently selected tag.
 * The tag cannot be re-enabled after this operation.
 *
 * Flow (two-stage: enter password → confirm → execute):
 *   Tag Menu → "Kill Tag"
 *     ┌─ Stage 1: ByteInput — enter Kill Password (4 bytes via hex editor)
 *     │   [Back] → return to Tag Menu
 *     │   [OK]   → Stage 2
 *     │
 *     └─ Stage 2: Confirmation Widget (strong warning)
 *         "KILL TAG"
 *         "Permanently disables tag."
 *         "Cannot be recovered."
 *         [Cancel] → return to Tag Menu
 *         [KILL!]  → loading popup while worker sends kill command
 *                 → result widget ("Tag Killed" or "Kill Failed")
 *                 → [Done] → return to Tag Menu
 *
 * Protocol:
 *   CMD_INACTIVATE_KILL_TAG (0x65), payload:
 *     bytes [5..8] overridden with the 4-byte kill password entered by user
 *   Response command code 0x65 in data[2] indicates acknowledged kill.
 *
 * Sub-states (tracked via scene_manager_set_scene_state):
 *   0 = KillTagSubStatePassword  — ByteInput view
 *   1 = KillTagSubStateConfirm   — Widget confirmation view
 *   2 = KillTagSubStateResult    — Widget result view
 */

#include "../uhf_app_i.h"

#define TAG         "UHFKillTag"
#define PW_BYTE_LEN 4

typedef enum {
    KillTagSubStatePassword = 0,
    KillTagSubStateConfirm,
    KillTagSubStateResult,
} KillTagSubState;

/* ── ByteInput callback ──────────────────────────────────────────────────────
 * Fires when the user confirms the password in the byte editor.
 * ────────────────────────────────────────────────────────────────────────── */
static void uhf_kill_tag_byte_input_callback(void* ctx) {
    UHFApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventByteInputDone);
}

/* ── Widget button callback ────────────────────────────────────────────────── */
static void uhf_scene_kill_tag_widget_callback(GuiButtonType result, InputType type, void* ctx) {
    if(type == InputTypeShort) {
        UHFApp* app = ctx;
        view_dispatcher_send_custom_event(app->view_dispatcher, result);
    }
}

/* ── Worker callback ─────────────────────────────────────────────────────────
 * Posts WorkerExit on success, WorkerFail on any failure.
 * ────────────────────────────────────────────────────────────────────────── */
static void uhf_kill_tag_worker_callback(UHFWorkerEvent event, void* ctx) {
    UHFApp* app = ctx;
    if(event == UHFWorkerEventSuccess) {
        view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventWorkerExit);
    } else {
        view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventWorkerFail);
    }
}

/* ── Show confirmation widget ────────────────────────────────────────────────
 * Two-button warning: [Cancel] (safe) and [KILL!] (destructive, right side).
 * ────────────────────────────────────────────────────────────────────────── */
static void show_kill_confirmation(UHFApp* app) {
    widget_reset(app->widget);

    widget_add_string_element(
        app->widget, 64, 10, AlignCenter, AlignCenter, FontPrimary, "KILL TAG");
    widget_add_string_element(
        app->widget, 64, 24, AlignCenter, AlignCenter, FontSecondary,
        "Permanently disables tag.");
    widget_add_string_element(
        app->widget, 64, 34, AlignCenter, AlignCenter, FontSecondary,
        "Cannot be recovered.");
    widget_add_string_element(
        app->widget, 64, 44, AlignCenter, AlignCenter, FontSecondary,
        "Continue?");

    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Cancel", uhf_scene_kill_tag_widget_callback, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "KILL!", uhf_scene_kill_tag_widget_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewWidget);
}

/* ── Show result widget ──────────────────────────────────────────────────────
 * Displays "Tag Killed" on success or kill-failure details on error.
 * ────────────────────────────────────────────────────────────────────────── */
static void show_kill_result(UHFApp* app, bool success) {
    widget_reset(app->widget);

    if(success) {
        notification_message(app->notifications, &sequence_success);
        widget_add_string_element(
            app->widget, 64, 12, AlignCenter, AlignCenter, FontPrimary, "Tag Killed");
        widget_add_string_element(
            app->widget, 64, 28, AlignCenter, AlignCenter, FontSecondary, "Tag is permanently");
        widget_add_string_element(
            app->widget, 64, 38, AlignCenter, AlignCenter, FontSecondary, "disabled.");
    } else {
        notification_message(app->notifications, &sequence_error);
        widget_add_string_element(
            app->widget, 64, 12, AlignCenter, AlignCenter, FontPrimary, "Kill Failed");
        widget_add_string_element(
            app->widget, 64, 28, AlignCenter, AlignCenter, FontSecondary, "Wrong password or");
        widget_add_string_element(
            app->widget, 64, 38, AlignCenter, AlignCenter, FontSecondary, "tag not in range.");
    }

    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Done", uhf_scene_kill_tag_widget_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewWidget);
}

/* ── on_enter ────────────────────────────────────────────────────────────────
 * Always starts at Stage 1: opens the Flipper hex byte editor pre-filled
 * with 00 00 00 00.
 * ────────────────────────────────────────────────────────────────────────── */
void uhf_scene_kill_tag_on_enter(void* ctx) {
    UHFApp* app = ctx;

    scene_manager_set_scene_state(
        app->scene_manager, UHFSceneKillTag, KillTagSubStatePassword);

    memset(app->byte_input_store, 0x00, PW_BYTE_LEN);

    byte_input_set_header_text(app->byte_input, "Kill Password");
    byte_input_set_result_callback(
        app->byte_input,
        uhf_kill_tag_byte_input_callback,
        NULL,
        app,
        app->byte_input_store,
        PW_BYTE_LEN);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewByteInput);
}

/* ── on_event ────────────────────────────────────────────────────────────────
 * Full two-stage state machine:
 *
 *  Stage 1 (KillTagSubStatePassword):
 *    ByteInputDone → store bytes → show confirmation widget
 *    Back          → return to Tag Menu
 *
 *  Stage 2 (KillTagSubStateConfirm):
 *    GuiButtonTypeLeft  (Cancel) → back to Tag Menu
 *    GuiButtonTypeRight (KILL!)  → start worker → show popup
 *
 *  Stage 3 (KillTagSubStateResult — entered after worker):
 *    WorkerExit           → success result widget
 *    WorkerFail           → failure result widget
 *    GuiButtonTypeLeft    → back to Tag Menu
 * ────────────────────────────────────────────────────────────────────────── */
bool uhf_scene_kill_tag_on_event(void* ctx, SceneManagerEvent event) {
    UHFApp*  app           = ctx;
    bool     consumed      = false;
    uint32_t current_state = scene_manager_get_scene_state(
        app->scene_manager, UHFSceneKillTag);

    if(event.type == SceneManagerEventTypeCustom) {

        /* ── Stage 1: password confirmed in byte editor ─────────────────── */
        if(event.event == UHFCustomEventByteInputDone &&
           current_state == KillTagSubStatePassword) {

            memcpy(
                app->worker->uhf_tag->security_password,
                app->byte_input_store,
                PW_BYTE_LEN);
            FURI_LOG_D(TAG, "Kill password: %02X%02X%02X%02X",
                app->byte_input_store[0], app->byte_input_store[1],
                app->byte_input_store[2], app->byte_input_store[3]);

            scene_manager_set_scene_state(
                app->scene_manager, UHFSceneKillTag, KillTagSubStateConfirm);
            show_kill_confirmation(app);
            consumed = true;

        /* ── Stage 2a: user cancels ─────────────────────────────────────── */
        } else if(event.event == GuiButtonTypeLeft &&
                  current_state == KillTagSubStateConfirm) {

            consumed = scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, UHFSceneTagMenu);

        /* ── Stage 2b: user confirms kill ───────────────────────────────── */
        } else if(event.event == GuiButtonTypeRight &&
                  current_state == KillTagSubStateConfirm) {

            FURI_LOG_I(TAG, "User confirmed kill tag operation");

            popup_reset(app->popup);
            popup_set_header(app->popup, "Killing Tag...", 68, 30, AlignLeft, AlignTop);
            popup_set_icon(app->popup, 0, 3, &I_RFIDDolphinSend_97x61);
            view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewPopup);

            scene_manager_set_scene_state(
                app->scene_manager, UHFSceneKillTag, KillTagSubStateResult);

            uhf_worker_start(
                app->worker,
                UHFWorkerStateKillTag,
                uhf_kill_tag_worker_callback,
                app);
            uhf_blink_start(app);
            consumed = true;

        /* ── Stage 3a: kill succeeded ───────────────────────────────────── */
        } else if(event.event == UHFCustomEventWorkerExit &&
                  current_state == KillTagSubStateResult) {

            uhf_blink_stop(app);
            uhf_worker_stop(app->worker);
            show_kill_result(app, true);
            consumed = true;

        /* ── Stage 3b: kill failed ──────────────────────────────────────── */
        } else if(event.event == UHFCustomEventWorkerFail &&
                  current_state == KillTagSubStateResult) {

            uhf_blink_stop(app);
            uhf_worker_stop(app->worker);
            show_kill_result(app, false);
            consumed = true;

        /* ── Stage 3c: "Done" on result widget ──────────────────────────── */
        } else if(event.event == GuiButtonTypeLeft &&
                  current_state == KillTagSubStateResult) {

            consumed = scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, UHFSceneTagMenu);
        }
    }

    return consumed;
}

/* ── on_exit ─────────────────────────────────────────────────────────────────
 * Stop any running worker and reset all borrowed views.
 * ────────────────────────────────────────────────────────────────────────── */
void uhf_scene_kill_tag_on_exit(void* ctx) {
    UHFApp* app = ctx;
    uhf_worker_stop(app->worker);
    uhf_blink_stop(app);
    widget_reset(app->widget);
    popup_reset(app->popup);
}
