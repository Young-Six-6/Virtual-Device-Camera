#include <Windows.h>
#include <InitGuid.h>
#include <SetupAPI.h>
#include <WinIoCtl.h>
#include "ioctl.h"
#include "transport.h"
#include "../common/pipe_io.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace {
constexpr size_t maximum_payload = 32 * 1024 * 1024;
constexpr size_t request_offset = offsetof(ioctl_usbtx_header_t, type);
static_assert(request_offset == 24 && sizeof(ioctl_usbtx_header_t) == 24 + sizeof(usbtx_header_t),
    "Legacy virtual USB driver ABI changed");
struct Device {
    vcam::unique_handle file, stop, semaphore, removed;
    std::vector<unsigned char> buffer;
    vcam::DeviceIdentity identity;
    int address = 0;
    bool plugged = false, remove_pending = false, active = false;
    size_t payload_capacity = 0;

    bool ioctl(DWORD code, void* input, DWORD input_bytes, void* output, DWORD output_bytes, DWORD& bytes) {
        if (WaitForSingleObject(stop.get(), 0) == WAIT_OBJECT_0) return false;
        vcam::unique_handle event(CreateEvent(NULL, TRUE, FALSE, NULL));
        if (!event) return false;
        OVERLAPPED operation = {};
        operation.hEvent = event.get();
        if (DeviceIoControl(file.get(), code, input, input_bytes, output, output_bytes, &bytes, &operation)) return true;
        if (GetLastError() != ERROR_IO_PENDING) return false;
        return vcam::finish_io(file.get(), operation, stop.get(), bytes);
    }
};
bool copy_id(WCHAR* output, const std::string& text, bool multi) {
    std::fill(output, output + HW_IDS_COUNT, wchar_t(0));
    if (text.empty()) return true;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, output, HW_IDS_COUNT-1)) return false;
    if (multi) for (int i = 0; i < HW_IDS_COUNT; ++i) if (output[i] == L'\n') output[i] = L'\0';
    return true;
}
vcam::unique_handle open_driver() {
    HDEVINFO list = SetupDiGetClassDevsW(&GUID_XUSB_VIRT_INTERFACE, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (list == INVALID_HANDLE_VALUE) return {};
    struct ListOwner { HDEVINFO value; ~ListOwner() { SetupDiDestroyDeviceInfoList(value); } } owner{list};
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA entry{sizeof(entry)};
        if (!SetupDiEnumDeviceInterfaces(list, NULL, &GUID_XUSB_VIRT_INTERFACE, index, &entry)) break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(list, &entry, NULL, 0, &needed, NULL);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<unsigned char> storage(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(list, &entry, detail, needed, NULL, NULL)) continue;
        vcam::unique_handle file(CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL));
        if (file) return file;
    }
    return {};
}
bool is_device_read(const usbtx_header_t& request) {
    if (request.type == 1) return request.descriptor.is_read != 0;
    if (request.type == 2) return request.vendor.is_read != 0;
    if (request.type == 3) return request.transfer.is_read != 0;
    return request.type == 5;
}
}

void* virt_usb_open() {
    try {
        auto device = std::make_unique<Device>();
        device->file = open_driver();
        device->stop.reset(CreateEvent(NULL, TRUE, FALSE, NULL));
        if (!device->file || !device->stop) return nullptr;
        device->buffer.resize(64 * 1024);
        return device.release();
    }
    catch (...) { return nullptr; }
}
void virt_usb_close(void* handle) {
    if (!handle) return;
    virt_usb_resume(handle);
    virt_usb_unplug(handle);
    delete static_cast<Device*>(handle);
}
void virt_usb_interrupt(void* handle) {
    if (auto* device = static_cast<Device*>(handle)) {
        SetEvent(device->stop.get());
        CancelIoEx(device->file.get(), NULL);
    }
}
void virt_usb_resume(void* handle) {
    if (auto* device = static_cast<Device*>(handle)) ResetEvent(device->stop.get());
}
size_t virt_usb_capacity(void* handle) {
    auto* device = static_cast<Device*>(handle);
    return device ? device->payload_capacity : 0;
}
int virt_usb_plugin(void* handle, const char* id, const char* hardware, const char* compatible) {
    auto* device = static_cast<Device*>(handle);
    if (!device || device->plugged || !hardware || !*hardware) return -1;
    try {
        vcam::DeviceIdentity identity{id ? id : hardware, hardware, compatible ? compatible : ""};
        ioctl_pdo_create_t request = {};
        if (!copy_id(request.device_id, identity.device_id, false) ||
            !copy_id(request.hardware_ids, identity.hardware_ids, true) ||
            !copy_id(request.compatible_ids, identity.compatible_ids, true)) return -1;
        vcam::unique_handle semaphore(CreateSemaphore(NULL, 0, MAXLONG, NULL));
        vcam::unique_handle removed(CreateEvent(NULL, FALSE, FALSE, NULL));
        if (!semaphore || !removed) return -1;
        request.hSemaphore = reinterpret_cast<ULONGLONG>(semaphore.get());
        request.hRemoveEvent = reinterpret_cast<ULONGLONG>(removed.get());
        request.BusAddress = device->address;
        DWORD bytes = 0;
        int address = 0;
        if (!device->ioctl(IOCTL_PDO_ADD, &request, sizeof(request), &address, sizeof(address), bytes) ||
            bytes < sizeof(address)) return -1;
        device->identity = std::move(identity);
        device->address = address;
        device->remove_pending = false;
        device->semaphore = std::move(semaphore);
        device->removed = std::move(removed);
        device->plugged = true;
        return 0;
    }
    catch (...) { return -1; }
}
int virt_usb_unplug(void* handle) {
    auto* device = static_cast<Device*>(handle);
    if (!device) return -1;
    if (!device->plugged) return 0;
    DWORD bytes = 0;
    if (!device->remove_pending) {
        if (!device->ioctl(IOCTL_PDO_REMOVE, NULL, 0, NULL, 0, bytes)) return -1;
        device->remove_pending = true;
    }
    HANDLE events[] = {device->removed.get(), device->stop.get()};
    const DWORD result = WaitForMultipleObjects(2, events, FALSE, 5000);
    if (result != WAIT_OBJECT_0) return -1;
    device->plugged = false;
    device->remove_pending = false;
    device->active = false;
    device->semaphore.reset();
    device->removed.reset();
    return 0;
}
int virt_usb_replug(void* handle) {
    auto* device = static_cast<Device*>(handle);
    if (!device || virt_usb_unplug(handle) < 0) return -1;
    const auto identity = device->identity;
    return virt_usb_plugin(handle, identity.device_id.c_str(), identity.hardware_ids.c_str(), identity.compatible_ids.c_str());
}
usbtx_header_t* virt_usb_begin(void* handle) {
    auto* device = static_cast<Device*>(handle);
    if (!device || !device->plugged || device->active) return nullptr;
    HANDLE events[] = {device->stop.get(), device->semaphore.get()};
    if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0+1) return nullptr;
    try {
        DWORD bytes = 0;
        bool complete = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::fill(device->buffer.begin(), device->buffer.end(), 0);
            complete = device->ioctl(IOCTL_BEGIN_TRANS_USB_DATA, NULL, 0, device->buffer.data(),
                static_cast<DWORD>(device->buffer.size()), bytes);
            if (complete) break;
            if (GetLastError() != ERROR_MORE_DATA || bytes != sizeof(ioctl_usbtx_header_t)) return nullptr;
            const auto* header = reinterpret_cast<const ioctl_usbtx_header_t*>(device->buffer.data());
            if (header->data_length < 0 || size_t(header->data_length) > maximum_payload) return nullptr;
            device->buffer.resize(sizeof(*header) + size_t(header->data_length));
        }
        if (!complete || bytes < sizeof(ioctl_usbtx_header_t) || bytes > device->buffer.size()) return nullptr;
        auto* raw = reinterpret_cast<ioctl_usbtx_header_t*>(device->buffer.data());
        if (raw->data_length < 0 || size_t(raw->data_length) > maximum_payload) return nullptr;
        const size_t declared = static_cast<size_t>(raw->data_length);
        const size_t received = bytes - sizeof(*raw);
        auto* request = reinterpret_cast<usbtx_header_t*>(device->buffer.data() + request_offset);
        if (!is_device_read(*request) && declared > received) return nullptr;
        if (request->type == 3 && request->transfer.type == 3) {
            if (received < ISO_PACKET_HDR_SIZE) return nullptr;
            const auto* iso = reinterpret_cast<const iso_packet_hdr*>(request->data);
            if (iso->number_packets > (received - ISO_PACKET_HDR_SIZE) / sizeof(iso_packet)) return nullptr;
        }
        if (device->buffer.size() < sizeof(*raw)+declared) device->buffer.resize(sizeof(*raw)+declared);
        request = reinterpret_cast<usbtx_header_t*>(device->buffer.data()+request_offset);
        request->data_length = static_cast<int>(declared);
        request->result = -1;
        device->payload_capacity = declared;
        device->active = true;
        return request;
    }
    catch (...) { return nullptr; }
}
int virt_usb_end(void* handle, usbtx_header_t* request) {
    auto* device = static_cast<Device*>(handle);
    if (!device || !device->active || request != reinterpret_cast<usbtx_header_t*>(device->buffer.data()+request_offset)) return -1;
    device->active = false;
    if (request->data_length < 0 || size_t(request->data_length) > device->payload_capacity) return -1;
    auto* raw = reinterpret_cast<ioctl_usbtx_header_t*>(device->buffer.data());
    raw->result = request->result;
    raw->data_length = request->data_length;
    DWORD bytes = 0;
    return device->ioctl(IOCTL_END_TRANS_USB_DATA, raw, static_cast<DWORD>(sizeof(*raw)+raw->data_length),
        NULL, 0, bytes) ? 0 : -1;
}

namespace vcam {
class VirtualTransport final : public UsbTransport {
    void* device_;
public:
    explicit VirtualTransport(void* device) : device_(device) {}
    ~VirtualTransport() override { virt_usb_close(device_); }
    bool plug(const DeviceIdentity& id) override { return virt_usb_plugin(device_, id.device_id.c_str(), id.hardware_ids.c_str(), id.compatible_ids.c_str()) == 0; }
    bool unplug() override { return virt_usb_unplug(device_) == 0; }
    bool replug() override { return virt_usb_replug(device_) == 0; }
    usbtx_header_t* begin() override { return virt_usb_begin(device_); }
    bool end(usbtx_header_t* value) override { return virt_usb_end(device_, value) == 0; }
    size_t capacity() const override { return virt_usb_capacity(device_); }
    void interrupt() noexcept override { virt_usb_interrupt(device_); }
    void resume() noexcept override { virt_usb_resume(device_); }
};
std::unique_ptr<UsbTransport> make_virtual_transport() {
    std::unique_ptr<void, decltype(&virt_usb_close)> device(virt_usb_open(), virt_usb_close);
    if (!device) return nullptr;
    auto transport = std::make_unique<VirtualTransport>(device.get());
    device.release();
    return transport;
}
}
