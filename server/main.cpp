#include <App.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint> // For uint16_t

#define WEBSOCKET_PORT 80
#define MAX_CLIENTS 75

// Canvas configuration
const int CANVAS_WIDTH = 400;
const int CANVAS_HEIGHT = 400;
const size_t CANVAS_BITS_SIZE = ((CANVAS_WIDTH * CANVAS_HEIGHT + 7) / 8); // 1 byte = 8 bits
std::vector<uint8_t> canvas_bits(CANVAS_BITS_SIZE, 0); // Bit array for canvas

struct MyUserData {
    std::string flipper_name;
};

using WebSocketType = uWS::WebSocket<false, true, MyUserData>; // Server, with SSL support

std::vector<WebSocketType*> clients;

// Sets a pixel in the bit array at (x, y) to the specified color (1 = painted, 0 = not painted)
void setPixel(int x, int y, bool color) {
    if (x < 0 || x >= CANVAS_WIDTH || y < 0 || y >= CANVAS_HEIGHT) {
        return; // Out of bounds, ignore
    }
    size_t bit_index = static_cast<size_t>(y) * CANVAS_WIDTH + x;
    size_t byte_index = bit_index / 8;
    size_t bit_pos = bit_index % 8;
    if (color) {
        canvas_bits[byte_index] |= (1 << bit_pos); // Set bit to 1
    } else {
        canvas_bits[byte_index] &= ~(1 << bit_pos); // Set bit to 0
    }
}

// Sends the entire canvas state to a client
void sendCanvas(WebSocketType* ws) {
    // Convert the byte array to a std::string
    std::string canvas_data(reinterpret_cast<const char*>(canvas_bits.data()), canvas_bits.size());

    // Add [CANVAS] prefix
    canvas_data = "[CANVAS]" + canvas_data;

    std::cout << "Sending canvas 🗺️ data to client: " << canvas_data.size() << " bytes" << std::endl;

    // Send to client as TEXT frame
    ws->send(canvas_data, uWS::TEXT);
}

int main() {
    std::cout << "Starting WebSocket server... 🚀" << std::endl;

    std::fill(canvas_bits.begin(), canvas_bits.end(), 0);

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

                    // ask for the needed information e.g for now: flipper name
                    std::string wake = "[WAKE]";
                    ws->send(wake, uWS::TEXT);

                    sendCanvas(ws); // Send initial canvas state
                },
                .message = [](WebSocketType* ws, std::string_view message, uWS::OpCode opCode) {
                    // when message is long don't process it
                    if (message.size() > 50) {
                        std::cout << "Received long message, ignoring" << std::endl;
                        return;
                    }

                    // if message contains "STOP]", close the connection, FlipperHTTP sends [SOCKET/STOP] when closing
                    if (message.find("STOP]") != std::string::npos) {
                        std::cout << "Received STOP command: " << message << ", closing connection" << std::endl;
                        ws->close();
                        return;
                    }

                    if (message.find("[MAP/SYNC]") != std::string::npos) {
                        std::cout << "Client requested canvas sync" << std::endl;
                        sendCanvas(ws); // Send full canvas back to the requesting client
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
    canvas_bits.clear();

    return 0;
}