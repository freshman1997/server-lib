#ifndef __NET_SSH_PROTOCOL_SSH_STRUCTURES_H__
#define __NET_SSH_PROTOCOL_SSH_STRUCTURES_H__

#include "protocol/ssh_constants.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    struct SshKexInitMessage
    {
        uint8_t cookie[16] = {};
        std::string kex_algorithms;
        std::string server_host_key_algorithms;
        std::string encryption_algorithms_client_to_server;
        std::string encryption_algorithms_server_to_client;
        std::string mac_algorithms_client_to_server;
        std::string mac_algorithms_server_to_client;
        std::string compression_algorithms_client_to_server;
        std::string compression_algorithms_server_to_client;
        std::string languages_client_to_server;
        std::string languages_server_to_client;
        bool first_kex_packet_follows = false;
        uint32_t reserved = 0;
    };

    struct SshDisconnectMessage
    {
        uint32_t reason_code = 0;
        std::string description;
        std::string language;
    };

    struct SshIgnoreMessage
    {
        std::string data;
    };

    struct SshUnimplementedMessage
    {
        uint32_t sequence_number = 0;
    };

    struct SshDebugMessage
    {
        bool always_display = false;
        std::string message;
        std::string language;
    };

    struct SshServiceRequestMessage
    {
        std::string service_name;
    };

    struct SshServiceAcceptMessage
    {
        std::string service_name;
    };

    struct SshKexEcdhInitMessage
    {
        std::vector<uint8_t> client_public_key;
    };

    struct SshKexEcdhReplyMessage
    {
        std::vector<uint8_t> host_key_blob;
        std::vector<uint8_t> server_public_key;
        std::vector<uint8_t> signature;
    };

    struct SshKexDhInitMessage
    {
        std::vector<uint8_t> client_public_key;
    };

    struct SshKexDhReplyMessage
    {
        std::vector<uint8_t> host_key_blob;
        std::vector<uint8_t> server_public_key;
        std::vector<uint8_t> signature;
    };

    struct SshKexDhGexRequestMessage
    {
        uint32_t min_bits = 0;
        uint32_t preferred_bits = 0;
        uint32_t max_bits = 0;
    };

    struct SshKexDhGexGroupMessage
    {
        std::vector<uint8_t> prime;
        std::vector<uint8_t> generator;
    };

    struct SshKexDhGexInitMessage
    {
        std::vector<uint8_t> client_public_key;
    };

    struct SshKexDhGexReplyMessage
    {
        std::vector<uint8_t> host_key_blob;
        std::vector<uint8_t> server_public_key;
        std::vector<uint8_t> signature;
    };

    struct SshUserauthRequestMessage
    {
        std::string username;
        std::string service_name;
        std::string method_name;
        std::vector<uint8_t> method_specific_data;
    };

    struct SshUserauthFailureMessage
    {
        std::string auth_methods_that_can_continue;
        bool partial_success = false;
    };

    struct SshUserauthBannerMessage
    {
        std::string message;
        std::string language;
    };

    struct SshUserauthPkOkMessage
    {
        std::string algorithm_name;
        std::vector<uint8_t> public_key_blob;
    };

    struct SshAuthPrompt
    {
        std::string prompt;
        bool echo = false;
    };

    struct SshUserauthInfoRequestMessage
    {
        std::string name;
        std::string instruction;
        std::string language;
        std::vector<SshAuthPrompt> prompts;
    };

    struct SshUserauthInfoResponseMessage
    {
        std::vector<std::string> responses;
    };

    struct SshChannelOpenMessage
    {
        std::string channel_type;
        uint32_t sender_channel = 0;
        uint32_t initial_window_size = 0;
        uint32_t maximum_packet_size = 0;
        std::vector<uint8_t> type_specific_data;
    };

    struct SshChannelOpenConfirmationMessage
    {
        uint32_t recipient_channel = 0;
        uint32_t sender_channel = 0;
        uint32_t initial_window_size = 0;
        uint32_t maximum_packet_size = 0;
    };

    struct SshChannelOpenFailureMessage
    {
        uint32_t recipient_channel = 0;
        uint32_t reason_code = 0;
        std::string description;
        std::string language;
    };

    struct SshChannelWindowAdjustMessage
    {
        uint32_t recipient_channel = 0;
        uint32_t bytes_to_add = 0;
    };

    struct SshChannelDataMessage
    {
        uint32_t recipient_channel = 0;
        std::vector<uint8_t> data;
    };

    struct SshChannelExtendedDataMessage
    {
        uint32_t recipient_channel = 0;
        uint32_t data_type_code = 0;
        std::vector<uint8_t> data;
    };

    struct SshChannelEofMessage
    {
        uint32_t recipient_channel = 0;
    };

    struct SshChannelCloseMessage
    {
        uint32_t recipient_channel = 0;
    };

    struct SshChannelRequestMessage
    {
        uint32_t recipient_channel = 0;
        std::string request_type;
        bool want_reply = false;
        std::vector<uint8_t> request_specific_data;
    };

    struct SshChannelSuccessMessage
    {
        uint32_t recipient_channel = 0;
    };

    struct SshChannelFailureMessage
    {
        uint32_t recipient_channel = 0;
    };

    struct SshGlobalRequestMessage
    {
        std::string request_name;
        bool want_reply = false;
        std::vector<uint8_t> request_specific_data;
    };

    struct SshNegotiatedAlgorithms
    {
        std::string kex_name;
        std::string kex_hash_name;
        std::string host_key_name;
        std::string client_to_server_cipher_name;
        std::string server_to_client_cipher_name;
        std::string client_to_server_mac_name;
        std::string server_to_client_mac_name;
        std::string client_to_server_compression_name;
        std::string server_to_client_compression_name;
    };

    struct SshPasswordAuthData
    {
        std::string password;
    };

    struct SshPublicKeyAuthData
    {
        std::string algorithm_name;
        std::vector<uint8_t> public_key_blob;
        std::vector<uint8_t> signature;
        bool has_signature = false;
    };

    struct SshKeyboardInteractiveAuthData
    {
        std::string language;
        std::string submethods;
    };

    struct SshPtyRequestData
    {
        std::string term_env;
        uint32_t terminal_width = 0;
        uint32_t terminal_height = 0;
        uint32_t terminal_width_pixels = 0;
        uint32_t terminal_height_pixels = 0;
        std::vector<uint8_t> terminal_modes;
    };

    struct SshExecRequestData
    {
        std::string command;
    };

    struct SshSubsystemRequestData
    {
        std::string subsystem_name;
    };

    struct SshEnvRequestData
    {
        std::string variable_name;
        std::string variable_value;
    };

    struct SshWindowChangeData
    {
        uint32_t terminal_width = 0;
        uint32_t terminal_height = 0;
        uint32_t terminal_width_pixels = 0;
        uint32_t terminal_height_pixels = 0;
    };

    struct SshSignalData
    {
        std::string signal_name;
    };

    struct SshExitStatusData
    {
        uint32_t exit_status = 0;
    };

    struct SshExitSignalData
    {
        std::string signal_name;
        bool core_dumped = false;
        std::string error_message;
        std::string language;
    };

    struct SshDirectTcpIpChannelData
    {
        std::string host_to_connect;
        uint32_t port_to_connect = 0;
        std::string originator_ip_address;
        uint32_t originator_port = 0;
    };

    struct SshForwardedTcpIpChannelData
    {
        std::string host_to_connect;
        uint32_t port_to_connect = 0;
        std::string originator_ip_address;
        uint32_t originator_port = 0;
    };

    struct SshTcpIpForwardRequest
    {
        std::string address_to_bind;
        uint32_t port = 0;
    };

    struct SshCancelTcpIpForwardRequest
    {
        std::string address_to_bind;
        uint32_t port = 0;
    };

    struct SshAuthCredentials
    {
        std::string password;
        std::string public_key_algorithm;
        std::vector<uint8_t> public_key_blob;
        std::vector<uint8_t> signature;
        bool has_signature = false;
        std::string kb_interactive_language;
        std::string kb_interactive_submethods;
        std::vector<std::string> kb_interactive_responses;
    };

    struct SshKeyPair
    {
        std::vector<uint8_t> private_key;
        std::vector<uint8_t> public_key;
    };
}

#endif
