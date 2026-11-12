// Request layouts adapted from Fanxiushu's 2016 implementation.
// Keep them byte-for-byte compatible with src/usb/ioctl.h.
#pragma once
#include <cstddef>


#pragma pack(push, 1)
struct iso_packet {
    unsigned int offset;
    unsigned int length;
    unsigned int status;
};

struct iso_packet_hdr {
    unsigned int start_frame;
    unsigned int flags;
    unsigned int error_count;
    unsigned int number_packets;
    iso_packet packets[1];
};

#define ISO_PACKET_HDR_SIZE    (4*sizeof(unsigned int))

struct usbtx_header_t {
    int type;
    int result;
    int data_length;
    int reserved;
    union {
        struct { int type, subtype, is_read, index, value; } descriptor;
        struct { int type, subtype, is_read, request, index, value; } vendor;
        struct {
            int type, ep_address, is_read;
            union {
                struct { int number_packets, reserved0; char is_split, reserved[3]; };
                struct { unsigned char setup_packet[8]; unsigned int timeout; };
            };
        } transfer;
        struct { int type, ep_address; } reset;
        struct { int type, index; } status;
        struct { int type, subtype, index, value; } feature;
    };
    char data[0];
};
#pragma pack(pop)

void* virt_usb_open();
void virt_usb_close(void* handle);
int virt_usb_plugin(void* handle, const char* dev_id, const char* hw_ids, const char* comp_ids);
int virt_usb_unplug(void* handle);
int virt_usb_replug(void* handle);
usbtx_header_t* virt_usb_begin(void* handle);
int virt_usb_end(void* handle, usbtx_header_t* header);
void virt_usb_interrupt(void* handle);
void virt_usb_resume(void* handle);
size_t virt_usb_capacity(void* handle);
