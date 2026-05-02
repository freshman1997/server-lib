#ifndef __NET_SMB_PROTOCOL_SMB2_STRUCTURES_H__
#define __NET_SMB_PROTOCOL_SMB2_STRUCTURES_H__

#include "protocol/smb2_constants.h"
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::smb
{
    struct Smb2Header
    {
        uint32_t protocol_id = SMB2_PROTOCOL_ID;
        uint16_t structure_size = SMB2_HEADER_SIZE;
        uint16_t credit_charge = 0;
        uint32_t status = 0;
        uint16_t command = 0;
        uint16_t credit_request = 0;
        uint32_t flags = 0;
        uint32_t next_command = 0;
        uint64_t message_id = 0;
        uint32_t reserved = 0;
        uint32_t tree_id = 0;
        uint64_t session_id = 0;
        uint8_t signature[SMB2_SIGNATURE_SIZE] = {};
    };

    struct Smb2TransformHeader
    {
        uint32_t protocol_id = SMB2_TRANSFORM_PROTOCOL_ID;
        uint8_t signature[SMB2_SIGNATURE_SIZE] = {};
        uint8_t nonce[16] = {};
        uint32_t original_message_size = 0;
        uint16_t reserved = 0;
        uint16_t encryption_algorithm = 0;
        uint64_t session_id = 0;
    };

    struct FileId
    {
        uint64_t persistent = 0;
        uint64_t volatile_id = 0;

        bool operator==(const FileId &other) const
        {
            return persistent == other.persistent && volatile_id == other.volatile_id;
        }

        bool operator!=(const FileId &other) const
        {
            return !(*this == other);
        }
    };

    struct Smb2NegotiateRequest
    {
        uint16_t structure_size = 36;
        uint16_t dialect_count = 0;
        uint16_t security_mode = 0;
        uint16_t reserved = 0;
        uint32_t capabilities = 0;
        uint8_t client_guid[16] = {};
        uint64_t client_start_time = 0;
        std::vector<uint16_t> dialects;
        std::vector<uint8_t> preauth_hash;
    };

    struct Smb2NegotiateResponse
    {
        uint16_t structure_size = 65;
        uint16_t security_mode = 0;
        uint16_t dialect_revision = 0;
        uint16_t reserved = 0;
        uint8_t server_guid[16] = {};
        uint32_t capabilities = 0;
        uint32_t max_transact_size = 0;
        uint32_t max_read_size = 0;
        uint32_t max_write_size = 0;
        uint64_t system_time = 0;
        uint64_t server_start_time = 0;
        uint16_t security_buffer_offset = 0;
        uint16_t security_buffer_length = 0;
        uint32_t negotiate_context_offset = 0;
        uint16_t negotiate_context_count = 0;
        uint16_t reserved2 = 0;
        std::vector<uint8_t> security_buffer;
        std::vector<uint8_t> negotiate_context;
    };

    struct Smb2SessionSetupRequest
    {
        uint16_t structure_size = 25;
        uint8_t flags = 0;
        uint8_t security_mode = 0;
        uint32_t capabilities = 0;
        uint32_t channel = 0;
        uint16_t security_buffer_offset = 0;
        uint16_t security_buffer_length = 0;
        uint64_t previous_session_id = 0;
        std::vector<uint8_t> security_buffer;
    };

    struct Smb2SessionSetupResponse
    {
        uint16_t structure_size = 9;
        uint16_t session_flags = 0;
        uint16_t security_buffer_offset = 0;
        uint16_t security_buffer_length = 0;
        std::vector<uint8_t> security_buffer;
    };

    struct Smb2LogoffRequest
    {
        uint16_t structure_size = 4;
        uint16_t reserved = 0;
    };

    struct Smb2LogoffResponse
    {
        uint16_t structure_size = 4;
        uint16_t reserved = 0;
    };

    struct Smb2TreeConnectRequest
    {
        uint16_t structure_size = 9;
        uint16_t reserved = 0;
        uint16_t path_offset = 0;
        uint16_t path_length = 0;
        std::u16string path;
    };

    struct Smb2TreeConnectResponse
    {
        uint16_t structure_size = 16;
        uint8_t share_type = 0;
        uint8_t reserved = 0;
        uint32_t share_flags = 0;
        uint32_t capabilities = 0;
        uint32_t maximal_access = 0;
    };

    struct Smb2TreeDisconnectRequest
    {
        uint16_t structure_size = 4;
        uint16_t reserved = 0;
    };

    struct Smb2TreeDisconnectResponse
    {
        uint16_t structure_size = 4;
        uint16_t reserved = 0;
    };

    struct Smb2CreateRequest
    {
        uint16_t structure_size = 57;
        uint8_t security_flags = 0;
        uint8_t requested_oplock_level = 0;
        uint32_t impersonation_level = 0;
        uint64_t smb_create_flags = 0;
        uint64_t reserved = 0;
        uint32_t desired_access = 0;
        uint32_t file_attributes = 0;
        uint32_t share_access = 0;
        uint32_t create_disposition = 0;
        uint32_t create_options = 0;
        uint16_t name_offset = 0;
        uint16_t name_length = 0;
        uint32_t create_contexts_offset = 0;
        uint32_t create_contexts_length = 0;
        std::u16string buffer;
        std::vector<uint8_t> create_contexts;
    };

    struct Smb2CreateResponse
    {
        uint16_t structure_size = 89;
        uint8_t oplock_level = 0;
        uint8_t flags = 0;
        uint32_t create_action = 0;
        uint64_t creation_time = 0;
        uint64_t last_access_time = 0;
        uint64_t last_write_time = 0;
        uint64_t change_time = 0;
        uint64_t allocation_size = 0;
        uint64_t end_of_file = 0;
        uint32_t file_attributes = 0;
        uint32_t reserved2 = 0;
        FileId file_id;
        uint32_t create_contexts_offset = 0;
        uint32_t create_contexts_length = 0;
        std::vector<uint8_t> create_contexts;
    };

    struct Smb2CloseRequest
    {
        uint16_t structure_size = 24;
        uint16_t flags = 0;
        uint32_t reserved = 0;
        FileId file_id;
    };

    struct Smb2CloseResponse
    {
        uint16_t structure_size = 60;
        uint16_t flags = 0;
        uint32_t reserved = 0;
        uint64_t creation_time = 0;
        uint64_t last_access_time = 0;
        uint64_t last_write_time = 0;
        uint64_t change_time = 0;
        uint64_t allocation_size = 0;
        uint64_t end_of_file = 0;
        uint32_t file_attributes = 0;
    };

    struct Smb2ReadRequest
    {
        uint16_t structure_size = 49;
        uint8_t padding = 0;
        uint8_t flags = 0;
        uint32_t length = 0;
        uint64_t offset = 0;
        FileId file_id;
        uint32_t minimum_count = 0;
        uint32_t channel = 0;
        uint32_t remaining_bytes = 0;
        uint16_t read_channel_info_offset = 0;
        uint16_t read_channel_info_length = 0;
    };

    struct Smb2ReadResponse
    {
        uint16_t structure_size = 17;
        uint8_t data_offset = 0;
        uint8_t reserved = 0;
        uint32_t data_length = 0;
        uint32_t data_remaining = 0;
        uint32_t reserved2 = 0;
        std::vector<uint8_t> buffer;
    };

    struct Smb2WriteRequest
    {
        uint16_t structure_size = 49;
        uint16_t data_offset = 0;
        uint32_t length = 0;
        uint64_t offset = 0;
        FileId file_id;
        uint32_t channel = 0;
        uint32_t remaining_bytes = 0;
        uint16_t write_channel_info_offset = 0;
        uint16_t write_channel_info_length = 0;
        uint32_t flags = 0;
        std::vector<uint8_t> buffer;
    };

    struct Smb2WriteResponse
    {
        uint16_t structure_size = 17;
        uint16_t reserved = 0;
        uint32_t count = 0;
        uint32_t remaining = 0;
        uint16_t write_channel_info_offset = 0;
        uint16_t write_channel_info_length = 0;
    };

    struct Smb2QueryDirectoryRequest
    {
        uint16_t structure_size = 33;
        uint8_t file_information_class = 0;
        uint8_t flags = 0;
        uint32_t file_index = 0;
        FileId file_id;
        uint16_t file_name_offset = 0;
        uint16_t file_name_length = 0;
        uint32_t output_buffer_length = 0;
        std::u16string file_name;
    };

    struct Smb2QueryDirectoryResponse
    {
        uint16_t structure_size = 9;
        uint16_t output_buffer_offset = 0;
        uint32_t output_buffer_length = 0;
        std::vector<uint8_t> buffer;
    };

    struct Smb2QueryInfoRequest
    {
        uint16_t structure_size = 41;
        uint8_t info_type = 0;
        uint8_t file_info_class = 0;
        uint32_t output_buffer_length = 0;
        uint16_t input_buffer_offset = 0;
        uint16_t reserved = 0;
        uint32_t input_buffer_length = 0;
        uint32_t additional_information = 0;
        uint32_t flags = 0;
        FileId file_id;
        std::vector<uint8_t> input_buffer;
    };

    struct Smb2QueryInfoResponse
    {
        uint16_t structure_size = 9;
        uint16_t output_buffer_offset = 0;
        uint32_t output_buffer_length = 0;
        std::vector<uint8_t> buffer;
    };

    struct Smb2SetInfoRequest
    {
        uint16_t structure_size = 33;
        uint8_t info_type = 0;
        uint8_t file_info_class = 0;
        uint32_t buffer_length = 0;
        uint16_t buffer_offset = 0;
        uint16_t reserved = 0;
        uint32_t additional_information = 0;
        FileId file_id;
        std::vector<uint8_t> buffer;
    };

    struct Smb2SetInfoResponse
    {
        uint16_t structure_size = 2;
        uint16_t output_buffer_offset = 0;
        uint32_t output_buffer_length = 0;
    };

    struct Smb2LockElement
    {
        uint64_t offset = 0;
        uint64_t length = 0;
        uint32_t flags = 0;
        uint32_t reserved = 0;
    };

    struct Smb2LockRequest
    {
        uint16_t structure_size = 48;
        uint16_t lock_count = 0;
        uint32_t lock_sequence = 0;
        FileId file_id;
        std::vector<Smb2LockElement> locks;
    };

    struct Smb2LockResponse
    {
        uint16_t structure_size = 4;
        uint16_t reserved = 0;
    };

    struct Smb2IoctlRequest
    {
        uint16_t structure_size = 57;
        uint16_t reserved = 0;
        uint32_t ctl_code = 0;
        FileId file_id;
        uint32_t input_offset = 0;
        uint32_t input_length = 0;
        uint32_t max_input_response = 0;
        uint32_t output_offset = 0;
        uint32_t output_length = 0;
        uint32_t max_output_response = 0;
        uint32_t flags = 0;
        uint32_t reserved2 = 0;
        std::vector<uint8_t> input_buffer;
        std::vector<uint8_t> output_buffer;
    };

    struct Smb2IoctlResponse
    {
        uint16_t structure_size = 49;
        uint16_t reserved = 0;
        uint32_t ctl_code = 0;
        FileId file_id;
        uint32_t input_offset = 0;
        uint32_t input_length = 0;
        uint32_t output_offset = 0;
        uint32_t output_length = 0;
        uint32_t flags = 0;
        uint32_t reserved2 = 0;
        std::vector<uint8_t> input_buffer;
        std::vector<uint8_t> output_buffer;
    };

    struct Smb2EchoRequest
    {
        uint16_t structure_size = 4;
        uint16_t reserved = 0;
    };

    struct Smb2EchoResponse
    {
        uint16_t structure_size = 4;
        uint16_t reserved = 0;
    };

    struct Smb2ChangeNotifyRequest
    {
        uint16_t structure_size = 32;
        uint16_t flags = 0;
        uint32_t output_buffer_length = 0;
        FileId file_id;
        uint32_t completion_filter = 0;
        uint32_t reserved = 0;
    };

    struct Smb2ChangeNotifyResponse
    {
        uint16_t structure_size = 9;
        uint16_t output_buffer_offset = 0;
        uint32_t output_buffer_length = 0;
        std::vector<uint8_t> buffer;
    };

    struct Smb2OplockBreakNotification
    {
        uint16_t structure_size = 24;
        uint8_t oplock_level = 0;
        uint8_t reserved = 0;
        uint32_t reserved2 = 0;
        FileId file_id;
    };

    struct Smb2OplockBreakAckRequest
    {
        uint16_t structure_size = 24;
        uint8_t oplock_level = 0;
        uint8_t reserved = 0;
        uint32_t reserved2 = 0;
        FileId file_id;
    };

    struct Smb2OplockBreakAckResponse
    {
        uint16_t structure_size = 24;
        uint8_t oplock_level = 0;
        uint8_t reserved = 0;
        uint32_t reserved2 = 0;
        FileId file_id;
    };

    struct Smb2LeaseBreakNotification
    {
        uint16_t structure_size = 44;
        uint16_t new_epoch = 0;
        uint32_t flags = 0;
        uint8_t lease_key[SMB2_LEASE_KEY_SIZE] = {};
        uint32_t current_lease_state = 0;
        uint32_t new_lease_state = 0;
        uint32_t break_reason = 0;
        uint32_t access_mask_hint = 0;
        uint32_t share_mask_hint = 0;
    };

    struct Smb2LeaseBreakAckRequest
    {
        uint16_t structure_size = 36;
        uint16_t reserved = 0;
        uint32_t flags = 0;
        uint8_t lease_key[SMB2_LEASE_KEY_SIZE] = {};
        uint32_t lease_state = 0;
        uint64_t lease_duration = 0;
    };

    struct Smb2LeaseBreakAckResponse
    {
        uint16_t structure_size = 36;
        uint16_t reserved = 0;
        uint32_t flags = 0;
        uint8_t lease_key[SMB2_LEASE_KEY_SIZE] = {};
        uint32_t lease_state = 0;
        uint64_t lease_duration = 0;
    };

    struct Smb2CancelRequest
    {
    };

    struct Smb2FlushRequest
    {
        uint16_t structure_size = 24;
        uint16_t reserved = 0;
        uint32_t reserved2 = 0;
        FileId file_id;
    };

    struct Smb2FlushResponse
    {
        uint16_t structure_size = 4;
        uint16_t reserved = 0;
    };

    struct Smb2ErrorContextResponse
    {
        uint32_t error_context_count = 0;
        uint32_t byte_count = 0;
        std::vector<uint8_t> error_context_data;
    };

    struct Smb2CreateContext
    {
        uint32_t next = 0;
        uint16_t name_offset = 0;
        uint16_t name_length = 0;
        uint16_t data_offset = 0;
        uint16_t data_length = 0;
        uint32_t reserved = 0;
        std::vector<uint8_t> name;
        std::vector<uint8_t> data;
    };

    struct NegotiateContext
    {
        uint16_t context_type = 0;
        uint16_t data_length = 0;
        uint32_t reserved = 0;
        std::vector<uint8_t> data;
    };
}
#endif
