#pragma once

#include <furi.h>
#include <furi_hal.h>
#include "uhf_data.h"

typedef enum {
    // Init states
    UHFWorkerStateNone,
    UHFWorkerStateBroken,
    UHFWorkerStateReady,
    UHFWorkerStateVerify,
    // Main worker states
    UHFWorkerStateDetectSingle,
    UHFWorkerStateWriteSingle,
    UHFWorkerStateWriteKey,
    // Security states
    UHFWorkerStateSetAccessPassword, /* write Access Password to Reserved bank words 2-3 */
    UHFWorkerStateSetKillPassword,   /* write Kill Password to Reserved bank words 0-1   */
    UHFWorkerStateKillTag,           /* send CMD_INACTIVATE_KILL_TAG (irreversible)       */
    // Bulk inventory
    UHFWorkerStateBulkScan,          /* continuous single-poll loop; EPC written to SD   */
    // Module configuration (no tag required)
    UHFWorkerStateSetBaudrate,       /* send CMD_SET_COMMUNICATION_BAUD_RATE (setting_u32=bps) */
    UHFWorkerStateSetPower,          /* send CMD_SET_TRANSMITTING_POWER (setting_u32=centidBm) */
    UHFWorkerStateSetRegion,         /* send CMD_SETUP_WORK_AREA (setting_u8=region code)      */
    // Single-bank tag write (poll+select+write one bank; setting_u8=UHFBank)
    UHFWorkerStateWriteBank,
    // Kill password audit: read reserved bank first; brute force if locked
    // setting_u32 = resume point (0 = fresh); updated to found PW on success
    // setting_u8  = 1 if password was read directly, 0 if brute-forced
    UHFWorkerStateBruteForceKill,
    // Transition
    UHFWorkerStateStop,
} UHFWorkerState;

typedef enum {
    UHFWorkerEventSuccess,
    UHFWorkerEventFail,
    UHFWorkerEventNoTagDetected,
    UHFWorkerEventAborted,
    UHFWorkerEventCardDetected,
} UHFWorkerEvent;

typedef void (*UHFWorkerCallback)(UHFWorkerEvent event, void* ctx);

typedef struct UHFWorker {
    FuriThread* thread;
    UHFResponseData* response_data;
    UHFTag* uhf_tag;
    UHFWorkerCallback callback;
    UHFWorkerState state;
    void* ctx;
    FuriHalSerialHandle* serial_handle; /* acquired in alloc, released in free */
    /* Settings parameters — set before starting a configuration worker state */
    uint32_t setting_u32;   /* baudrate (bps) or power (centidBm)                */
    uint8_t  setting_u8;    /* region code (UHFWorkArea) or bank (UHFBank)        */
    uint32_t current_baud;  /* active module baud rate; defaults to DEFAULT_BAUD_RATE */
} UHFWorker;

int32_t uhf_worker_task(void* ctx);
UHFWorker* uhf_worker_alloc();
void uhf_worker_change_state(UHFWorker* worker, UHFWorkerState state);
void uhf_worker_start(
    UHFWorker* uhf_worker,
    UHFWorkerState state,
    UHFWorkerCallback callback,
    void* ctx);
void uhf_worker_stop(UHFWorker* uhf_worker);
void uhf_worker_free(UHFWorker* uhf_worker);
