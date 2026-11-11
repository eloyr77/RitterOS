/*
 * Minimal UEFI definitions for the RitterOS bootloader.
 * Layouts follow the UEFI Specification 2.10. Only what we use is typed;
 * other table slots are kept as opaque pointers so the offsets stay correct.
 */
#ifndef RITTEROS_EFI_H
#define RITTEROS_EFI_H

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef UINT64             UINTN;
typedef UINT16             CHAR16;
typedef UINT8              BOOLEAN;
typedef void              *EFI_HANDLE;
typedef void              *EFI_EVENT;
typedef UINTN              EFI_STATUS;
typedef int                INT32;
typedef UINT64             EFI_PHYSICAL_ADDRESS;
typedef UINT64             EFI_VIRTUAL_ADDRESS;
typedef short              INT16;

/* UEFI uses the Microsoft calling convention on x86-64 and the standard one
 * (AAPCS64) on ARM. */
#if defined(__x86_64__)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

#define EFI_SUCCESS 0
#define EFI_ERROR_BIT (1ULL << 63)
#define EFI_ERROR(s) (((s) & EFI_ERROR_BIT) != 0)
#define EFI_LOAD_ERROR       (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED      (EFI_ERROR_BIT | 3)
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5)
#define EFI_OUT_OF_RESOURCES (EFI_ERROR_BIT | 9)
#define EFI_VOLUME_CORRUPTED (EFI_ERROR_BIT | 10)
#define EFI_NOT_FOUND        (EFI_ERROR_BIT | 14)
#define EFI_CRC_ERROR        (EFI_ERROR_BIT | 27)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* --- Console input -------------------------------------------------------- */

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self, EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
};

/* --- Console output ------------------------------------------------------- */

typedef struct {
    INT32 MaxMode;          /* number of valid text modes */
    INT32 Mode;             /* current mode */
    INT32 Attribute;
    INT32 CursorColumn;
    INT32 CursorRow;
    BOOLEAN CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, const CHAR16 *String);
    void *TestString;
    EFI_STATUS (EFIAPI *QueryMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, UINTN ModeNumber,
                                   UINTN *Columns, UINTN *Rows);
    EFI_STATUS (EFIAPI *SetMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, UINTN ModeNumber);
    EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, UINTN Attribute);
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self);
    void *SetCursorPosition;
    void *EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE *Mode;
};

#define EFI_BLACK        0x00
#define EFI_LIGHTGRAY    0x07
#define EFI_LIGHTGREEN   0x0A
#define EFI_LIGHTCYAN    0x0B
#define EFI_YELLOW       0x0E
#define EFI_WHITE        0x0F
#define EFI_BACKGROUND_BLACK     0x00
#define EFI_BACKGROUND_LIGHTGRAY 0x70

/* --- Memory map ------------------------------------------------------------ */

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

#define EFI_PAGE_SIZE 4096

/* Descriptor attribute: the region is used by runtime services and must be
 * given a virtual address by SetVirtualAddressMap. */
#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress
} EFI_ALLOCATE_TYPE;

/*
 * One entry of the memory map. The firmware may use a larger stride than
 * sizeof(EFI_MEMORY_DESCRIPTOR); always step by the DescriptorSize it returns.
 */
typedef struct {
    UINT32 Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* --- Runtime services ----------------------------------------------------- */

typedef struct {
    UINT16 Year;           /* 1900 - 9999 */
    UINT8  Month;          /* 1 - 12 */
    UINT8  Day;            /* 1 - 31 */
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;       /* minutes from UTC, or 0x07FF if unspecified */
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

typedef enum {
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown,
    EfiResetPlatformSpecific
} EFI_RESET_TYPE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (EFIAPI *GetTime)(EFI_TIME *Time, void *Capabilities);
    void *SetTime;
    void *GetWakeupTime;
    void *SetWakeupTime;
    EFI_STATUS (EFIAPI *SetVirtualAddressMap)(UINTN MemoryMapSize, UINTN DescriptorSize,
                                              UINT32 DescriptorVersion,
                                              EFI_MEMORY_DESCRIPTOR *VirtualMap);
    void *ConvertPointer;
    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid,
                                     UINT32 *Attributes, UINTN *DataSize, void *Data);
    void *GetNextVariableName;
    EFI_STATUS (EFIAPI *SetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid,
                                     UINT32 Attributes, UINTN DataSize, void *Data);
    void *GetNextHighMonotonicCount;
    void (EFIAPI *ResetSystem)(EFI_RESET_TYPE ResetType, EFI_STATUS ResetStatus,
                               UINTN DataSize, void *ResetData);
} EFI_RUNTIME_SERVICES;

/* --- Boot services -------------------------------------------------------- */

typedef enum {
    AllHandles,
    ByRegisterNotify,
    ByProtocol                 /* every handle that supports a protocol */
} EFI_LOCATE_SEARCH_TYPE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                                       UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      UINTN *MapKey, UINTN *DescriptorSize,
                                      UINT32 *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);
    void *CreateEvent;
    void *SetTimer;
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode,
                                          UINTN DataSize, CHAR16 *WatchdogData);
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Protocol,
                                            void *SearchKey, UINTN *NoHandles,
                                            EFI_HANDLE **Buffer);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
} EFI_BOOT_SERVICES;

/* --- Loaded image: tells us which device we were loaded from ------------- */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;       /* the disk/partition this image came from */
    void *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* --- File access ---------------------------------------------------------- */

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_FILE_POSITION_END 0xFFFFFFFFFFFFFFFFULL   /* SetPosition: seek to end */

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *self, EFI_FILE_PROTOCOL **NewHandle,
                              const CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *self);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *self, UINTN *BufferSize, void *Buffer);
    void *Write;
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *self, UINT64 *Position);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *self, UINT64 Position);
    void *GetInfo;
    void *SetInfo;
    void *Flush;
};

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,
                                    EFI_FILE_PROTOCOL **Root);
};

/* --- Disks and partitions ---------------------------------------------------
 * The firmware creates one handle for each disk and one for each partition it
 * finds; each has Block I/O (whole blocks) and Disk I/O (any byte range). */

#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT32 MediaId;            /* changes when the medium changes */
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;  /* a partition, not the whole disk */
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT32 BlockSize;
    UINT32 IoAlign;
    UINT64 LastBlock;          /* number of the last block: size = (LastBlock + 1) blocks */
} EFI_BLOCK_IO_MEDIA;

typedef struct {
    UINT64 Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    void *Reset;
    void *ReadBlocks;
    void *WriteBlocks;
    void *FlushBlocks;
} EFI_BLOCK_IO_PROTOCOL;

#define EFI_DISK_IO_PROTOCOL_GUID \
    { 0xce345171, 0xba0b, 0x11d2, { 0x8e, 0x4f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct EFI_DISK_IO_PROTOCOL EFI_DISK_IO_PROTOCOL;
struct EFI_DISK_IO_PROTOCOL {
    UINT64 Revision;
    /* Offset in bytes from the start of the disk or partition. */
    EFI_STATUS (EFIAPI *ReadDisk)(EFI_DISK_IO_PROTOCOL *self, UINT32 MediaId, UINT64 Offset,
                                  UINTN BufferSize, void *Buffer);
    void *WriteDisk;
};

/* The partition table entry a partition handle came from (UEFI 2.7). */
#define EFI_PARTITION_INFO_PROTOCOL_GUID \
    { 0x8cf2f62c, 0xbc9b, 0x4821, { 0x80, 0x8d, 0xec, 0x9e, 0xc4, 0x21, 0xa1, 0xa0 } }

#define PARTITION_TYPE_GPT 2

typedef struct {
    EFI_GUID PartitionTypeGUID;
    EFI_GUID UniquePartitionGUID;
    UINT64 StartingLBA;
    UINT64 EndingLBA;
    UINT64 Attributes;
    CHAR16 PartitionName[36];
} EFI_PARTITION_ENTRY;

typedef struct {
    UINT32 Revision;
    UINT32 Type;               /* PARTITION_TYPE_GPT for a GPT partition */
    UINT8 System;              /* 1: an EFI system partition */
    UINT8 Reserved[7];
    union {
        UINT8 Mbr[16];
        EFI_PARTITION_ENTRY Gpt;
    } Info;
} __attribute__((packed)) EFI_PARTITION_INFO_PROTOCOL;

/* --- Graphics Output Protocol (GOP) --------------------------------------- */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

/* (65e-34) The EDID of a video output (UEFI 2.x, 12.9): the active one (what
 * the output uses), or the one discovered from the display; on the GOP's
 * handle. */
#define EFI_EDID_ACTIVE_PROTOCOL_GUID \
    { 0xbd8c1056, 0x9f36, 0x44ec, { 0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86 } }
#define EFI_EDID_DISCOVERED_PROTOCOL_GUID \
    { 0x1c0c34f6, 0xd380, 0x41fa, { 0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa } }
typedef struct {
    UINT32 SizeOfEdid;
    UINT8 *Edid;
} EFI_EDID_PROTOCOL;

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,   /* bytes in memory: R, G, B, x */
    PixelBlueGreenRedReserved8BitPerColor,   /* bytes in memory: B, G, R, x */
    PixelBitMask,                            /* layout given by PixelInformation */
    PixelBltOnly                             /* no directly accessible framebuffer */
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;                /* row stride, may exceed the width */
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *self, UINT32 ModeNumber,
                                   UINTN *SizeOfInfo,
                                   EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (EFIAPI *SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *self, UINT32 ModeNumber);
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* --- Configuration tables: firmware data structures found by GUID --------- */

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }
#define EFI_SMBIOS3_TABLE_GUID \
    { 0xf2fd1544, 0x9794, 0x4a2c, { 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94 } }
#define EFI_SMBIOS_TABLE_GUID \
    { 0xeb9d2d31, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

/* --- Device paths: where a device or partition sits in the machine --------- */

#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* A device path is a list of variable-length nodes, ended by type 0x7F/0xFF. */
typedef struct {
    UINT8 Type;
    UINT8 SubType;
    UINT8 Length[2];       /* little-endian, includes this header */
} EFI_DEVICE_PATH_PROTOCOL;

#define DEVICE_PATH_END_TYPE 0x7F
#define DEVICE_PATH_END_SUBTYPE 0xFF
#define DEVICE_PATH_MEDIA_TYPE 0x04
#define DEVICE_PATH_HARD_DRIVE_SUBTYPE 0x01    /* a partition: UINT32 number at offset 4 */

/* --- Random numbers --------------------------------------------------------- */

#define EFI_RNG_PROTOCOL_GUID \
    { 0x3152bca5, 0xeade, 0x433d, { 0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44 } }

typedef struct EFI_RNG_PROTOCOL EFI_RNG_PROTOCOL;
struct EFI_RNG_PROTOCOL {
    void *GetInfo;
    /* Algorithm 0 (NULL) means the firmware's default algorithm. */
    EFI_STATUS (EFIAPI *GetRNG)(EFI_RNG_PROTOCOL *self, EFI_GUID *Algorithm,
                                UINTN Length, UINT8 *Value);
};

/* --- System table --------------------------------------------------------- */

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif
