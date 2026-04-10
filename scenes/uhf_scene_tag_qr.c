/**
 * uhf_scene_tag_qr.c
 *
 * Scene: Show EPC as QR Code
 *
 * Displays the current tag's EPC as a large, high-contrast QR code on the
 * right half of the Flipper screen, with the raw hex EPC text on the left.
 *
 * Entry point: tag_menu scene -> "Show EPC as QR Code"
 * Exit:        Back button -> returns to tag_menu
 *
 * QR encoding:
 *   - Data:  uppercase hex EPC string (e.g. "E20000123456789012345678")
 *   - Mode:  byte (compatible with all QR readers; alphanumeric subset)
 *   - ECC:   Low (maximises data capacity on small screen)
 *   - Version: smallest that fits (tried 3-10; v3 covers 96-bit EPC)
 *
 * Layout (128x64 px screen):
 *   +----------------------------------------------------------------+
 *   |  EPC:          |                                              |
 *   |  XXXXXXXX      |         QR CODE (scaled to fit)             |
 *   |  XXXXXXXX      |         centered in right 64px column       |
 *   |  XXXXXXXX      |                                             |
 *   +----------------------------------------------------------------+
 *   <-- 63px ------->|<-------------- 64px ------------------------->
 */

#include "../uhf_app_i.h"

#define TAG                "UHFTagQR"
#define QR_AREA_X          64  /* right-column start pixel                */
#define QR_AREA_W          64  /* width/height of the QR drawing area     */
#define QR_MIN_VERSION     2   /* v2=25x25, pixel_size=2 -> 50px; fits 96-bit EPC */
#define QR_MAX_VERSION     10  /* highest version attempted               */
#define EPC_CHARS_PER_LINE 8   /* hex chars per text line (~48px at 6px/ch) */

/* QR byte-mode capacity table (ECC_LOW), indexed by version 0-10. */
static const uint16_t QR_BYTE_CAP_ECC_LOW[] = {0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271};

/* ── Draw callback ──────────────────────────────────────────────────────────── */
void uhf_scene_tag_qr_draw_callback(Canvas* canvas, void* model) {
    UHFApp* app = *(UHFApp**)model;

    canvas_clear(canvas);

    /* Left column: EPC header + hex bytes */
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 7, "EPC:");

    if(app->qr_epc_str_len > 0) {
        canvas_set_font(canvas, FontKeyboard);
        uint8_t line_y = 17;
        for(uint8_t i = 0; i < app->qr_epc_str_len && line_y <= 60; i += EPC_CHARS_PER_LINE) {
            char line_buf[EPC_CHARS_PER_LINE + 1];
            uint8_t chunk = app->qr_epc_str_len - i;
            if(chunk > EPC_CHARS_PER_LINE) chunk = EPC_CHARS_PER_LINE;
            memcpy(line_buf, app->qr_epc_str + i, chunk);
            line_buf[chunk] = '\0';
            canvas_draw_str(canvas, 0, line_y, line_buf);
            line_y += 9;
        }
    }

    /* Divider */
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_line(canvas, QR_AREA_X - 1, 0, QR_AREA_X - 1, 63);

    /* Right column: QR code */
    if(!app->qr_encoded) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, QR_AREA_X + 32, 28, AlignCenter, AlignCenter, "QR Error");
        canvas_draw_str_aligned(canvas, QR_AREA_X + 32, 38, AlignCenter, AlignCenter, "EPC empty?");
        return;
    }

    QRCode* qr = &app->qr_code;

    uint8_t pixel_size = QR_AREA_W / qr->size;
    if(pixel_size < 1) pixel_size = 1;

    uint8_t qr_draw_w = qr->size * pixel_size;
    uint8_t qr_x      = (uint8_t)(QR_AREA_X + (QR_AREA_W - qr_draw_w) / 2);
    uint8_t qr_y      = (uint8_t)((64 - qr_draw_w) / 2);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, QR_AREA_X, 0, QR_AREA_W, 64);

    canvas_set_color(canvas, ColorBlack);
    for(uint8_t y = 0; y < qr->size; y++) {
        for(uint8_t x = 0; x < qr->size; x++) {
            if(qrcode_getModule(qr, x, y)) {
                canvas_draw_box(
                    canvas,
                    (uint8_t)(qr_x + x * pixel_size),
                    (uint8_t)(qr_y + y * pixel_size),
                    pixel_size,
                    pixel_size);
            }
        }
    }
}

/* ── Input callback ─────────────────────────────────────────────────────────── */
bool uhf_scene_tag_qr_input_callback(InputEvent* event, void* ctx) {
    UNUSED(event);
    UNUSED(ctx);
    return false; /* let ViewDispatcher handle navigation */
}

/* ── on_enter ───────────────────────────────────────────────────────────────── */
void uhf_scene_tag_qr_on_enter(void* ctx) {
    UHFApp* app = ctx;
    UHFTag* tag = app->worker->uhf_tag;

    app->qr_encoded     = false;
    app->qr_epc_str_len = 0;
    app->qr_epc_str[0]  = '\0';

    if(tag->epc_length <= 2) {
        FURI_LOG_W(TAG, "EPC is empty - nothing to encode");
        view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewTagQr);
        return;
    }

    uint8_t epc_byte_count = tag->epc_length - 2;

    for(uint8_t i = 0; i < epc_byte_count && (i * 2 + 2) < (int)sizeof(app->qr_epc_str); i++) {
        snprintf(app->qr_epc_str + i * 2, 3, "%02X", tag->epc[i + 2]);
    }
    app->qr_epc_str_len              = epc_byte_count * 2;
    app->qr_epc_str[app->qr_epc_str_len] = '\0';

    FURI_LOG_D(TAG, "Encoding EPC QR: \"%s\" (%u chars)", app->qr_epc_str, app->qr_epc_str_len);

    uint8_t start_version = QR_MIN_VERSION;
    for(uint8_t v = QR_MIN_VERSION; v <= QR_MAX_VERSION; v++) {
        if(QR_BYTE_CAP_ECC_LOW[v] >= app->qr_epc_str_len) {
            start_version = v;
            break;
        }
        start_version = QR_MAX_VERSION;
    }

    for(uint8_t v = start_version; v <= QR_MAX_VERSION; v++) {
        uint16_t buf_size = qrcode_getBufferSize(v);

        if(app->qr_modules) {
            free(app->qr_modules);
            app->qr_modules = NULL;
        }

        app->qr_modules = (uint8_t*)malloc(buf_size);
        if(!app->qr_modules) {
            FURI_LOG_E(TAG, "malloc failed for QR buffer v%u (%u bytes)", v, buf_size);
            break;
        }

        if(qrcode_initBytes(
               &app->qr_code,
               app->qr_modules,
               v,
               ECC_LOW,
               (uint8_t*)app->qr_epc_str,
               (uint16_t)app->qr_epc_str_len) == 0) {
            app->qr_encoded = true;
            FURI_LOG_D(
                TAG,
                "QR encoded: v%u, %u modules, pixel_size=%u",
                v,
                app->qr_code.size,
                (uint8_t)(QR_AREA_W / app->qr_code.size));
            break;
        }
    }

    if(!app->qr_encoded) {
        FURI_LOG_E(TAG, "QR encoding failed for %u-char EPC string", app->qr_epc_str_len);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, UHFViewTagQr);
}

/* ── on_event ───────────────────────────────────────────────────────────────── */
bool uhf_scene_tag_qr_on_event(void* ctx, SceneManagerEvent event) {
    UNUSED(ctx);
    UNUSED(event);
    return false;
}

/* ── on_exit ────────────────────────────────────────────────────────────────── */
void uhf_scene_tag_qr_on_exit(void* ctx) {
    UHFApp* app = ctx;

    if(app->qr_modules) {
        free(app->qr_modules);
        app->qr_modules = NULL;
    }
    app->qr_encoded = false;
}
