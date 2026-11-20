#pragma once

/* IsoPacket.Length is an output value for IN transfers, not an input capacity.
 * Callers derive packet_capacity from the endpoint limit and the packet's
 * slice of the transfer buffer. Zero-length packets may retain offsets beyond
 * the returned payload when a UVC frame ends partway through an URB. */
static __inline int xusb_iso_packet_fits_reply(
    unsigned long offset, unsigned long length, unsigned long packet_capacity,
    unsigned long returned_bytes, unsigned long capacity)
{
    if (length > packet_capacity || returned_bytes > capacity ||
        offset > capacity || length > capacity - offset)
        return 0;
    return length == 0 || (offset <= returned_bytes && length <= returned_bytes - offset);
}
