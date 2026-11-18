#include "../src/camera/camera_session.h"
#include "../src/app/control_server.h"
#include "../src/common/pipe_security.h"
#include "../src/app/app_config.h"
#include "../src/app/web_server.h"
#include "../src/common/win_handle.h"
#include "../src/protocol/iso_reply.h"
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
int mjpeg_fixture(mjpeg_frame_t* frame) {
    static constexpr unsigned char jpeg[] = {0xff,0xd8,0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,0xff,0xd9};
    if (!frame || frame->capacity < static_cast<int>(sizeof(jpeg))) return -1;
    std::memcpy(frame->buffer, jpeg, sizeof(jpeg));
    frame->length = sizeof(jpeg);
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
const uvc_frame_uncompressed* video_frame_descriptor(const usbtx_header_t& response) {
    size_t position = 0;
    while (position + 2 <= size_t(response.data_length)) {
        const auto* entry = reinterpret_cast<const unsigned char*>(response.data)+position;
        require(entry[0] >= 2 && position+entry[0] <= size_t(response.data_length), "Invalid descriptor chain");
        if (entry[1] == 0x24 && entry[0] == 34 && entry[2] == 5)
            return reinterpret_cast<const uvc_frame_uncompressed*>(entry);
        position += entry[0];
    }
    throw std::runtime_error("Frame descriptor missing");
}
uint16_t iso_endpoint_max_packet(const usbtx_header_t& response) {
    size_t position = 0;
    while (position + 2 <= size_t(response.data_length)) {
        const auto* entry = reinterpret_cast<const unsigned char*>(response.data)+position;
        require(entry[0] >= 2 && position+entry[0] <= size_t(response.data_length), "Invalid descriptor chain");
        if (entry[1] == 0x05 && entry[0] >= 7 && entry[2] == 0x82)
            return static_cast<uint16_t>(entry[4] | (uint16_t(entry[5]) << 8));
        position += entry[0];
    }
    throw std::runtime_error("Isochronous endpoint descriptor missing");
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
void iso_reply_validation_tests() {
    require(xusb_iso_packet_fits_reply(24, 0, 10, 32),
        "Trailing zero-length ISO packet beyond the returned payload was rejected");
    require(xusb_iso_packet_fits_reply(32, 0, 0, 32),
        "Zero-length ISO packet at transfer capacity was rejected");
    require(xusb_iso_packet_fits_reply(4, 4, 8, 32),
        "ISO packet ending at the returned payload boundary was rejected");
    require(!xusb_iso_packet_fits_reply(8, 1, 8, 32),
        "Nonempty ISO packet beyond the returned payload was accepted");
    require(!xusb_iso_packet_fits_reply(33, 0, 0, 32),
        "Zero-length ISO packet beyond transfer capacity was accepted");
    require(!xusb_iso_packet_fits_reply(0, 0, 33, 32),
        "Returned ISO payload larger than transfer capacity was accepted");
    require(!xusb_iso_packet_fits_reply(0xfffffffful, 1, 0xfffffffful, 0xfffffffful),
        "ISO offset/length overflow was accepted");

    for (unsigned long capacity = 0; capacity <= 16; ++capacity) {
        for (unsigned long returned = 0; returned <= capacity; ++returned) {
            for (unsigned long offset = 0; offset <= capacity + 1; ++offset) {
                for (unsigned long length = 0; length <= capacity + 1; ++length) {
                    const bool withinCapacity = offset <= capacity && length <= capacity - offset;
                    const bool withinReply = length == 0 ||
                        (offset <= returned && length <= returned - offset);
                    const bool expected = withinCapacity && withinReply;
                    require(bool(xusb_iso_packet_fits_reply(offset, length, returned, capacity)) == expected,
                        "ISO reply validator disagreed with exhaustive boundary cases");
                }
            }
        }
    }
}
void protocol_tests() {
    vcam::UvcDevice device(configuration(), {640,480});
    for (auto size : {vcam::Resolution{640,480}, {1280,720}, {1920,1080}}) {
        require(device.set_resolution(size), "Resolution update failed");
        auto packet = descriptor();
        device.handle(packet.header(), packet.capacity);
        require(packet.header().result == 0 && packet.intact() && descriptor_size(packet.header()) == size, "Updated descriptor mismatch");
        const auto* frame = video_frame_descriptor(packet.header());
        constexpr uint64_t available_video_bytes_per_second = uint64_t(3070) * 8000;
        require(iso_endpoint_max_packet(packet.header()) == 0x1400, "High-speed isochronous endpoint does not reserve three transactions per microframe");
        require(frame->dwFrameInterval[0] <= frame->dwFrameInterval[1] &&
            uint64_t(frame->dwMaxVideoFrameBufferSize) * 10000000u <=
                available_video_bytes_per_second * frame->dwFrameInterval[0],
            "Advertised maximum frame rate exceeds the USB 2.0 isochronous payload budget");
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
    {
        Packet packet(sizeof(vcam::StreamControl));
        auto& hdr = packet.header();
        hdr.type = 2; hdr.vendor.index = 1; hdr.vendor.value = 0x0100; hdr.vendor.request = UVC_GET_DEF;
        device.handle(hdr, packet.capacity);
        vcam::StreamControl control{};
        std::memcpy(&control, hdr.data, sizeof(control));
        require(hdr.result == 0 && control.payload_bytes == 3072 && control.interval >= 333333,
            "Default UVC probe did not negotiate the enlarged isochronous payload");
    }
    for (int status_type = 1; status_type <= 7; ++status_type) {
        Packet packet(4);
        auto& hdr = packet.header();
        hdr.type = 5;
        hdr.status.type = status_type;
        hdr.status.index = 1;
        device.handle(hdr, packet.capacity);
        const size_t expected = status_type == 5 || status_type == 6 ? 1 : 2;
        require(packet.intact() && hdr.result == 0 && size_t(hdr.data_length) == expected,
            "Status/configuration/interface/frame-number response mismatch");
    }
    for (int feature_type : {1, 2}) for (int recipient = 1; recipient <= 4; ++recipient) {
        Packet packet(0);
        auto& hdr = packet.header();
        hdr.type = 6;
        hdr.feature.type = feature_type;
        hdr.feature.subtype = recipient;
        hdr.feature.index = 1;
        hdr.feature.value = 1;
        device.handle(hdr, packet.capacity);
        require(packet.intact() && hdr.result == 0 && hdr.data_length == 0,
            "Set/clear feature transaction failed");
    }
    for (int reset_type : {1, 2, 3, 4}) {
        Packet packet(0);
        auto& hdr = packet.header();
        hdr.type = 4;
        hdr.reset.type = reset_type;
        hdr.reset.ep_address = 0x82;
        device.handle(hdr, packet.capacity);
        require(packet.intact() && hdr.result == 0 && hdr.data_length == 0,
            "Reset/abort/port-cycle transaction failed");
    }
    {
        Packet packet(16);
        auto& hdr = packet.header();
        hdr.type = 2;
        hdr.vendor.type = 1; // UVC class request
        hdr.vendor.index = 1;
        hdr.vendor.value = 0x0200;
        hdr.vendor.request = UVC_GET_INFO;
        hdr.vendor.is_read = 1;
        device.handle(hdr, packet.capacity);
        require(packet.intact() && hdr.result == 0 && hdr.data_length == 1,
            "Direct class-request wire mapping failed");
    }
    for (int index : {0,1,2,255}) {
        Packet packet(sizeof(vcam::StreamControl));
        auto& hdr = packet.header();
        hdr.type = 2; hdr.vendor.index = 1; hdr.vendor.value = 0x0200; hdr.vendor.request = UVC_SET_CUR;
        vcam::StreamControl value;
        value.interval = 0; // Select the resolution-specific default frame interval.
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
        if (pass == 0) Sleep(40); // Let the negotiated 30 FPS frame interval elapse.
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

void mjpeg_protocol_tests() {
    vcam::UvcDevice device(configuration(), {4,2}, mjpeg_fixture);
    auto descriptors = descriptor();
    device.handle(descriptors.header(), descriptors.capacity);
    require(descriptors.header().result == 0 && descriptors.intact(), "MJPEG descriptor request failed");
    const auto* bytes = reinterpret_cast<const unsigned char*>(descriptors.header().data);
    const size_t descriptor_length = static_cast<size_t>(descriptors.header().data_length);
    bool found_yuy2 = false, found_mjpeg = false, found_stream_header = false;
    for (size_t position = 0; position + 2 <= descriptor_length;) {
        const size_t length = bytes[position];
        require(length >= 2 && position + length <= descriptor_length, "MJPEG descriptor chain is malformed");
        if (bytes[position+1] == 0x24 && bytes[position] == 27 && bytes[position+2] == 0x04) {
            const auto* format = reinterpret_cast<const uvc_format_uncompressed*>(bytes + position);
            found_yuy2 = format->bFormatIndex == 1;
        }
        if (bytes[position+1] == 0x24 && bytes[position+2] == 0x01 &&
            bytes[position] == UVC_DT_INPUT_HEADER_SIZE(2,1)) {
            const auto* header = reinterpret_cast<const uvc_input_header_descriptor*>(bytes + position);
            require(header->bNumFormats == 2 && header->bControlSize == 1 &&
                    header->bmaControls[0] == 0 && header->bmaControls[1] == 0 &&
                    header->wTotalLength == UVC_DT_INPUT_HEADER_SIZE(2,1) + 27 + 34 + 6 +
                        UVC_DT_FORMAT_MJPEG_SIZE + UVC_DT_FRAME_MJPEG_SIZE(2),
                "UVC input header length/control array does not match the advertised formats");
            found_stream_header = true;
        }
        if (bytes[position+1] == 0x24 && bytes[position] == UVC_DT_FORMAT_MJPEG_SIZE && bytes[position+2] == 0x06) {
            const auto* format = reinterpret_cast<const uvc_format_mjpeg*>(bytes + position);
            require(format->bFormatIndex == 2 && format->bNumFrameDescriptors == 1,
                "MJPEG format index/frame count mismatch");
            require(position + format->bLength + UVC_DT_FRAME_MJPEG_SIZE(2) <= descriptor_length,
                "MJPEG frame descriptor is truncated");
            const auto* frame = reinterpret_cast<const uvc_frame_mjpeg*>(bytes + position + format->bLength);
            uint32_t intervals[2]{};
            std::memcpy(intervals, reinterpret_cast<const char*>(frame) + UVC_DT_FRAME_MJPEG_SIZE(0), sizeof(intervals));
            require(frame->bLength == UVC_DT_FRAME_MJPEG_SIZE(2) && frame->bDescriptorType == 0x24 &&
                    frame->bDescriptorSubType == 0x07 && frame->bFrameIndex == 1 &&
                    frame->wWidth == 4 && frame->wHeight == 2 && frame->dwMaxVideoFrameBufferSize >= 12 &&
                    frame->bFrameIntervalType == 2 && intervals[0] == 333333 && intervals[1] == 666667,
                "MJPEG frame mode or USB 2.0 frame budget descriptor is invalid");
            found_mjpeg = true;
        }
        position += length;
    }
    require(found_yuy2 && found_mjpeg && found_stream_header,
        "MJPEG mode was not added alongside the original YUY2 mode with a complete input header");

    Packet negotiation(sizeof(vcam::StreamControl));
    auto& negotiation_request = negotiation.header();
    negotiation_request.type = 2;
    negotiation_request.vendor.type = 1;
    negotiation_request.vendor.index = 1;
    negotiation_request.vendor.value = 0x0100;
    negotiation_request.vendor.request = UVC_SET_CUR;
    vcam::StreamControl stream{};
    stream.format = 2;
    stream.frame = 1;
    stream.interval = 333333;
    stream.payload_bytes = 3072;
    std::memcpy(negotiation_request.data, &stream, sizeof(stream));
    device.handle(negotiation_request, negotiation.capacity);
    require(negotiation_request.result == 0 && negotiation_request.data_length == sizeof(stream),
        "MJPEG stream negotiation failed");

    auto packet = make_iso_packet();
    device.handle(packet.header(), packet.capacity);
    require(packet.header().result == 0 && packet.intact(), "MJPEG ISO transfer failed");
    const auto* iso = reinterpret_cast<const iso_packet_hdr*>(packet.header().data);
    const auto* payload = reinterpret_cast<const unsigned char*>(packet.header().data) +
        ISO_PACKET_HDR_SIZE + 3*sizeof(iso_packet);
    std::vector<unsigned char> image;
    for (unsigned index = 0; index < 3; ++index) {
        const auto& entry = iso->packets[index];
        const auto* fragment = payload + entry.offset;
        if (!entry.length) continue;
        require(entry.length >= 2 && fragment[0] == 2 && (fragment[1] & UVC_STREAM_EOH),
            "MJPEG payload header is missing");
        require((fragment[1] & 1) == 0, "MJPEG frame identifier changed inside a frame");
        image.insert(image.end(), fragment + 2, fragment + entry.length);
    }
    static constexpr unsigned char expected[] = {0xff,0xd8,0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,0xff,0xd9};
    require(image.size() == sizeof(expected) && std::memcmp(image.data(), expected, sizeof(expected)) == 0,
        "MJPEG ISO fragments did not reassemble into the encoded image");
    require(iso->packets[1].length &&
            (payload[iso->packets[1].offset+1] & UVC_STREAM_EOF),
        "MJPEG end-of-frame flag was not set on the final packet");
}

void superspeed_protocol_tests() {
    vcam::UvcDevice device(configuration(), {1280,720}, nullptr, true);

    Packet deviceDescriptor(64);
    deviceDescriptor.header().type = 1;
    deviceDescriptor.header().descriptor.type = 1;
    deviceDescriptor.header().descriptor.is_read = 1;
    deviceDescriptor.header().descriptor.subtype = 1;
    device.handle(deviceDescriptor.header(), deviceDescriptor.capacity);
    require(deviceDescriptor.header().result == 0 && deviceDescriptor.header().data_length == 18 &&
            deviceDescriptor.header().data[2] == 0 && deviceDescriptor.header().data[3] == 3 &&
            deviceDescriptor.header().data[7] == 9,
        "SuperSpeed device descriptor has the wrong USB version or EP0 encoding");

    Packet bos(64);
    bos.header().type = 1;
    bos.header().descriptor.type = 1;
    bos.header().descriptor.is_read = 1;
    bos.header().descriptor.subtype = 15;
    device.handle(bos.header(), bos.capacity);
    const auto* bosBytes = reinterpret_cast<const unsigned char*>(bos.header().data);
    require(bos.header().result == 0 && bos.header().data_length == 22 && bosBytes[0] == 5 &&
            bosBytes[1] == 0x0f && bosBytes[2] == 22 && bosBytes[4] == 2 &&
            bosBytes[14] == 3 && bosBytes[16] == 0x08,
        "SuperSpeed BOS descriptor is incomplete or malformed");

    auto config = descriptor();
    device.handle(config.header(), config.capacity);
    require(config.header().result == 0 && config.intact(), "SuperSpeed configuration descriptor failed");
    const auto* bytes = reinterpret_cast<const unsigned char*>(config.header().data);
    const size_t length = static_cast<size_t>(config.header().data_length);
    bool foundEndpoint = false;
    for (size_t position = 0; position + 2 <= length;) {
        const size_t itemLength = bytes[position];
        require(itemLength >= 2 && position + itemLength <= length,
            "SuperSpeed configuration descriptor chain is malformed");
        if (bytes[position + 1] == 0x05 && bytes[position] == 7 && bytes[position + 2] == 0x82) {
            require(bytes[position + 4] == 0 && bytes[position + 5] == 4,
                "SuperSpeed ISO endpoint must use a 1024-byte packet size");
            require(position + 13 <= length && bytes[position + 7] == 6 && bytes[position + 8] == 0x30 &&
                    bytes[position + 9] == 7 && bytes[position + 10] == 0 &&
                    bytes[position + 11] == 0 && bytes[position + 12] == 0x20,
                "SuperSpeed endpoint companion descriptor does not advertise an 8,192-byte interval");
            foundEndpoint = true;
        }
        position += itemLength;
    }
    require(foundEndpoint, "SuperSpeed isochronous endpoint descriptor is missing");

    Packet probe(sizeof(vcam::StreamControl));
    probe.header().type = 2;
    probe.header().vendor.index = 1;
    probe.header().vendor.value = 0x0100;
    probe.header().vendor.request = UVC_GET_MAX;
    device.handle(probe.header(), probe.capacity);
    vcam::StreamControl stream{};
    std::memcpy(&stream, probe.header().data, sizeof(stream));
    require(probe.header().result == 0 && stream.payload_bytes == 8192 && stream.interval == 666667,
        "SuperSpeed UVC probe does not expose its endpoint interval budget");

    Packet minimum(sizeof(vcam::StreamControl));
    minimum.header().type = 2;
    minimum.header().vendor.index = 1;
    minimum.header().vendor.value = 0x0100;
    minimum.header().vendor.request = UVC_GET_MIN;
    device.handle(minimum.header(), minimum.capacity);
    std::memcpy(&stream, minimum.header().data, sizeof(stream));
    require(minimum.header().result == 0 && stream.payload_bytes == 8192 &&
            stream.frame_bytes == 1280u * 720u * 2u && stream.interval == 333333,
        "SuperSpeed UVC probe does not advertise 720p YUY2 at 30 fps");
}

struct FakeState {
    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false, plugged = false, destroyed = false;
    int plugs = 0, unplugs = 0, fail_plug = -1, cycles = 0;
    bool cancel_next_end = false, fail_next_end = false;
    std::deque<Packet> requests, responses;
};
class FakeTransport final : public vcam::UsbTransport {
    std::shared_ptr<FakeState> state_;
    std::unique_ptr<Packet> active_;
    bool super_speed_;
public:
    explicit FakeTransport(std::shared_ptr<FakeState> state, bool super_speed = false)
        : state_(std::move(state)), super_speed_(super_speed) {}
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
    vcam::UsbEndResult end(usbtx_header_t*) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        require(active_->intact(), "Worker overran fake transaction");
        if (state_->cancel_next_end) {
            state_->cancel_next_end = false;
            active_.reset();
            state_->changed.notify_all();
            return vcam::UsbEndResult::canceled;
        }
        if (state_->fail_next_end) {
            state_->fail_next_end = false;
            active_.reset();
            state_->changed.notify_all();
            return vcam::UsbEndResult::failed;
        }
        state_->responses.push_back(std::move(*active_)); active_.reset(); state_->changed.notify_all();
        return vcam::UsbEndResult::completed;
    }
    size_t capacity() const override { return active_ ? active_->capacity : 0; }
    bool is_superspeed() const override { return super_speed_; }
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
            auto status = session.status_snapshot();
            require(status.running && status.usb_mode == "high-speed", "USB 2.0 camera link status is wrong");
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
    {
        auto superspeed_state = std::make_shared<FakeState>();
        vcam::CameraSession session(configuration(), {640,480},
            std::make_unique<FakeTransport>(superspeed_state, true));
        require(session.status_snapshot().usb_mode == "disconnected", "Unplugged camera reported an active USB link");
        require(session.start(), "SuperSpeed camera worker start failed");
        require(session.status_snapshot().usb_mode == "superspeed", "SuperSpeed camera link mode is wrong");
        {
            std::lock_guard<std::mutex> lock(superspeed_state->mutex);
            superspeed_state->fail_next_end = true;
            superspeed_state->requests.push_back(descriptor());
            superspeed_state->changed.notify_all();
        }
        const ULONGLONG failure_deadline = GetTickCount64() + 1000;
        while (session.running() && GetTickCount64() < failure_deadline) Sleep(1);
        const auto stopped_status = session.status_snapshot();
        require(!stopped_status.running && stopped_status.usb_mode == "superspeed",
            "A stopped USB worker did not expose its stopped state alongside the selected link mode");
    }
    require(GetTickCount64()-started < 10000, "Worker shutdown did not cancel waits promptly");
    {
        auto state = std::make_shared<FakeState>();
        vcam::CameraSession session(configuration(), {640,480}, std::make_unique<FakeTransport>(state));
        require(session.start(), "Cancellation recovery camera start failed");
        Packet canceled_cycle(0);
        canceled_cycle.header().type = 4;
        canceled_cycle.header().reset.type = 2;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->cancel_next_end = true;
            state->requests.push_back(std::move(canceled_cycle));
            state->requests.push_back(descriptor());
            state->changed.notify_all();
        }
        auto response = round_trip(state, descriptor());
        require(descriptor_size(response.header()) == vcam::Resolution{640,480} && session.running(),
            "Canceled USB transaction stopped the camera worker");
        std::lock_guard<std::mutex> lock(state->mutex);
        require(state->cycles == 0, "Canceled cycle request unexpectedly replugged the camera");
    }
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
        iso_reply_validation_tests();
        protocol_tests();
        mjpeg_protocol_tests();
        superspeed_protocol_tests();
        lifecycle_tests();
        cancellation_test();
        GetProcessHandleCount(GetCurrentProcess(), &after);
        require(after <= before+4, "Camera/control lifecycle leaked Windows handles");
        puts("PASS: USB 2.0/3.0 YUY2/MJPEG descriptors and negotiation, ISO frame reassembly, wire types 2/4/5/6, 20000 malformed requests, lifecycle/replug rollback, and real control pipe.");
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
