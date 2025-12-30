#pragma once

// =============================================================================
// BOOM DETECTION CLIENT
// =============================================================================
//
// Client for communicating with the boom detection Python server.
// Sends simulation state data via Unix socket and receives boom frame prediction.
//
// The server uses ML models to detect the "boom" frame - the moment when
// the pendulum swarm transitions from organized to chaotic motion.
//
// USAGE:
//   BoomClient client("/path/to/socket");
//   auto result = client.predictBinary(states.data(), frames, pendulums);
//   if (result.ok && result.accepted) {
//       // Use result.boom_frame
//   }
//
// =============================================================================

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

namespace boom {

// Result from boom detection server
struct BoomResult {
    bool ok = false;           // true if request succeeded (check accepted next)
    bool accepted = false;     // true if boom was confidently detected

    // Only valid when accepted=true
    int boom_frame = -1;       // Frame where boom occurs (-1 if rejected)

    // Model predictions (always available when ok=true)
    int cnn_pred = -1;         // CNN model prediction
    int hgb_pred = -1;         // HistGBM model prediction
    int disagreement = -1;     // |cnn_pred - hgb_pred|

    // Confidence scores (always available when ok=true)
    float predicted_quality = 0.0f;  // Quality score (0-1)
    float accept_score = 0.0f;       // Combined confidence (0-1), threshold is 0.60

    // Error info (only when ok=false)
    std::string error_message;

    // Raw JSON response for debugging
    std::string raw_json;
};

// =============================================================================
// Simple JSON parser (no dependencies)
// =============================================================================

namespace json {

inline std::optional<std::string> getString(std::string const& json, std::string const& key) {
    std::string pattern = "\"" + key + "\":";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return std::nullopt;

    pos += pattern.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.size()) return std::nullopt;
    if (json[pos] == 'n') return std::nullopt;  // null
    if (json[pos] != '"') return std::nullopt;
    pos++;

    size_t end = json.find('"', pos);
    if (end == std::string::npos) return std::nullopt;

    return json.substr(pos, end - pos);
}

inline std::optional<int> getInt(std::string const& json, std::string const& key) {
    std::string pattern = "\"" + key + "\":";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return std::nullopt;

    pos += pattern.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.size()) return std::nullopt;
    if (json[pos] == 'n') return std::nullopt;  // null

    bool negative = false;
    if (json[pos] == '-') {
        negative = true;
        pos++;
    }

    int value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        value = value * 10 + (json[pos] - '0');
        pos++;
    }

    return negative ? -value : value;
}

inline std::optional<float> getFloat(std::string const& json, std::string const& key) {
    std::string pattern = "\"" + key + "\":";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return std::nullopt;

    pos += pattern.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.size()) return std::nullopt;
    if (json[pos] == 'n') return std::nullopt;  // null

    size_t end = pos;
    while (end < json.size() && (json[end] == '-' || json[end] == '.' ||
           (json[end] >= '0' && json[end] <= '9'))) {
        end++;
    }

    try {
        return std::stof(json.substr(pos, end - pos));
    } catch (...) {
        return std::nullopt;
    }
}

inline std::optional<bool> getBool(std::string const& json, std::string const& key) {
    std::string pattern = "\"" + key + "\":";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return std::nullopt;

    pos += pattern.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return std::nullopt;
}

}  // namespace json

// =============================================================================
// Boom Client
// =============================================================================

class BoomClient {
public:
    explicit BoomClient(std::string const& socket_path) {
        sock_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_ < 0) {
            throw std::runtime_error("Failed to create socket");
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock_);
            throw std::runtime_error("Failed to connect to boom server at " + socket_path +
                                   ". Is boom_server.py running?");
        }
    }

    ~BoomClient() {
        if (sock_ >= 0) {
            close(sock_);
        }
    }

    // Non-copyable
    BoomClient(BoomClient const&) = delete;
    BoomClient& operator=(BoomClient const&) = delete;

    // Predict boom frame from in-memory simulation data
    //
    // Data layout: [frames][pendulums][8] as contiguous float32 array
    // 8 values per pendulum: x1, y1, x2, y2, th1, th2, w1, w2
    //
    // @param data      Pointer to float32 array
    // @param frames    Number of frames in simulation
    // @param pendulums Number of pendulums (should be 2000 for best results)
    // @param values    Values per pendulum per frame (default: 8)
    // @return BoomResult with detection results
    BoomResult predictBinary(float const* data, int frames, int pendulums, int values = 8) {
        std::string header = R"({"type":"binary","frames":)" + std::to_string(frames) +
                            R"(,"pendulums":)" + std::to_string(pendulums) +
                            R"(,"values":)" + std::to_string(values) + "}";

        size_t data_size = static_cast<size_t>(frames) * pendulums * values * sizeof(float);
        return parseResponse(sendRequest(header, data, data_size));
    }

    // Predict boom frame from a simulation file
    //
    // @param path Path to simulation_data.bin file
    // @return BoomResult with detection results
    BoomResult predictPath(std::string const& path) {
        std::string header = R"({"type":"path","path":")" + path + R"("})";
        return parseResponse(sendRequest(header, nullptr, 0));
    }

private:
    int sock_ = -1;

    std::string sendRequest(std::string const& header, void const* data, size_t data_size) {
        // Send header length (4 bytes, little-endian uint32)
        uint32_t header_len = static_cast<uint32_t>(header.size());
        sendAll(&header_len, sizeof(header_len));

        // Send JSON header
        sendAll(header.data(), header.size());

        // Send binary data if present
        if (data && data_size > 0) {
            sendAll(data, data_size);
        }

        // Receive response
        return recvResponse();
    }

    void sendAll(void const* data, size_t size) {
        char const* ptr = static_cast<char const*>(data);
        size_t sent = 0;
        while (sent < size) {
            ssize_t n = send(sock_, ptr + sent, size - sent, 0);
            if (n <= 0) {
                throw std::runtime_error("Send failed - connection lost");
            }
            sent += static_cast<size_t>(n);
        }
    }

    void recvAll(void* data, size_t size) {
        char* ptr = static_cast<char*>(data);
        size_t received = 0;
        while (received < size) {
            ssize_t n = recv(sock_, ptr + received, size - received, 0);
            if (n <= 0) {
                throw std::runtime_error("Recv failed - connection lost");
            }
            received += static_cast<size_t>(n);
        }
    }

    std::string recvResponse() {
        // Read response length (4 bytes, little-endian uint32)
        uint32_t resp_len;
        recvAll(&resp_len, sizeof(resp_len));

        // Read response JSON
        std::string response(resp_len, '\0');
        recvAll(response.data(), resp_len);

        return response;
    }

    BoomResult parseResponse(std::string const& response) {
        BoomResult result;
        result.raw_json = response;

        // Check status
        auto status = json::getString(response, "status");
        if (!status || *status != "ok") {
            result.ok = false;
            auto msg = json::getString(response, "message");
            result.error_message = msg.value_or("Unknown error");
            return result;
        }

        result.ok = true;
        result.accepted = json::getBool(response, "accepted").value_or(false);

        // boom_frame is null when rejected
        auto boom = json::getInt(response, "boom_frame");
        result.boom_frame = boom.value_or(-1);

        result.cnn_pred = json::getInt(response, "cnn_pred").value_or(-1);
        result.hgb_pred = json::getInt(response, "hgb_pred").value_or(-1);
        result.disagreement = json::getInt(response, "disagreement").value_or(-1);
        result.predicted_quality = json::getFloat(response, "predicted_quality").value_or(0.0f);
        result.accept_score = json::getFloat(response, "accept_score").value_or(0.0f);

        return result;
    }
};

}  // namespace boom
