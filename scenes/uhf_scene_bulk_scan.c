/**
 * uhf_scene_bulk_scan.c
 *
 * Scene: Bulk Inventory Scan
 *
 * Continuously polls for UHF Gen2 tags and counts unique EPCs.
 * No sound or vibration during scan — display shows a live tag count only.
 *
 * Memory design (supports 80 000+ tags in one session):
 *   - The SD card is the primary store: every new unique EPC is streamed to
 *     /ext/uhf_bulk_scan.txt as it is found (one uppercase hex EPC per line).
 *   - A 4096-entry FNV-1a fingerprint ring buffer (16 KB heap) deduplicates
 *     tags within the last 4096 unique reads.  A sequential inventory walk
 *     through a facility will never revisit the same tag within that window.
 *   - The running count (uint32_t) and ring buffer are the only RAM that grows
 *     with the scan; total working set stays well under 30 KB.
 *
 * Flow:
 *   Start Menu → "Bulk Scan"
 *     → Popup view: "Tags: 0 / Scanning..."  (live count, no sound/vibration)
 *       [Back] → signals worker to stop → "Stopping..."
 *               → worker exits → Widget: count + [Discard] [Save]
 *               [Save]    → keeps /ext/uhf_bulk_scan.txt, returns to Start
 *               [Discard] → deletes file, returns to Start
 *               [Back]    → same as Discard
 *
 * Sub-states (scene_manager_set_scene_state):
 *   0 = BulkScanSubStateScanning  — popup + worker running
 *   1 = BulkScanSubStateStopping  — Back pressed; waiting for worker thread
 *   2 = BulkScanSubStateStopped   — widget with Save / Discard buttons
 */

#include "../uhf_app_i.h"

#define TAG            "UHFBulkScan"
#define BULK_SCAN_PATH "/ext/uhf_bulk_scan.txt"

typedef enum {
    BulkScanSubStateScanning = 0,
    BulkScanSubStateStopping,
    BulkScanSubStateStopped,
} BulkScanSubState;

/* ── Widget button callback ────────────────────────────────────────────────── */
static void uhf_bulk_scan_widget_callback(GuiButtonType result, InputType type, void* ctx) {
    if(type == InputTypeShort) {
        UHFApp* app = ctx;
        view_dispatcher_send_custom_event(app->view_dispatcher, result);
    }
}

/* ── Worker callback (runs on the worker thread) ────────────────────────────
 * Fires for every new unique tag (UHFWorkerEventCardDetected) and once on
 * exit (UHFWorkerEventAborted).
 * Storage API is thread-safe; we write the EPC directly to the SD file here.
 * ─────────────────────────────────────────────────────────────────────────── */
static void uhf_bulk_scan_worker_callback(UHFWorkerEvent event, void* ctx) {
    UHFApp* app = ctx;

    if(event == UHFWorkerEventCardDetected) {
        /* Write EPC hex line to SD file */
        if(app->bulk_scan_file) {
            UHFTag* tag = app->worker->uhf_tag;
            /* Max EPC = 64 bytes → 128 hex chars + '\n' + '\0' = 130 */
            char line[MAX_BANK_SIZE * 2 + 2];
            size_t pos = 0;
            for(size_t i = 0; i < tag->epc_length && pos + 2 < sizeof(line); i++) {
                pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%02X", tag->epc[i]);
            }
            line[pos++] = '\n';
            storage_file_write(app->bulk_scan_file, line, (uint16_t)pos);
        }
        app->bulk_scan_count++;
        view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventBulkTagFound);

    } else if(event == UHFWorkerEventAborted) {
        view_dispatcher_send_custom_event(app->view_dispatcher, UHFCustomEventWorkerExit);
    }
}

/* ── Show the stopped-state widget ──────────────────────────────────────────
 * Displays the final count in a large font with Save / Discard buttons.
 * ─────────────────────────────────────────────────────────────────────────── */
static void show_stopped_widget(UHFApp* app) {
    widget_reset(app->widget);

    char count_str[12]; /* enough for UINT32_MAX (10 digits) */
    snprintf(count_str, sizeof(count_str), "%lu", (unsigned long)app->bulk_scan_count);

    widget_add_string_element(
        app->widget, 64, 8, AlignCenter, AlignCenter, FontPrimary, "Scan Complete");
    widget_add_string_element(
        app->widget, 64, 26, AlignCenter, AlignCenter, FontBigNumbers, count_str);
    widget_add_string_element(
        app->widget, 64, 44, AlignCenter, AlignCenter, FontSecondary, "unique tags");
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Discard", uhf_bulk_scan_widget_callback, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Save", uhf_bulk_scan_widget_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewWidget);
}

/* ── on_enter ────────────────────────────────────────────────────────────────
 * Opens the SD file, shows popup, starts worker.
 * ─────────────────────────────────────────────────────────────────────────── */
void uhf_scene_bulk_scan_on_enter(void* ctx) {
    UHFApp* app = ctx;

    app->bulk_scan_count  = 0;
    app->bulk_scan_saved  = false;
    app->bulk_scan_file    = NULL;
    app->bulk_scan_storage = NULL;

    /* Open session file — FSOM_CREATE_ALWAYS truncates any previous scan */
    app->bulk_scan_storage = furi_record_open(RECORD_STORAGE);
    app->bulk_scan_file    = storage_file_alloc(app->bulk_scan_storage);
    if(!storage_file_open(
           app->bulk_scan_file,
           BULK_SCAN_PATH,
           FSAM_WRITE,
           FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "Cannot open scan file: %s", BULK_SCAN_PATH);
        storage_file_free(app->bulk_scan_file);
        furi_record_close(RECORD_STORAGE);
        app->bulk_scan_file    = NULL;
        app->bulk_scan_storage = NULL;
        /* Continue without SD — count still works, just no file saved */
    }

    scene_manager_set_scene_state(
        app->scene_manager, UHFSceneBulkScan, BulkScanSubStateScanning);

    popup_reset(app->popup);
    popup_set_header(app->popup, "Bulk Inventory", 64, 5, AlignCenter, AlignTop);
    popup_set_text(app->popup, "Tags: 0\nScanning...", 64, 36, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewPopup);

    uhf_worker_start(
        app->worker, UHFWorkerStateBulkScan, uhf_bulk_scan_worker_callback, app);
    uhf_blink_start(app);
}

/* ── on_event ────────────────────────────────────────────────────────────────
 * Handles:
 *   Back key      (Scanning)  → signal worker to stop
 *   Back key      (Stopping)  → consumed/ignored
 *   Back key      (Stopped)   → Discard; fall through to pop scene
 *   BulkTagFound  (Scanning)  → update popup count
 *   WorkerExit               → show stopped widget
 *   GuiButtonTypeRight (Save)    → mark saved, navigate back to Start
 *   GuiButtonTypeLeft  (Discard) → navigate back to Start (file deleted in exit)
 * ─────────────────────────────────────────────────────────────────────────── */
bool uhf_scene_bulk_scan_on_event(void* ctx, SceneManagerEvent event) {
    UHFApp*  app     = ctx;
    bool     consumed = false;
    uint32_t state   = scene_manager_get_scene_state(app->scene_manager, UHFSceneBulkScan);

    /* ── Back key ─────────────────────────────────────────────────────────── */
    if(event.type == SceneManagerEventTypeBack) {
        if(state == BulkScanSubStateScanning) {
            /* Non-blocking signal: worker exits on its next 100 ms loop tick */
            uhf_worker_change_state(app->worker, UHFWorkerStateStop);
            popup_set_text(app->popup, "Stopping...", 64, 36, AlignCenter, AlignCenter);
            scene_manager_set_scene_state(
                app->scene_manager, UHFSceneBulkScan, BulkScanSubStateStopping);
            consumed = true;
        } else if(state == BulkScanSubStateStopping) {
            consumed = true; /* wait for UHFCustomEventWorkerExit */
        }
        /* BulkScanSubStateStopped: consumed = false → scene pops → on_exit discards */
    }

    /* ── Custom events ────────────────────────────────────────────────────── */
    if(event.type == SceneManagerEventTypeCustom) {

        if(event.event == UHFCustomEventBulkTagFound) {
            /* Update live counter in popup text */
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "Tags: %lu\nScanning...",
                (unsigned long)app->bulk_scan_count);
            popup_set_text(app->popup, app->text_store, 64, 36, AlignCenter, AlignCenter);
            consumed = true;

        } else if(event.event == UHFCustomEventWorkerExit) {
            /* Worker thread has exited; join it (fast — already stopped) */
            uhf_blink_stop(app);
            uhf_worker_stop(app->worker);
            show_stopped_widget(app);
            scene_manager_set_scene_state(
                app->scene_manager, UHFSceneBulkScan, BulkScanSubStateStopped);
            consumed = true;

        } else if(state == BulkScanSubStateStopped) {
            if(event.event == (uint32_t)GuiButtonTypeRight) { /* Save */
                app->bulk_scan_saved = true;
                scene_manager_search_and_switch_to_previous_scene(
                    app->scene_manager, UHFSceneStart);
                consumed = true;
            } else if(event.event == (uint32_t)GuiButtonTypeLeft) { /* Discard */
                /* bulk_scan_saved stays false → on_exit deletes file */
                scene_manager_search_and_switch_to_previous_scene(
                    app->scene_manager, UHFSceneStart);
                consumed = true;
            }
        }
    }

    return consumed;
}

/* ── on_exit ─────────────────────────────────────────────────────────────────
 * Always runs.  Stops worker, closes and optionally deletes the scan file.
 * ─────────────────────────────────────────────────────────────────────────── */
void uhf_scene_bulk_scan_on_exit(void* ctx) {
    UHFApp* app = ctx;

    uhf_worker_stop(app->worker); /* no-op if already stopped */
    uhf_blink_stop(app);

    /* Close file and apply save/discard decision */
    if(app->bulk_scan_file) {
        storage_file_close(app->bulk_scan_file);
        if(!app->bulk_scan_saved) {
            storage_simply_remove(app->bulk_scan_storage, BULK_SCAN_PATH);
            FURI_LOG_D(TAG, "bulk_scan: file discarded");
        } else {
            FURI_LOG_I(TAG, "bulk_scan: saved %lu tags to %s",
                       (unsigned long)app->bulk_scan_count, BULK_SCAN_PATH);
        }
        storage_file_free(app->bulk_scan_file);
        app->bulk_scan_file = NULL;
    }
    if(app->bulk_scan_storage) {
        furi_record_close(RECORD_STORAGE);
        app->bulk_scan_storage = NULL;
    }

    widget_reset(app->widget);
    popup_reset(app->popup);
}
