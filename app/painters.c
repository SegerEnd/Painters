#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification.h>
#include <flipper_http/flipper_http.h>

#define TAG                   "PAINTERS"
#define MAP_WIDTH             550
#define MAP_HEIGHT            550
#define SCREEN_WIDTH          128
#define SCREEN_HEIGHT         64
#define PAINTED_BYTES_SIZE    ((MAP_WIDTH * MAP_HEIGHT + 7) / 8) // 1 byte = 8 bits
#define ZOOM_MESSAGE_DURATION 2000 // 2 seconds in milliseconds
#define WEBSOCKET_URL         "ws://painters.segerend.nl"
#define WEBSOCKET_PORT        80

typedef enum {
    ZoomOut = 1,
    Zoom1x = 4,
    Zoom2x = 8,
    Zoom4x = 16,
} ZoomLevel;

typedef struct {
    int16_t x, y;
} Cursor;

typedef struct {
    int16_t x, y;
} Camera;

typedef struct {
    FuriMutex* mutex;
    Cursor cursor;
    Camera camera;
    uint8_t* painted_bytes;
    ZoomLevel zoom;
    uint32_t zoom_message_start_time;
    bool connected;
} PaintData;

static void clamp_cursor(Cursor* cursor) {
    if(cursor->x < 0) cursor->x = 0;
    if(cursor->x >= MAP_WIDTH) cursor->x = MAP_WIDTH - 1;
    if(cursor->y < 0) cursor->y = 0;
    if(cursor->y >= MAP_HEIGHT) cursor->y = MAP_HEIGHT - 1;
}

static void clamp_camera(Camera* camera, ZoomLevel zoom) {
    int view_w = SCREEN_WIDTH / zoom;
    int view_h = SCREEN_HEIGHT / zoom;
    if(camera->x < 0) camera->x = 0;
    if(camera->y < 0) camera->y = 0;
    if(camera->x > MAP_WIDTH - view_w) camera->x = MAP_WIDTH - view_w;
    if(camera->y > MAP_HEIGHT - view_h) camera->y = MAP_HEIGHT - view_h;
}

static void center_camera_on_cursor(PaintData* state) {
    int view_w = SCREEN_WIDTH / state->zoom;
    int view_h = SCREEN_HEIGHT / state->zoom;

    // Center camera on cursor
    state->camera.x = state->cursor.x - view_w / 2;
    state->camera.y = state->cursor.y - view_h / 2;

    clamp_camera(&state->camera, state->zoom);
}

static void draw_board(Canvas* canvas, const PaintData* state) {
    uint8_t tile_size = state->zoom;
    int view_w = SCREEN_WIDTH / tile_size;
    int view_h = SCREEN_HEIGHT / tile_size;

    canvas_set_color(canvas, ColorBlack);
    for(int y = 0; y < view_h; y++) {
        for(int x = 0; x < view_w; x++) {
            int map_x = state->camera.x + x;
            int map_y = state->camera.y + y;
            if(map_x < MAP_WIDTH && map_y < MAP_HEIGHT) {
                int index = map_y * MAP_WIDTH + map_x;
                int byte_index = index / 8;
                int bit_index = index % 8;
                if(state->painted_bytes[byte_index] & (1 << bit_index)) {
                    canvas_draw_box(canvas, x * tile_size, y * tile_size, tile_size, tile_size);
                }
            }
        }
    }
}

static void draw_cursor(Canvas* canvas, const PaintData* state) {
    uint8_t tile_size = state->zoom;
    int screen_x = (state->cursor.x - state->camera.x) * tile_size;
    int screen_y = (state->cursor.y - state->camera.y) * tile_size;

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, screen_x, screen_y, tile_size, tile_size);

    if(tile_size >= 4) {
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, screen_x + 1, screen_y + 1, tile_size - 2, tile_size - 2);
    }
}

static void draw_ui(Canvas* canvas, const PaintData* state) {
    uint32_t current_time = furi_get_tick();
    if(state->zoom_message_start_time > 0 &&
       (current_time - state->zoom_message_start_time) < ZOOM_MESSAGE_DURATION) {
        // draw box behind the text
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_rbox(canvas, 0, 0, 50, 14, 2);

        char zoom_text[32];
        snprintf(zoom_text, sizeof(zoom_text), "Zoom: %dx", state->zoom);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_str(canvas, 2, 10, zoom_text);
    }
}

void paint_draw_callback(Canvas* canvas, void* ctx) {
    furi_assert(ctx);
    PaintData* state = ctx;

    furi_mutex_acquire(state->mutex, FuriWaitForever);

    if(state->connected) {
        canvas_clear(canvas);
        draw_cursor(canvas, state);
        draw_board(canvas, state);
        draw_ui(canvas, state);
    } else {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 1, 10, "Not connected to server");
    }
    furi_mutex_release(state->mutex);
}

void paint_input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

static void cycle_zoom(PaintData* state) {
    // ZoomLevel old_zoom = state->zoom;

    switch(state->zoom) {
    case ZoomOut:
        state->zoom = Zoom1x;
        break;
    case Zoom1x:
        state->zoom = Zoom2x;
        break;
    case Zoom2x:
        state->zoom = Zoom4x;
        break;
    case Zoom4x:
        state->zoom = ZoomOut;
        break;
    default:
        state->zoom = Zoom1x;
        break;
    }

    // Adjust camera to keep cursor centered
    int view_w = SCREEN_WIDTH / state->zoom;
    int view_h = SCREEN_HEIGHT / state->zoom;
    state->camera.x = state->cursor.x - view_w / 2;
    state->camera.y = state->cursor.y - view_h / 2;

    clamp_camera(&state->camera, state->zoom);

    state->zoom_message_start_time = furi_get_tick();

    // Trigger zoom message
    state->zoom_message_start_time = furi_get_tick();
}

static bool game_start_websocket(FlipperHTTP* fhttp) {
    if(!fhttp) {
        FURI_LOG_E(TAG, "FlipperHTTP is NULL");
        return false;
    }

    fhttp->state = IDLE; // ensure it's set to IDLE for the next request
    char websocket_url[128];
    snprintf(websocket_url, sizeof(websocket_url), "%s", WEBSOCKET_URL);
    if(!flipper_http_websocket_start(fhttp, websocket_url, WEBSOCKET_PORT, "{\"Content-Type\":\"application/json\"}")) {
        FURI_LOG_E(TAG, "Failed to start websocket");
        return false;
    }
    fhttp->state = RECEIVING;
    while(fhttp->state != IDLE) {
        furi_delay_ms(100);
    }

    return true;
}

int32_t painters_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    if(!queue) return -1;

    PaintData* state = malloc(sizeof(PaintData));
    if(!state) {
        furi_message_queue_free(queue);
        return -1;
    }

    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!state->mutex) {
        free(state);
        furi_message_queue_free(queue);
        return -1;
    }

    state->connected = false;

    // Center the cursor in the middle of the map on start
    state->cursor.x = MAP_WIDTH / 2;
    state->cursor.y = MAP_HEIGHT / 2;

    state->camera = (Camera){.x = 0, .y = 0};
    state->zoom = Zoom1x;
    state->zoom_message_start_time = 0;

    center_camera_on_cursor(state);

    // Allocate bit array for the map
    state->painted_bytes = malloc(PAINTED_BYTES_SIZE);
    if(!state->painted_bytes) {
        furi_mutex_free(state->mutex);
        free(state);
        furi_message_queue_free(queue);
        return -1;
    }
    memset(state->painted_bytes, 0, PAINTED_BYTES_SIZE);

    ViewPort* vp = view_port_alloc();
    if(!vp) {
        free(state->painted_bytes);
        furi_mutex_free(state->mutex);
        free(state);
        furi_message_queue_free(queue);
        return -1;
    }
    view_port_draw_callback_set(vp, paint_draw_callback, state);
    view_port_input_callback_set(vp, paint_input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    uint32_t counter = 10;

    FlipperHTTP* fhttp = flipper_http_alloc();
    if(!fhttp) {
        FURI_LOG_E(TAG, "Failed to allocate memory for FlipperHTTP");
        return -1;
    }

    // Try to wait for pong response.
    if(!flipper_http_send_command(fhttp, HTTP_CMD_PING)) {
        FURI_LOG_E(TAG, "Failed to ping the device");
        return -1;
    }
    while(fhttp->state == INACTIVE && --counter > 0) {
        FURI_LOG_D(TAG, "Waiting for PONG");
        furi_delay_ms(100);
    }
    if(counter == 0) {
        FURI_LOG_E(TAG, "Failed to receive PONG response");
        return -1;
    }

    furi_delay_ms(1000); // Wait for a second before starting the websocket

    InputEvent event;

    while(furi_message_queue_get(queue, &event, FuriWaitForever) == FuriStatusOk) {
        bool should_update = false;

        if(event.type == InputTypeShort) {
            switch(event.key) {
            case InputKeyUp:
                state->cursor.y--;
                break;
            case InputKeyDown:
                state->cursor.y++;
                break;
            case InputKeyLeft:
                state->cursor.x--; // Left decreases x
                break;
            case InputKeyRight:
                state->cursor.x++; // Right increases x
                break;
            case InputKeyOk: {
                int index = state->cursor.y * MAP_WIDTH + state->cursor.x;
                int byte_index = index / 8;
                int bit_index = index % 8;
                if(state->painted_bytes[byte_index] & (1 << bit_index)) {
                    // If painted, erase it
                    state->painted_bytes[byte_index] &= ~(1 << bit_index);
                } else {
                    // If not painted, paint it
                    state->painted_bytes[byte_index] |= (1 << bit_index);
                }

                if(!game_start_websocket(fhttp)) {
                    FURI_LOG_E(TAG, "Failed to start websocket connection");
                    return -1;
                } else {
                    state->connected = true;
                    view_port_update(vp);
                }
            } break;
            case InputKeyBack:
                flipper_http_websocket_stop(fhttp);
                goto cleanup;
            default:
                break;
            }
            clamp_cursor(&state->cursor);
            center_camera_on_cursor(state);
            should_update = true;
        } else if(event.type == InputTypeLong) {
            switch(event.key) {
            case InputKeyOk:
                cycle_zoom(state);
                should_update = true;
                break;
            default:
                break;
            }
        } else if(event.type == InputTypeRepeat) {
            if(event.key == InputKeyLeft || event.key == InputKeyRight) {
                // Jump 3 pixels
                state->cursor.x += (event.key == InputKeyLeft) ? -3 : 3;
                should_update = true;
            } else if(event.key == InputKeyUp || event.key == InputKeyDown) {
                // Jump 3 pixels
                state->cursor.y += (event.key == InputKeyUp) ? -3 : 3;
                should_update = true;
            }
        }

        if(should_update) {
            clamp_cursor(&state->cursor);
            center_camera_on_cursor(state);
            view_port_update(vp);
        }
    }

cleanup:
    //if(state->connected) {
    flipper_http_websocket_stop(fhttp);
    //}
    flipper_http_free(fhttp);
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_message_queue_free(queue);
    furi_mutex_free(state->mutex);
    free(state->painted_bytes);
    free(state);
    furi_record_close(RECORD_GUI);

    return 0;
}
