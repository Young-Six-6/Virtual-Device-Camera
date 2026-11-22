/// By fanxiushu 2016-04-06
// Frozen user/kernel ABI declarations; keep field order, width, packing, and IOCTL values unchanged.
#pragma once



DEFINE_GUID(GUID_XUSB_VIRT_INTERFACE,
	0xed3fb623, 0xc23, 0x48b8, 0x88, 0xa, 0x9d, 0x1a, 0x7, 0x26, 0x3f, 0xfd);


#define IOCTL_PDO_ADD \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_PDO_REMOVE \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Additive FOSS-only path for a virtual device on the SuperSpeed root hub.
// IOCTL_PDO_ADD remains byte-for-byte compatible with the original driver.
#define IOCTL_PDO_ADD_SUPERSPEED \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80c, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Additive FOSS-only path for USB 2.0 low/full-speed devices. The original
// IOCTL_PDO_ADD keeps its historical high-speed behavior and request layout.
#define IOCTL_PDO_ADD_WITH_USB_SPEED \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80d, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Values intentionally match USB_DEVICE_SPEED in the Windows USB DDI.
#define XUSB_USB_SPEED_LOW  0UL
#define XUSB_USB_SPEED_FULL 1UL


#define IOCTL_BEGIN_TRANS_USB_DATA \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_NEITHER, FILE_ANY_ACCESS)

#define IOCTL_END_TRANS_USB_DATA \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_NEITHER, FILE_ANY_ACCESS)

// Additive query used by the reconstructed driver; the original four IOCTLs remain unchanged.
#define IOCTL_XUSB_QUERY_DRIVER_INFO \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80a, METHOD_BUFFERED, FILE_READ_DATA)

// Additive UCX initialization diagnostics; the frozen legacy IOCTLs stay unchanged.
#define IOCTL_XUSB_QUERY_UCX_STATE \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80b, METHOD_BUFFERED, FILE_READ_DATA)

#define XUSB_DRIVER_INFO_SIGNATURE 0x53534f46UL // "FOSS"
#define XUSB_DRIVER_KIND_RECONSTRUCTED 1UL
#define XUSB_DRIVER_ABI_VERSION 1UL

#define XUSB_UCX_STATE_SIGNATURE 0x53554358UL // "XCUS"
#define XUSB_UCX_STATE_ABI_VERSION 1UL

#define XUSB_UCX_STAGE_DEVICE_INIT       0x00000001UL
#define XUSB_UCX_STAGE_WDF_DEVICE        0x00000002UL
#define XUSB_UCX_STAGE_VIRT_INTERFACE   0x00000004UL
#define XUSB_UCX_STAGE_HCD_INTERFACE    0x00000008UL
#define XUSB_UCX_STAGE_IO_QUEUE          0x00000010UL
#define XUSB_UCX_STAGE_CONTROLLER        0x00000020UL
#define XUSB_UCX_STAGE_ROOT_HUB          0x00000040UL

typedef struct xusb_driver_info_t {
	ULONG Signature;
	ULONG StructureSize;
	ULONG DriverKind;
	ULONG AbiVersion;
} xusb_driver_info_t;

typedef struct xusb_ucx_state_t {
	ULONG Signature;
	ULONG StructureSize;
	ULONG AbiVersion;
	ULONG StageFlags;
	LONG DeviceInitStatus;
	LONG WdfDeviceStatus;
	LONG VirtInterfaceStatus;
	LONG HcdInterfaceStatus;
	LONG IoQueueStatus;
	LONG ControllerStatus;
	LONG RootHubStatus;
	LONG PrepareHardwareStatus;
	ULONG PrepareHardwareCount;
	ULONG RootHubGetInfoCount;
	ULONG ControllerHandlePresent;
	ULONG RootHubHandlePresent;
    // Optional tail; the original 64-byte diagnostic response remains supported.
    ULONG RootHubGet20PortInfoCount;
    ULONG RootHubGet30PortInfoCount;
    ULONG RootHubControlCount;
    ULONG RootHubInterruptCount;
    LONG RootHubGetInfoStatus;
    LONG RootHubGet20PortInfoStatus;
    ULONG RootHubLastUrbFunction;
    ULONG RootHubLastSetup;
    ULONG RootHubLastValueIndex;
    ULONG RootHubLastUsbStatus;
    LONG RootHubGet30PortInfoStatus;
    ULONG RootHubGet30NumberOfPorts;
    ULONG RootHubGet30PortInfoSize;
    ULONG RootHubControl20Count;
    ULONG RootHubControl30Count;
    ULONG RootHubInterrupt20Count;
    ULONG RootHubInterrupt30Count;
    // Optional lifecycle diagnostics, appended without changing the legacy ABI.
    ULONG ReleaseHardwareCount;
    ULONG ReleaseHardwareCompletedCount;
    ULONG D0EntryCount;
    ULONG D0ExitCount;
    ULONG CurrentPowerState;
    ULONG Stopping;
    ULONG PendingTransactions;
    ULONG PendingRemovalWorkItems;
    ULONG UserQueueState;
	ULONG RootHubInvalidRequestCount;
	ULONG RootHubLastInvalidUrbFunction;
	ULONG RootHubLastInvalidSetup;
	ULONG RootHubLastInvalidValueIndex;
	ULONG RootHubLastInvalidSetupLength;
	ULONG RootHubLastInvalidTransferLength;
	ULONG RootHubLastInvalidUsbStatus;
	ULONG RootHubLastInvalidIsUsb30;
} xusb_ucx_state_t;


#define HW_IDS_COUNT    260

typedef struct ioctl_pdo_create_t
{

	ULONGLONG     hSemaphore;
	ULONGLONG     hRemoveEvent;
	LONG          BusAddress;


	WCHAR device_id[HW_IDS_COUNT];

	WCHAR hardware_ids[HW_IDS_COUNT];

	WCHAR compatible_ids[HW_IDS_COUNT];

} ioctl_pdo_create_t;

typedef struct ioctl_pdo_create_with_speed_t
{
	ioctl_pdo_create_t Create;
	ULONG UsbSpeed; // XUSB_USB_SPEED_LOW or XUSB_USB_SPEED_FULL
} ioctl_pdo_create_with_speed_t;

#pragma pack(1)


typedef struct iso_packet_t
{
	unsigned int offset;
	unsigned int length;
	unsigned int status;
} iso_packet_t;
typedef struct iso_packet_hdr_t
{
	unsigned int start_frame;
	unsigned int flags;
	unsigned int error_count;
	unsigned int number_packets;

	iso_packet_t packets[1];
} iso_packet_hdr_t;
#define ISO_PACKET_HDR_SIZE    (4*sizeof(unsigned int))

typedef struct ioctl_usbtx_header_t
{
	ULONGLONG        inter_handle;
	LONG             inter_seqno;
	LONG             data_length;
	LONG             result;
	LONG             reserved1;



	int        type;
	int        reserved[ 3 ];

	union{

		struct{
			int          type;
			int          subtype;
			int          is_read;
			int          index;
			int          value;

		}descriptor;

		struct{
			int          type;
			int          subtype;
			int          is_read;
			int          request;
			int          index;
			int          value;
		}vendor;

		struct {
			int           type;
			int           ep_address;
			int           is_read;
			union{
				struct {
					int           number_packets;
					int           reserved0;
					char          is_split;
					char          reserved[3];
				};
				struct{
					unsigned char setup_packet[8];
					unsigned int  timeout;
				};
			};

		}transfer;

		struct {
			int           type;
			int           ep_address;
		}reset;

		struct {
			int           type;
			int           index;
		}status;

		struct {
			int           type;
			int           subtype;
			int           index;
			int           value;
		}feature;

	};


} ioctl_usbtx_header_t;

#pragma pack()
