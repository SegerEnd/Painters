#include <App.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint> // For uint16_t

// Canvas configuration
const int CANVAS_WIDTH = 550;
const int CANVAS_HEIGHT = 550;
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
                .idleTimeout = 120,
                .open = [](WebSocketType* ws) {
                    std::string ip = std::string(ws->getRemoteAddressAsText());
                    std::cout << "New client connected from " << ip << std::endl;

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
                        std::cout << "Client set name to [" << new_name << "]" << std::endl;
                        return;
                    }

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
                        return;
                    } else {
                        std::cout << "Invalid message size: " << message.size() << " bytes (expected 5) " << message
                                  << std::endl;

                        // close the connection, for now to test
                        ws->close();
                        return;
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