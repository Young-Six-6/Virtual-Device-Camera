#include "../src/camera/camera_session.h"
#include "../src/app/control_server.h"
#include "../src/common/pipe_security.h"
#include "../src/app/app_config.h"
#include "../src/app/web_server.h"
#include "../src/common/win_handle.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <fstream>
#include <filesystem>
#include <random>
#include <stdexcept>

namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
bool descriptor_grants(PSECURITY_DESCRIPTOR descriptor, WELL_KNOWN_SID_TYPE group) {
    BOOL present = FALSE, defaulted = FALSE;
    PACL acl = nullptr;
    if (!GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) || !present || !acl) return false;
    ACL_SIZE_INFORMATION information{};
    if (!GetAclInformation(acl, &information, sizeof(information), AclSizeInformation)) return false;
    BYTE sid_buffer[SECURITY_MAX_SID_SIZE]{};
    DWORD sid_size = sizeof(sid_buffer);
    if (!CreateWellKnownSid(group, nullptr, sid_buffer, &sid_size)) return false;
    for (DWORD index = 0; index < information.AceCount; ++index) {
        void* raw = nullptr;
        if (!GetAce(acl, index, &raw)) return false;
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        if (EqualSid(const_cast<DWORD*>(&ace->SidStart), sid_buffer)) return true;
    }
    return false;
}
int pattern(frame_t* frame) {
    for (int i = 0; i < frame->length; ++i) frame->buffer[i] = static_cast<char>(i & 0xff);
    return 0;
}
uvc_vcam_t configuration() {
    uvc_vcam_t config{};
    config.pid = 0xcc10; config.vid = 0xbb10;
    config.manu_fact = "test"; config.product = "camera";
    config.frame_callback = pattern;
    return config;
}
struct Packet {
    static constexpr size_t guard = 32;
    std::vector<unsigned char> bytes;
    size_t capacity;
    explicit Packet(size_t size) : bytes(size + sizeof(usbtx_header_t) + guard*2, 0xa5), capacity(size) {
        std::memset(&header(), 0, sizeof(usbtx_header_t)+size);
        header().data_length = static_cast<int>(size);
    }
    usbtx_header_t& header() { return *reinterpret_cast<usbtx_header_t*>(bytes.data()+guard); }
    bool intact() const {
        return std::all_of(bytes.begin(), bytes.begin()+guard, [](auto x) { return x == 0xa5; }) &&
            std::all_of(bytes.end()-guard, bytes.end(), [](auto x) { return x == 0xa5; });
    }
};
Packet descriptor(size_t capacity = 4096) {
    Packet packet(capacity);
    packet.header().type = 1;
    packet.header().descriptor.type = 1;
    packet.header().descriptor.is_read = 1;
    packet.header().descriptor.subtype = 2;
    return packet;
}
vcam::Resolution descriptor_size(const usbtx_header_t& response) {
    size_t position = 0;
    while (position + 2 <= size_t(response.data_length)) {
        const auto* entry = reinterpret_cast<const unsigned char*>(response.data)+position;
        require(entry[0] >= 2 && position+entry[0] <= size_t(response.data_length), "Invalid descriptor chain");
        if (entry[1] == 0x24 && entry[0] == 34 && entry[2] == 5) {
            const auto* frame = reinterpret_cast<const uvc_frame_uncompressed*>(entry);
            require(frame->dwMaxVideoFrameBufferSize == size_t(frame->wWidth)*frame->wHeight*2, "Incorrect frame byte count");
            return {frame->wWidth,frame->wHeight};
        }
        position += entry[0];
    }
    throw std::runtime_error("Frame descriptor missing");
}
Packet make_iso_packet() {
    Packet packet(ISO_PACKET_HDR_SIZE + 3*sizeof(iso_packet) + 24);
    auto& request = packet.header();
    request.type = 3; request.transfer.type = 3; request.transfer.is_read = 1;
    request.transfer.ep_address = 0x82; request.transfer.number_packets = 3;
    auto* iso = reinterpret_cast<iso_packet_hdr*>(request.data);
    iso->number_packets = 3;
    for (unsigned i = 0; i < 3; ++i) iso->packets[i].offset = i*8;
    return packet;
}
void protocol_tests() {
    vcam::UvcDevice device(configuration(), {640,480});
    for (auto size : {vcam::Resolution{640,480}, {1280,720}, {1920,1080}}) {
        require(device.set_resolution(size), "Resolution update failed");
        auto packet = descriptor();
        device.handle(packet.header(), packet.capacity);
        require(packet.header().result == 0 && packet.intact() && descriptor_size(packet.header()) == size, "Updated descriptor mismatch");
    }
    require(!device.set_resolution({641,480}), "Odd output width accepted");
    for (int subtype = 1; subtype <= 3; ++subtype) for (size_t length = 0; length < 32; ++length) {
        auto packet = descriptor(length);
        packet.header().descriptor.subtype = subtype;
        packet.header().descriptor.index = 1;
        device.handle(packet.header(), packet.capacity);
        require(packet.intact() && packet.header().data_length <= int(length), "Short descriptor request overflowed");
    }
    for (int request : {UVC_GET_CUR,UVC_GET_DEF,UVC_GET_MIN,UVC_GET_MAX,UVC_GET_RES,UVC_GET_INFO,UVC_GET_LEN}) {
        for (size_t length = 0; length <= 40; ++length) {
            Packet packet(length);
            auto& hdr = packet.header();
            hdr.type = 2; hdr.vendor.index = 1; hdr.vendor.value = 0x0100; hdr.vendor.request = request;
            device.handle(hdr, packet.capacity);
            require(packet.intact() && hdr.result == 0 && size_t(hdr.data_length) <= length, "Streaming reply overflowed");
        }
    }
    for (int index : {0,1,2,255}) {
        Packet packet(sizeof(vcam::StreamControl));
        auto& hdr = packet.header();
        hdr.type = 2; hdr.vendor.index = 1; hdr.vendor.value = 0x0200; hdr.vendor.request = UVC_SET_CUR;
        vcam::StreamControl value;
        value.frame = static_cast<uint8_t>(index);
        std::memcpy(hdr.data, &value, sizeof(value));
        device.handle(hdr, packet.capacity);
        require((hdr.result == 0) == (index == 1), "Invalid frame index negotiated");
    }
    Packet control(64);
    auto& request = control.header();
    request.type = 3; request.transfer.type = 1;
    const unsigned char setup[] = {0x80,6,2,3,9,4,64,0};
    std::memcpy(request.transfer.setup_packet, setup, sizeof(setup));
    device.handle(request, control.capacity);
    require(request.result == 0 && request.type == 3 && control.intact(), "GET_DESCRIPTOR string setup failed");
    device.set_resolution({4,2});
    for (int pass = 0; pass < 2; ++pass) {
        auto packet = make_iso_packet();
        device.handle(packet.header(), packet.capacity);
        auto* iso = reinterpret_cast<iso_packet_hdr*>(packet.header().data);
        const auto* data = reinterpret_cast<const unsigned char*>(packet.header().data)+ISO_PACKET_HDR_SIZE+3*sizeof(iso_packet);
        std::vector<unsigned char> frame;
        for (unsigned i = 0; i < 3; ++i) {
            const auto* payload = data + iso->packets[i].offset;
            require(iso->packets[i].length >= 2 && payload[0] == 2 && (payload[1]&1) == pass, "Invalid UVC payload header/FID");
            frame.insert(frame.end(), payload+2, payload+iso->packets[i].length);
        }
        require(packet.header().result == 0 && packet.intact() && frame.size() == 16 && (data[17]&UVC_STREAM_EOF), "ISO frame completion failed");
        for (unsigned i = 0; i < frame.size(); ++i) require(frame[i] == i, "ISO packet reassembly mismatch");
    }
    for (int failure = 0; failure < 4; ++failure) {
        auto packet = make_iso_packet();
        auto* iso = reinterpret_cast<iso_packet_hdr*>(packet.header().data);
        if (failure == 0) iso->number_packets = UINT_MAX;
        if (failure == 1) iso->packets[1].offset = 25;
        if (failure == 2) iso->packets[2].offset = 1;
        if (failure == 3) packet.header().data_length = 4;
        device.handle(packet.header(), packet.capacity);
        require(packet.header().result < 0 && packet.intact(), "Malformed ISO request accepted");
    }
    std::mt19937 random(20261111);
    for (int iteration = 0; iteration < 20000; ++iteration) {
        Packet packet(random()%2048);
        auto* data = reinterpret_cast<unsigned char*>(&packet.header());
        for (size_t i = 0; i < sizeof(usbtx_header_t)+packet.capacity; ++i) data[i] = static_cast<unsigned char>(random());
        packet.header().type = int(random()%8);
        packet.header().data_length = int(random()%(packet.capacity+100))-20;
        device.handle(packet.header(), packet.capacity);
        require(packet.intact(), "Protocol fuzz case overwrote guard bytes");
    }
}

struct FakeState {
    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false, plugged = false, destroyed = false;
    int plugs = 0, unplugs = 0, fail_plug = -1, cycles = 0;
    std::deque<Packet> requests, responses;
};
class FakeTransport final : public vcam::UsbTransport {
    std::shared_ptr<FakeState> state_;
    std::unique_ptr<Packet> active_;
public:
    explicit FakeTransport(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
    ~FakeTransport() override { std::lock_guard<std::mutex> lock(state_->mutex); state_->destroyed = true; }
    bool plug(const vcam::DeviceIdentity&) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (++state_->plugs == state_->fail_plug) return false;
        state_->plugged = true; return true;
    }
    bool unplug() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->unplugs; state_->plugged = false; state_->requests.clear(); active_.reset(); return true;
    }
    bool replug() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->cycles; state_->changed.notify_all(); return true;
    }
    usbtx_header_t* begin() override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->changed.wait(lock, [&] { return state_->stopped || !state_->requests.empty(); });
        if (state_->stopped) return nullptr;
        active_ = std::make_unique<Packet>(std::move(state_->requests.front()));
        state_->requests.pop_front(); return &active_->header();
    }
    bool end(usbtx_header_t*) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        require(active_->intact(), "Worker overran fake transaction");
        state_->responses.push_back(std::move(*active_)); active_.reset(); state_->changed.notify_all(); return true;
    }
    size_t capacity() const override { return active_ ? active_->capacity : 0; }
    void interrupt() noexcept override { std::lock_guard<std::mutex> lock(state_->mutex); state_->stopped = true; state_->changed.notify_all(); }
    void resume() noexcept override { std::lock_guard<std::mutex> lock(state_->mutex); state_->stopped = false; }
};
Packet round_trip(const std::shared_ptr<FakeState>& state, Packet request) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->requests.push_back(std::move(request)); state->changed.notify_all();
    require(state->changed.wait_for(lock, std::chrono::seconds(2), [&] { return !state->responses.empty(); }), "USB response timeout");
    Packet response = std::move(state->responses.front()); state->responses.pop_front(); return response;
}
std::string command(HANDLE pipe, const std::string& text) {
    DWORD count = 0;
    const auto bytes = text+"\n";
    require(WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &count, NULL) != FALSE, "Control write failed");
    std::string response;
    char c;
    while (ReadFile(pipe, &c, 1, &count, NULL) && count && c != '\n') response += c;
    return response;
}
void lifecycle_tests() {
    const auto started = GetTickCount64();
    for (int iteration = 0; iteration < 20; ++iteration) {
        auto state = std::make_shared<FakeState>();
        {
            vcam::CameraSession session(configuration(), {640,480}, std::make_unique<FakeTransport>(state));
            require(session.start(), "Camera worker start failed");
            for (auto size : {vcam::Resolution{1280,720}, {1920,1080}, {640,480}}) {
                std::string error;
                require(session.resize(size, error), "Hot resolution failed");
                auto response = round_trip(state, descriptor());
                require(descriptor_size(response.header()) == size, "Re-enumeration descriptor mismatch");
            }
            { std::lock_guard<std::mutex> lock(state->mutex); state->fail_plug = state->plugs+1; }
            std::string error;
            require(!session.resize({1280,720}, error) && session.resolution() == vcam::Resolution{640,480} && session.running(), "Failed replug did not roll back");
            Packet reset(0); reset.header().type = 4; reset.header().reset.type = 2;
            require(round_trip(state, std::move(reset)).header().result == 0, "Cycle-port reset failed");
            std::unique_lock<std::mutex> lock(state->mutex);
            require(state->changed.wait_for(lock, std::chrono::seconds(1), [&] { return state->cycles == 1; }), "Reset did not replug synchronously");
        }
        require(state->destroyed && !state->plugged, "Transport released before shutdown completed");
    }
    require(GetTickCount64()-started < 10000, "Worker shutdown did not cancel waits promptly");
    auto state = std::make_shared<FakeState>();
    vcam::CameraSession session(configuration(), {640,480}, std::make_unique<FakeTransport>(state));
    require(session.start(), "Control camera start failed");
    auto resizer = [&session](vcam::Resolution size) {
        for (int pass = 0; pass < 5; ++pass) {
            std::string error;
            require(session.resize(size, error), "Concurrent resolution change failed");
        }
    };
    auto first = std::async(std::launch::async, resizer, vcam::Resolution{1280,720});
    auto second = std::async(std::launch::async, resizer, vcam::Resolution{1920,1080});
    first.get(); second.get();
    std::string resize_error;
    require(session.resize({640,480}, resize_error), "Cannot restore initial control resolution");
    vcam::ControlRouter router({{[&](vcam::Resolution size, std::string& error) { return session.resize(size,error); },
        [&] { return session.resolution(); }, [&] { return session.running(); }}});
    const std::string path = "\\\\.\\pipe\\vcam_test_control_" + std::to_string(GetCurrentProcessId());
    vcam::ControlServer server;
    require(server.start(path,[&](const auto& value) { return router.execute(value); }), "Control server start failed");
    vcam::unique_handle client(CreateFileA(path.c_str(), GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL));
    require(bool(client), "Control client connect failed");
    require(command(client.get(), "RESOLUTION 1 1280 720") == "OK RESOLUTION 1 1280 720 REOPEN_REQUIRED", "Control resize command failed");
    require(command(client.get(), "STATUS 1") == "OK STATUS 1 1280 720 RUNNING", "Control status failed");
    require(command(client.get(), "RESOLUTION 1 641 480") == "ERR invalid-resolution", "Invalid control resolution accepted");
    require(command(client.get(), "RESOLUTION 2 640 480") == "ERR camera-not-found", "Missing camera accepted");
    auto response = round_trip(state, descriptor());
    require(descriptor_size(response.header()) == vcam::Resolution{1280,720}, "Control ack preceded descriptor update");
    DWORD written = 0;
    WriteFile(client.get(), "RESO", 4, &written, NULL);
    const auto stop_started = GetTickCount64();
    server.stop();
    require(GetTickCount64()-stop_started < 1000, "Partial control command blocked shutdown");
}

void cancellation_test() {
    struct Gate { std::mutex mutex; std::condition_variable changed; bool entered = false, cancelled = false; } gate;
    auto config = configuration();
    config.param = &gate;
    config.frame_callback = [](frame_t* frame) {
        auto& state = *static_cast<Gate*>(frame->param);
        std::unique_lock<std::mutex> lock(state.mutex);
        state.entered = true; state.changed.notify_all();
        state.changed.wait(lock, [&] { return state.cancelled; });
        return -1;
    };
    config.request_stop = [](void* pointer) {
        auto& state = *static_cast<Gate*>(pointer);
        std::lock_guard<std::mutex> lock(state.mutex);
        state.cancelled = true; state.changed.notify_all();
    };
    auto state = std::make_shared<FakeState>();
    auto camera = std::make_unique<vcam::CameraSession>(config, vcam::Resolution{4,2}, std::make_unique<FakeTransport>(state));
    require(camera->start(), "Blocking-callback camera failed to start");
    { std::lock_guard<std::mutex> lock(state->mutex); state->requests.push_back(make_iso_packet()); state->changed.notify_all(); }
    { std::unique_lock<std::mutex> lock(gate.mutex); require(gate.changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate.entered; }), "Callback did not start"); }
    const auto started = GetTickCount64();
    camera.reset();
    require(GetTickCount64()-started < 1000 && state->destroyed, "Blocking frame callback was not cancelled and joined");
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--web-fixture") {
            const auto port = static_cast<unsigned short>(std::stoi(argv[2]));
            vcam::WebServer web;
            require(web.start(port, argv[3], [] { return std::string("{\"fixture\":true}"); },
                [](int camera) -> std::optional<vcam::CameraFrame> {
                    if (camera != 1) return std::nullopt;
                    return vcam::CameraFrame{{2,1}, {char(16), char(128), char(235), char(128)}};
                }, [](const vcam::WebUpdate& update, std::string& error) {
                    if (update.camera != 1 || update.resolution != vcam::Resolution{640,480} ||
                        update.control_access != vcam::PipeAccess::everyone) {
                        error = "fixture-rejected";
                        return false;
                    }
                    return true;
                }), "Web UI fixture start failed");
            puts("READY"); fflush(stdout);
            (void)getchar();
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--control-fixture") {
            auto state = std::make_shared<FakeState>();
            vcam::CameraSession session(configuration(), {640,480}, std::make_unique<FakeTransport>(state));
            require(session.start(), "Fixture camera start failed");
            vcam::ControlRouter router({{[&](vcam::Resolution size, std::string& error) { return session.resize(size,error); },
                [&] { return session.resolution(); }, [&] { return session.running(); }}});
            vcam::ControlServer server;
            require(server.start(argv[2], [&](const auto& command) { return router.execute(command); }), "Fixture control start failed");
            puts("READY"); fflush(stdout);
            (void)getchar();
            return 0;
        }
        vcam::local_pipe_security security_check;
        require(static_cast<bool>(security_check), "Unable to initialize local pipe security descriptor");
        vcam::local_pipe_security control_security(vcam::PipeAccess::everyone);
        require(static_cast<bool>(control_security) &&
                descriptor_grants(control_security.get()->lpSecurityDescriptor, WinWorldSid),
            "Default control pipe security descriptor does not grant Everyone access");
        {
            const auto directory = std::filesystem::temp_directory_path() /
                ("vcam-config-" + std::to_string(GetCurrentProcessId()));
            require(std::filesystem::create_directory(directory), "Unable to create isolated config test directory");
            const auto config = directory / "camera.ini";
            const auto preset = directory / "default_res.txt";
            { std::ofstream output(preset); output << "720\n"; }
            auto settings = vcam::load_app_settings(config.u8string());
            require(settings.cameras[0] == vcam::Resolution{1280,720}, "Default resolution preset was not loaded");
            require(settings.rgb_pipe_access == vcam::PipeAccess::owner &&
                    settings.control_pipe_access == vcam::PipeAccess::everyone,
                "Default pipe access scopes were not applied independently");
            settings.cameras[0] = {1024,768};
            std::string error;
            require(vcam::save_app_settings(config.u8string(), settings, error), "Persistent settings save failed");
            auto restored = vcam::load_app_settings(config.u8string());
            require(restored.cameras[0] == vcam::Resolution{1024,768} && restored.rgb_pipe == settings.rgb_pipe &&
                    restored.rgb_pipe_access == vcam::PipeAccess::owner &&
                    restored.control_pipe_access == vcam::PipeAccess::everyone,
                "Persistent settings did not round-trip");
            const auto legacy = directory / "legacy.ini";
            { std::ofstream output(legacy); output << "pipe_access=users\n"; }
            const auto migrated = vcam::load_app_settings(legacy.u8string());
            require(migrated.rgb_pipe_access == vcam::PipeAccess::local_users &&
                    migrated.control_pipe_access == vcam::PipeAccess::local_users,
                "Legacy shared pipe access was not migrated");
            std::filesystem::remove(legacy);
            std::filesystem::remove(config);
            std::filesystem::remove(preset);
            std::filesystem::remove(directory);
        }
        DWORD before = 0, after = 0;
        GetProcessHandleCount(GetCurrentProcess(), &before);
        protocol_tests();
        lifecycle_tests();
        cancellation_test();
        GetProcessHandleCount(GetCurrentProcess(), &after);
        require(after <= before+4, "Camera/control lifecycle leaked Windows handles");
        puts("PASS: descriptors, negotiation, ISO framing, 20000 malformed requests, lifecycle/replug rollback, and real control pipe.");
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
