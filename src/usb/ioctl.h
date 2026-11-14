/// By fanxiushu 2016-04-06
// Frozen user/kernel ABI declarations; keep field order, width, packing, and IOCTL values unchanged.
#pragma once



DEFINE_GUID(GUID_XUSB_VIRT_INTERFACE,
	0xed3fb623, 0xc23, 0x48b8, 0x88, 0xa, 0x9d, 0x1a, 0x7, 0x26, 0x3f, 0xfd);


#define IOCTL_PDO_ADD \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_PDO_REMOVE \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)


#define IOCTL_BEGIN_TRANS_USB_DATA \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_NEITHER, FILE_ANY_ACCESS)

#define IOCTL_END_TRANS_USB_DATA \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_NEITHER, FILE_ANY_ACCESS)


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
