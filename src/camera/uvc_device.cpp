// USB/UVC descriptors derived from Fanxiushu's original implementation.
#include "uvc_device.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace vcam {
UvcDevice::UvcDevice(const uvc_vcam_t& config, Resolution size)
    : callback_(config.frame_callback), parameter_(config.param) {
    if (!callback_) throw std::invalid_argument("Camera frame callback is required");
    fill_dev_desc(static_cast<unsigned short>(config.pid), config.vid);
    fill_str_desc(config.manu_fact, config.product);
    if (!set_resolution(size)) throw std::invalid_argument("Invalid camera resolution");
}
bool UvcDevice::set_resolution(Resolution size) {
    if (!valid_output(size)) return false;
    std::vector<char> replacement(yuy2_bytes(size), 0);
    pixels_.swap(replacement);
    frames[0] = {size.width, size.height};
    position_ = 0;
    flip_ = 0;
    curr_config = curr_interface = curr_alt_setting = 0;
    stream_ = StreamControl{};
    stream_.frame_bytes = static_cast<uint32_t>(pixels_.size());
    std::memset(cfg_buffer, 0, sizeof(cfg_buffer));
    fill_cfg_desc();
    return cfg_desc && cfg_desc->wTotalLength <= sizeof(cfg_buffer);
}

void UvcDevice::fill_dev_desc(unsigned short pid, unsigned int vid)
{
	dev_desc.bLength = 0x12;
	dev_desc.bDescriptorType = 0x01;
	dev_desc.bcdUSB = 0x0200;
	dev_desc.bDeviceClass = 0xEF;
	dev_desc.bDeviceSubClass = 0x02;
	dev_desc.bDeviceProtocol = 0x01;
	dev_desc.bMaxPacketSize0 = 0x40;
	dev_desc.idVendor = vid;
	dev_desc.idProduct = pid;
	dev_desc.bcdDevice = 0x00;
	dev_desc.iManufacturer = 0x01;
	dev_desc.iProduct = 0x02;
	dev_desc.iSerialNumber = 0x00;
	dev_desc.bNumConfigurations = 1;
}
static void set_usb_string_descriptor(usb_string_descriptor* descriptor, const char* value)
{
	descriptor->bDescriptorType = 0x03;
	const int converted_chars = value
		? ::MultiByteToWideChar(CP_ACP, 0, value, -1, (PWCHAR)descriptor->wData, 127)
		: 0;
	if (converted_chars <= 0)
	{
		descriptor->wData[0] = L'\0';
		descriptor->bLength = 2;
		return;
	}

	const size_t text_bytes = (size_t)(converted_chars - 1) * sizeof(WCHAR);
	descriptor->bLength = (UCHAR)(2 + text_bytes);
}

void UvcDevice::fill_str_desc(const char* manu_fact, const char* product)
{
	usb_string_descriptor* str = (usb_string_descriptor*)str_buffer;
	str->bLength = 0x04;
	str->bDescriptorType = 0x03;
	str->wData[0] = 0x0409;
	str_desc[0] = str;

	str = (usb_string_descriptor*)( (char*)str + str->bLength );
	set_usb_string_descriptor(str, manu_fact);
	str_desc[1] = str;

	str = (usb_string_descriptor*)((char*)str + str->bLength);
	set_usb_string_descriptor(str, product);
	str_desc[2] = str;

	str_count = 3;
}

// Builds the existing UVC topology: video control, one YUY2 frame, and one ISO endpoint.
void UvcDevice::fill_cfg_desc()
{

	cfg_desc = (usb_config_descriptor*)cfg_buffer;

	cfg_desc->bLength = 0x09;
	cfg_desc->bDescriptorType = 0x02;
	cfg_desc->wTotalLength = 9;
	cfg_desc->bNumInterfaces = 2;
	cfg_desc->bConfigurationValue = 1;
	cfg_desc->iConfiguration = 0;
	cfg_desc->bmAttributes = 0x80;
	cfg_desc->bMaxPower = 0x40;

	usb_interface_assoc_descriptor* iad = (usb_interface_assoc_descriptor*)( (char*)cfg_desc + cfg_desc->bLength );
	iad->bLength = 0x08;
	iad->bDescriptorType = 0x0B;
	iad->bFirstInterface = 0;
	iad->bInterfaceCount = 0x02;
	iad->bFunctionClass = 0x0E;
	iad->bFunctionSubClass = 0x03;
	iad->bFunctionProtocol = 0x00;
	iad->iFunction = 0x02;

	usb_interface_descriptor* intr = (usb_interface_descriptor*)((char*)iad + iad->bLength);
	intr->bLength = 0x09;
	intr->bDescriptorType = 0x04;
	intr->bInterfaceNumber = 0;
	intr->bAlternateSetting = 0;
	intr->bNumEndpoints = 0x00;
	intr->bInterfaceClass = 0x0E;
	intr->bInterfaceSubClass = 0x01;
	intr->bInterfaceProtocol = 0x00;
	intr->iInterface = 0x02;

	uvc_header_descriptor* vchdr = (uvc_header_descriptor*)((char*)intr + intr->bLength);
	vchdr->bLength = 0x0D;
	vchdr->bDescriptorType = 0x24;
	vchdr->bDescriptorSubType = 0x01;
	vchdr->bcdUVC = 0x0100;
	vchdr->wTotalLength = 0x4F;
	vchdr->dwClockFrequency = 0x01C9C380;
	vchdr->bInCollection = 0x01;
	vchdr->baInterfaceNr[0] = 0x01;

	uvc_camera_terminal_descriptor* ctd = (uvc_camera_terminal_descriptor*)((char*)vchdr + vchdr->bLength);
	ctd->bLength = 0x12;
	ctd->bDescriptorType = 0x24;
	ctd->bDescriptorSubType = 0x02;
	ctd->bTerminalID = 0x01;
	ctd->wTerminalType = 0x0201;
	ctd->bAssocTerminal = 0x00;
	ctd->iTerminal = 0;
	ctd->wObjectiveFocalLengthMax = 0;
	ctd->wObjectiveFocalLengthMin = 0;
	ctd->wOcularFocalLength = 0;
	ctd->bControlSize = 3;
	ctd->bmControls[0] = 0x0A;
	ctd->bmControls[1] = ctd->bmControls[2] = 0x00;

	uvc_processing_unit_descriptor* pud = (uvc_processing_unit_descriptor*)((char*)ctd + ctd->bLength);
	pud->bLength = 0x0B;
	pud->bDescriptorType = 0x24;
	pud->bDescriptorSubType = 0x05;
	pud->bUnitID = 0x02;
	pud->bSourceID = 0x01;
	pud->wMaxMultiplier = 0x00;
	pud->bControlSize = 0x02;
	pud->bmControls[0] = 0x7B;
	pud->bmControls[1] = 0x17;
	pud->iProcessing = 0;

	uvc_output_terminal_descriptor* otd = (uvc_output_terminal_descriptor*)((char*)pud + pud->bLength);
	otd->bLength = 0x09;
	otd->bDescriptorType = 0x24;
	otd->bDescriptorSubType = 0x03;
	otd->bTerminalID = 0x03;
	otd->wTerminalType = 0x0101;
	otd->bAssocTerminal = 0x00;
	otd->bSourceID = 0x02;
	otd->iTerminal = 0;

	uvc_extension_unit_descriptor_1_3* eud = (uvc_extension_unit_descriptor_1_3*)((char*)otd + otd->bLength);
	eud->bLength = 0x1C;
	eud->bDescriptorType = 0x24;
	eud->bDescriptorSubType = 0x06;
	eud->bUnitID = 0x04;

	static const GUID g =
	{ 0x88f04da6, 0xf713, 0x454a,{ 0xb6, 0x25, 0x37, 0x93, 0xae, 0x44, 0x75, 0x16 } };
	memcpy(eud->guidExtensionCode, &g, sizeof(GUID));
	eud->bNumControls = 0x08;
	eud->bNrInPins = 0x01;
	eud->baSourceID[0] = 0x01;
	eud->bControlSize = 0x03;
	eud->bmControls[0] = 0x00;
	eud->bmControls[1] = 0x00;
	eud->bmControls[2] = 0x00;
	eud->iExtension = 0x00;

	intr = (usb_interface_descriptor*)((char*)eud + eud->bLength);
	intr->bLength = 0x09;
	intr->bDescriptorType = 0x04;
	intr->bInterfaceNumber = 0x01;
	intr->bAlternateSetting = 0x00;
	intr->bNumEndpoints = 0x00;
	intr->bInterfaceClass = 0x0E;
	intr->bInterfaceSubClass = 0x02;
	intr->bInterfaceProtocol = 0x00;
	intr->iInterface = 0x00;

	uvc_input_header_descriptor* vshdr = (uvc_input_header_descriptor*)((char*)intr + intr->bLength);
	vshdr->bLength = 0x0E;
	vshdr->bDescriptorType = 0x24;
	vshdr->bDescriptorSubType = 0x01;
	vshdr->bNumFormats = 0x01;
	vshdr->wTotalLength = 14 + 27 + frame_count* 34 + 6 ;
	vshdr->bEndpointAddress = iso_ep_addr;
	vshdr->bmInfo = 0x00;
	vshdr->bTerminalLink = 0x03;
	vshdr->bStillCaptureMethod = 0x00;
	vshdr->bTriggerSupport = 0x00;
	vshdr->bTriggerUsage = 0x00;
	vshdr->bControlSize = 0x01;
	vshdr->bmaControls[0] = 0x00;

	uvc_format_uncompressed* fmt = (uvc_format_uncompressed*)((char*)vshdr + vshdr->bLength);
	fmt->bLength = 0x1B;
	fmt->bDescriptorType = 0x24;
	fmt->bDescriptorSubType = 0x04;
	fmt->bFormatIndex = 0x01;
	fmt->bNumFrameDescriptors = frame_count;
	memcpy(fmt->guidFormat, &guidFormat, sizeof(GUID));
	fmt->bBitsPerPixel = bitsPerPixel;
	fmt->bDefaultFrameIndex = 0x01;
	fmt->bAspectRatioX = 0x00;
	fmt->bAspectRatioY = 0x00;
	fmt->bmInterfaceFlags = 0x00;
	fmt->bCopyProtect = 0x00;

	uvc_frame_uncompressed* frame = (uvc_frame_uncompressed*)((char*)fmt + fmt->bLength);
	for (int i = 0; i < frame_count; ++i) {

		frame->bLength = 34;
		frame->bDescriptorType = 0x24;
		frame->bDescriptorSubType = 0x05;
		frame->bFrameIndex = i + 1;
		frame->bmCapabilities = 0x00;
		frame->wWidth = frames[i].width;
		frame->wHeight = frames[i].height;
		frame->dwMinBitRate = (bitsPerPixel * frame->wWidth * frame->wHeight) * 15u;
		frame->dwMaxBitRate = (bitsPerPixel * frame->wWidth * frame->wHeight) * 30u;
		frame->dwMaxVideoFrameBufferSize = (bitsPerPixel / 8)* frame->wWidth* frame->wHeight;
		frame->dwDefaultFrameInterval = 0x00051615;
		frame->bFrameIntervalType = 0x02;
		frame->dwFrameInterval[0] = 0x00051615;
		frame->dwFrameInterval[1] = 0x000A2C2B;

		frame = (uvc_frame_uncompressed*)((char*)frame + frame->bLength);
	}

	uvc_color_matching_descriptor* cmd = (uvc_color_matching_descriptor*)frame;
	cmd->bLength = 0x06;
	cmd->bDescriptorType = 0x24;
	cmd->bDescriptorSubType = 0x0D;
	cmd->bColorPrimaries = 0x01;
	cmd->bTransferCharacteristics = 0x01;
	cmd->bMatrixCoefficients = 0x04;

	intr = (usb_interface_descriptor*)((char*)cmd + cmd->bLength);
	intr->bLength = 0x09;
	intr->bDescriptorType = 0x04;
	intr->bInterfaceNumber = 0x01;
	intr->bAlternateSetting = 0x01;
	intr->bNumEndpoints = 0x01;
	intr->bInterfaceClass = 0x0E;
	intr->bInterfaceSubClass = 0x02;
	intr->bInterfaceProtocol = 0x00;
	intr->iInterface = 0x00;

	usb_endpoint_descriptor* ep = (usb_endpoint_descriptor*)((char*)intr + intr->bLength);
	ep->bLength = 0x07;
	ep->bDescriptorType = 0x05;
	ep->bEndpointAddress = iso_ep_addr;
	ep->bmAttributes = 0x05;
	ep->wMaxPacketSize = 0x0C00;

	ep->bInterval = 0x01;

	const size_t config_length = (size_t)(((char*)ep + ep->bLength) - cfg_buffer);
	if (config_length > sizeof(cfg_buffer) || config_length > 0xffff)
	{
		printf("Invalid USB configuration descriptor length: %zu bytes\n", config_length);
		return;
	}
	cfg_desc->wTotalLength = (USHORT)config_length;

}

namespace {
uint16_t little16(const unsigned char* data) { return uint16_t(data[0] | (uint16_t(data[1]) << 8)); }
void reply(usbtx_header_t& request, const void* data, size_t size) {
    const size_t count = (std::min)(static_cast<size_t>(request.data_length), size);
    if (count) std::memcpy(request.data, data, count);
    request.data_length = static_cast<int>(count);
    request.result = 0;
}
}
void UvcDevice::descriptor(usbtx_header_t& request) {
    if (request.descriptor.type == 1 && request.descriptor.is_read) {
        switch (request.descriptor.subtype) {
        case 1: reply(request, &dev_desc, sizeof(dev_desc)); break;
        case 2: reply(request, cfg_desc, cfg_desc->wTotalLength); break;
        case 3:
            if (request.descriptor.index >= 0 && request.descriptor.index < str_count)
                reply(request, str_desc[request.descriptor.index], str_desc[request.descriptor.index]->bLength);
            break;
        }
    }
    else if (request.descriptor.type == 2) {
        if (request.descriptor.subtype == 1) {
            const int value = request.descriptor.value;
            if (value == -1 || value == 0 || value == 1) {
                curr_config = value < 0 ? 0 : value;
                request.result = request.data_length = 0;
            }
        }
        else if (request.descriptor.subtype == 2 && request.descriptor.index >= 0 &&
                 request.descriptor.index <= 1 && request.descriptor.value >= 0 &&
                 request.descriptor.value <= (request.descriptor.index == 1 ? 1 : 0)) {
            curr_interface = request.descriptor.index;
            curr_alt_setting = request.descriptor.value;
            position_ = 0;
            request.result = request.data_length = 0;
        }
    }
}
void UvcDevice::streaming(usbtx_header_t& request) {
    if ((request.vendor.index & 0xff) != 1) return;
    const int selector = (request.vendor.value >> 8) & 0xff;
    if (selector != 1 && selector != 2) return;
    switch (request.vendor.request) {
    case UVC_GET_INFO: {
        const uint8_t info = 3;
        reply(request, &info, sizeof(info));
        break;
    }
    case UVC_GET_LEN: {
        const uint16_t length = sizeof(StreamControl);
        reply(request, &length, sizeof(length));
        break;
    }
    case UVC_GET_DEF:
    case UVC_GET_CUR:
    case UVC_GET_MIN:
    case UVC_GET_MAX:
    case UVC_GET_RES: {
        StreamControl value = stream_;
        if (request.vendor.request == UVC_GET_DEF || request.vendor.request == UVC_GET_MIN)
            value.interval = 333333;
        if (request.vendor.request == UVC_GET_MAX) value.interval = 666667;
        if (request.vendor.request == UVC_GET_RES) value.interval = 0;
        reply(request, &value, sizeof(value));
        break;
    }
    case UVC_SET_CUR: {
        if (request.data_length < sizeof(StreamControl)) break;
        StreamControl value;
        std::memcpy(&value, request.data, sizeof(value));
        if (value.format != 1 || value.frame != 1 ||
            (value.interval != 0 && value.interval != 333333 && value.interval != 666667)) break;
        stream_.interval = value.interval ? value.interval : 333333;
        position_ = 0;
        request.data_length = sizeof(StreamControl);
        request.result = 0;
        break;
    }
    }
}
void UvcDevice::control(usbtx_header_t& request) {
    const auto original = request.transfer;
    const auto* setup = original.setup_packet;
    const uint16_t value = little16(setup + 2), index = little16(setup + 4);
    const uint16_t length = little16(setup + 6);
    request.data_length = (std::min)(request.data_length, static_cast<int>(length));
    if ((setup[0] & 0x60) == 0x20) {
        request.type = 2;
        request.vendor.type = 1;
        request.vendor.is_read = (setup[0] & 0x80) != 0;
        request.vendor.request = setup[1];
        request.vendor.index = index;
        request.vendor.value = value;
        streaming(request);
    }
    else if ((setup[0] & 0x60) == 0) {
        switch (setup[1]) {
        case 6:
            if (setup[0] & 0x80) {
                request.type = 1;
                request.descriptor.type = 1;
                request.descriptor.is_read = 1;
                request.descriptor.subtype = value >> 8;
                request.descriptor.index = value & 0xff;
                request.descriptor.value = index;
                descriptor(request);
            }
            break;
        case 8: {
            const uint8_t configured = static_cast<uint8_t>(curr_config);
            if (setup[0] & 0x80) reply(request, &configured, 1);
            break;
        }
        case 9:
            if (!(setup[0] & 0x80) && value <= 1) {
                curr_config = value;
                request.result = request.data_length = 0;
            }
            break;
        case 10: {
            const uint8_t alternate = static_cast<uint8_t>(curr_alt_setting);
            if ((setup[0] & 0x80) && index <= 1) reply(request, &alternate, 1);
            break;
        }
        case 11:
            if (!(setup[0] & 0x80) && index <= 1 && value <= (index == 1 ? 1 : 0)) {
                curr_interface = index;
                curr_alt_setting = value;
                position_ = 0;
                request.result = request.data_length = 0;
            }
            break;
        case 0: {
            const uint16_t status = 0;
            if (setup[0] & 0x80) reply(request, &status, sizeof(status));
            break;
        }
        case 1:
        case 3: request.result = request.data_length = 0; break;
        }
    }
    request.type = 3;
    request.transfer = original;
}
int UvcDevice::delay_before(const usbtx_header_t& request) const {
    return request.type == 3 && request.transfer.type == 3 && position_ == 0 ? delay_ : 0;
}
void UvcDevice::iso(usbtx_header_t& request) {
    if (!request.transfer.is_read || request.transfer.ep_address != iso_ep_addr ||
        request.transfer.number_packets < 0 || request.data_length < ISO_PACKET_HDR_SIZE) return;
    auto* header = reinterpret_cast<iso_packet_hdr*>(request.data);
    const size_t count = header->number_packets;
    const size_t capacity = static_cast<size_t>(request.data_length);
    if (!count || count > (capacity - ISO_PACKET_HDR_SIZE) / sizeof(iso_packet) ||
        (request.transfer.number_packets > 0 && count != size_t(request.transfer.number_packets))) return;
    const size_t metadata = ISO_PACKET_HDR_SIZE + count * sizeof(iso_packet);
    const size_t payload = capacity - metadata;

    for (size_t i = 0; i < count; ++i) {
        if (header->packets[i].offset > payload ||
            (i && header->packets[i].offset < header->packets[i-1].offset)) return;
    }
    if (position_ == 0) {
        const auto size = resolution();
        frame_t frame{pixels_.data(), static_cast<int>(pixels_.size()), size.width, size.height, delay_, parameter_};
        if (callback_(&frame) < 0 || frame.width != size.width || frame.height != size.height ||
            frame.length != pixels_.size()) return;
        delay_ = std::clamp(frame.delay_msec, 1, 1000);
    }
    header->error_count = 0;
    for (size_t i = 0; i < count; ++i) {
        header->packets[i].length = 0;
        header->packets[i].status = 0;
    }
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t start = header->packets[i].offset;
        const size_t end = i+1 < count ? header->packets[i+1].offset : payload;
        const int bytes = encode(reinterpret_cast<unsigned char*>(request.data) + metadata + start, end-start);
        if (bytes <= 0) continue;
        header->packets[i].length = bytes;
        used = start + bytes;
        if (!position_) break;
    }
    request.result = 0;
    request.data_length = static_cast<int>(metadata + used);
}
int UvcDevice::encode(unsigned char* destination, size_t capacity) {
    if (capacity <= 2 || position_ >= pixels_.size()) return 0;
    const size_t count = (std::min)(pixels_.size()-position_, capacity-2);
    destination[0] = 2;
    destination[1] = UVC_STREAM_EOH | flip_;
    std::memcpy(destination+2, pixels_.data()+position_, count);
    position_ += count;
    if (position_ == pixels_.size()) {
        destination[1] |= UVC_STREAM_EOF;
        flip_ ^= 1;
        position_ = 0;
    }
    return static_cast<int>(count+2);
}
void UvcDevice::handle(usbtx_header_t& request, size_t payload_capacity) {
    request.result = -1;
    if (request.data_length < 0 || size_t(request.data_length) > payload_capacity) {
        request.data_length = 0;
        return;
    }
    switch (request.type) {
    case 1: descriptor(request); break;
    case 2: streaming(request); break;
    case 3:
        if (request.transfer.type == 1) control(request);
        else if (request.transfer.type == 3) iso(request);
        break;
    case 4:
        position_ = 0;
        request.result = request.data_length = 0;
        break;
    case 5: {
        if (request.status.type == 5 || request.status.type == 6) {
            const uint8_t value = static_cast<uint8_t>(request.status.type == 5 ? curr_config : curr_alt_setting);
            reply(request, &value, 1);
        }
        else {
            const uint16_t value = 0;
            reply(request, &value, sizeof(value));
        }
        break;
    }
    case 6: request.result = request.data_length = 0; break;
    }
    if (request.result < 0) request.data_length = 0;
}
}
