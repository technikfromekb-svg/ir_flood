#include <furi.h>
#include <furi_hal_infrared.h>
#include <gui/gui.h>
#include <input/input.h>

#define RAW_BUFFER_SIZE   256
#define MAX_COMMAND       255

typedef enum {
    ProtocolNec,
    ProtocolSamsung,
    ProtocolSony12,
    ProtocolRc5,
    ProtocolRc6,
    ProtocolMaxCount
} IrProtocol;

typedef struct {
    IrProtocol protocol;
    uint8_t address;
    uint8_t command;
    bool is_tx_active;
    uint32_t flood_interval_ms;
    FuriMessageQueue* event_queue;
    FuriTimer* timer;
} IrFloodApp;

typedef enum {
    EventTypeInput,
    EventTypeTick,
} AppEventType;

typedef struct {
    AppEventType type;
    InputEvent input;
} AppEvent;

static uint32_t raw_durations[RAW_BUFFER_SIZE];
static volatile uint32_t tx_index = 0;
static volatile uint32_t current_tx_size = 0;
static uint32_t current_carrier_freq = 38000;
static volatile bool is_ir_tx_busy = false;

static const char* protocol_names[] = {
    "NEC (38kHz)",
    "SAMSUNG (38kHz)",
    "SONY 12B (40kHz)",
    "RC5 (36kHz)",
    "RC6 (36kHz)"
};

static bool add_duration(uint32_t duration) {
    if(tx_index >= RAW_BUFFER_SIZE) return false;
    raw_durations[tx_index++] = duration;
    return true;
}

static FuriHalInfraredTxGetDataState tx_get_data_callback(void* context, uint32_t* duration, bool* level) {
    UNUSED(context);
    if(tx_index >= current_tx_size) {
        is_ir_tx_busy = false;
        return FuriHalInfraredTxGetDataStateLastDone;
    }
    *duration = raw_durations[tx_index];
    *level = (tx_index % 2 == 0);
    tx_index++;
    return (tx_index >= current_tx_size) ? FuriHalInfraredTxGetDataStateLastDone : FuriHalInfraredTxGetDataStateOk;
}

static bool encode_rc5(uint8_t addr, uint8_t cmd) {
    uint32_t rc5_data = (1 << 13) | (1 << 12) | ((addr & 0x1F) << 6) | (cmd & 0x3F);
    for(int i = 13; i >= 0; i--) {
        bool bit = (rc5_data >> i) & 1;
        if(bit) {
            if(!add_duration(889)) return false;
            if(!add_duration(889)) return false;
        } else {
            if(!add_duration(889)) return false;
            if(!add_duration(889)) return false;
        }
    }
    return true;
}

static bool encode_rc6(uint8_t addr, uint8_t cmd) {
    if(!add_duration(2666)) return false;
    if(!add_duration(889)) return false;
    uint32_t rc6_data = (1 << 20) | (0 << 19) | ((addr & 0xFF) << 8) | (cmd & 0xFF);
    for(int i = 20; i >= 0; i--) {
        bool bit = (rc6_data >> i) & 1;
        if(bit) {
            if(!add_duration(444)) return false;
            if(!add_duration(444)) return false;
        } else {
            if(!add_duration(444)) return false;
            if(!add_duration(444)) return false;
        }
    }
    return true;
}

static void encode_and_transmit(IrFloodApp* app) {
    if(is_ir_tx_busy) {
        furi_hal_infrared_async_tx_stop();
        is_ir_tx_busy = false;
    }

    tx_index = 0;
    bool success = true;

    switch(app->protocol) {
        case ProtocolNec:
            current_carrier_freq = 38000;
            add_duration(9000); add_duration(4500);
            {
                uint32_t tx_data = app->address | ((~app->address & 0xFF) << 8) |
                                   (app->command << 16) | ((~app->command & 0xFF) << 24);
                for(int i = 0; i < 32; i++) {
                    add_duration(560);
                    add_duration((tx_data & (1UL << i)) ? 1690 : 560);
                }
                add_duration(560);
            }
            break;

        case ProtocolSamsung:
            current_carrier_freq = 38000;
            add_duration(4500); add_duration(4500);
            {
                uint32_t tx_data = app->address | (app->address << 8) |
                                   (app->command << 16) | ((~app->command & 0xFF) << 24);
                for(int i = 0; i < 32; i++) {
                    add_duration(560);
                    add_duration((tx_data & (1UL << i)) ? 1680 : 560);
                }
                add_duration(560);
            }
            break;

        case ProtocolSony12:
            current_carrier_freq = 40000;
            add_duration(2400); add_duration(600);
            {
                uint32_t tx_data = (app->command & 0x7F) | ((app->address & 0x1F) << 7);
                for(int i = 0; i < 12; i++) {
                    add_duration((tx_data & (1UL << i)) ? 1200 : 600);
                    add_duration(600);
                }
            }
            break;

        case ProtocolRc5:
            current_carrier_freq = 36000;
            success = encode_rc5(app->address, app->command);
            break;

        case ProtocolRc6:
            current_carrier_freq = 36000;
            success = encode_rc6(app->address, app->command);
            break;

        default: break;
    }

    if(!success || tx_index == 0) {
        is_ir_tx_busy = false;
        return;
    }

    current_tx_size = tx_index;
    tx_index = 0;
    is_ir_tx_busy = true;

    furi_hal_infrared_async_tx_set_data_isr_callback(tx_get_data_callback, NULL);
    furi_hal_infrared_async_tx_start(current_carrier_freq, 0.5f);
}

static void draw_callback(Canvas* canvas, void* context) {
    IrFloodApp* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "IR Multi-Flood Pro");

    canvas_set_font(canvas, FontSecondary);
    char buffer[64];

    snprintf(buffer, sizeof(buffer), "Proto: %s", protocol_names[app->protocol]);
    canvas_draw_str(canvas, 2, 21, buffer);

    if(app->is_tx_active) {
        canvas_draw_str(canvas, 2, 32, "STATUS: FLOODING...");
        snprintf(buffer, sizeof(buffer), "CMD: 0x%02X  Addr: 0x%02X", app->command, app->address);
        canvas_draw_str(canvas, 2, 43, buffer);
    } else {
        canvas_draw_str(canvas, 2, 32, "Proto sel (UP/DN)");
        canvas_draw_str(canvas, 2, 43, "OK: start flood");
    }

    uint8_t progress_width = (uint8_t)((uint32_t)app->command * 60 / MAX_COMMAND);
    canvas_draw_frame(canvas, 2, 46, 62, 6);
    if(progress_width > 0) {
        canvas_draw_box(canvas, 3, 47, progress_width, 4);
    }
    snprintf(buffer, sizeof(buffer), "%d%%", (app->command * 100) / MAX_COMMAND);
    canvas_draw_str(canvas, 68, 52, buffer);

    snprintf(buffer, sizeof(buffer), "Speed: %lums (< L/R >)", app->flood_interval_ms);
    canvas_draw_str(canvas, 2, 62, buffer);
}

static void input_callback(InputEvent* input_event, void* context) {
    FuriMessageQueue* queue = context;
    AppEvent event = {.type = EventTypeInput, .input = *input_event};
    furi_message_queue_put(queue, &event, FuriWaitForever);
}

static void timer_callback(void* context) {
    FuriMessageQueue* queue = context;
    AppEvent event = {.type = EventTypeTick};
    furi_message_queue_put(queue, &event, 0);
}

int32_t ir_flood_app(void* p) {
    (void)p;
    IrFloodApp* app = malloc(sizeof(IrFloodApp));
    app->protocol = ProtocolNec;
    app->address = 0;
    app->command = 0;
    app->is_tx_active = false;
    app->flood_interval_ms = 150;

    ViewPort* view_port = view_port_alloc();
    app->event_queue = furi_message_queue_alloc(8, sizeof(AppEvent));
    app->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, app->event_queue);

    view_port_draw_callback_set(view_port, draw_callback, app);
    view_port_input_callback_set(view_port, input_callback, app->event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    AppEvent event;
    bool running = true;

    while(running) {
        FuriStatus status = furi_message_queue_get(app->event_queue, &event, FuriWaitForever);
        if(status != FuriStatusOk) continue;

        if(event.type == EventTypeInput) {
            InputEvent input = event.input;
            if(input.type == InputTypeShort || input.type == InputTypeLong || input.type == InputTypeRepeat) {
                if(input.key == InputKeyBack) {
                    running = false;
                }
                else if(input.key == InputKeyOk && input.type == InputTypeShort) {
                    app->is_tx_active = !app->is_tx_active;
                    if(app->is_tx_active) {
                        furi_timer_start(app->timer, furi_ms_to_ticks(app->flood_interval_ms));
                    } else {
                        furi_timer_stop(app->timer);
                        furi_hal_infrared_async_tx_stop();
                        is_ir_tx_busy = false;
                    }
                }
                else if(input.key == InputKeyLeft) {
                    if(app->flood_interval_ms > 50) {
                        app->flood_interval_ms -= 10;
                        if(app->is_tx_active) {
                            furi_timer_restart(app->timer, furi_ms_to_ticks(app->flood_interval_ms));
                        }
                    }
                }
                else if(input.key == InputKeyRight) {
                    if(app->flood_interval_ms < 500) {
                        app->flood_interval_ms += 10;
                        if(app->is_tx_active) {
                            furi_timer_restart(app->timer, furi_ms_to_ticks(app->flood_interval_ms));
                        }
                    }
                }
                else if(!app->is_tx_active) {
                    if(input.key == InputKeyUp) {
                        if(app->protocol > 0) app->protocol--;
                        else app->protocol = ProtocolMaxCount - 1;
                    }
                    else if(input.key == InputKeyDown) {
                        app->protocol = (app->protocol + 1) % ProtocolMaxCount;
                    }
                }
            }
        }
        else if(event.type == EventTypeTick) {
            if(app->is_tx_active) {
                encode_and_transmit(app);
                if(app->command < MAX_COMMAND) {
                    app->command++;
                } else {
                    app->command = 0;
                    if(app->address < 255) {
                        app->address++;
                    } else {
                        app->address = 0;
                    }
                }
            }
        }

        view_port_update(view_port);
    }

    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    if(app->is_tx_active || is_ir_tx_busy) {
        furi_hal_infrared_async_tx_stop();
        is_ir_tx_busy = false;
    }
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(app->event_queue);
    furi_record_close(RECORD_GUI);
    free(app);

    return 0;
}