#pragma once
#include "virt_dev.h"
#include <memory>
#include <string>

namespace vcam {
struct DeviceIdentity { std::string device_id, hardware_ids, compatible_ids; };
class UsbTransport {
public:
    virtual ~UsbTransport() = default;
    virtual bool plug(const DeviceIdentity&) = 0;
    virtual bool unplug() = 0;
    virtual bool replug() = 0;
    virtual usbtx_header_t* begin() = 0;
    virtual bool end(usbtx_header_t*) = 0;
    virtual size_t capacity() const = 0;
    virtual void interrupt() noexcept = 0;
    virtual void resume() noexcept = 0;
};
std::unique_ptr<UsbTransport> make_virtual_transport();
}
