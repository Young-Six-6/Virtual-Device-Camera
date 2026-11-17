#pragma once

/* A zero-length ISO packet can retain its original offset beyond the returned
 * payload, for example after a UVC frame ends partway through an URB. Every
 * packet still stays within the original transfer capacity. */
static __inline int xusb_iso_packet_fits_reply(
    unsigned long offset, unsigned long length,
    unsigned long returned_bytes, unsigned long capacity)
{
    if (returned_bytes > capacity || offset > capacity || length > capacity - offset)
        return 0;
    return length == 0 || (offset <= returned_bytes && length <= returned_bytes - offset);
}
