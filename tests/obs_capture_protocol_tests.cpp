#include <winsock2.h>
#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../src/media/obs_capture.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool receive_all(SOCKET socket, void* output, size_t size) {
    auto* bytes = static_cast<char*>(output);
    size_t offset = 0;
    while (offset < size) {
        const int count = recv(socket, bytes + offset, static_cast<int>((std::min)(size - offset, size_t(INT_MAX))), 0);
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool send_all(SOCKET socket, const void* input, size_t size) {
    const auto* bytes = static_cast<const char*>(input);
    size_t offset = 0;
    while (offset < size) {
        const int count = send(socket, bytes + offset, static_cast<int>((std::min)(size - offset, size_t(INT_MAX))), 0);
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

std::string encode_base64(const uint8_t* bytes, size_t size) {
    DWORD output_size = 0;
    require(size <= MAXDWORD && CryptBinaryToStringA(bytes, static_cast<DWORD>(size),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &output_size) != FALSE, "Base64 size query failed");
    std::string output(output_size, '\0');
    require(CryptBinaryToStringA(bytes, static_cast<DWORD>(size), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        output.data(), &output_size) != FALSE, "Base64 encoding failed");
    if (!output.empty() && output.back() == '\0') output.pop_back();
    return output;
}

std::string websocket_accept(const std::string& key) {
    const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, returned = 0;
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0,
        "Unable to open SHA-1 provider for mock server");
    std::vector<uint8_t> object;
    uint8_t digest[20]{};
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
        sizeof(object_size), &returned, 0) == 0;
    if (ok) object.resize(object_size);
    ok = ok && BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) == 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
            static_cast<ULONG>(input.size()), 0) == 0 && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    require(ok, "Unable to calculate WebSocket accept value");
    const std::string result = encode_base64(digest, sizeof(digest));
    SecureZeroMemory(digest, sizeof(digest));
    return result;
}

bool receive_frame(SOCKET socket, uint8_t& opcode, std::string& payload) {
    uint8_t header[2]{};
    if (!receive_all(socket, header, sizeof(header))) return false;
    opcode = header[0] & 0x0f;
    const bool masked = (header[1] & 0x80) != 0;
    uint64_t length = header[1] & 0x7f;
    if (length == 126) {
        uint8_t extended[2]{};
        if (!receive_all(socket, extended, sizeof(extended))) return false;
        length = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
    } else if (length == 127) {
        uint8_t extended[8]{};
        if (!receive_all(socket, extended, sizeof(extended))) return false;
        length = 0;
        for (uint8_t byte : extended) length = (length << 8) | byte;
    }
    if (!masked || length > 32 * 1024 * 1024) throw std::runtime_error("Invalid client WebSocket frame");
    uint8_t mask[4]{};
    if (!receive_all(socket, mask, sizeof(mask))) return false;
    payload.resize(static_cast<size_t>(length));
    if (length && !receive_all(socket, payload.data(), static_cast<size_t>(length))) return false;
    for (size_t index = 0; index < payload.size(); ++index) payload[index] ^= static_cast<char>(mask[index % 4]);
    return true;
}

bool send_text(SOCKET socket, const std::string& value) {
    std::vector<uint8_t> header;
    header.push_back(0x81);
    if (value.size() < 126) header.push_back(static_cast<uint8_t>(value.size()));
    else if (value.size() <= 0xffff) {
        header.push_back(126);
        header.push_back(static_cast<uint8_t>(value.size() >> 8));
        header.push_back(static_cast<uint8_t>(value.size()));
    } else {
        header.push_back(127);
        const uint64_t length = value.size();
        for (int shift = 56; shift >= 0; shift -= 8) header.push_back(static_cast<uint8_t>(length >> shift));
    }
    return send_all(socket, header.data(), header.size()) && send_all(socket, value.data(), value.size());
}

std::string request_id(const std::string& message) {
    const std::string prefix = "\"requestId\":\"";
    const size_t begin = message.find(prefix);
    if (begin == std::string::npos) throw std::runtime_error("OBS request has no requestId");
    const size_t value_begin = begin + prefix.size();
    const size_t end = message.find('"', value_begin);
    if (end == std::string::npos) throw std::runtime_error("OBS request has malformed requestId");
    return message.substr(value_begin, end - value_begin);
}

class ObsMockServer {
    SOCKET listener_ = INVALID_SOCKET;
    std::thread worker_;
    std::atomic<SOCKET> client_{INVALID_SOCKET};
    std::atomic<bool> stopping_{false};
    mutable std::mutex error_mutex_;
    std::string error_;
    std::vector<uint8_t> jpeg_;
    uint16_t port_ = 0;

    void run() noexcept {
        SOCKET client = accept(listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        client_.store(client);
        DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        try {
            std::string headers;
            char buffer[1024];
            while (headers.find("\r\n\r\n") == std::string::npos) {
                const int count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) return;
                headers.append(buffer, count);
                if (headers.size() > 16 * 1024) throw std::runtime_error("Oversized HTTP handshake");
            }
            const std::string key_name = "Sec-WebSocket-Key:";
            const size_t key_at = headers.find(key_name);
            require(key_at != std::string::npos, "WebSocket client omitted Sec-WebSocket-Key");
            size_t key_begin = key_at + key_name.size();
            while (key_begin < headers.size() && headers[key_begin] == ' ') ++key_begin;
            size_t key_end = headers.find("\r\n", key_begin);
            require(key_end != std::string::npos, "Malformed Sec-WebSocket-Key header");
            const std::string accept = websocket_accept(headers.substr(key_begin, key_end - key_begin));
            const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
            require(send_all(client, response.data(), response.size()), "Unable to send WebSocket upgrade response");

            require(send_text(client, "{\"op\":0,\"d\":{\"rpcVersion\":1,\"authentication\":{"
                "\"challenge\":\"+IxH4CnCiqpX1rM9scsNynZzbOe4KhDeYcTNS3PDaeY=\","
                "\"salt\":\"lM1GncleQOaCu9lT1yeUZhFYnqhsLLP1G5lAGo3ixaI=\"}}}"),
                "Unable to send OBS Hello");
            uint8_t opcode = 0;
            std::string message;
            require(receive_frame(client, opcode, message) && opcode == 1, "OBS client did not identify");
            if (message.find("\"eventSubscriptions\":0") == std::string::npos ||
                message.find("\"authentication\":\"1Ct943GAT+6YQUUX47Ia/ncufilbe6+oD6lY+5kaCu4=\"") == std::string::npos)
                throw std::runtime_error("OBS client authentication did not match the v5 challenge response: " + message);
            require(send_text(client, "{\"op\":2,\"d\":{\"negotiatedRpcVersion\":1}}"),
                "Unable to send OBS Identified");

            const std::string jpeg = encode_base64(jpeg_.data(), jpeg_.size());
            for (;;) {
                if (!receive_frame(client, opcode, message) || opcode == 8) break;
                if (opcode != 1) throw std::runtime_error("OBS client sent a non-text request");
                const std::string id = request_id(message);
                if (message.find("GetVideoSettings") != std::string::npos) {
                    require(send_text(client, std::string("{\"op\":7,\"d\":{\"requestType\":\"GetVideoSettings\",\"requestId\":") +
                        "\"" + id + "\",\"requestStatus\":{\"result\":true,\"code\":100},"
                        "\"responseData\":{\"baseWidth\":1280,\"baseHeight\":720}}}"),
                        "Unable to return OBS video settings");
                } else if (message.find("GetSourceScreenshot") != std::string::npos) {
                    require(message.find("\"sourceName\":\"Fixture Scene\"") != std::string::npos &&
                        message.find("\"imageWidth\":1280") != std::string::npos &&
                        message.find("\"imageHeight\":720") != std::string::npos,
                        "OBS screenshot request did not preserve configured source and canvas size");
                    require(send_text(client, std::string("{\"op\":7,\"d\":{\"requestType\":\"GetSourceScreenshot\",\"requestId\":\"") +
                        id + "\",\"requestStatus\":{\"result\":true,\"code\":100},\"responseData\":{\"imageData\":\"data:image/jpeg;base64," +
                        jpeg + "\"}}}"), "Unable to return OBS screenshot");
                } else throw std::runtime_error("OBS client sent an unexpected request");
            }
        } catch (const std::exception& error) {
            if (!stopping_.load()) {
                std::lock_guard<std::mutex> lock(error_mutex_);
                error_ = error.what();
            }
        }
        closesocket(client);
        client_.store(INVALID_SOCKET);
    }

public:
    explicit ObsMockServer(const char* jpeg_path) {
        std::ifstream input(jpeg_path, std::ios::binary);
        jpeg_.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        require(input.good() || input.eof(), "Unable to read generated JPEG fixture");
        require(!jpeg_.empty(), "Generated JPEG fixture is empty");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener_ != INVALID_SOCKET, "Unable to create mock OBS socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener_, 1) == 0,
            "Unable to bind mock OBS socket");
        int size = sizeof(address);
        require(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Unable to get mock OBS port");
        port_ = ntohs(address.sin_port);
        worker_ = std::thread(&ObsMockServer::run, this);
    }
    ~ObsMockServer() {
        stopping_.store(true);
        const SOCKET client = client_.exchange(INVALID_SOCKET);
        if (client != INVALID_SOCKET) shutdown(client, SD_BOTH);
        if (listener_ != INVALID_SOCKET) { closesocket(listener_); listener_ = INVALID_SOCKET; }
        if (worker_.joinable()) worker_.join();
    }
    uint16_t port() const { return port_; }
    void join_and_check() {
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lock(error_mutex_);
        if (!error_.empty()) throw std::runtime_error(error_);
    }
};
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: obs-capture-tests fixture.jpg\n");
        return 2;
    }
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2,2), &winsock) != 0) return 1;
    try {
        ObsMockServer server(argv[1]);
        vcam::ObsSettings settings;
        settings.host = "127.0.0.1";
        settings.port = server.port();
        settings.source = "Fixture Scene";
        settings.password = "supersecretpassword";
        settings.fps = 30;
        settings.auto_size = true;
        vcam::ObsCapture capture(settings, {640,480});
        std::string error;
        require(capture.start(error), "Unable to start native OBS capture worker");
        const ULONGLONG deadline = GetTickCount64() + 8000;
        vcam::ObsCaptureStatus status;
        while (GetTickCount64() < deadline) {
            status = capture.status();
            if (status.frames) break;
            if (status.state == "retrying" && !status.error.empty()) {
                const std::string capture_error = status.error;
                capture.stop();
                server.join_and_check();
                throw std::runtime_error(capture_error);
            }
            Sleep(5);
        }
        status = capture.status();
        require(status.frames > 0 && status.state == "connected" &&
            status.frame_size == vcam::Resolution{1280,720}, "OBS screenshot was not decoded and published");
        auto frame = capture.latest();
        require(frame && frame->pixels.size() >= static_cast<size_t>(1280) * 720 * 3,
            "OBS source did not retain a complete RGB frame");
        capture.stop();
        server.join_and_check();
        WSACleanup();
        std::puts("PASS: WinHTTP WebSocket upgrade, OBS v5 challenge authentication, video settings, source screenshot, JPEG decode and background capture lifecycle.");
        return 0;
    } catch (const std::exception& error) {
        WSACleanup();
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
