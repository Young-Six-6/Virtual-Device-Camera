#include "obs_capture.h"
#include "../common/resolution.h"
#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace vcam {
namespace {
constexpr size_t max_message_size = 32 * 1024 * 1024;

struct Json {
    enum class Type { null_value, boolean, number, string, object, array } type = Type::null_value;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::map<std::string, Json> object;
    std::vector<Json> array;

    const Json* get(const char* key) const {
        if (type != Type::object) return nullptr;
        const auto found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    }
    std::string string_value() const { return type == Type::string ? string : std::string{}; }
    int integer_value() const {
        return type == Type::number && std::isfinite(number) && number >= 0 && number <= 100000000
            ? static_cast<int>(number) : 0;
    }
    bool bool_value() const { return type == Type::boolean && boolean; }
};

class JsonParser {
    const std::string& input_;
    size_t position_ = 0;
    unsigned depth_ = 0;
    void whitespace() {
        while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\r' || input_[position_] == '\n')) ++position_;
    }
    bool consume(char expected) {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }
    unsigned hex4() {
        if (position_ + 4 > input_.size()) throw std::runtime_error("truncated-json-unicode-escape");
        unsigned value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            const char c = input_[position_++];
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) throw std::runtime_error("invalid-json-unicode-escape");
            value = value * 16 + static_cast<unsigned>(digit);
        }
        return value;
    }
    static void utf8(std::string& output, unsigned point) {
        if (point <= 0x7f) output.push_back(static_cast<char>(point));
        else if (point <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (point >> 6)));
            output.push_back(static_cast<char>(0x80 | (point & 0x3f)));
        } else if (point <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (point >> 12)));
            output.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (point & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (point >> 18)));
            output.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (point & 0x3f)));
        }
    }
    std::string parse_string() {
        if (!consume('"')) throw std::runtime_error("invalid-json-string");
        std::string output;
        while (position_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[position_++]);
            if (c == '"') return output;
            if (c < 0x20) throw std::runtime_error("invalid-json-control-character");
            if (c != '\\') { output.push_back(static_cast<char>(c)); continue; }
            if (position_ >= input_.size()) break;
            switch (input_[position_++]) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                unsigned point = hex4();
                if (point >= 0xd800 && point <= 0xdbff) {
                    if (position_ + 2 > input_.size() || input_[position_] != '\\' || input_[position_ + 1] != 'u')
                        throw std::runtime_error("invalid-json-surrogate-pair");
                    position_ += 2;
                    const unsigned low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) throw std::runtime_error("invalid-json-surrogate-pair");
                    point = 0x10000 + ((point - 0xd800) << 10) + low - 0xdc00;
                } else if (point >= 0xdc00 && point <= 0xdfff) throw std::runtime_error("invalid-json-surrogate-pair");
                utf8(output, point);
                break;
            }
            default: throw std::runtime_error("invalid-json-escape");
            }
            if (output.size() > max_message_size) throw std::runtime_error("json-string-too-large");
        }
        throw std::runtime_error("unterminated-json-string");
    }
    Json value() {
        whitespace();
        if (position_ >= input_.size() || depth_ >= 64) throw std::runtime_error("invalid-json-value");
        if (input_[position_] == '"') {
            Json result; result.type = Json::Type::string; result.string = parse_string(); return result;
        }
        if (input_[position_] == '{') {
            ++position_; ++depth_;
            Json result; result.type = Json::Type::object;
            whitespace();
            if (consume('}')) { --depth_; return result; }
            for (;;) {
                whitespace();
                const std::string key = parse_string();
                if (!consume(':')) throw std::runtime_error("invalid-json-object");
                result.object.insert_or_assign(key, value());
                if (consume('}')) break;
                if (!consume(',')) throw std::runtime_error("invalid-json-object");
            }
            --depth_;
            return result;
        }
        if (input_[position_] == '[') {
            ++position_; ++depth_;
            Json result; result.type = Json::Type::array;
            whitespace();
            if (consume(']')) { --depth_; return result; }
            for (;;) {
                result.array.push_back(value());
                if (consume(']')) break;
                if (!consume(',')) throw std::runtime_error("invalid-json-array");
            }
            --depth_;
            return result;
        }
        if (input_.compare(position_, 4, "true") == 0) {
            position_ += 4; Json result; result.type = Json::Type::boolean; result.boolean = true; return result;
        }
        if (input_.compare(position_, 5, "false") == 0) {
            position_ += 5; Json result; result.type = Json::Type::boolean; return result;
        }
        if (input_.compare(position_, 4, "null") == 0) { position_ += 4; return {}; }
        const size_t begin = position_;
        if (input_[position_] == '-') ++position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        if (begin == position_) throw std::runtime_error("invalid-json-token");
        const std::string token = input_.substr(begin, position_ - begin);
        char* end = nullptr;
        Json result; result.type = Json::Type::number; result.number = std::strtod(token.c_str(), &end);
        if (!end || end != token.c_str() + token.size() || !std::isfinite(result.number))
            throw std::runtime_error("invalid-json-number");
        return result;
    }
public:
    explicit JsonParser(const std::string& input) : input_(input) {}
    Json parse() {
        if (input_.size() > max_message_size) throw std::runtime_error("obs-message-too-large");
        Json result = value();
        whitespace();
        if (position_ != input_.size()) throw std::runtime_error("trailing-json-data");
        return result;
    }
};

std::string json_string(const std::string& value) {
    std::string result = "\"";
    static constexpr char digits[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result.push_back('\\'); result.push_back(static_cast<char>(c)); }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c < 0x20) {
            result += "\\u00"; result.push_back(digits[c >> 4]); result.push_back(digits[c & 15]);
        } else result.push_back(static_cast<char>(c));
    }
    result.push_back('"');
    return result;
}

std::wstring widen_utf8(const std::string& value) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("invalid-obs-host-utf8");
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size) != size) throw std::runtime_error("invalid-obs-host-utf8");
    return result;
}

std::string base64(const uint8_t* data, size_t size) {
    if (size > MAXDWORD) throw std::runtime_error("obs-auth-data-too-large");
    DWORD output_size = 0;
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(size), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        nullptr, &output_size)) throw std::runtime_error("obs-auth-encoding-failed");
    std::string output(output_size, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(size), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        output.data(), &output_size)) throw std::runtime_error("obs-auth-encoding-failed");
    if (!output.empty() && output.back() == '\0') output.pop_back();
    return output;
}

std::string sha256_base64(const std::string& input) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("obs-auth-hash-unavailable");
    std::vector<uint8_t> object;
    uint8_t digest[32]{};
    try {
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size), &returned, 0) != 0 || object_size == 0 ||
            input.size() > (std::numeric_limits<ULONG>::max)())
            throw std::runtime_error("obs-auth-hash-unavailable");
        object.resize(object_size);
        if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) != 0 ||
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                static_cast<ULONG>(input.size()), 0) != 0 ||
            BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0)
            throw std::runtime_error("obs-auth-hash-failed");
        const std::string result = base64(digest, sizeof(digest));
        SecureZeroMemory(digest, sizeof(digest));
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return result;
    } catch (...) {
        SecureZeroMemory(digest, sizeof(digest));
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}

struct HttpHandle {
    HINTERNET value = nullptr;
    HttpHandle() = default;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    HttpHandle(HttpHandle&& other) noexcept : value(other.value) { other.value = nullptr; }
    HttpHandle& operator=(HttpHandle&& other) noexcept {
        if (this != &other) { if (value) WinHttpCloseHandle(value); value = other.value; other.value = nullptr; }
        return *this;
    }
};

class ObsWebSocket {
    HttpHandle session_;
    HttpHandle connection_;
    HttpHandle request_;
    HttpHandle socket_;
    uint64_t next_request_ = 1;
    std::string receive_message() {
        std::string message;
        std::vector<char> buffer(64 * 1024);
        bool complete = false;
        while (!complete) {
            DWORD received = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD result = WinHttpWebSocketReceive(socket_.value, buffer.data(),
                static_cast<DWORD>(buffer.size()), &received, &type);
            if (result != NO_ERROR) throw std::runtime_error("obs-websocket-read-failed-" + std::to_string(result));
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) throw std::runtime_error("obs-websocket-closed");
            if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
                type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                throw std::runtime_error("obs-websocket-invalid-frame-type");
            if (received > max_message_size - message.size()) throw std::runtime_error("obs-message-too-large");
            message.append(buffer.data(), received);
            complete = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        }
        return message;
    }
    void send_message(const std::string& message) {
        if (message.size() > MAXDWORD) throw std::runtime_error("obs-websocket-message-too-large");
        const DWORD result = WinHttpWebSocketSend(socket_.value, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            const_cast<char*>(message.data()), static_cast<DWORD>(message.size()));
        if (result != NO_ERROR) throw std::runtime_error("obs-websocket-write-failed-" + std::to_string(result));
    }
public:
    static const Json& required(const Json& parent, const char* key) {
        const Json* value = parent.get(key);
        if (!value) throw std::runtime_error(std::string("obs-response-missing-") + key);
        return *value;
    }
    Json response(const std::string& request_id) {
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const Json root = JsonParser(receive_message()).parse();
            const Json* opcode = root.get("op");
            const Json* data = root.get("d");
            if (!opcode || opcode->integer_value() != 7 || !data || data->type != Json::Type::object) continue;
            const Json* id = data->get("requestId");
            if (!id || id->string_value() != request_id) continue;
            const Json& status = required(*data, "requestStatus");
            const Json& success = required(status, "result");
            if (!success.bool_value()) {
                const Json* comment = status.get("comment");
                std::string message = comment ? comment->string_value() : "request-failed";
                if (message.size() > 256) message.resize(256);
                throw std::runtime_error("obs-request-failed: " + message);
            }
            return *data;
        }
        throw std::runtime_error("obs-response-not-found");
    }
    ~ObsWebSocket() {
        if (socket_.value) WinHttpWebSocketClose(socket_.value, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }
    void connect(const ObsSettings& settings) {
        session_ = HttpHandle(WinHttpOpen(L"VirtualDeviceCamera/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session_.value) throw std::runtime_error("obs-websocket-session-failed");
        if (!WinHttpSetTimeouts(session_.value, 3000, 3000, 5000, 5000))
            throw std::runtime_error("obs-websocket-timeout-setup-failed");
        const std::wstring host = widen_utf8(settings.host);
        connection_ = HttpHandle(WinHttpConnect(session_.value, host.c_str(), settings.port, 0));
        if (!connection_.value) throw std::runtime_error("obs-websocket-connect-failed-" + std::to_string(GetLastError()));
        request_ = HttpHandle(WinHttpOpenRequest(connection_.value, L"GET", L"/", nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
        if (!request_.value || !WinHttpSetOption(request_.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            throw std::runtime_error("obs-websocket-upgrade-failed-" + std::to_string(GetLastError()));
        if (!WinHttpSendRequest(request_.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request_.value, nullptr))
            throw std::runtime_error("obs-websocket-handshake-failed-" + std::to_string(GetLastError()));
        DWORD http_status = 0, status_size = sizeof(http_status);
        if (!WinHttpQueryHeaders(request_.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &http_status, &status_size, WINHTTP_NO_HEADER_INDEX) || http_status != 101)
            throw std::runtime_error("obs-websocket-upgrade-rejected-" + std::to_string(http_status));
        socket_ = HttpHandle(WinHttpWebSocketCompleteUpgrade(request_.value, 0));
        WinHttpCloseHandle(request_.value);
        request_.value = nullptr;
        if (!socket_.value) throw std::runtime_error("obs-websocket-upgrade-failed-" + std::to_string(GetLastError()));

        const Json hello = JsonParser(receive_message()).parse();
        const Json* opcode = hello.get("op");
        const Json* data = hello.get("d");
        if (!opcode || opcode->integer_value() != 0 || !data || data->type != Json::Type::object)
            throw std::runtime_error("obs-websocket-invalid-hello");
        const int rpc_version = (std::min)(1, required(*data, "rpcVersion").integer_value());
        if (rpc_version < 1) throw std::runtime_error("obs-websocket-rpc-unsupported");
        std::string identify = "{\"op\":1,\"d\":{\"rpcVersion\":" + std::to_string(rpc_version) +
            ",\"eventSubscriptions\":0";
        if (const Json* auth = data->get("authentication")) {
            const std::string challenge = required(*auth, "challenge").string_value();
            const std::string salt = required(*auth, "salt").string_value();
            if (settings.password.empty()) throw std::runtime_error("obs-password-required");
            const std::string secret = sha256_base64(settings.password + salt);
            identify += ",\"authentication\":" + json_string(sha256_base64(secret + challenge));
        }
        identify += "}}";
        send_message(identify);
        const Json identified = JsonParser(receive_message()).parse();
        const Json* identified_opcode = identified.get("op");
        if (!identified_opcode || identified_opcode->integer_value() != 2)
            throw std::runtime_error("obs-websocket-identification-failed");
    }
    Json request(const char* type, const std::string& request_data = "{}") {
        const std::string id = "vcam-" + std::to_string(next_request_++);
        send_message("{\"op\":6,\"d\":{\"requestType\":" + json_string(type) +
            ",\"requestId\":" + json_string(id) + ",\"requestData\":" + request_data + "}}");
        return response(id);
    }
};

std::vector<uint8_t> decode_base64_image(const std::string& data_url) {
    const size_t comma = data_url.find(',');
    constexpr char base64_suffix[] = ";base64";
    const std::string header = comma == std::string::npos ? std::string{} : data_url.substr(0, comma);
    if (comma == std::string::npos || data_url.compare(0, 11, "data:image/") != 0 ||
        header.size() < sizeof(base64_suffix) - 1 ||
        header.compare(header.size() - (sizeof(base64_suffix) - 1), sizeof(base64_suffix) - 1, base64_suffix) != 0 ||
        data_url.size() > max_message_size)
        throw std::runtime_error("obs-screenshot-invalid-data-url");
    const char* encoded = data_url.data() + comma + 1;
    const DWORD encoded_size = static_cast<DWORD>(data_url.size() - comma - 1);
    DWORD decoded_size = 0;
    if (!CryptStringToBinaryA(encoded, encoded_size, CRYPT_STRING_BASE64, nullptr, &decoded_size, nullptr, nullptr) ||
        !decoded_size || decoded_size > 16 * 1024 * 1024)
        throw std::runtime_error("obs-screenshot-invalid-base64");
    std::vector<uint8_t> output(decoded_size);
    if (!CryptStringToBinaryA(encoded, encoded_size, CRYPT_STRING_BASE64, output.data(), &decoded_size, nullptr, nullptr))
        throw std::runtime_error("obs-screenshot-invalid-base64");
    output.resize(decoded_size);
    return output;
}

struct CodecDeleter { void operator()(AVCodecContext* value) const { avcodec_free_context(&value); } };
struct FrameDeleter { void operator()(AVFrame* value) const { av_frame_free(&value); } };
struct PacketDeleter { void operator()(AVPacket* value) const { av_packet_free(&value); } };
struct ScaleDeleter { void operator()(SwsContext* value) const { sws_freeContext(value); } };
using Codec = std::unique_ptr<AVCodecContext, CodecDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Scale = std::unique_ptr<SwsContext, ScaleDeleter>;

std::shared_ptr<RgbFrame> decode_jpeg(const std::vector<uint8_t>& jpeg, Scale& scaler) {
    if (jpeg.empty() || jpeg.size() > MAXINT) throw std::runtime_error("obs-screenshot-invalid-size");
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    Codec codec(decoder ? avcodec_alloc_context3(decoder) : nullptr);
    Frame frame(av_frame_alloc());
    Packet packet(av_packet_alloc());
    if (!codec || !frame || !packet || avcodec_open2(codec.get(), decoder, nullptr) < 0 ||
        av_new_packet(packet.get(), static_cast<int>(jpeg.size())) < 0)
        throw std::runtime_error("obs-screenshot-decoder-unavailable");
    std::memcpy(packet->data, jpeg.data(), jpeg.size());
    if (avcodec_send_packet(codec.get(), packet.get()) < 0 || avcodec_receive_frame(codec.get(), frame.get()) < 0 ||
        frame->width <= 0 || frame->height <= 0 || !valid_input({frame->width, frame->height}))
        throw std::runtime_error("obs-screenshot-decode-failed");
    auto output = std::make_shared<RgbFrame>();
    output->size = {frame->width, frame->height};
    output->pixels.resize(rgb_bytes(output->size) + 64, 0);
    scaler.reset(sws_getCachedContext(scaler.release(), frame->width, frame->height,
        static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) throw std::runtime_error("obs-screenshot-conversion-unavailable");
    uint8_t* destination[] = {output->pixels.data(), nullptr, nullptr, nullptr};
    const int stride[] = {frame->width * 3, 0, 0, 0};
    if (sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height,
        destination, stride) != frame->height)
        throw std::runtime_error("obs-screenshot-conversion-failed");
    return output;
}

bool valid_host(const std::string& host) {
    if (host.empty() || host.size() > 253 || host.find_first_of("/:\\?#@ \t\r\n") != std::string::npos) return false;
    return std::none_of(host.begin(), host.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
}

ObsCapture::ObsCapture(ObsSettings settings, Resolution target)
    : settings_(std::move(settings)), target_(target) {}
ObsCapture::~ObsCapture() { stop(); }

bool ObsCapture::start(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable()) { error = "obs-capture-already-running"; return false; }
    if (!valid_host(settings_.host) || settings_.port == 0 || settings_.source.empty() ||
        settings_.source.size() > 256 || settings_.fps < 1 || settings_.fps > 60 ||
        !valid_input(target_)) {
        error = "invalid-obs-settings";
        return false;
    }
    stopping_ = false;
    status_ = {};
    status_.state = settings_.test_color ? "test-color" : "connecting";
    try { worker_ = std::thread(&ObsCapture::run, this); }
    catch (...) { status_.state = "stopped"; error = "obs-worker-start-failed"; return false; }
    return true;
}

void ObsCapture::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.reset();
    status_.state = "stopped";
}

void ObsCapture::set_target(Resolution target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (valid_input(target)) target_ = target;
}

std::shared_ptr<const RgbFrame> ObsCapture::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

ObsCaptureStatus ObsCapture::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void ObsCapture::run() noexcept {
    unsigned backoff = 1;
    Scale scaler;
    for (;;) {
        ObsSettings settings;
        Resolution target{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) break;
            settings = settings_;
            target = target_;
            status_.state = settings.test_color ? "test-color" : "connecting";
            status_.error.clear();
        }
        try {
            std::unique_ptr<ObsWebSocket> socket;
            Resolution screenshot_size = target;
            if (!settings.test_color) {
                socket = std::make_unique<ObsWebSocket>();
                socket->connect(settings);
                if (settings.auto_size) {
                    try {
                        const Json video = socket->request("GetVideoSettings");
                        const Json* response_data = video.get("responseData");
                        if (response_data) {
                            const int width = response_data->get("baseWidth") ? response_data->get("baseWidth")->integer_value() : 0;
                            const int height = response_data->get("baseHeight") ? response_data->get("baseHeight")->integer_value() : 0;
                            if (valid_input({width, height})) screenshot_size = {width, height};
                        }
                    } catch (...) {
                        // Match the previous connector: continue at the configured camera size.
                    }
                }
                std::lock_guard<std::mutex> lock(mutex_);
                status_.state = "connected";
                status_.error.clear();
            }

            auto next_frame = std::chrono::steady_clock::now();
            const auto frame_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / settings.fps));
            while (true) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopping_) return;
                    if (!settings.auto_size && valid_input(target_)) screenshot_size = target_;
                    target = target_;
                }
                auto frame = std::make_shared<RgbFrame>();
                if (settings.test_color) {
                    frame->size = target;
                    const size_t bytes = rgb_bytes(frame->size);
                    frame->pixels.resize(bytes + 64, 0);
                    for (size_t offset = 0; offset < bytes; offset += 3) {
                        frame->pixels[offset] = settings.color[0];
                        frame->pixels[offset + 1] = settings.color[1];
                        frame->pixels[offset + 2] = settings.color[2];
                    }
                } else {
                    std::string request_data = "{\"sourceName\":" + json_string(settings.source) +
                        ",\"imageFormat\":\"jpeg\",\"imageCompressionQuality\":80";
                    if (screenshot_size.width >= 8 && screenshot_size.height >= 8)
                        request_data += ",\"imageWidth\":" + std::to_string(screenshot_size.width) +
                            ",\"imageHeight\":" + std::to_string(screenshot_size.height);
                    request_data += "}";
                    const Json result = socket->request("GetSourceScreenshot", request_data);
                    const Json& data = result.get("responseData") ? *result.get("responseData") : result;
                    const Json& image = ObsWebSocket::required(data, "imageData");
                    frame = decode_jpeg(decode_base64_image(image.string_value()), scaler);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_ = std::move(frame);
                    status_.frames++;
                    status_.last_frame_ms = GetTickCount64();
                    status_.frame_size = latest_->size;
                    status_.state = settings.test_color ? "test-color" : "connected";
                    status_.error.clear();
                }
                backoff = 1;
                next_frame += frame_interval;
                const auto now = std::chrono::steady_clock::now();
                if (next_frame < now) next_frame = now;
                std::unique_lock<std::mutex> lock(mutex_);
                if (wake_.wait_until(lock, next_frame, [&] { return stopping_; })) return;
            }
        } catch (const std::exception& error) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) break;
                status_.state = "retrying";
                status_.error = error.what();
                if (status_.error.size() > 256) status_.error.resize(256);
                latest_.reset();
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (wake_.wait_for(lock, std::chrono::seconds(backoff), [&] { return stopping_; })) break;
            backoff = (std::min)(backoff * 2, 30u);
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) break;
                status_.state = "retrying";
                status_.error = "obs-unknown-error";
                latest_.reset();
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (wake_.wait_for(lock, std::chrono::seconds(backoff), [&] { return stopping_; })) break;
            backoff = (std::min)(backoff * 2, 30u);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = "stopped";
}
}
