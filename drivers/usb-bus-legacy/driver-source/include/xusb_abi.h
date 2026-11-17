#pragma once

#include <ntddk.h>
#include "../../../../src/usb/ioctl.h"

C_ASSERT(IOCTL_PDO_ADD == 0x222004);
C_ASSERT(IOCTL_PDO_REMOVE == 0x222008);
C_ASSERT(IOCTL_BEGIN_TRANS_USB_DATA == 0x222023);
C_ASSERT(IOCTL_END_TRANS_USB_DATA == 0x222027);
C_ASSERT(FIELD_OFFSET(ioctl_usbtx_header_t, type) == 24);
C_ASSERT(sizeof(ioctl_usbtx_header_t) == 64);
C_ASSERT(sizeof(iso_packet_t) == 12);
C_ASSERT(FIELD_OFFSET(ioctl_pdo_create_t, device_id) == 20);
C_ASSERT(sizeof(ioctl_pdo_create_t) == 1584);
C_ASSERT(IOCTL_XUSB_QUERY_DRIVER_INFO == 0x226028);
C_ASSERT(sizeof(xusb_driver_info_t) == 16);
C_ASSERT(FIELD_OFFSET(xusb_ucx_state_t, RootHubGet20PortInfoCount) == 64);
C_ASSERT(FIELD_OFFSET(xusb_ucx_state_t, RootHubGet30PortInfoStatus) == 104);
C_ASSERT(FIELD_OFFSET(xusb_ucx_state_t, ReleaseHardwareCount) == 132);
C_ASSERT(sizeof(xusb_ucx_state_t) == 168);
C_ASSERT(IOCTL_XUSB_QUERY_UCX_STATE == 0x22602c);
