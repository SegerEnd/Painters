#include <App.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <thread>
#include <fstream>   // for file operations
#include <atomic>    // for safe thread stop flag
#include <chrono>    // for sleep_for
#include <filesystem>

#define WEBSOCKET_PORT 80
#define MAX_CLIENTS 75
#define SAVE_INTERVAL (10 * 60) // 10 minutes
#define PIXEL_PLACE_TIMEOUT   1000 // 1 second in milliseconds

// Canvas configuration
const int CANVAS_WIDTH = 500;
const int CANVAS_HEIGHT = 500;
const size_t PAINTED_BYTES_SIZE = ((CANVAS_WIDTH * CANVAS_HEIGHT + 7) / 8); // 1 byte = 8 bits
const int MAX_PAYLOAD_SIZE = 2048;
const int CHUNK_SEND_DELAY_MS = 250; // Delay between sending chunks in milliseconds

struct MyUserData {
    std::string flipper_name;
    // timeout for pixel updates
    std::chrono::time_point<std::chrono::steady_clock> last_pixel_update;
};

uint8_t* painted_bytes = nullptr; // Global variable to hold the painted bytes (canvas)

// string for the current map file name
std::string current_map_file = "flipper_map.bin";

std::atomic<bool> keep_saving(true); // Flag to control the save thread

using WebSocketType = uWS::WebSocket<false, true, MyUserData>; // Server, with SSL support

// Global vector to keep track of all connected clients
std::vector<WebSocketType*> clients;

// funxtion to get the name of the client if not unknown
std::string getClientName(WebSocketType* ws) {
    std::string client_name = ws->getUserData()->flipper_name;
    if (client_name.empty()) {
        client_name = "Unknown";
    }
    return client_name;
}

void sendCanvasInChunks(WebSocketType* ws) {    
    std::cout << "Sending canvas 🗺️ to client " << getClientName(ws) << "..." << std::endl;
    ws->send("[MAP/SEND]", uWS::TEXT);

    size_t total_size = PAINTED_BYTES_SIZE;

    size_t start = 0;
    size_t chunk_id = 0;

    while (start < total_size) {
        size_t available_space = MAX_PAYLOAD_SIZE;

        // Create header with chunk id and start offset
        std::string chunk_header = "[MAP/CHUNK:" + std::to_string(chunk_id) + ":" + std::to_string(start) + "]";
        size_t header_length = chunk_header.size();
        available_space -= header_length;

        size_t bytes_can_send = available_space / 2;
        size_t end = std::min(start + bytes_can_send, total_size);

        std::string chunk_message = chunk_header;

        for (size_t i = start; i < end; ++i) {
            char hex_byte[3];
            snprintf(hex_byte, sizeof(hex_byte), "%02X", painted_bytes[i]);
            chunk_message += hex_byte;
        }

        ws->send(chunk_message, uWS::TEXT);

        start = end;
        chunk_id++;
    }

    ws->send("[MAP/END]", uWS::TEXT);
}

// Sets a pixel in the bit array at (x, y) to the specified color (1 = painted, 0 = not painted)
void setPixel(int x, int y, bool color) {
    if (x < 0 || x >= CANVAS_WIDTH || y < 0 || y >= CANVAS_HEIGHT) {
        std::cerr << "Invalid pixel coordinates: (" << x << ", " << y << ")" << std::endl;
        return;
    }

    size_t index = (y * CANVAS_WIDTH + x) / 8;
    size_t bit = (y * CANVAS_WIDTH + x) % 8;

    if (color) {
        painted_bytes[index] |= (1 << bit); // Set the bit to 1
    } else {
        painted_bytes[index] &= ~(1 << bit); // Set the bit to 0
    }
}

void saveCanvasToFile(const std::string& filename) {
    std::ofstream out_file(filename, std::ios::binary);
    if (!out_file) {
        std::cerr << "Failed to open file for saving: " << filename << std::endl;
        return;
    }
    out_file.write(reinterpret_cast<char*>(painted_bytes), PAINTED_BYTES_SIZE);
    if (!out_file) {
        std::cerr << "Failed to write canvas to file: " << filename << std::endl;
    } else {
        std::cout << "Canvas saved to file: " << filename << std::endl;
    }
}

int main() {
    std::cout << "Starting WebSocket server... 🚀" << std::endl;

    painted_bytes = (uint8_t*)malloc(PAINTED_BYTES_SIZE);
    if(!painted_bytes) {
        std::cerr << "Failed to allocate memory for painted bytes (canvas)" << std::endl;
        return -1;
    }
    memset(painted_bytes, 0, PAINTED_BYTES_SIZE);

    std::thread save_thread;

    // Start background thread to save canvas
    save_thread = std::thread([](){
        const std::chrono::seconds save_interval(SAVE_INTERVAL);
        std::cout << "Saving canvas to file every " << SAVE_INTERVAL / 60 << " minutes..." << std::endl;

        // save in /maps directory
        std::string maps_dir = "maps/";
        std::string maps_path = maps_dir + current_map_file;

        // check if maps directory exists
        if (std::filesystem::exists(maps_dir)) {
            std::cout << "Maps 📂 directory exists: " << maps_dir << std::endl;
        } else {
            std::cout << "Maps 📁 directory does not exist, creating: " << maps_dir << std::endl;
            std::filesystem::create_directory(maps_dir);
        }

        // if map file exists, load it in painted_bytes
        if (std::filesystem::exists(maps_path)) {
            std::cout << "Loading saved map 🗺️ 💾: " << maps_path << std::endl;
            std::ifstream in_file(maps_path, std::ios::binary);
            if (in_file) {
                in_file.read(reinterpret_cast<char*>(painted_bytes), PAINTED_BYTES_SIZE);
                if (!in_file) {
                    std::cerr << "Failed to read canvas from file: " << maps_path << std::endl;
                } else {
                    std::cout << "Canvas loaded from file: " << maps_path << std::endl;
                }
            } else {
                std::cerr << "Failed to open file for loading: " << maps_path << std::endl;
            }
        }

        while (keep_saving) {
            std::this_thread::sleep_for(save_interval);
            // check if there are any clients connected if not, don't save
            if (clients.empty()) {
                continue;
            }
            saveCanvasToFile(maps_path);
        }
    });

    uWS::App()
        .ws<MyUserData>(
            "/*",
            {
                .compression = uWS::SHARED_COMPRESSOR,
                .maxPayloadLength = 64, // For incoming messages (5 bytes < 1024)
                .idleTimeout = 420, // 7 minutes idle timeout
                .open = [](WebSocketType* ws) {
                    // limit the number of connected clients
                    if (clients.size() > MAX_CLIENTS) {
                        std::cout << "Max clients reached" << std::endl;
                        ws->close();
                        return;
                    }

                    // get the time to print when the client connected
                    auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                    std::cout << std::ctime(&time) << "New client connected, addr: " << ws->getRemoteAddressAsText() << std::endl;

                    clients.push_back(ws);

                    // std::string wake = "[WAKE]";
                    // ws->send(wake, uWS::TEXT);

                    // Send a wake with all neeced information like, canvas size, timeout time, payload size, etc
                    std::string wake = "[WAKE:cw:" + std::to_string(CANVAS_WIDTH) + ":ch:" + std::to_string(CANVAS_HEIGHT) +
                        ":t:" + std::to_string(PIXEL_PLACE_TIMEOUT) + ":ps:" + std::to_string(MAX_PAYLOAD_SIZE) + "]";
                    ws->send(wake, uWS::TEXT);
                },
                .message = [](WebSocketType* ws, std::string_view message, uWS::OpCode opCode) {
                    // when message is long don't process it
                    if (message.size() > 50) {
                        std::cout << "Received long message, ignoring" << std::endl;
                        return;
                    }

                    // if (message.starts_with("[MAP/RESEND:")) {
                    // }

                    // if message contains "STOP]", close the connection, FlipperHTTP sends [SOCKET/STOP] when closing
                    if (message.find("STOP]") != std::string::npos) {
                        std::cout << "Received STOP command: " << message << ", closing connection" << std::endl;
                        ws->close();
                        return;
                    }

                    if (message.find("[MAP/SYNC]") != std::string::npos) {
                        std::cout << "Client requested canvas sync" << std::endl;
                        sendCanvasInChunks(ws);
                        return;
                    }

                    if (message.starts_with("[NAME]")) {
                        // Set flipper name
                        std::string new_name(message.substr(6)); // after "[NAME]"

                        new_name.erase(std::remove_if(new_name.begin(), new_name.end(), ::isspace), new_name.end());
                        if (new_name.size() > 10) {
                            new_name = new_name.substr(0, 10);
                        }
                        if (new_name.empty()) {
                            std::cout << "Invalid name received, ignoring" << std::endl;
                            return;
                        }

                        ws->getUserData()->flipper_name = new_name;
                        std::cout << "Client set name to: " << new_name << std::endl;

                        sendCanvasInChunks(ws); // Send initial canvas state
                        return;
                    }

                    if (message.starts_with("[PIXEL]")) {
                        // check if pixel update is under timeout
                        auto now = std::chrono::steady_clock::now();
                        auto last_update = ws->getUserData()->last_pixel_update;
                        if (now - last_update < std::chrono::milliseconds(PIXEL_PLACE_TIMEOUT)) {
                            return;
                        }
                        ws->getUserData()->last_pixel_update = now;

                        std::string_view pixel_data = message.substr(7); // get value after "[PIXEL]"
                    
                        auto x_pos = pixel_data.find("x:");
                        auto y_pos = pixel_data.find(",y:");
                        auto c_pos = pixel_data.find(",c:");
                    
                        if (x_pos != 0 || y_pos == std::string_view::npos || c_pos == std::string_view::npos) {
                            std::cout << "Invalid pixel update format: " << message << std::endl;
                            return;
                        }
                    
                        auto x = std::stoul(std::string(pixel_data.substr(2, y_pos - 2)));
                        auto y = std::stoul(std::string(pixel_data.substr(y_pos + 3, c_pos - (y_pos + 3))));
                        auto color = std::stoul(std::string(pixel_data.substr(c_pos + 3)));
                    
                        if (x >= CANVAS_WIDTH || y >= CANVAS_HEIGHT) {
                            std::cout << "Invalid pixel coordinates: (" << x << ", " << y << ")" << std::endl;
                            return;
                        }
                        if (color > 1) {
                            std::cout << "Invalid color value: " << color << std::endl;
                            return;
                        }
                    
                        setPixel(x, y, color == 1);

                        // get name of the client
                        std::string client_name = ws->getUserData()->flipper_name;
                        if (client_name.empty()) {
                            client_name = "Unknown";
                            // send who are you message?
                            // std::string who_are_you = "[WHOAREYOU]";
                            // ws->send(who_are_you, uWS::TEXT);
                        }
                    
                        std::cout << client_name << ": Set pixel (" << x << "," << y << ") to "
                                  << (color ? "black" : "white") << std::endl;
                    
                        // send the updated pixel to all connected clients
                        for (auto client : clients) {
                            client->send(message, opCode);
                        }
                        return;
                    }

                    std::cout << "Received message: " << message << std::endl;
                },
                .close = [](WebSocketType* ws, int /*code*/, std::string_view /*message*/) {
                    // get the time to print when the client disconnected
                    auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                    std::cout << std::ctime(&time) << " Client disconnected" << std::endl;
                    clients.erase(std::remove(clients.begin(), clients.end(), ws), clients.end());
                }
            })
        .any("/*", [](auto *res, auto *req) {
            std::string addr = std::string(res->getRemoteAddressAsText());
            std::cout << "📡 Received an HTTP " << req->getMethod() << " request from " << addr 
              << " for URL: " << req->getMethod() << " " << req->getUrl() << std::endl;
            res->writeStatus("404 Not Found")->end("This server expects WebSocket connections.");
        })
        .listen(
            WEBSOCKET_PORT,
            [](auto* listen_socket) {
                if (listen_socket) {
                    std::cout << "Server listening on port " << WEBSOCKET_PORT << std::endl << "Start painting! 🎨" << std::endl;
                } else {
                    std::cerr << "Failed to listen on port " << WEBSOCKET_PORT << std::endl;
                }
            })
        .run();

    clients.clear();

    // save once before exiting
    saveCanvasToFile(current_map_file);

    keep_saving = false;
    if (save_thread.joinable()) {
        save_thread.join();
    }

    free(painted_bytes);
    painted_bytes = nullptr;
    
    std::cout << "Server stopped." << std::endl;

    return 0;
}