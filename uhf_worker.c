#include "uhf_worker.h"
#include "uhf_cmd.h"
#include <storage/storage.h>
#include <stdio.h>

#define TAG         "UHFWorker"
#define CB_DELAY    100   /* ms for read/verify/select operations */
#define WRITE_DELAY 2000  /* ms for write/kill (inventory + T_prog per word + RF round trips) */
#define SD_LOG      "/ext/uhf_ops.log"

/* Append one formatted line to the SD card log for post-run analysis. */
static void sd_log(const char* fmt, ...) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SD_LOG, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
        va_end(args);
        if(n > 0) {
            buf[n] = '\n';
            storage_file_write(file, buf, (uint16_t)(n + 1));
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* ── Serial RX callback ─────────────────────────────────────────────────────
 * Called from the serial ISR for each received byte.
 * Reads the byte via furi_hal_serial_async_rx and appends it to the buffer
 * that was registered by set_rx_buffer().
 * ─────────────────────────────────────────────────────────────────────────── */
static void module_rx_callback(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* ctx) {
    if(event & FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(handle);
        UHFData* uhf_data = ctx;
        uhf_data_append(uhf_data, data);
    }
}

/* ── Switch the active RX buffer ───────────────────────────────────────────
 * Stops the current async RX, then restarts it with a new context pointer
 * so subsequent bytes accumulate in `buffer`.
 * ─────────────────────────────────────────────────────────────────────────── */
static void set_rx_buffer(FuriHalSerialHandle* handle, UHFData* buffer) {
    furi_hal_serial_async_rx_stop(handle);
    furi_hal_serial_async_rx_start(handle, module_rx_callback, buffer, false);
}

// yrm100 module commands
UHFWorkerEvent verify_module_connected(UHFWorker* uhf_worker) {
    FuriHalSerialHandle* handle = uhf_worker->serial_handle;
    UHFResponseData* uhf_response_data = uhf_worker->response_data;

    /* Give the YRM100 module time to power on and stabilize on the UART bus.
     * Without this delay, the first version query arrives before the module is
     * ready and returns no data → "no module" false-negative on first launch. */
    FURI_LOG_I(TAG, "verify_module_connected: waiting for module power-on ...");
    furi_delay_ms(500);

    furi_hal_serial_set_br(handle, uhf_worker->current_baud);
    uhf_response_data_reset(uhf_response_data);
    UHFData* hardware_version = uhf_response_data->head;
    UHFData* software_version = uhf_response_data_add_new_uhf_data(uhf_response_data);
    UHFData* manufacturer     = uhf_response_data_add_new_uhf_data(uhf_response_data);

    // read hardware version
    FURI_LOG_D(TAG, "Querying hardware version ...");
    set_rx_buffer(handle, hardware_version);
    furi_hal_serial_tx(handle, CMD_HARDWARE_VERSION.cmd, CMD_HARDWARE_VERSION.length);
    furi_delay_ms(CB_DELAY);

    // read software version
    FURI_LOG_D(TAG, "Querying software version ...");
    set_rx_buffer(handle, software_version);
    furi_hal_serial_tx(handle, CMD_SOFTWARE_VERSION.cmd, CMD_SOFTWARE_VERSION.length);
    furi_delay_ms(CB_DELAY);

    // read manufacturer
    FURI_LOG_D(TAG, "Querying manufacturer ...");
    set_rx_buffer(handle, manufacturer);
    furi_hal_serial_tx(handle, CMD_MANUFACTURERS.cmd, CMD_MANUFACTURERS.length);
    furi_delay_ms(CB_DELAY);

    // verify that we received all data
    if(!hardware_version->end || !software_version->end || !manufacturer->end) {
        FURI_LOG_E(TAG, "verify_module_connected FAIL: incomplete responses "
                        "(hw_end=%d sw_end=%d mfr_end=%d) — module not ready or not connected",
                   (int)hardware_version->end,
                   (int)software_version->end,
                   (int)manufacturer->end);
        sd_log("verify_module_connected FAIL: hw_end=%d sw_end=%d mfr_end=%d",
               (int)hardware_version->end, (int)software_version->end, (int)manufacturer->end);
        return UHFWorkerEventFail;
    }

    // verify checksums
    if(!uhf_data_verfiy_checksum(hardware_version) ||
       !uhf_data_verfiy_checksum(software_version) ||
       !uhf_data_verfiy_checksum(manufacturer)) {
        FURI_LOG_E(TAG, "verify_module_connected FAIL: checksum mismatch");
        sd_log("verify_module_connected FAIL: checksum mismatch");
        return UHFWorkerEventFail;
    }

    FURI_LOG_I(TAG, "Module connected OK (hw len=%u sw len=%u mfr len=%u)",
               (unsigned)hardware_version->length,
               (unsigned)software_version->length,
               (unsigned)manufacturer->length);
    sd_log("verify_module_connected OK: hw=%u sw=%u mfr=%u",
           (unsigned)hardware_version->length,
           (unsigned)software_version->length,
           (unsigned)manufacturer->length);
    return UHFWorkerEventSuccess;
}

uint8_t get_epc_length_in_bits(uint8_t pc) {
    uint8_t epc_length = pc;
    epc_length >>= 3;
    return (uint8_t)epc_length * 16; // x-words * 16 bits
}

bool send_set_select_command(
    FuriHalSerialHandle* handle,
    UHFData* selected_tag,
    UHFBank bank) {
    bool success = false;
    // Set select
    UHFData* select_cmd = uhf_data_alloc();
    select_cmd->start = true;
    select_cmd->length = CMD_SET_SELECT_PARAMETER.length;
    memcpy((void*)&select_cmd->data, (void*)&CMD_SET_SELECT_PARAMETER.cmd[0], select_cmd->length);
    // set select param
    size_t mask_length_bits = (size_t)get_epc_length_in_bits(selected_tag->data[6]);
    size_t mask_length_bytes = (size_t)mask_length_bits / 8;
    select_cmd->data[5] = bank; // 0x00=rfu, 0x01=epc, 0x10=tid, 0x11=user
    // set ptr
    select_cmd->data[9] = 0x20; // epc data begins after 0x20
    // set mask length
    select_cmd->data[10] = mask_length_bits;
    // set mask starting position
    select_cmd->length = 12;
    // set mask (EPC bytes from poll response at offset 8)
    for(size_t i = 0; i < mask_length_bytes; i++) {
        uhf_data_append(select_cmd, selected_tag->data[8 + i]);
    }
    uhf_data_append(select_cmd, 0x00); // add checksum section
    uhf_data_append(select_cmd, FRAME_END); // command end
    // add checksum
    select_cmd->data[select_cmd->length - 2] = uhf_data_calculate_checksum(select_cmd);

    FURI_LOG_D(TAG, "select: bank=%u mask_bits=%u cmd_len=%u",
               (unsigned)bank, (unsigned)mask_length_bits, (unsigned)select_cmd->length);

    UHFData* select_response = uhf_data_alloc();
    set_rx_buffer(handle, select_response);
    furi_hal_serial_tx(handle, select_cmd->data, select_cmd->length);
    furi_delay_ms(CB_DELAY);

    success = select_response->data[5] == 0x00;
    if(!success) {
        FURI_LOG_W(TAG, "select FAILED: response[5]=0x%02X (expected 0x00)",
                   (unsigned)select_response->data[5]);
    } else {
        FURI_LOG_D(TAG, "select OK");
    }

    uhf_data_free(select_cmd);
    uhf_data_free(select_response);

    return success;
}

bool read_bank(
    FuriHalSerialHandle* handle,
    UHFData* read_bank_cmd,
    UHFData* response_bank,
    UHFBank bank) {
    set_rx_buffer(handle, response_bank);
    read_bank_cmd->data[9] = bank;
    read_bank_cmd->data[read_bank_cmd->length - 2] = uhf_data_calculate_checksum(read_bank_cmd);
    uhf_data_reset(response_bank);
    furi_hal_serial_tx(handle, read_bank_cmd->data, read_bank_cmd->length);
    furi_delay_ms(CB_DELAY);
    bool ok = response_bank->data[2] == read_bank_cmd->data[2];
    if(!ok) {
        FURI_LOG_W(TAG, "read_bank(%u) FAIL: resp[2]=0x%02X expected=0x%02X end=%d",
                   (unsigned)bank,
                   (unsigned)response_bank->data[2],
                   (unsigned)read_bank_cmd->data[2],
                   (int)response_bank->end);
    }
    return ok;
}

bool write_bank(
    FuriHalSerialHandle* handle,
    UHFData* write_bank_cmd,
    UHFBank bank,
    uint8_t* bank_data,
    size_t bank_len) {
    UHFData* rp_data = uhf_data_alloc();
    write_bank_cmd->end = false;
    for(size_t i = 0; i < write_bank_cmd->length; i++) {
        continue;
    }
    set_rx_buffer(handle, rp_data);
    for(int i = 5; i < 9; i++) { // no access password for now
        write_bank_cmd->data[i] = 0;
    }
    write_bank_cmd->data[9] = bank;
    size_t word_len = bank_len / 2;
    write_bank_cmd->data[13] = word_len;
    write_bank_cmd->length = 14;
    write_bank_cmd->start = true;
    for(size_t i = 0; i < bank_len; i++) {
        uhf_data_append(write_bank_cmd, bank_data[i]);
    }
    uhf_data_append(write_bank_cmd, 00);
    uhf_data_append(write_bank_cmd, FRAME_END);
    write_bank_cmd->data[4] = write_bank_cmd->length - 7;
    write_bank_cmd->data[write_bank_cmd->length - 2] = uhf_data_calculate_checksum(write_bank_cmd);

    FURI_LOG_D(TAG, "write_bank: bank=%u word_addr=0x%02X word_cnt=%u data_len=%u",
               (unsigned)bank,
               (unsigned)write_bank_cmd->data[11],
               (unsigned)write_bank_cmd->data[13],
               (unsigned)bank_len);

    /* Dump TX frame to SD log for format verification */
    {
        char hex[128] = {0};
        size_t pos = 0;
        for(size_t i = 0; i < write_bank_cmd->length && pos + 3 < sizeof(hex); i++) {
            pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02X ", write_bank_cmd->data[i]);
        }
        sd_log("write_bank TX (%u bytes): %s", (unsigned)write_bank_cmd->length, hex);
    }

    furi_hal_serial_tx(handle, write_bank_cmd->data, write_bank_cmd->length);
    furi_delay_ms(WRITE_DELAY);
    bool success = rp_data->data[2] == write_bank_cmd->data[2];
    if(!success) {
        FURI_LOG_E(TAG,
                   "write_bank(%u) FAIL: resp[2]=0x%02X expected=0x%02X end=%d "
                   "err_code=0x%02X resp_len=%u",
                   (unsigned)bank,
                   (unsigned)rp_data->data[2],
                   (unsigned)write_bank_cmd->data[2],
                   (int)rp_data->end,
                   (unsigned)rp_data->data[5],  /* YRM100 specific error code */
                   (unsigned)rp_data->length);
        sd_log("write_bank(%u) FAIL: resp[2]=0x%02X expected=0x%02X end=%d err_code=0x%02X resp_len=%u",
               (unsigned)bank,
               (unsigned)rp_data->data[2],
               (unsigned)write_bank_cmd->data[2],
               (int)rp_data->end,
               (unsigned)rp_data->data[5],
               (unsigned)rp_data->length);
        /* Dump whatever bytes we received */
        {
            char hex[128] = {0};
            size_t pos = 0;
            for(size_t i = 0; i < rp_data->length && pos + 3 < sizeof(hex); i++) {
                pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02X ", rp_data->data[i]);
            }
            sd_log("write_bank RX (%u bytes): %s", (unsigned)rp_data->length, hex);
        }
    } else {
        FURI_LOG_I(TAG, "write_bank(%u) OK", (unsigned)bank);
        sd_log("write_bank(%u) OK", (unsigned)bank);
    }
    uhf_data_free(rp_data);
    return success;
}

UHFWorkerEvent send_polling_command(UHFWorker* uhf_worker, UHFData* raw_read_data) {
    FuriHalSerialHandle* handle = uhf_worker->serial_handle;
    set_rx_buffer(handle, raw_read_data);
    uhf_data_reset(raw_read_data);
    FURI_LOG_D(TAG, "poll: waiting for tag ...");
    uint32_t poll_count = 0;
    while(true) {
        furi_hal_serial_tx(handle, CMD_SINGLE_POLLING.cmd, CMD_SINGLE_POLLING.length);
        furi_delay_ms(100);
        poll_count++;
        if(uhf_worker->state == UHFWorkerStateStop) {
            FURI_LOG_I(TAG, "poll: aborted after %lu attempts", poll_count);
            sd_log("poll: ABORTED after %lu attempts", poll_count);
            return UHFWorkerEventAborted;
        }
        if(raw_read_data->end) {
            if(raw_read_data->data[1] == 0x01 && raw_read_data->data[5] == 0x15) {
                /* Error response 0x15 = no tag found; keep polling */
                uhf_data_reset(raw_read_data);
                continue;
            } else if(raw_read_data->data[1] == 0x02) {
                /* Inventory response — tag found */
                FURI_LOG_I(TAG, "poll: tag found after %lu poll(s), resp_len=%u",
                           poll_count, (unsigned)raw_read_data->length);
                sd_log("poll: tag found after %lu polls, resp_len=%u",
                       poll_count, (unsigned)raw_read_data->length);
                break;
            }
        }
    }
    return UHFWorkerEventSuccess;
}

UHFWorkerEvent read_single_card(UHFWorker* uhf_worker) {
    FuriHalSerialHandle* handle = uhf_worker->serial_handle;
    UHFResponseData* uhf_response_data = uhf_worker->response_data;
    uhf_response_data_reset(uhf_response_data);
    UHFData* raw_read_data = uhf_response_data_get_uhf_data(uhf_response_data, 0);
    furi_hal_serial_set_br(handle, uhf_worker->current_baud);
    FURI_LOG_I(TAG, "read_single_card: starting");

    send_polling_command(uhf_worker, raw_read_data);

    UHFTag* uhf_tag = uhf_worker->uhf_tag;
    uhf_tag_reset(uhf_tag);

    UHFData* raw_bank_data = uhf_data_alloc();
    size_t epc_length = (size_t)get_epc_length_in_bits(raw_read_data->data[6]) / 8;
    size_t offset = (size_t)(8 + epc_length);
    FURI_LOG_D(TAG, "read_single_card: epc_length=%u offset=%u", (unsigned)epc_length, (unsigned)offset);

    UHFData* read_bank_cmd = uhf_data_alloc();
    read_bank_cmd->length = CMD_READ_LABEL_DATA_STORAGE.length;
    memcpy(
        (void*)&read_bank_cmd->data[0],
        (void*)&CMD_READ_LABEL_DATA_STORAGE.cmd[0],
        read_bank_cmd->length);

    if(!send_set_select_command(handle, raw_read_data, EPC_BANK)) {
        FURI_LOG_E(TAG, "read_single_card: select failed — aborting");
        sd_log("read_single_card: select FAILED");
        uhf_data_free(raw_bank_data);
        uhf_data_free(read_bank_cmd);
        return UHFWorkerEventFail;
    }
    sd_log("read_single_card: select OK, reading EPC ...");

    bool epc_ok = false;
    int retry = 3;
    do {
        if(read_bank(handle, read_bank_cmd, raw_bank_data, EPC_BANK)) {
            uhf_tag_set_epc(uhf_tag, raw_bank_data->data + offset, epc_length + 2);
            epc_ok = true;
            FURI_LOG_I(TAG, "read_single_card: EPC read OK (len=%u)", (unsigned)uhf_tag->epc_length);
            break;
        }
        FURI_LOG_W(TAG, "read_single_card: EPC read attempt failed (resp[2]=0x%02X)",
                   (unsigned)raw_bank_data->data[2]);
        /* Re-select between retries in case the tag de-selected after the failed response */
        send_set_select_command(handle, raw_read_data, EPC_BANK);
    } while(retry--);

    if(!epc_ok) {
        FURI_LOG_E(TAG, "read_single_card: EPC bank read failed after all retries — OOM guard");
        sd_log("read_single_card: EPC read FAILED");
        uhf_data_free(raw_bank_data);
        uhf_data_free(read_bank_cmd);
        return UHFWorkerEventFail;
    }
    /* Log EPC hex inline with "reading TID" so only one sd_log (SD write) delays
     * the transition — extra writes let the tag de-select before TID read */
    {
        char hex[33] = {0};
        for(size_t i = 0; i < uhf_tag->epc_length && i < 16; i++)
            snprintf(hex + i * 2, 3, "%02X", uhf_tag->epc[i]);
        sd_log("read_single_card: EPC OK: %s; reading TID ...", hex);
    }

    /* TID bank word count: many Gen2 tags have only 4-6 words of TID.
     * The read template uses 8 words (0x08) which causes 0x09 overrun errors.
     * Use 4 words (8 bytes) as a safe minimum — expands later if needed. */
    read_bank_cmd->data[13] = 4;

    uhf_data_reset(raw_bank_data);
    retry = 3;
    do {
        /* TID EEPROM is slower and the tag needs settling time after EPC read */
        furi_delay_ms(150);
        if(read_bank(handle, read_bank_cmd, raw_bank_data, TID_BANK)) {
            uhf_tag_set_tid(uhf_tag, raw_bank_data->data + offset, 8); /* 4 words = 8 bytes */
            FURI_LOG_D(TAG, "read_single_card: TID read OK (len=%u)", (unsigned)uhf_tag->tid_length);
            /* Log TID hex */
            char tid_hex[33] = {0};
            for(size_t i = 0; i < uhf_tag->tid_length && i < 16; i++)
                snprintf(tid_hex + i * 2, 3, "%02X", uhf_tag->tid[i]);
            sd_log("read_single_card: TID=%s (len=%u)", tid_hex, (unsigned)uhf_tag->tid_length);
            break;
        }
        sd_log("read_single_card: TID attempt failed (resp[2]=0x%02X err=0x%02X)",
               (unsigned)raw_bank_data->data[2], (unsigned)raw_bank_data->data[5]);
        /* Re-select before next retry; the prior failed response may have de-selected the tag */
        send_set_select_command(handle, raw_read_data, EPC_BANK);
    } while(retry--);

    /* Restore word_count=8 for USER bank read */
    read_bank_cmd->data[13] = 8;
    uhf_data_reset(raw_bank_data);
    retry = 3;
    if(raw_read_data->data[6] & 0x04) {
        FURI_LOG_D(TAG, "read_single_card: USER bank present, reading ...");
        do {
            if(read_bank(handle, read_bank_cmd, raw_bank_data, USER_BANK)) {
                uhf_tag_set_user(uhf_tag, raw_bank_data->data + offset, 16);
                FURI_LOG_D(TAG, "read_single_card: USER bank read OK");
                break;
            }
        } while(retry--);
    }
    uhf_data_reset(raw_bank_data);
    uhf_data_free(raw_bank_data);
    uhf_data_free(read_bank_cmd);

    FURI_LOG_I(TAG, "read_single_card: SUCCESS");
    sd_log("read_single_card: SUCCESS -> ReadTagSuccess scene should show now");
    return UHFWorkerEventSuccess;
}

UHFWorkerEvent write_single_card(UHFWorker* uhf_worker) {
    FuriHalSerialHandle* handle = uhf_worker->serial_handle;
    UHFResponseData* uhf_response_data = uhf_worker->response_data;
    uhf_response_data_reset(uhf_response_data);
    UHFData* raw_read_data = uhf_response_data_get_uhf_data(uhf_response_data, 0);
    furi_hal_serial_set_br(handle, uhf_worker->current_baud);

    send_polling_command(uhf_worker, raw_read_data);
    // todo : rfu ?
    UHFTag* uhf_tag = uhf_worker->uhf_tag;

    UHFData* write_bank_cmd = uhf_data_alloc();
    write_bank_cmd->length = CMD_WRITE_LABEL_DATA_STORAGE.length;

    memcpy(
        (void*)&write_bank_cmd->data[0],
        (void*)&CMD_WRITE_LABEL_DATA_STORAGE.cmd[0],
        write_bank_cmd->length);
    if(!send_set_select_command(handle, raw_read_data, EPC_BANK)) {
        /* Bug fix: free allocation before early return to avoid leak */
        uhf_data_free(write_bank_cmd);
        return UHFWorkerEventFail;
    }

    if(raw_read_data->data[6] & 0x04) {
        if(!write_bank(handle, write_bank_cmd, USER_BANK, uhf_tag->user, uhf_tag->user_length))
            return UHFWorkerEventFail;
    }
    uint8_t write_data[uhf_tag->epc_length + 2];
    memcpy(&write_data, &raw_read_data->data[raw_read_data->length - 4], 2);
    memcpy(&write_data[2], &uhf_tag->epc, uhf_tag->epc_length);
    write_data[10] = 0xF1;
    if(!write_bank(handle, write_bank_cmd, EPC_BANK, write_data, uhf_tag->epc_length + 2)) {
        return UHFWorkerEventFail;
    }
    return UHFWorkerEventSuccess;
}

/* ── Set a 32-bit password in the Reserved (RFU) bank ───────────────────────
 * word_addr_lo:
 *   0x00 → Kill Password    (Reserved bank words 0-1)
 *   0x02 → Access Password  (Reserved bank words 2-3)
 *
 * Reads uhf_worker->uhf_tag->security_password[4] as the new value.
 * Returns UHFWorkerEventSuccess or UHFWorkerEventFail.
 * ─────────────────────────────────────────────────────────────────────────── */
static UHFWorkerEvent
    uhf_worker_set_reserved_bank_pw(UHFWorker* uhf_worker, uint8_t word_addr_lo) {
    FuriHalSerialHandle* handle = uhf_worker->serial_handle;
    UHFResponseData* uhf_response_data = uhf_worker->response_data;
    uhf_response_data_reset(uhf_response_data);
    UHFData* raw_read_data = uhf_response_data_get_uhf_data(uhf_response_data, 0);
    furi_hal_serial_set_br(handle, uhf_worker->current_baud);

    const char* pw_type = (word_addr_lo == 0x00) ? "Kill" : "Access";
    uint8_t* pw = uhf_worker->uhf_tag->security_password;
    FURI_LOG_I(TAG, "set_%s_password: %02X%02X%02X%02X -> word_addr=0x%02X",
               pw_type, pw[0], pw[1], pw[2], pw[3], (unsigned)word_addr_lo);

    /* 1. Poll until a tag responds */
    sd_log("set_%s_password: starting poll ...", pw_type);
    UHFWorkerEvent poll_result = send_polling_command(uhf_worker, raw_read_data);
    if(poll_result != UHFWorkerEventSuccess) {
        FURI_LOG_E(TAG, "set_%s_password: poll aborted", pw_type);
        sd_log("set_%s_password: poll ABORTED", pw_type);
        return UHFWorkerEventFail;
    }

    /* 2. Select the specific tag by EPC (required before any write) */
    if(!send_set_select_command(handle, raw_read_data, EPC_BANK)) {
        FURI_LOG_E(TAG, "set_%s_password: select failed", pw_type);
        sd_log("set_%s_password: select FAILED", pw_type);
        return UHFWorkerEventFail;
    }
    sd_log("set_%s_password: select OK, about to write_bank", pw_type);

    /* 3. Build the write command from the template */
    UHFData* write_cmd = uhf_data_alloc();
    write_cmd->length = CMD_WRITE_LABEL_DATA_STORAGE.length;
    memcpy(write_cmd->data, CMD_WRITE_LABEL_DATA_STORAGE.cmd, write_cmd->length);

    /* Set word address; write_bank() preserves data[10] and data[11] */
    write_cmd->data[10] = 0x00;          /* word_addr_hi (always 0 for Reserved bank) */
    write_cmd->data[11] = word_addr_lo;  /* 0x00 = Kill PW, 0x02 = Access PW         */

    /* 4. Write 4 bytes to Reserved bank (bank = RFU_BANK = 0) */
    bool ok = write_bank(handle, write_cmd, RFU_BANK, uhf_worker->uhf_tag->security_password, 4);
    uhf_data_free(write_cmd);

    if(ok) {
        FURI_LOG_I(TAG, "set_%s_password: SUCCESS", pw_type);
        sd_log("set_%s_password: SUCCESS", pw_type);
    } else {
        FURI_LOG_E(TAG, "set_%s_password: FAILED — tag may have non-zero access password "
                        "or was out of range", pw_type);
        sd_log("set_%s_password: FAILED (see write_bank entry above for err_code)", pw_type);
    }
    return ok ? UHFWorkerEventSuccess : UHFWorkerEventFail;
}

/* ── Send CMD_INACTIVATE_KILL_TAG ────────────────────────────────────────────
 * Reads uhf_worker->uhf_tag->security_password[4] as the kill password.
 * Polls for a tag, selects it, overrides the password bytes in the kill
 * command template, recomputes the checksum, sends the frame, checks the
 * response, and returns the result.
 *
 * WARNING: A successful kill permanently and irreversibly disables the tag.
 * ─────────────────────────────────────────────────────────────────────────── */
static UHFWorkerEvent uhf_worker_kill_tag_op(UHFWorker* uhf_worker) {
    FuriHalSerialHandle* handle = uhf_worker->serial_handle;
    UHFResponseData* uhf_response_data = uhf_worker->response_data;
    uhf_response_data_reset(uhf_response_data);
    UHFData* raw_read_data = uhf_response_data_get_uhf_data(uhf_response_data, 0);
    furi_hal_serial_set_br(handle, uhf_worker->current_baud);

    uint8_t* pw = uhf_worker->uhf_tag->security_password;
    FURI_LOG_I(TAG, "kill_tag: starting — kill_password=%02X%02X%02X%02X",
               pw[0], pw[1], pw[2], pw[3]);

    /* 1. Poll until a tag responds */
    UHFWorkerEvent poll_result = send_polling_command(uhf_worker, raw_read_data);
    if(poll_result != UHFWorkerEventSuccess) {
        FURI_LOG_E(TAG, "kill_tag: poll aborted");
        return UHFWorkerEventFail;
    }

    /* 2. Select the specific tag by EPC (module requires selection before kill) */
    if(!send_set_select_command(handle, raw_read_data, EPC_BANK)) {
        FURI_LOG_E(TAG, "kill_tag: select failed");
        return UHFWorkerEventFail;
    }

    /* 3. Build the kill command from the template */
    UHFData* cmd = uhf_data_alloc();
    memcpy(cmd->data, CMD_INACTIVATE_KILL_TAG.cmd, CMD_INACTIVATE_KILL_TAG.length);
    cmd->length = CMD_INACTIVATE_KILL_TAG.length; /* = 11 bytes */

    /* Override kill password bytes [5..8] with the user-supplied password */
    cmd->data[5] = pw[0];
    cmd->data[6] = pw[1];
    cmd->data[7] = pw[2];
    cmd->data[8] = pw[3];

    /* Recompute checksum at [length-2] = [9]:
     * uhf_data_calculate_checksum sums data[1..length-3] = data[1..8] */
    cmd->data[cmd->length - 2] = uhf_data_calculate_checksum(cmd);

    FURI_LOG_D(TAG, "kill_tag: sending CMD_INACTIVATE (0x65) checksum=0x%02X",
               (unsigned)cmd->data[cmd->length - 2]);

    /* 4. Send and wait for the module response */
    UHFData* rp = uhf_data_alloc();
    set_rx_buffer(handle, rp);
    {
        char hex[64] = {0};
        size_t pos = 0;
        for(size_t i = 0; i < cmd->length && pos + 3 < sizeof(hex); i++) {
            pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02X ", cmd->data[i]);
        }
        sd_log("kill_tag TX (%u bytes): %s", (unsigned)cmd->length, hex);
    }
    furi_hal_serial_tx(handle, cmd->data, cmd->length);
    furi_delay_ms(WRITE_DELAY);

    /* A successful kill response: frame is complete and echoes cmd code 0x65 */
    bool ok = rp->end && (rp->data[2] == 0x65);

    if(ok) {
        FURI_LOG_I(TAG, "kill_tag: SUCCESS — tag permanently disabled");
        sd_log("kill_tag: SUCCESS");
    } else {
        FURI_LOG_E(TAG,
                   "kill_tag: FAILED rp->end=%d rp->data[2]=0x%02X err_code=0x%02X resp_len=%u",
                   (int)rp->end,
                   (unsigned)rp->data[2],
                   (unsigned)rp->data[5],
                   (unsigned)rp->length);
        {
            char hex[64] = {0};
            size_t pos = 0;
            for(size_t i = 0; i < rp->length && pos + 3 < sizeof(hex); i++) {
                pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02X ", rp->data[i]);
            }
            sd_log("kill_tag FAILED: end=%d data[2]=0x%02X err_code=0x%02X resp_len=%u rx: %s",
                   (int)rp->end,
                   (unsigned)rp->data[2],
                   (unsigned)rp->data[5],
                   (unsigned)rp->length, hex);
        }
    }

    uhf_data_free(rp);
    uhf_data_free(cmd);

    return ok ? UHFWorkerEventSuccess : UHFWorkerEventFail;
}

/* ── Bulk Inventory Scan ──────────────────────────────────────────────────────
 * Continuously polls for tags using CMD_SINGLE_POLLING.
 * Deduplication: FNV-1a fingerprint ring buffer (4096 entries = 16 KB).
 *   - Same tag read within the last 4096 unique tags → skipped (dedup hit).
 *   - New tag → EPC written to app->bulk_scan_file (SD card), callback fired.
 * The scene (ctx = UHFApp*) increments the count and posts a display-refresh
 * event from the callback.  SD writes use Flipper's thread-safe Storage API.
 *
 * Exits when uhf_worker->state is set to UHFWorkerStateStop (non-blocking
 * signal).  Returns UHFWorkerEventAborted; callback fires once with Aborted.
 * ─────────────────────────────────────────────────────────────────────────── */
#define BULK_DEDUP_SIZE 4096u  /* ring buffer capacity: 4096 × 4 B = 16 KB */

static UHFWorkerEvent uhf_worker_bulk_scan(UHFWorker* worker) {
    FuriHalSerialHandle* handle = worker->serial_handle;
    furi_hal_serial_set_br(handle, worker->current_baud);

    UHFData* raw = uhf_data_alloc();
    set_rx_buffer(handle, raw);

    uint32_t* dedup = (uint32_t*)malloc(BULK_DEDUP_SIZE * sizeof(uint32_t));
    if(!dedup) {
        FURI_LOG_E(TAG, "bulk_scan: dedup alloc failed");
        uhf_data_free(raw);
        return UHFWorkerEventFail;
    }
    memset(dedup, 0, BULK_DEDUP_SIZE * sizeof(uint32_t));
    uint32_t dedup_head = 0;
    uint32_t dedup_fill = 0;

    FURI_LOG_I(TAG, "bulk_scan: starting");

    while(worker->state != UHFWorkerStateStop) {
        uhf_data_reset(raw);
        furi_hal_serial_tx(handle, CMD_SINGLE_POLLING.cmd, CMD_SINGLE_POLLING.length);
        furi_delay_ms(100);

        if(!raw->end) continue;
        if(raw->data[1] == 0x01) continue; /* error frame (0x15 = no tag, others) */
        if(raw->data[1] != 0x02) continue; /* not an inventory response            */

        /* PC byte: bits [7:3] = EPC word count; 1 word = 2 bytes */
        uint8_t epc_len = (uint8_t)((raw->data[6] >> 3u) * 2u);
        if(epc_len == 0 || (uint16_t)(8u + epc_len) > (uint16_t)raw->length) continue;

        uint8_t* epc_ptr = &raw->data[8];

        /* FNV-1a 32-bit fingerprint — ~1 collision in 80 K tags (birthday bound) */
        uint32_t fp = 2166136261u;
        for(uint8_t i = 0; i < epc_len; i++) {
            fp ^= (uint32_t)epc_ptr[i];
            fp *= 16777619u;
        }

        /* Linear scan of ring buffer; O(4096) uint32 compares ≈ 2 µs */
        uint32_t n = (dedup_fill < BULK_DEDUP_SIZE) ? dedup_fill : BULK_DEDUP_SIZE;
        bool is_dup = false;
        for(uint32_t i = 0; i < n; i++) {
            if(dedup[i] == fp) { is_dup = true; break; }
        }
        if(is_dup) continue;

        dedup[dedup_head] = fp;
        dedup_head = (dedup_head + 1u) % BULK_DEDUP_SIZE;
        if(dedup_fill < BULK_DEDUP_SIZE) dedup_fill++;

        /* Expose EPC through worker->uhf_tag so callback can write it to SD */
        uhf_tag_reset(worker->uhf_tag);
        uhf_tag_set_epc(worker->uhf_tag, epc_ptr, epc_len);

        worker->callback(UHFWorkerEventCardDetected, worker->ctx);
    }

    free(dedup);
    uhf_data_free(raw);
    FURI_LOG_I(TAG, "bulk_scan: stopped, fingerprints seen=%lu", (unsigned long)dedup_fill);
    return UHFWorkerEventAborted;
}

/* ── Send CMD_SET_COMMUNICATION_BAUD_RATE ────────────────────────────────────
 * worker->setting_u32 = desired baud rate in bps (e.g. 115200).
 * Encoding: big-endian uint16 = baud/600 placed in bytes[5:6].
 * If the module ACKs, re-initialises the serial handle at the new rate.
 * ─────────────────────────────────────────────────────────────────────────── */
static UHFWorkerEvent uhf_worker_set_baudrate_op(UHFWorker* worker) {
    FuriHalSerialHandle* handle = worker->serial_handle;
    uint32_t new_baud = worker->setting_u32;

    uint8_t cmd[9];
    memcpy(cmd, CMD_SET_COMMUNICATION_BAUD_RATE.cmd, CMD_SET_COMMUNICATION_BAUD_RATE.length);
    uint16_t enc = (uint16_t)(new_baud / 600u);
    cmd[5] = (uint8_t)(enc >> 8);
    cmd[6] = (uint8_t)(enc & 0xFFu);
    uint8_t cs = 0;
    for(uint8_t i = 1; i <= 6; i++) cs += cmd[i];
    cmd[7] = cs;

    UHFData* rp = uhf_data_alloc();
    set_rx_buffer(handle, rp);
    furi_hal_serial_tx(handle, cmd, CMD_SET_COMMUNICATION_BAUD_RATE.length);
    furi_delay_ms(CB_DELAY);

    bool ok = rp->end && (rp->data[2] == CMD_SET_COMMUNICATION_BAUD_RATE.cmd[2]);
    uhf_data_free(rp);

    if(ok) {
        worker->current_baud = new_baud;
        furi_hal_serial_set_br(handle, new_baud);
        FURI_LOG_I(TAG, "set_baudrate: %lu bps OK", (unsigned long)new_baud);
        sd_log("set_baudrate: %lu bps OK", (unsigned long)new_baud);
    } else {
        FURI_LOG_E(TAG, "set_baudrate: FAILED");
        sd_log("set_baudrate: FAILED for %lu bps", (unsigned long)new_baud);
    }
    return ok ? UHFWorkerEventSuccess : UHFWorkerEventFail;
}

/* ── Send CMD_SET_TRANSMITTING_POWER ─────────────────────────────────────────
 * worker->setting_u32 = power in centidBm (e.g. 2000 = 20.00 dBm).
 * Encoding: big-endian uint16 in bytes[5:6].
 * ─────────────────────────────────────────────────────────────────────────── */
static UHFWorkerEvent uhf_worker_set_power_op(UHFWorker* worker) {
    FuriHalSerialHandle* handle = worker->serial_handle;
    uint32_t cdBm = worker->setting_u32;

    uint8_t cmd[9];
    memcpy(cmd, CMD_SET_TRANSMITTING_POWER.cmd, CMD_SET_TRANSMITTING_POWER.length);
    cmd[5] = (uint8_t)((cdBm >> 8) & 0xFFu);
    cmd[6] = (uint8_t)(cdBm & 0xFFu);
    uint8_t cs = 0;
    for(uint8_t i = 1; i <= 6; i++) cs += cmd[i];
    cmd[7] = cs;

    UHFData* rp = uhf_data_alloc();
    set_rx_buffer(handle, rp);
    furi_hal_serial_tx(handle, cmd, CMD_SET_TRANSMITTING_POWER.length);
    furi_delay_ms(CB_DELAY);

    bool ok = rp->end && (rp->data[2] == CMD_SET_TRANSMITTING_POWER.cmd[2]);
    uhf_data_free(rp);
    FURI_LOG_I(TAG, "set_power: %lu cdBm %s", (unsigned long)cdBm, ok ? "OK" : "FAIL");
    sd_log("set_power: %lu cdBm %s", (unsigned long)cdBm, ok ? "OK" : "FAIL");
    return ok ? UHFWorkerEventSuccess : UHFWorkerEventFail;
}

/* ── Send CMD_SETUP_WORK_AREA ────────────────────────────────────────────────
 * worker->setting_u8 = region code (UHFWorkArea enum value).
 * ─────────────────────────────────────────────────────────────────────────── */
static UHFWorkerEvent uhf_worker_set_region_op(UHFWorker* worker) {
    FuriHalSerialHandle* handle = worker->serial_handle;

    uint8_t cmd[8];
    memcpy(cmd, CMD_SETUP_WORK_AREA.cmd, CMD_SETUP_WORK_AREA.length);
    cmd[5] = worker->setting_u8;
    uint8_t cs = 0;
    for(uint8_t i = 1; i <= 5; i++) cs += cmd[i];
    cmd[6] = cs;

    UHFData* rp = uhf_data_alloc();
    set_rx_buffer(handle, rp);
    furi_hal_serial_tx(handle, cmd, CMD_SETUP_WORK_AREA.length);
    furi_delay_ms(CB_DELAY);

    bool ok = rp->end && (rp->data[2] == CMD_SETUP_WORK_AREA.cmd[2]);
    uhf_data_free(rp);
    FURI_LOG_I(TAG, "set_region: code=%u %s", (unsigned)worker->setting_u8, ok ? "OK" : "FAIL");
    sd_log("set_region: code=%u %s", (unsigned)worker->setting_u8, ok ? "OK" : "FAIL");
    return ok ? UHFWorkerEventSuccess : UHFWorkerEventFail;
}

/* ── Write a single bank from uhf_tag to the next detected tag ───────────────
 * worker->setting_u8 = target UHFBank (EPC_BANK=1, TID_BANK=2, USER_BANK=3,
 *                                        RFU_BANK=0).
 * For EPC_BANK: prepends the 2 PC bytes from the poll response (same as
 *               write_single_card) so the EPC word count in the PC is correct.
 * ─────────────────────────────────────────────────────────────────────────── */
static UHFWorkerEvent uhf_worker_write_bank_single(UHFWorker* worker) {
    FuriHalSerialHandle* handle = worker->serial_handle;
    UHFResponseData* uhf_response_data = worker->response_data;
    uhf_response_data_reset(uhf_response_data);
    UHFData* raw_read_data = uhf_response_data_get_uhf_data(uhf_response_data, 0);
    furi_hal_serial_set_br(handle, worker->current_baud);

    UHFBank bank = (UHFBank)worker->setting_u8;
    UHFTag* tag = worker->uhf_tag;

    /* Poll for a tag */
    UHFWorkerEvent poll_result = send_polling_command(worker, raw_read_data);
    if(poll_result != UHFWorkerEventSuccess) {
        FURI_LOG_E(TAG, "write_bank_single: poll aborted");
        return UHFWorkerEventFail;
    }

    /* Select tag by EPC to prevent writes to wrong tags in field */
    if(!send_set_select_command(handle, raw_read_data, EPC_BANK)) {
        FURI_LOG_E(TAG, "write_bank_single: select failed");
        return UHFWorkerEventFail;
    }

    UHFData* write_cmd = uhf_data_alloc();
    write_cmd->length = CMD_WRITE_LABEL_DATA_STORAGE.length;
    memcpy(write_cmd->data, CMD_WRITE_LABEL_DATA_STORAGE.cmd, write_cmd->length);

    bool ok = false;
    if(bank == EPC_BANK && tag->epc_length > 0) {
        /* Prepend 2 PC bytes from poll response (same pattern as write_single_card) */
        uint8_t write_data[MAX_BANK_SIZE + 2];
        memcpy(write_data, &raw_read_data->data[raw_read_data->length - 4], 2);
        memcpy(write_data + 2, tag->epc, tag->epc_length);
        write_cmd->data[11] = 0x00;
        ok = write_bank(handle, write_cmd, EPC_BANK, write_data, tag->epc_length + 2);
    } else if(bank == TID_BANK && tag->tid_length > 0) {
        write_cmd->data[11] = 0x00;
        ok = write_bank(handle, write_cmd, TID_BANK, tag->tid, tag->tid_length);
    } else if(bank == USER_BANK && tag->user_length > 0) {
        write_cmd->data[11] = 0x00;
        ok = write_bank(handle, write_cmd, USER_BANK, tag->user, tag->user_length);
    } else if(bank == RFU_BANK) {
        write_cmd->data[10] = 0x00;
        write_cmd->data[11] = 0x00;
        ok = write_bank(handle, write_cmd, RFU_BANK, tag->security_password, 4);
    }

    uhf_data_free(write_cmd);
    FURI_LOG_I(TAG, "write_bank_single: bank=%u %s", (unsigned)bank, ok ? "OK" : "FAIL");
    return ok ? UHFWorkerEventSuccess : UHFWorkerEventFail;
}

/* ── Kill Password Audit ─────────────────────────────────────────────────────
 * Two-phase approach:
 *   Phase 1 – Try to READ kill password from reserved bank (words 0-1).
 *             Works when the access password is default (all zeros) and the
 *             bank is not permalocked.  Completes in < 1 second if unlocked.
 *   Phase 2 – Sequential brute force (0x00000000 → 0xFFFFFFFF).
 *             setting_u32 = resume point on entry; updated every BF_LOG_INT
 *             attempts so the Pi monitoring script can log progress and the
 *             scene can display a live count.  On success, setting_u32 holds
 *             the found password.  setting_u8: 1=read, 0=brute-forced.
 *
 * Progress callback: UHFWorkerEventCardDetected (fires every BF_LOG_INT att.)
 * Success callback:  UHFWorkerEventSuccess
 * Abort callback:    UHFWorkerEventAborted
 *
 * SD log markers parsed by pi_brute_monitor.py:
 *   [BF_SUCCESS] 0xXXXXXXXX          ← password found
 *   BF_progress: pw=0xXX att=NNN     ← periodic progress
 * ─────────────────────────────────────────────────────────────────────────── */
#define BF_DELAY_MS   50u    /* ms to wait after each kill attempt (tune down for speed) */
#define BF_REPOLL_INT 200u   /* re-poll every N attempts to verify tag still present     */
#define BF_LOG_INT    10000u /* SD log + progress callback every N attempts              */
#define BF_RESUME_PATH "/ext/uhf_bf_resume.txt"

#define DEAD_CONFIRM_POLLS    3u   /* number of polls that must all fail to declare tag dead */
#define DEAD_CONFIRM_DELAY_MS 300u /* ms between confirmation polls                         */
#define DEAD_SETTLE_MS        500u /* ms to let module recover before polling                */

/* Poll the tag DEAD_CONFIRM_POLLS times after a kill attempt.
 * Returns true only if every poll fails to see the tag — confirming it is dead. */
static bool confirm_tag_dead(FuriHalSerialHandle* handle) {
    furi_delay_ms(DEAD_SETTLE_MS);
    UHFData* poll = uhf_data_alloc();
    bool confirmed = true;
    for(uint8_t i = 0; i < DEAD_CONFIRM_POLLS; i++) {
        uhf_data_reset(poll);
        set_rx_buffer(handle, poll);
        furi_hal_serial_tx(handle, CMD_SINGLE_POLLING.cmd, CMD_SINGLE_POLLING.length);
        furi_delay_ms(CB_DELAY);
        if(poll->end && poll->data[1] == 0x02) {
            confirmed = false; /* tag still alive */
            break;
        }
        if(i < DEAD_CONFIRM_POLLS - 1u) furi_delay_ms(DEAD_CONFIRM_DELAY_MS);
    }
    uhf_data_free(poll);
    return confirmed;
}

static void write_resume_point(uint32_t pw) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, BF_RESUME_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[11];
        int n = snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)pw);
        if(n > 0) storage_file_write(file, buf, (uint16_t)n);
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static UHFWorkerEvent uhf_worker_brute_force_kill(UHFWorker* worker) {
    FuriHalSerialHandle* handle = worker->serial_handle;
    UHFResponseData* uhf_response_data = worker->response_data;
    uhf_response_data_reset(uhf_response_data);
    UHFData* raw_read_data = uhf_response_data_get_uhf_data(uhf_response_data, 0);
    furi_hal_serial_set_br(handle, worker->current_baud);

    worker->setting_u8 = 0; /* default: brute-forced */

    /* ── Wait for a tag ──────────────────────────────────────────────────── */
    FURI_LOG_I(TAG, "bf_kill: waiting for tag ...");
    sd_log("bf_kill: starting (resume=0x%08lX)", (unsigned long)worker->setting_u32);

    while(worker->state != UHFWorkerStateStop) {
        uhf_data_reset(raw_read_data);
        set_rx_buffer(handle, raw_read_data);
        furi_hal_serial_tx(handle, CMD_SINGLE_POLLING.cmd, CMD_SINGLE_POLLING.length);
        furi_delay_ms(CB_DELAY);
        if(raw_read_data->end && raw_read_data->data[1] == 0x02) break;
    }
    if(worker->state == UHFWorkerStateStop) return UHFWorkerEventAborted;

    /* Compute EPC length and offset from poll response */
    uint8_t epc_len = (uint8_t)((raw_read_data->data[6] >> 3u) * 2u);
    size_t  offset  = (size_t)(8u + epc_len);
    FURI_LOG_I(TAG, "bf_kill: tag found (epc_len=%u)", (unsigned)epc_len);

    /* ── Phase 1: Attempt direct read of reserved bank ───────────────────── */
    UHFData* read_cmd = uhf_data_alloc();
    read_cmd->length  = CMD_READ_LABEL_DATA_STORAGE.length;
    memcpy(read_cmd->data, CMD_READ_LABEL_DATA_STORAGE.cmd, read_cmd->length);
    read_cmd->data[9]  = 0x00; /* RFU_BANK */
    read_cmd->data[11] = 0x00; /* word_addr = 0 (kill PW is words 0-1) */
    read_cmd->data[13] = 0x02; /* word_count = 2 (4 bytes = kill PW)   */
    read_cmd->data[read_cmd->length - 2] = uhf_data_calculate_checksum(read_cmd);

    UHFData* read_rsp = uhf_data_alloc();

    if(send_set_select_command(handle, raw_read_data, EPC_BANK)) {
        if(read_bank(handle, read_cmd, read_rsp, RFU_BANK) &&
           (uint16_t)(offset + 4u) <= (uint16_t)read_rsp->length) {

            uint32_t found_pw = ((uint32_t)read_rsp->data[offset]     << 24) |
                                ((uint32_t)read_rsp->data[offset + 1] << 16) |
                                ((uint32_t)read_rsp->data[offset + 2] <<  8) |
                                 (uint32_t)read_rsp->data[offset + 3];

            sd_log("bf_kill: direct read returned 0x%08lX — verifying by kill attempt",
                   (unsigned long)found_pw);

            /* Verify: actually send the kill command with the read PW */
            UHFData* vkill_cmd = uhf_data_alloc();
            UHFData* vkill_rsp = uhf_data_alloc();
            memcpy(vkill_cmd->data, CMD_INACTIVATE_KILL_TAG.cmd, CMD_INACTIVATE_KILL_TAG.length);
            vkill_cmd->length = CMD_INACTIVATE_KILL_TAG.length;
            vkill_cmd->data[5] = (uint8_t)(found_pw >> 24);
            vkill_cmd->data[6] = (uint8_t)(found_pw >> 16);
            vkill_cmd->data[7] = (uint8_t)(found_pw >>  8);
            vkill_cmd->data[8] = (uint8_t)(found_pw & 0xFFu);
            vkill_cmd->data[vkill_cmd->length - 2] = uhf_data_calculate_checksum(vkill_cmd);

            uhf_data_reset(vkill_rsp);
            set_rx_buffer(handle, vkill_rsp);
            furi_hal_serial_tx(handle, vkill_cmd->data, vkill_cmd->length);
            furi_delay_ms(WRITE_DELAY);

            /* Whether the module replied 0x65 or went silent, confirm with
             * multiple polls so module-recovery noise doesn't give a false positive. */
            bool kill_ok = confirm_tag_dead(handle);

            uhf_data_free(vkill_cmd);
            uhf_data_free(vkill_rsp);

            if(kill_ok) {
                /* Kill confirmed — tag is dead */
                worker->setting_u32 = found_pw;
                worker->setting_u8  = 1; /* found via read */
                FURI_LOG_I(TAG, "[BF_SUCCESS] 0x%08lX (direct read + kill verified)",
                           (unsigned long)found_pw);
                sd_log("[BF_SUCCESS] 0x%08lX via direct read, kill confirmed",
                       (unsigned long)found_pw);
                uhf_data_free(read_cmd);
                uhf_data_free(read_rsp);
                return UHFWorkerEventSuccess;
            } else {
                /* Read returned bad data — fall through to brute force */
                FURI_LOG_W(TAG, "bf_kill: direct read PW 0x%08lX failed to kill — starting BF",
                           (unsigned long)found_pw);
                sd_log("bf_kill: direct read PW 0x%08lX rejected — falling through to brute force",
                       (unsigned long)found_pw);
            }
        } else {
            FURI_LOG_I(TAG, "bf_kill: reserved bank locked, starting brute force");
            sd_log("bf_kill: reserved bank locked, starting brute force");
        }
    }
    uhf_data_free(read_rsp);
    uhf_data_free(read_cmd);

    /* ── Phase 2: Sequential brute force ─────────────────────────────────── */
    UHFData* kill_cmd = uhf_data_alloc();
    memcpy(kill_cmd->data, CMD_INACTIVATE_KILL_TAG.cmd, CMD_INACTIVATE_KILL_TAG.length);
    kill_cmd->length = CMD_INACTIVATE_KILL_TAG.length;

    UHFData* kill_rsp = uhf_data_alloc();
    UHFData* poll_buf = uhf_data_alloc();

    uint32_t pw              = worker->setting_u32; /* resume point */
    uint32_t attempts        = 0;
    uint32_t repoll_start_pw = pw; /* first pw in the current repoll window */

    while(worker->state != UHFWorkerStateStop) {

        /* Build kill command with this password candidate */
        kill_cmd->data[5] = (uint8_t)(pw >> 24);
        kill_cmd->data[6] = (uint8_t)(pw >> 16);
        kill_cmd->data[7] = (uint8_t)(pw >>  8);
        kill_cmd->data[8] = (uint8_t)(pw & 0xFFu);
        kill_cmd->data[kill_cmd->length - 2] = uhf_data_calculate_checksum(kill_cmd);

        uhf_data_reset(kill_rsp);
        set_rx_buffer(handle, kill_rsp);
        furi_hal_serial_tx(handle, kill_cmd->data, kill_cmd->length);
        furi_delay_ms(BF_DELAY_MS);

        if(kill_rsp->end && kill_rsp->data[2] == 0x65) {
            /* Module accepted the kill command — verify tag is actually dead */
            if(confirm_tag_dead(handle)) {
                worker->setting_u32 = pw;
                worker->setting_u8  = 0; /* brute-forced */
                FURI_LOG_I(TAG, "[BF_SUCCESS] 0x%08lX (brute force)", (unsigned long)pw);
                sd_log("[BF_SUCCESS] 0x%08lX brute force after %lu attempts",
                       (unsigned long)pw, (unsigned long)attempts);
                uhf_data_free(kill_cmd);
                uhf_data_free(kill_rsp);
                uhf_data_free(poll_buf);
                return UHFWorkerEventSuccess;
            }
            /* confirm_tag_dead() failed — tag still alive despite 0x65 response.
             * Log and continue brute force. */
            sd_log("bf_kill: 0x%08lX got 0x65 but tag still alive — continuing",
                   (unsigned long)pw);
        }

        attempts++;

        /* Progress update every BF_LOG_INT attempts */
        if(attempts % BF_LOG_INT == 0) {
            worker->setting_u32 = pw; /* scene reads this for live display */
            worker->callback(UHFWorkerEventCardDetected, worker->ctx);
            sd_log("BF_progress: pw=0x%08lX att=%lu",
                   (unsigned long)pw, (unsigned long)attempts);
            write_resume_point(pw); /* persist resume point to SD */
        }

        /* Periodic re-poll — if tag vanishes it was killed without an explicit response */
        if(attempts % BF_REPOLL_INT == 0) {
            uhf_data_reset(poll_buf);
            set_rx_buffer(handle, poll_buf);
            furi_hal_serial_tx(handle, CMD_SINGLE_POLLING.cmd, CMD_SINGLE_POLLING.length);
            furi_delay_ms(CB_DELAY);
            if(!poll_buf->end || poll_buf->data[1] != 0x02) {
                /* Tag appears gone — run full confirmation before declaring success */
                if(confirm_tag_dead(handle)) {
                    /* Tag is actually dead — silent kill confirmed */
                    FURI_LOG_W(TAG,
                        "[BF_SUCCESS] silent kill: last_sent=0x%08lX window=[0x%08lX..0x%08lX]",
                        (unsigned long)pw,
                        (unsigned long)repoll_start_pw,
                        (unsigned long)pw);
                    sd_log("[BF_SUCCESS] silent kill: last_sent=0x%08lX window=[0x%08lX..0x%08lX] (%lu candidates)",
                        (unsigned long)pw,
                        (unsigned long)repoll_start_pw,
                        (unsigned long)pw,
                        (unsigned long)(pw - repoll_start_pw + 1u));
                    worker->setting_u32 = pw; /* display shows last-sent PW */
                    worker->setting_u8  = 0;
                    uhf_data_free(kill_cmd);
                    uhf_data_free(kill_rsp);
                    uhf_data_free(poll_buf);
                    return UHFWorkerEventSuccess;
                }
                /* confirm_tag_dead() says tag is still alive — module glitch, keep going */
                sd_log("bf_kill: repoll at pw=0x%08lX looked dead but confirmed alive — continuing",
                       (unsigned long)pw);
            }
            /* Window passed (or false alarm), tag still alive — advance window start */
            repoll_start_pw = pw + 1u;
        }

        if(pw == 0xFFFFFFFFu) {
            sd_log("bf_kill: exhausted full 32-bit space without success");
            break;
        }
        pw++;
    }

    /* Write final resume point so restart begins where we left off */
    if(worker->state == UHFWorkerStateStop) {
        write_resume_point(pw);
        worker->setting_u32 = pw;
        sd_log("bf_kill: stopped at 0x%08lX — resume point saved", (unsigned long)pw);
    }

    uhf_data_free(kill_cmd);
    uhf_data_free(kill_rsp);
    uhf_data_free(poll_buf);
    return UHFWorkerEventAborted;
}

int32_t uhf_worker_task(void* ctx) {
    UHFWorker* uhf_worker = ctx;
    if(uhf_worker->state == UHFWorkerStateVerify) {
        UHFWorkerEvent event = verify_module_connected(uhf_worker);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateDetectSingle) {
        UHFWorkerEvent event = read_single_card(uhf_worker);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateWriteSingle) {
        UHFWorkerEvent event = write_single_card(uhf_worker);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateSetAccessPassword) {
        UHFWorkerEvent event = uhf_worker_set_reserved_bank_pw(uhf_worker, 0x02);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateSetKillPassword) {
        UHFWorkerEvent event = uhf_worker_set_reserved_bank_pw(uhf_worker, 0x00);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateKillTag) {
        UHFWorkerEvent event = uhf_worker_kill_tag_op(uhf_worker);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateBulkScan) {
        UHFWorkerEvent event = uhf_worker_bulk_scan(uhf_worker);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateSetBaudrate) {
        UHFWorkerEvent event = uhf_worker_set_baudrate_op(uhf_worker);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateSetPower) {
        UHFWorkerEvent event = uhf_worker_set_power_op(uhf_worker);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateSetRegion) {
        UHFWorkerEvent event = uhf_worker_set_region_op(uhf_worker);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateWriteBank) {
        UHFWorkerEvent event = uhf_worker_write_bank_single(uhf_worker);
        uhf_worker->callback(event, uhf_worker->ctx);
    } else if(uhf_worker->state == UHFWorkerStateBruteForceKill) {
        UHFWorkerEvent event = uhf_worker_brute_force_kill(uhf_worker);
        uhf_worker->callback(event, uhf_worker->ctx);
    }
    return 0;
}

UHFWorker* uhf_worker_alloc() {
    FURI_LOG_I(TAG, "uhf_worker_alloc: acquiring USART serial @ %u baud", DEFAULT_BAUD_RATE);
    UHFWorker* uhf_worker = (UHFWorker*)malloc(sizeof(UHFWorker));
    uhf_worker->thread = furi_thread_alloc_ex("UHFWorker", 8 * 1024, uhf_worker_task, uhf_worker);
    uhf_worker->response_data = uhf_response_data_alloc();
    uhf_worker->callback = NULL;
    uhf_worker->ctx = NULL;
    uhf_worker->setting_u32 = 0;
    uhf_worker->setting_u8  = 0;
    uhf_worker->current_baud = DEFAULT_BAUD_RATE;
    uhf_worker->serial_handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    furi_assert(uhf_worker->serial_handle);
    furi_hal_serial_init(uhf_worker->serial_handle, DEFAULT_BAUD_RATE);
    FURI_LOG_I(TAG, "uhf_worker_alloc: OK");
    return uhf_worker;
}

void uhf_worker_change_state(UHFWorker* worker, UHFWorkerState state) {
    worker->state = state;
}

void uhf_worker_start(
    UHFWorker* uhf_worker,
    UHFWorkerState state,
    UHFWorkerCallback callback,
    void* ctx) {
    uhf_worker->state = state;
    uhf_worker->callback = callback;
    uhf_worker->ctx = ctx;
    furi_thread_start(uhf_worker->thread);
}

void uhf_worker_stop(UHFWorker* uhf_worker) {
    furi_assert(uhf_worker);
    furi_assert(uhf_worker->thread);

    if(furi_thread_get_state(uhf_worker->thread) != FuriThreadStateStopped) {
        uhf_worker_change_state(uhf_worker, UHFWorkerStateStop);
        furi_thread_join(uhf_worker->thread);
    }
}

void uhf_worker_free(UHFWorker* uhf_worker) {
    furi_assert(uhf_worker);
    FURI_LOG_I(TAG, "uhf_worker_free: releasing serial and resources");
    furi_thread_free(uhf_worker->thread);
    uhf_response_data_free(uhf_worker->response_data);
    furi_hal_serial_async_rx_stop(uhf_worker->serial_handle);
    furi_hal_serial_deinit(uhf_worker->serial_handle);
    furi_hal_serial_control_release(uhf_worker->serial_handle);
    free(uhf_worker);
    FURI_LOG_I(TAG, "uhf_worker_free: done");
}
