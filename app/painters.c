#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification.h>
#include <flipper_http/flipper_http.h>

#define TAG                   "PAINTERS"
#define MAP_WIDTH             200
#define MAP_HEIGHT            200
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
    FlipperHTTP* fhttp;
    ViewPort* vp;
    FuriMutex* mutex;
    Cursor cursor;
    Camera camera;
    uint8_t* painted_bytes;
    ZoomLevel zoom;
    uint32_t zoom_message_start_time;
    bool connected;
    char* last_server_response;
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

static bool collecting_canvas = false;
static size_t received_bytes = 0;
static uint16_t expected_chunk = 0;

static bool game_start_websocket(FlipperHTTP* fhttp) {
    if(!fhttp) {
        FURI_LOG_E(TAG, "FlipperHTTP is NULL");
        return false;
    }

    fhttp->state = IDLE; // ensure it's set to IDLE for the next request
    char websocket_url[128];
    snprintf(websocket_url, sizeof(websocket_url), "%s", WEBSOCKET_URL);
    if(!flipper_http_websocket_start(
           fhttp, websocket_url, WEBSOCKET_PORT, "{\"Content-Type\":\"application/json\"}")) {
        FURI_LOG_E(TAG, "Failed to start websocket");
        return false;
    }
    fhttp->state = RECEIVING;
    while(fhttp->state != IDLE) {
        furi_delay_ms(100);
    }

    return true;
}

static void send_pixel(FlipperHTTP* fhttp, int x, int y, int color) {
    if(!fhttp) {
        FURI_LOG_E(TAG, "FlipperHTTP is NULL");
        return;
    }

    char message[64];
    snprintf(message, sizeof(message), "[PIXEL]x:%d,y:%d,c:%d", x, y, color);
    if(!flipper_http_send_data(fhttp, message)) {
        FURI_LOG_E(TAG, "Failed to send pixel update to server");
    } else {
        FURI_LOG_I(TAG, "Pixel update sent: %s", message);
    }
}

void paint_receive_canvas(PaintData* state, size_t response_len) {
    char* response = state->fhttp->last_response;
    if(strncmp(response, "[MAP/SEND]", 10) == 0) {
        collecting_canvas = true;
        received_bytes = 0;
        expected_chunk = 0;
        memset(state->painted_bytes, 0, PAINTED_BYTES_SIZE);
        FURI_LOG_I(TAG, "Started receiving canvas");
    } else if(strncmp(response, "[MAP/CHUNK:", 11) == 0) {
        uint16_t chunk_id = atoi(response + 11); // extract chunk id
        char* data_start = strstr(response, "]") + 1;
        size_t chunk_size = response_len - (data_start - response);

        if(chunk_id != expected_chunk) {
            FURI_LOG_E(
                TAG,
                "Missing chunk! Expected %u, got %u, Asking server again",
                expected_chunk,
                chunk_id);
            // Ask server to resend
            char resend_request[64];
            snprintf(resend_request, sizeof(resend_request), "[MAP/RESEND:%u]", expected_chunk);
            flipper_http_send_data(state->fhttp, resend_request);
            return;
        }

        // Copy chunk into painted_bytes
        size_t copy_len = chunk_size;
        if(received_bytes + copy_len > PAINTED_BYTES_SIZE) {
            copy_len = PAINTED_BYTES_SIZE - received_bytes;
        }
        memcpy(state->painted_bytes + received_bytes, data_start, copy_len);
        received_bytes += copy_len;
        expected_chunk++;
        FURI_LOG_I(TAG, "Received chunk %u (%zu bytes total)", chunk_id, received_bytes);

    } else if(strncmp(response, "[MAP/END]", 9) == 0) {
        collecting_canvas = false;
        FURI_LOG_I(TAG, "Finished receiving canvas (%zu bytes)", received_bytes);
    }
}

long int websocket_listener_thread(void* context) {
    PaintData* state = (PaintData*)context;
    FlipperHTTP* fhttp = state->fhttp;

    while(furi_thread_flags_get() != WorkerEvtStop) {
        if(!state->last_server_response) {
            state->last_server_response = (char*)malloc(RX_BUF_SIZE);
        }
        if(fhttp && fhttp->last_response && strlen(fhttp->last_response) > 0 &&
           strcmp(fhttp->last_response, state->last_server_response) != 0) {
            // New server message received
            FURI_LOG_I(TAG, "Received server message: %s", fhttp->last_response);

            // If last message contains [MAP only save the part between brackets, including brackets
            if(strncmp(fhttp->last_response, "[MAP", 4) == 0) {
                paint_receive_canvas(state, strlen(fhttp->last_response));

                char* start = strchr(fhttp->last_response, '[');
                char* end = strrchr(fhttp->last_response, ']');
                if(start && end && end > start) {
                    size_t len = end - start + 1;
                    strncpy(state->last_server_response, start, len);
                    state->last_server_response[len] = '\0'; // Null-terminate the string
                }
            } else {
                // Save the last server response
                strncpy(state->last_server_response, fhttp->last_response, RX_BUF_SIZE - 1);
                state->last_server_response[RX_BUF_SIZE - 1] = '\0'; // Null-terminate the string
            }

            // Redraw viewport (maybe you show some UI progress)
            view_port_update(state->vp);
        }
        furi_delay_ms(50); // Sleep a little
    }

    return 0;
}

int32_t painters_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    if(!queue) return -1;

    PaintData* state = (PaintData*)malloc(sizeof(PaintData));
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

    state->fhttp = fhttp; // Set the fhttp pointer in the state
    state->vp = vp; // Set the view port pointer in the state

    flipper_http_websocket_stop(fhttp); // Stop any existing websocket connection

    furi_delay_ms(500); // Wait for a second before starting the websocket
    if(!game_start_websocket(fhttp)) {
        FURI_LOG_E(TAG, "Failed to start websocket connection");
        return -1;
    } else {
        state->connected = true;
        view_port_update(vp);

        char name[16];
        snprintf(name, sizeof(name), "[NAME]%s", furi_hal_version_get_name_ptr());
        flipper_http_send_data(fhttp, name);
    }

    FuriThread* ws_thread = furi_thread_alloc();
    furi_thread_set_name(ws_thread, "WebSocketListener");
    furi_thread_set_callback(ws_thread, websocket_listener_thread);
    furi_thread_set_context(ws_thread, state);
    furi_thread_set_stack_size(ws_thread, 1024);
    furi_thread_set_priority(ws_thread, FuriThreadPriorityNormal);
    furi_thread_start(ws_thread); // Start the thread

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
                bool changed = false;
                if(state->painted_bytes[byte_index] & (1 << bit_index)) {
                    // If painted, erase it
                    state->painted_bytes[byte_index] &= ~(1 << bit_index);
                    changed = true;
                } else {
                    // If not painted, paint it
                    state->painted_bytes[byte_index] |= (1 << bit_index);
                    changed = true;
                }
                if(changed) {
                    send_pixel(
                        fhttp,
                        state->cursor.x,
                        state->cursor.y,
                        state->painted_bytes[byte_index] & (1 << bit_index) ? 1 : 0);
                    should_update = true;
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

        // delay for a short time to reduce CPU usage
        furi_delay_ms(100);
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

    furi_record_close(RECORD_GUI);

    // Stop the thread
    furi_thread_flags_set(furi_thread_get_id(ws_thread), WorkerEvtStop);
    furi_thread_join(ws_thread);
    furi_thread_free(ws_thread);

    if(state->last_server_response) {
        free(state->last_server_response);
    }
    free(state->painted_bytes);
    free(state);

    return 0;
}
