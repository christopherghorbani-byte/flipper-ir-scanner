#include <furi.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <infrared_worker.h>
#include <infrared.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG             "IRScanner"
#define WAVEFORM_MAX    64
#define WAVEFORM_X      0
#define WAVEFORM_Y      42
#define WAVEFORM_W      128
#define WAVEFORM_H      10

typedef struct {
    // Signal data (mutex-protected)
    bool     signal_received;
    bool     is_decoded;
    char     protocol_str[40];
    char     data_str[48];
    uint16_t waveform[WAVEFORM_MAX];
    uint8_t  waveform_len;
    uint32_t signal_count;

    // Sync
    FuriMutex* mutex;

    // IR
    InfraredWorker* worker;

    // GUI
    Gui*      gui;
    ViewPort* view_port;

    // Input
    FuriMessageQueue* input_queue;

    bool running;
} IRScannerState;

// ─── Draw ────────────────────────────────────────────────────────────────────

static void ir_scanner_draw(Canvas* canvas, void* ctx) {
    IRScannerState* s = (IRScannerState*)ctx;
    furi_mutex_acquire(s->mutex, FuriWaitForever);

    canvas_clear(canvas);

    // Header
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "IR Scanner");
    canvas_draw_line(canvas, 0, 13, 128, 13);

    canvas_set_font(canvas, FontSecondary);

    if(!s->signal_received) {
        canvas_draw_str(canvas, 2, 27, "Waiting for IR signal...");
        canvas_draw_str(canvas, 2, 38, "Point a remote and press");
        canvas_draw_str(canvas, 2, 49, "any button.");
    } else {
        // Protocol line
        canvas_draw_str(canvas, 2, 25, s->protocol_str);

        // Address / command line
        canvas_draw_str(canvas, 2, 36, s->data_str);

        // Waveform box outline
        canvas_draw_frame(canvas, WAVEFORM_X, WAVEFORM_Y, WAVEFORM_W, WAVEFORM_H);

        // Draw waveform pulses inside the box
        if(s->waveform_len > 0) {
            int x = WAVEFORM_X + 1;
            bool high = true;
            for(int i = 0; i < s->waveform_len && x < (WAVEFORM_X + WAVEFORM_W - 1); i++) {
                int w = s->waveform[i];
                if(w < 1) w = 1;
                if(high) {
                    // Draw filled pulse bar
                    canvas_draw_box(
                        canvas,
                        x,
                        WAVEFORM_Y + 2,
                        w,
                        WAVEFORM_H - 4);
                }
                x += w;
                high = !high;
            }
        }

        // Signal count
        char count_buf[24];
        snprintf(count_buf, sizeof(count_buf), "Signals: %lu", (unsigned long)s->signal_count);
        canvas_draw_str(canvas, 2, 62, count_buf);
    }

    // Back hint
    elements_button_left(canvas, "Back");

    furi_mutex_release(s->mutex);
}

// ─── Input ───────────────────────────────────────────────────────────────────

static void ir_scanner_input(InputEvent* event, void* ctx) {
    IRScannerState* s = (IRScannerState*)ctx;
    furi_message_queue_put(s->input_queue, event, FuriWaitForever);
}

// ─── IR callback ─────────────────────────────────────────────────────────────

static void ir_scanner_signal_callback(void* ctx, InfraredWorkerSignal* received_signal) {
    IRScannerState* s = (IRScannerState*)ctx;
    furi_mutex_acquire(s->mutex, FuriWaitForever);

    s->signal_received = true;
    s->signal_count++;

    if(infrared_worker_signal_is_decoded(received_signal)) {
        // Known protocol — show decoded data
        const InfraredMessage* msg = infrared_worker_get_decoded_signal(received_signal);

        snprintf(
            s->protocol_str,
            sizeof(s->protocol_str),
            "%s%s",
            infrared_get_protocol_name(msg->protocol),
            msg->repeat ? " (repeat)" : "");

        snprintf(
            s->data_str,
            sizeof(s->data_str),
            "Addr:0x%04lX  Cmd:0x%04lX",
            (unsigned long)msg->address,
            (unsigned long)msg->command);

        // Simple fixed waveform placeholder for decoded signals
        // (shows a generic NEC-style burst pattern)
        static const uint16_t decoded_wave[] = {8, 4, 1, 1, 1, 3, 1, 1, 1, 3, 1, 1, 1, 1};
        s->waveform_len = sizeof(decoded_wave) / sizeof(decoded_wave[0]);
        memcpy(s->waveform, decoded_wave, sizeof(decoded_wave));

    } else {
        // Unknown protocol — show raw timing data
        const uint32_t* timings;
        size_t          timings_cnt;
        infrared_worker_get_raw_signal(received_signal, &timings, &timings_cnt);

        snprintf(
            s->protocol_str,
            sizeof(s->protocol_str),
            "RAW  (%d pulses)",
            (int)timings_cnt);

        snprintf(s->data_str, sizeof(s->data_str), "Unknown protocol");

        // Scale timings to fit the waveform display width
        size_t   use = timings_cnt < WAVEFORM_MAX ? timings_cnt : WAVEFORM_MAX;
        uint32_t total = 0;
        for(size_t i = 0; i < use; i++) total += timings[i];

        s->waveform_len = 0;
        if(total > 0) {
            for(size_t i = 0; i < use; i++) {
                uint32_t w = (timings[i] * (WAVEFORM_W - 2)) / total;
                s->waveform[s->waveform_len++] = (uint16_t)(w < 1 ? 1 : w);
            }
        }
    }

    furi_mutex_release(s->mutex);
    view_port_update(s->view_port);
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int32_t ir_scanner_app(void* p) {
    UNUSED(p);

    IRScannerState* s = malloc(sizeof(IRScannerState));
    furi_check(s);
    memset(s, 0, sizeof(IRScannerState));
    s->running = true;

    s->mutex       = furi_mutex_alloc(FuriMutexTypeNormal);
    s->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    // GUI setup
    s->view_port = view_port_alloc();
    view_port_draw_callback_set(s->view_port, ir_scanner_draw, s);
    view_port_input_callback_set(s->view_port, ir_scanner_input, s);

    s->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(s->gui, s->view_port, GuiLayerFullscreen);

    // IR worker
    s->worker = infrared_worker_alloc();
    infrared_worker_rx_set_received_signal_callback(s->worker, ir_scanner_signal_callback, s);
    infrared_worker_rx_start(s->worker);

    FURI_LOG_I(TAG, "IR Scanner started");

    // Main loop
    InputEvent event;
    while(s->running) {
        if(furi_message_queue_get(s->input_queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypePress && event.key == InputKeyBack) {
                s->running = false;
            }
        }
    }

    // Cleanup
    infrared_worker_rx_stop(s->worker);
    infrared_worker_free(s->worker);

    gui_remove_view_port(s->gui, s->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(s->view_port);

    furi_message_queue_free(s->input_queue);
    furi_mutex_free(s->mutex);
    free(s);

    return 0;
}
