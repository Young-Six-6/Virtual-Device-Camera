#pragma once
#include "virt_dev.h"
#include <memory>
#include <string>

namespace vcam {
struct DeviceIdentity { std::string device_id, hardware_ids, compatible_ids; };
enum class UsbEndResult { completed, canceled, failed };
class UsbTransport {
public:
    virtual ~UsbTransport() = default;
    virtual bool plug(const DeviceIdentity&) = 0;
    virtual bool unplug() = 0;
    virtual bool replug() = 0;
    // begin() lends a transaction buffer until end(); callers must treat its
    // pointer as invalid as soon as end() returns.
    virtual usbtx_header_t* begin() = 0;
    virtual UsbEndResult end(usbtx_header_t*) = 0;
    virtual size_t capacity() const = 0;
    virtual bool is_superspeed() const { return false; }
    virtual VirtUsbDriverKind driver_kind() const { return VirtUsbDriverKind::unknown; }
    // True once when the device was removed outside this transport's unplug path.
    virtual bool consume_removed() noexcept { return false; }
    virtual void interrupt() noexcept = 0;
    virtual void resume() noexcept = 0;
};
std::unique_ptr<UsbTransport> make_virtual_transport();
}
