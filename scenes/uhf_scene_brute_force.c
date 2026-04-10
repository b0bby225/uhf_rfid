/**
 * uhf_scene_brute_force.c
 *
 * Scene: Kill Password Audit
 *
 * Phase 1 – Attempts to directly read the kill password from the reserved bank
 *            (succeeds when the access password is default all-zeros and the
 *            bank is not permalocked).  Completes in < 1 second if unlocked.
 *
 * Phase 2 – Sequential brute force 0x00000000 → 0xFFFFFFFF if Phase 1 fails.
 *            Progress is displayed live and logged to /ext/uhf_ops.log for the
 *            Raspberry Pi monitoring script (pi_brute_monitor.py) to parse.
 *
 * Sub-states:
 *   0 = BFSubStateRunning  — popup + worker active
 *   1 = BFSubStateStopping — Back pressed; waiting for worker thread
 *   2 = BFSubStateDone     — result widget (found or stopped)
 */

#include "../uhf_app_i.h"
#include <string.h>
#include <storage/storage.h>

#define BF_RESUME_PATH "/ext/uhf_bf_resume.txt"

#define TAG "UHFBruteForce"

typedef enum {
    BFSubStateRunning  = 0,
    BFSubStateStopping = 1,
    BFSubStateDone     = 2,
} BFSubState;

/* ── Widget button callback ─────────────────────────────────────────────── */
static void bf_widget_callback(GuiButtonType result, InputType type, void* ctx) {
    if(type == InputTypeShort) {
        UHFApp* app = ctx;
        view_dispatcher_send_custom_event(app->view_dispatcher, result);
    }
}

/* ── Worker callback (runs on worker thread) ────────────────────────────── */
static void bf_worker_callback(UHFWorkerEvent event, void* ctx) {
    UHFApp* app = ctx;
    if(event == UHFWorkerEventCardDetected) {
        /* Progress tick — worker updated setting_u32 to current PW */
        view_dispatcher_send_custom_event(
            app->view_dispatcher, UHFCustomEventBruteForceProgress);
    } else if(event == UHFWorkerEventSuccess) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, UHFCustomEventWorkerSuccess);
    } else {
        /* Aborted or failed */
        view_dispatcher_send_custom_event(
            app->view_dispatcher, UHFCustomEventWorkerExit);
    }
}

/* ── Show the done-state widget ─────────────────────────────────────────── */
static void show_done_widget(UHFApp* app, bool found) {
    widget_reset(app->widget);

    uint32_t pw       = app->worker->setting_u32;
    uint8_t  via_read = app->worker->setting_u8;
    uint32_t attempts = pw - app->bf_start_pw;

    char pw_str[11];
    snprintf(pw_str, sizeof(pw_str), "0x%08lX", (unsigned long)pw);

    if(found) {
        /* Remove resume file — audit is complete, no need to resume */
        Storage* _stor = furi_record_open(RECORD_STORAGE);
        storage_simply_remove(_stor, BF_RESUME_PATH);
        furi_record_close(RECORD_STORAGE);

        widget_add_string_element(
            app->widget, 64, 4, AlignCenter, AlignTop, FontPrimary, "Tag Killed!");
        widget_add_string_element(
            app->widget, 64, 20, AlignCenter, AlignCenter, FontPrimary, pw_str);

        char detail[32];
        if(via_read) {
            snprintf(detail, sizeof(detail), "Read+killed (unlocked)");
        } else {
            snprintf(detail, sizeof(detail), "BF: ~%lu attempts", (unsigned long)attempts);
        }
        widget_add_string_element(
            app->widget, 64, 34, AlignCenter, AlignCenter, FontSecondary, detail);
        widget_add_string_element(
            app->widget, 64, 46, AlignCenter, AlignCenter, FontSecondary,
            "See /ext/uhf_ops.log");
    } else {
        widget_add_string_element(
            app->widget, 64, 4, AlignCenter, AlignTop, FontPrimary, "Audit Stopped");
        widget_add_string_element(
            app->widget, 64, 20, AlignCenter, AlignCenter, FontSecondary, "Resumed from:");
        widget_add_string_element(
            app->widget, 64, 32, AlignCenter, AlignCenter, FontPrimary, pw_str);
        widget_add_string_element(
            app->widget, 64, 46, AlignCenter, AlignCenter, FontSecondary,
            "Log has resume point");
    }

    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "OK", bf_widget_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewWidget);
}

/* ── on_enter ───────────────────────────────────────────────────────────── */
void uhf_scene_brute_force_on_enter(void* ctx) {
    UHFApp* app = ctx;

    app->bf_start_pw         = 0;
    app->worker->setting_u32 = 0; /* default: start from 0x00000000 */
    app->worker->setting_u8  = 0;

    /* ── Check for saved resume point ──────────────────────────────────────── */
    bool resuming = false;
    Storage* stor = furi_record_open(RECORD_STORAGE);
    File* rf = storage_file_alloc(stor);
    if(storage_file_open(rf, BF_RESUME_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[12];
        memset(buf, 0, sizeof(buf));
        storage_file_read(rf, buf, 11);
        storage_file_close(rf);
        unsigned long v = 0;
        if(sscanf(buf, "0x%lX", &v) == 1 && v > 0) {
            app->bf_start_pw         = (uint32_t)v;
            app->worker->setting_u32 = (uint32_t)v;
            resuming = true;
        }
    }
    storage_file_free(rf);
    furi_record_close(RECORD_STORAGE);

    scene_manager_set_scene_state(
        app->scene_manager, UHFSceneBruteForce, BFSubStateRunning);

    popup_reset(app->popup);
    popup_set_header(app->popup, "Kill PW Audit", 64, 4, AlignCenter, AlignTop);

    if(resuming) {
        char resume_str[32];
        snprintf(resume_str, sizeof(resume_str), "Resuming from\n0x%08lX",
                 (unsigned long)app->bf_start_pw);
        popup_set_text(app->popup, resume_str, 64, 30, AlignCenter, AlignCenter);
    } else {
        popup_set_text(app->popup, "Phase 1: Reading\nreserved bank...", 64, 30, AlignCenter, AlignCenter);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewPopup);

    uhf_worker_start(app->worker, UHFWorkerStateBruteForceKill, bf_worker_callback, app);
    uhf_blink_start(app);
}

/* ── on_event ───────────────────────────────────────────────────────────── */
bool uhf_scene_brute_force_on_event(void* ctx, SceneManagerEvent event) {
    UHFApp*  app      = ctx;
    bool     consumed = false;
    uint32_t state    = scene_manager_get_scene_state(
                            app->scene_manager, UHFSceneBruteForce);

    /* ── Back key ─────────────────────────────────────────────────────────── */
    if(event.type == SceneManagerEventTypeBack) {
        if(state == BFSubStateRunning) {
            uhf_worker_change_state(app->worker, UHFWorkerStateStop);
            popup_set_text(app->popup, "Stopping...", 64, 36, AlignCenter, AlignCenter);
            scene_manager_set_scene_state(
                app->scene_manager, UHFSceneBruteForce, BFSubStateStopping);
            consumed = true;
        } else if(state == BFSubStateStopping) {
            consumed = true; /* wait for worker */
        }
        /* BFSubStateDone: consumed=false → scene pops normally */
    }

    /* ── Custom events ────────────────────────────────────────────────────── */
    if(event.type == SceneManagerEventTypeCustom) {

        /* Live progress update from worker */
        if(event.event == UHFCustomEventBruteForceProgress) {
            uint32_t pw  = app->worker->setting_u32;
            uint32_t att = pw - app->bf_start_pw;
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "Phase 2: Brute Force\nPW: 0x%08lX\nAtt: ~%lu",
                (unsigned long)pw,
                (unsigned long)att);
            popup_set_text(app->popup, app->text_store, 64, 24, AlignCenter, AlignCenter);
            consumed = true;

        /* Worker found the password */
        } else if(event.event == UHFCustomEventWorkerSuccess) {
            uhf_blink_stop(app);
            uhf_worker_stop(app->worker);
            show_done_widget(app, true);
            scene_manager_set_scene_state(
                app->scene_manager, UHFSceneBruteForce, BFSubStateDone);
            consumed = true;

        /* Worker aborted (stopped by user or exhausted) */
        } else if(event.event == UHFCustomEventWorkerExit) {
            uhf_blink_stop(app);
            uhf_worker_stop(app->worker);
            show_done_widget(app, false);
            scene_manager_set_scene_state(
                app->scene_manager, UHFSceneBruteForce, BFSubStateDone);
            consumed = true;

        /* [OK] button on done widget → back to Start */
        } else if(state == BFSubStateDone) {
            if(event.event == (uint32_t)GuiButtonTypeRight) {
                scene_manager_search_and_switch_to_previous_scene(
                    app->scene_manager, UHFSceneStart);
                consumed = true;
            }
        }
    }

    return consumed;
}

/* ── on_exit ────────────────────────────────────────────────────────────── */
void uhf_scene_brute_force_on_exit(void* ctx) {
    UHFApp* app = ctx;
    uhf_worker_stop(app->worker);
    uhf_blink_stop(app);
    widget_reset(app->widget);
    popup_reset(app->popup);
}
