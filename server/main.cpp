#include <App.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint> // For uint16_t

// Canvas configuration
const int CANVAS_WIDTH = 550;
const int CANVAS_HEIGHT = 550;
const size_t CANVAS_BITS_SIZE = ((CANVAS_WIDTH * CANVAS_HEIGHT + 7) / 8); // 37,813 bytes
std::vector<uint8_t> canvas_bits(CANVAS_BITS_SIZE, 0); // Bit array for canvas

struct MyUserData {
    // Placeholder for per-socket data if needed
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
    std::string_view canvas_data(
        reinterpret_cast<const char*>(canvas_bits.data()), canvas_bits.size());
    ws->send(canvas_data, uWS::BINARY);
    std::cout << "Sent canvas data (" << canvas_bits.size() << " bytes) to client" << std::endl;
}

int main() {
    std::cout << "Starting WebSocket server... 🚀" << std::endl;

    uWS::App()
        .ws<MyUserData>(
            "/*",
            {
                .compression = uWS::SHARED_COMPRESSOR,
                .maxPayloadLength = 1 * 1024, // For incoming messages (5 bytes < 1024)
                .idleTimeout = 10,
                .open = [](WebSocketType* ws) {
                    std::string ip = std::string(ws->getRemoteAddressAsText());
                    std::cout << "New client connected from " << ip << std::endl;

                    clients.push_back(ws);
                    std::string hello = "Hello world!";
                    ws->send(hello, uWS::TEXT);
                    sendCanvas(ws); // Send initial canvas state
                },
                .message = [](WebSocketType* ws, std::string_view message, uWS::OpCode opCode) {
                    if (message.size() == 5) {
                        // Parse 5-byte message: x (2 bytes), y (2 bytes), color (1 byte)
                        uint16_t x = static_cast<uint16_t>(static_cast<uint8_t>(message[0])) |
                                     (static_cast<uint16_t>(static_cast<uint8_t>(message[1])) << 8);
                        uint16_t y = static_cast<uint16_t>(static_cast<uint8_t>(message[2])) |
                                     (static_cast<uint16_t>(static_cast<uint8_t>(message[3])) << 8);
                        bool color = (message[4] == 1);

                        if (x < CANVAS_WIDTH && y < CANVAS_HEIGHT) {
                            setPixel(x, y, color);
                            std::cout << "Received pixel update: (" << x << ", " << y << ", "
                                      << (color ? "white" : "black") << ")" << std::endl;
                            // Broadcast the update to all clients
                            for (auto client : clients) {
                                client->send(message, opCode);
                            }
                        } else {
                            std::cout << "Out-of-bounds pixel update: (" << x << ", " << y << ")"
                                      << std::endl;
                        }
                    } else {
                        std::cout << "Invalid message size: " << message.size() << " bytes (expected 5) " << message
                                  << std::endl;
                    }
                },
                .close = [](WebSocketType* ws, int /*code*/, std::string_view /*message*/) {
                    std::cout << "Client disconnected" << std::endl;
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
            80,
            [](auto* listen_socket) {
                if (listen_socket) {
                    std::cout << "Server listening on port 80" << std::endl;
                } else {
                    std::cerr << "Failed to listen on port 80" << std::endl;
                }
            })
        .run();

    clients.clear();
    canvas_bits.clear();

    return 0;
}