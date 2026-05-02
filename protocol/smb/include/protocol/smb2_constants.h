#ifndef __NET_SMB_PROTOCOL_SMB2_CONSTANTS_H__
#define __NET_SMB_PROTOCOL_SMB2_CONSTANTS_H__

#include <cstdint>

namespace yuan::net::smb
{
    constexpr uint32_t SMB2_PROTOCOL_ID = 0xFE534D42;
    constexpr uint32_t SMB1_PROTOCOL_ID = 0xFF534D42;
    constexpr uint32_t SMB2_TRANSFORM_PROTOCOL_ID = 0xFD534D42;
    constexpr uint16_t SMB2_HEADER_SIZE = 64;
    constexpr uint16_t SMB2_TRANSFORM_HEADER_SIZE = 52;

    enum class Smb2Command : uint16_t {
        NEGOTIATE = 0x0000,
        SESSION_SETUP = 0x0001,
        LOGOFF = 0x0002,
        TREE_CONNECT = 0x0003,
        TREE_DISCONNECT = 0x0004,
        CREATE = 0x0005,
        CLOSE = 0x0006,
        FLUSH = 0x0007,
        READ = 0x0008,
        WRITE = 0x0009,
        LOCK = 0x000A,
        IOCTL = 0x000B,
        CANCEL = 0x000C,
        ECHO = 0x000D,
        QUERY_DIRECTORY = 0x000E,
        CHANGE_NOTIFY = 0x000F,
        QUERY_INFO = 0x0010,
        SET_INFO = 0x0011,
        OPLOCK_BREAK = 0x0012
    };

    enum class DialectRevision : uint16_t {
        SMB_2_002 = 0x0202,
        SMB_2_1 = 0x0210,
        SMB_3_0 = 0x0300,
        SMB_3_0_2 = 0x0302,
        SMB_3_1_1 = 0x0311
    };

    enum class NtStatus : uint32_t {
        SUCCESS = 0x00000000,
        PENDING = 0x00000103,
        UNSUCCESSFUL = 0xC0000001,
        INVALID_PARAMETER = 0xC000000D,
        INVALID_DEVICE_REQUEST = 0xC0000010,
        EOF_REACHED = 0xC0000011,
        MORE_PROCESSING_REQUIRED = 0xC0000016,
        INVALID_HANDLE = 0xC0000008,
        ACCESS_DENIED = 0xC0000022,
        OBJECT_NAME_NOT_FOUND = 0xC0000034,
        OBJECT_NAME_COLLISION = 0xC0000035,
        OBJECT_PATH_NOT_FOUND = 0xC000003A,
        NOT_SUPPORTED = 0xC00000BB,
        SHARING_VIOLATION = 0xC0000043,
        FILE_LOCK_CONFLICT = 0xC0000054,
        LOCK_NOT_GRANTED = 0xC0000055,
        INSUFFICIENT_RESOURCES = 0xC000009A,
        LOGON_FAILURE = 0xC000006D,
        BAD_IMPERSONATION_LEVEL = 0xC00000A5,
        NOT_IMPLEMENTED = 0xC0000002,
        CANCELLED = 0xC0000120,
        RANGE_NOT_LOCKED = 0xC000007E,
        NOT_FOUND = 0xC0000225,
        NO_MORE_FILES = 0x80000006,
        NETWORK_NAME_DELETED = 0xC00000C9,
        USER_SESSION_DELETED = 0xC0000203,
        STATUS_NOTIFY_CLEANUP = 0x0000010B,
        NOTIFY_ENUM_DIR = 0x0000010C,
        INTERNAL_ERROR = 0xC00000E5,
        BUFFER_OVERFLOW = 0x80000005,
        INVALID_BUFFER_SIZE = 0xC0000206,
        DRIVER_FAILED_SLEEP = 0xC00002C2,
        NOT_A_DIRECTORY = 0xC0000103,
        DIRECTORY_NOT_EMPTY = 0xC0000101,
        CANNOT_DELETE = 0xC0000121,
        UNSUPPORTED_TYPE = 0xC00000BD
    };

    constexpr uint32_t SMB2_FLAGS_SERVER_TO_REDIR = 0x00000001;
    constexpr uint32_t SMB2_FLAGS_ASYNC_COMMAND = 0x00000002;
    constexpr uint32_t SMB2_FLAGS_RELATED_OPERATIONS = 0x00000004;
    constexpr uint32_t SMB2_FLAGS_SIGNED = 0x00000008;
    constexpr uint32_t SMB2_FLAGS_PRIORITY_MASK = 0x00000070;
    constexpr uint32_t SMB2_FLAGS_DFS_OPERATIONS = 0x10000000;
    constexpr uint32_t SMB2_FLAGS_REPLAY_OPERATION = 0x20000000;

    constexpr uint32_t SMB2_GLOBAL_CAP_ENCRYPTION = 0x00000001;
    constexpr uint32_t SMB2_GLOBAL_CAP_LEASING = 0x00000002;
    constexpr uint32_t SMB2_GLOBAL_CAP_LARGE_MTU = 0x00000004;
    constexpr uint32_t SMB2_GLOBAL_CAP_MULTI_CHANNEL = 0x00000008;
    constexpr uint32_t SMB2_GLOBAL_CAP_PERSISTENT_HANDLES = 0x00000010;
    constexpr uint32_t SMB2_GLOBAL_CAP_DIRECTORY_LEASING = 0x00000020;

    constexpr uint32_t SMB2_NEGOTIATE_CAP_ENCRYPTION = 0x00000001;
    constexpr uint32_t SMB2_NEGOTIATE_CAP_LEASING = 0x00000002;
    constexpr uint32_t SMB2_NEGOTIATE_CAP_LARGE_MTU = 0x00000004;
    constexpr uint32_t SMB2_NEGOTIATE_CAP_MULTI_CHANNEL = 0x00000008;
    constexpr uint32_t SMB2_NEGOTIATE_CAP_PERSISTENT_HANDLES = 0x00000010;
    constexpr uint32_t SMB2_NEGOTIATE_CAP_DIRECTORY_LEASING = 0x00000020;

    constexpr uint16_t SMB2_SESSION_FLAG_BINDING = 0x0001;
    constexpr uint16_t SMB2_SESSION_FLAG_IS_GUEST = 0x0001;
    constexpr uint16_t SMB2_SESSION_FLAG_IS_NULL = 0x0002;
    constexpr uint16_t SMB2_SESSION_FLAG_ENCRYPT_DATA = 0x0004;

    constexpr uint32_t SMB2_SHAREFLAG_DFS = 0x00000001;
    constexpr uint32_t SMB2_SHAREFLAG_DFS_ROOT = 0x00000002;
    constexpr uint32_t SMB2_SHAREFLAG_RESTRICT_EXCLUSIVE_OPENS = 0x00000100;
    constexpr uint32_t SMB2_SHAREFLAG_FORCE_SHARED_DELETE = 0x00000200;
    constexpr uint32_t SMB2_SHAREFLAG_ALLOW_NAMESPACE_CACHING = 0x00000400;
    constexpr uint32_t SMB2_SHAREFLAG_ACCESS_BASED_DIRECTORY_ENUM = 0x00000800;
    constexpr uint32_t SMB2_SHAREFLAG_FORCE_LEVELII_OPLOCK = 0x00001000;
    constexpr uint32_t SMB2_SHAREFLAG_ENABLE_HASH_V1 = 0x00002000;
    constexpr uint32_t SMB2_SHAREFLAG_ENABLE_HASH_V2 = 0x00004000;
    constexpr uint32_t SMB2_SHAREFLAG_ENCRYPT_DATA = 0x00008000;

    enum class ShareType : uint8_t {
        DISK = 0x01,
        PIPE = 0x02,
        PRINT = 0x03
    };

    constexpr uint32_t SMB_FILE_SUPERSEDE = 0x00000000;
    constexpr uint32_t SMB_FILE_OPEN = 0x00000001;
    constexpr uint32_t SMB_FILE_CREATE = 0x00000002;
    constexpr uint32_t SMB_FILE_OPEN_IF = 0x00000003;
    constexpr uint32_t SMB_FILE_OVERWRITE = 0x00000004;
    constexpr uint32_t SMB_FILE_OVERWRITE_IF = 0x00000005;

    constexpr uint32_t SMB_FILE_SUPERSEDED = 0x00000000;
    constexpr uint32_t SMB_FILE_OPENED = 0x00000001;
    constexpr uint32_t SMB_FILE_CREATED = 0x00000002;
    constexpr uint32_t SMB_FILE_OVERWRITTEN = 0x00000003;

    constexpr uint32_t SMB_GENERIC_READ = 0x80000000;
    constexpr uint32_t SMB_GENERIC_WRITE = 0x40000000;

    constexpr uint32_t SMB_FILE_DIRECTORY_FILE = 0x00000001;
    constexpr uint32_t SMB_FILE_NON_DIRECTORY_FILE = 0x00000040;
    constexpr uint32_t SMB_FILE_WRITE_THROUGH = 0x00000002;
    constexpr uint32_t SMB_FILE_SEQUENTIAL_ONLY = 0x00000004;
    constexpr uint32_t SMB_FILE_RANDOM_ACCESS = 0x00000800;
    constexpr uint32_t SMB_FILE_DELETE_ON_CLOSE = 0x00001000;

    constexpr uint32_t SMB_FILE_READ_DATA = 0x00000001;
    constexpr uint32_t SMB_FILE_WRITE_DATA = 0x00000002;
    constexpr uint32_t SMB_FILE_APPEND_DATA = 0x00000004;
    constexpr uint32_t SMB_FILE_EXECUTE = 0x00000020;
    constexpr uint32_t SMB_FILE_READ_ATTRIBUTES = 0x00000080;
    constexpr uint32_t SMB_FILE_WRITE_ATTRIBUTES = 0x00000100;
    constexpr uint32_t SMB_DELETE = 0x00010000;
    constexpr uint32_t SMB_READ_CONTROL = 0x00020000;
    constexpr uint32_t SMB_WRITE_DAC = 0x00040000;
    constexpr uint32_t SMB_SYNCHRONIZE = 0x00100000;
    constexpr uint32_t SMB_FILE_GENERIC_READ = 0x00120089;
    constexpr uint32_t SMB_FILE_GENERIC_WRITE = 0x00120116;
    constexpr uint32_t SMB_FILE_GENERIC_EXECUTE = 0x001200A0;
    constexpr uint32_t SMB_FILE_ALL_ACCESS = 0x001F01FF;
    constexpr uint32_t SMB_MAXIMUM_ALLOWED = 0x02000000;

    constexpr uint32_t SMB_FILE_SHARE_READ = 0x00000001;
    constexpr uint32_t SMB_FILE_SHARE_WRITE = 0x00000002;
    constexpr uint32_t SMB_FILE_SHARE_DELETE = 0x00000004;

    constexpr uint32_t SMB_FILE_ATTRIBUTE_READONLY = 0x00000001;
    constexpr uint32_t SMB_FILE_ATTRIBUTE_HIDDEN = 0x00000002;
    constexpr uint32_t SMB_FILE_ATTRIBUTE_SYSTEM = 0x00000004;
    constexpr uint32_t SMB_FILE_ATTRIBUTE_DIRECTORY = 0x00000010;
    constexpr uint32_t SMB_FILE_ATTRIBUTE_ARCHIVE = 0x00000020;
    constexpr uint32_t SMB_FILE_ATTRIBUTE_NORMAL = 0x00000080;
    constexpr uint32_t SMB_FILE_ATTRIBUTE_TEMPORARY = 0x00000100;
    constexpr uint32_t SMB_FILE_ATTRIBUTE_SPARSE_FILE = 0x00000200;
    constexpr uint32_t SMB_FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400;

    constexpr uint8_t SMB2_OPLOCK_LEVEL_NONE = 0x00;
    constexpr uint8_t SMB2_OPLOCK_LEVEL_II = 0x01;
    constexpr uint8_t SMB2_OPLOCK_LEVEL_EXCLUSIVE = 0x08;
    constexpr uint8_t SMB2_OPLOCK_LEVEL_BATCH = 0x09;

    constexpr uint32_t SMB2_LEASE_READ_CACHING = 0x00000001;
    constexpr uint32_t SMB2_LEASE_HANDLE_CACHING = 0x00000002;
    constexpr uint32_t SMB2_LEASE_WRITE_CACHING = 0x00000004;

    constexpr uint32_t FILE_READ_DATA_FLAG = 0x01;
    constexpr uint32_t FILE_WRITE_DATA_FLAG = 0x02;

    constexpr uint8_t SMB2_0_INFO_FILE = 0x01;
    constexpr uint8_t SMB2_0_INFO_FILESYSTEM = 0x02;
    constexpr uint8_t SMB2_0_INFO_SECURITY = 0x03;
    constexpr uint8_t SMB2_0_INFO_QUOTA = 0x04;

    constexpr uint32_t SMB_FILE_CASE_SENSITIVE_SEARCH = 0x00000001;
    constexpr uint32_t SMB_FILE_CASE_PRESERVED_NAMES = 0x00000002;
    constexpr uint32_t SMB_FILE_UNICODE_ON_DISK = 0x00000004;
    constexpr uint32_t SMB_FILE_PERSISTENT_ACLS = 0x00000008;
    constexpr uint32_t SMB_FILE_SUPPORTS_SPARSE_FILES = 0x00000040;
    constexpr uint32_t SMB_FILE_SUPPORTS_REPARSE_POINTS = 0x00000080;
    constexpr uint32_t SMB_FILE_SUPPORTS_EXTENDED_ATTRIBUTES = 0x00800000;

    constexpr uint32_t SMB_FILE_DEVICE_DISK = 0x00000007;
    constexpr uint32_t SMB_FILE_DEVICE_NAMED_PIPE = 0x00000011;
    constexpr uint32_t FILE_DEVICE_SECURE_OPEN = 0x00000100;

    constexpr uint32_t SL_RESTART_SCAN = 0x00000001;
    constexpr uint32_t SL_RETURN_SINGLE_ENTRY = 0x00000002;
    constexpr uint32_t SL_INDEX_SPECIFIED = 0x00000004;

    enum class FileInfoClass : uint8_t {
        FileDirectoryInformation = 1,
        FileFullDirectoryInformation = 2,
        FileBothDirectoryInformation = 3,
        FileBasicInformation = 4,
        FileStandardInformation = 5,
        FileInternalInformation = 6,
        FileEaInformation = 7,
        FileAccessInformation = 8,
        FileNameInformation = 9,
        FileRenameInformation = 10,
        FileLinkInformation = 11,
        FileDispositionInformation = 13,
        FilePositionInformation = 14,
        FileModeInformation = 16,
        FileAlignmentInformation = 17,
        FileAllInformation = 18,
        FileAllocationInformation = 19,
        FileEndOfFileInformation = 20,
        FileIdBothDirectoryInformation = 37,
        FileIdFullDirectoryInformation = 38,
        FileValidDataLengthInformation = 39,
        FileShortNameInformation = 40,
        FileSystemInformation = 11,
        FileFsAttributeInformation = 5,
        FileFsSizeInformation = 3,
        FileFsDeviceInformation = 4,
        FileFsVolumeInformation = 1,
        FileFsFullSizeInformation = 7,
        FileFsSectorSizeInformation = 11
    };

    enum class SecurityMode : uint16_t {
        NEGOTIATE_SIGNING_ENABLED = 0x0001,
        NEGOTIATE_SIGNING_REQUIRED = 0x0002
    };

    enum class EncryptionAlgorithm : uint16_t {
        NONE = 0x0000,
        AES_128_CCM = 0x0001,
        AES_128_GCM = 0x0002
    };

    constexpr uint32_t SMB2_PREAUTH_INTEGRITY_CAP_SHA_512 = 0x00000001;
    constexpr uint32_t SMB2_ENCRYPTION_CAP_AES_128_CCM = 0x00000001;
    constexpr uint32_t SMB2_ENCRYPTION_CAP_AES_128_GCM = 0x00000002;

    constexpr uint32_t HASH_SIZE_SHA_512 = 64;
    constexpr uint32_t SMB2_SIGNATURE_SIZE = 16;
    constexpr uint32_t SMB2_SESSION_ID_SIZE = 8;
    constexpr uint32_t SMB2_LEASE_KEY_SIZE = 16;
    constexpr uint32_t SMB2_FILE_ID_SIZE = 16;

    constexpr uint16_t SMB2_CREDIT_GRANT_INITIAL = 1;
    constexpr uint16_t SMB2_CREDIT_GRANT_LARGE_MTU = 128;
    constexpr uint32_t SMB2_MAX_BUFFER_SIZE = 1024 * 1024;

    constexpr uint32_t SMB2_CREATE_CONTEXT_TYPE_RQ = 0x51714E54;
    constexpr uint32_t SMB2_CREATE_CONTEXT_TYPE_MQ_AC = 0x434D516D;
    constexpr uint32_t SMB2_CREATE_CONTEXT_TYPE_EA = 0x45787441;
    constexpr uint32_t SMB2_CREATE_CONTEXT_TYPE_SD = 0x44536353;
    constexpr uint32_t SMB2_CREATE_CONTEXT_TYPE_DH_QC = 0x44514843;
    constexpr uint32_t SMB2_CREATE_CONTEXT_TYPE_DH_NC = 0x44484E43;
    constexpr uint32_t SMB2_CREATE_CONTEXT_TYPE_AL = 0x416C6C6F;
    constexpr uint32_t SMB2_CREATE_CONTEXT_TYPE_MN = 0x4D6E4472;
    constexpr uint32_t SMB2_CREATE_CONTEXT_TYPE_TW = 0x54576872;

    constexpr uint32_t FSCTL_DFS_GET_REFERRALS = 0x00060194;
    constexpr uint32_t FSCTL_DFS_GET_REFERRALS_EX = 0x000601B4;
    constexpr uint32_t FSCTL_PIPE_PEEK = 0x0011400C;
    constexpr uint32_t FSCTL_PIPE_TRANSCEIVE = 0x0011C017;
    constexpr uint32_t FSCTL_PIPE_WAIT = 0x00110018;
    constexpr uint32_t FSCTL_SRV_ENUMERATE_SNAPSHOTS = 0x00144064;
    constexpr uint32_t FSCTL_SRV_REQUEST_RESUME_KEY = 0x00140078;
    constexpr uint32_t FSCTL_SRV_COPYCHUNK = 0x001440F2;
    constexpr uint32_t FSCTL_SRV_COPYCHUNK_WRITE = 0x001480F2;
    constexpr uint32_t FSCTL_SRV_READ_HASH = 0x001441BB;
    constexpr uint32_t FSCTL_LMR_REQUEST_RESILIENCY = 0x001401D4;
    constexpr uint32_t FSCTL_QUERY_NETWORK_INTERFACE_INFO = 0x001401FC;
    constexpr uint32_t FSCTL_VALIDATE_NEGOTIATE_INFO = 0x00140204;
    constexpr uint32_t SMB_FSCTL_SET_REPARSE_POINT = 0x000900A4;
    constexpr uint32_t SMB_FSCTL_GET_REPARSE_POINT = 0x000900A8;
    constexpr uint32_t SMB_FSCTL_DELETE_REPARSE_POINT = 0x000900AC;

    constexpr uint32_t SMB2_NETWORK_INTERFACE_RSS_CAPABLE = 0x00000001;
    constexpr uint32_t SMB2_NETWORK_INTERFACE_RDMA_CAPABLE = 0x00000002;

    constexpr uint16_t SMB2_SIGNING_HMAC_SHA256 = 0x0000;
    constexpr uint16_t SMB2_SIGNING_AES128_CMAC = 0x0001;
    constexpr uint16_t SMB2_SIGNING_AES128_GMAC = 0x0002;

    constexpr uint16_t SMB2_NEGOTIATE_CTX_PREAUTH_INTEGRITY = 0x0001;
    constexpr uint16_t SMB2_NEGOTIATE_CTX_ENCRYPTION = 0x0002;
    constexpr uint16_t SMB2_NEGOTIATE_CTX_COMPRESSION = 0x0003;
    constexpr uint16_t SMB2_NEGOTIATE_CTX_SIGNING = 0x0008;

    constexpr uint32_t NTLMSSP_NEGOTIATE_56 = 0x80000000;
    constexpr uint32_t NTLMSSP_NEGOTIATE_128 = 0x20000000;
    constexpr uint32_t NTLMSSP_NEGOTIATE_KEY_EXCH = 0x40000000;
    constexpr uint32_t NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY = 0x00080000;
    constexpr uint32_t NTLMSSP_TARGET_TYPE_SERVER = 0x00020000;
    constexpr uint32_t NTLMSSP_NEGOTIATE_NTLM = 0x00000200;
    constexpr uint32_t NTLMSSP_NEGOTIATE_ALWAYS_SIGN = 0x00008000;
    constexpr uint32_t NTLMSSP_NEGOTIATE_UNICODE = 0x00000001;
    constexpr uint32_t NTLMSSP_NEGOTIATE_SIGN = 0x00000010;
    constexpr uint32_t NTLMSSP_REQUEST_TARGET = 0x00000004;
    constexpr uint32_t NTLMSSP_NEGOTIATE_TARGET_INFO = 0x00800000;
    constexpr uint32_t NTLMSSP_NEGOTIATE_VERSION = 0x02000000;

    constexpr uint8_t NTLMSSP_MESSAGE_TYPE_NEGOTIATE = 1;
    constexpr uint8_t NTLMSSP_MESSAGE_TYPE_CHALLENGE = 2;
    constexpr uint8_t NTLMSSP_MESSAGE_TYPE_AUTHENTICATE = 3;

    constexpr uint32_t NTLMSSP_AV_FLAGS = 0x0006;
    constexpr uint32_t NTLMSSP_AV_EOL = 0x0000;
    constexpr uint32_t NTLMSSP_AV_HOSTNAME = 0x0001;
    constexpr uint32_t NTLMSSP_AV_DOMAINNAME = 0x0002;
    constexpr uint32_t NTLMSSP_AV_DNS_HOSTNAME = 0x0003;
    constexpr uint32_t NTLMSSP_AV_DNS_DOMAINNAME = 0x0004;
    constexpr uint32_t NTLMSSP_AV_TIMESTAMP = 0x0007;
}
#endif
