#include <App.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <thread>

#define WEBSOCKET_PORT 80
#define MAX_CLIENTS 75

// Canvas configuration
const int CANVAS_WIDTH = 200;
const int CANVAS_HEIGHT = 200;
const size_t PAINTED_BYTES_SIZE = ((CANVAS_WIDTH * CANVAS_HEIGHT + 7) / 8); // 1 byte = 8 bits

const int CHUNK_SEND_DELAY_MS = 250; // Delay between sending chunks in milliseconds

struct MyUserData {
    std::string flipper_name;
};

uint8_t* painted_bytes = nullptr; // Global variable to hold the painted bytes (canvas)

using WebSocketType = uWS::WebSocket<false, true, MyUserData>; // Server, with SSL support

// Global vector to keep track of all connected clients
std::vector<WebSocketType*> clients;

void sendCanvasInChunks(WebSocketType* ws) {
    std::cout << "Sending canvas 🗺️ to client..." << std::endl;
    ws->send("[MAP/SEND]", uWS::TEXT); // Tell client we're starting canvas

    size_t total_size = PAINTED_BYTES_SIZE;
    
    // Maximum payload size including header
    const int MAX_PAYLOAD_SIZE = 128;
    
    size_t num_chunks = (total_size + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;

    // Debug the size of painted_bytes
    std::cout << "Canvas size: " << total_size << " bytes." << std::endl;

    for (size_t chunk_index = 0; chunk_index < num_chunks; ++chunk_index) {
        size_t start = chunk_index * MAX_PAYLOAD_SIZE;
        size_t end = std::min(start + MAX_PAYLOAD_SIZE, total_size);
        size_t chunk_size = end - start;

        // Create the chunk header with the chunk index (dynamic length)
        std::string chunk_header = "[MAP/CHUNK:" + std::to_string(chunk_index) + "]";

        // Calculate the actual header length based on the chunk index
        size_t header_length = chunk_header.size();

        // Calculate the chunk data size (payload) after accounting for header size
        size_t remaining_payload_size = MAX_PAYLOAD_SIZE - header_length;

        // Create the chunk message that includes the header and the chunk data
        std::string chunk_message = chunk_header;

        // Convert the chunk data into a string of hex numbers (human-readable format)
        for (size_t i = start; i < end; ++i) {
            char hex_byte[3];
            snprintf(hex_byte, sizeof(hex_byte), "%02X", painted_bytes[i]);
            chunk_message += hex_byte;
        }
        // Send the chunk (header + data) as a single message
        ws->send(chunk_message, uWS::TEXT);

        std::cout << "Sent " << chunk_message << " number: " << (chunk_index + 1) << " of " << num_chunks
                  << " (" << chunk_size << " bytes)" << std::endl;
    }

    // Notify client that we have finished sending the canvas
    ws->send("[MAP/END]", uWS::TEXT); // Tell client we're finished sending
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

int main() {
    std::cout << "Starting WebSocket server... 🚀" << std::endl;

    painted_bytes = (uint8_t*)malloc(PAINTED_BYTES_SIZE);
    if(!painted_bytes) {
        std::cerr << "Failed to allocate memory for painted bytes (canvas)" << std::endl;
        return -1;
    }
    memset(painted_bytes, 0, PAINTED_BYTES_SIZE);

    uWS::App()
        .ws<MyUserData>(
            "/*",
            {
                .compression = uWS::SHARED_COMPRESSOR,
                .maxPayloadLength = 1 * 1024, // For incoming messages (5 bytes < 1024)
                .idleTimeout = 120,
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
                        }
                    
                        std::cout << client_name << ": Set pixel at (" << x << ", " << y << ") to color "
                                  << (color ? "black" : "white") << std::endl;
                    
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
            std::string ip = std::string(res->getRemoteAddressAsText());
            std::cout << "📡 Received an HTTP " << req->getMethod() << " request from " << ip 
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

    free(painted_bytes);
    painted_bytes = nullptr;
    
    std::cout << "Server stopped." << std::endl;

    return 0;
}