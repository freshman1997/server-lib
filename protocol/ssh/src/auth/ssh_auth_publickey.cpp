#include "auth/ssh_auth_publickey.h"
#include "protocol/ssh_message_codec.h"
#include "ssh_session.h"

namespace yuan::net::ssh
{
    SshAuthPublickey::SshAuthPublickey(SshCrypto * crypto)
        : crypto_(crypto)
    {
    }

    SshAuthResult SshAuthPublickey::authenticate(SshSession * session,
                                                 const std::string & username,
                                                 const SshAuthCredentials & credentials)
    {
        if (!credentials.has_signature)
            return SshAuthResult::NEED_MORE;

        if (credentials.signature.empty() || credentials.public_key_blob.empty())
            return SshAuthResult::FAILURE;

        if (!session)
            return SshAuthResult::FAILURE;

        const auto &session_id = session->session_id_proto();
        return verify_signature(session_id, username,
                                credentials.public_key_algorithm,
                                credentials.public_key_blob,
                                credentials.signature)
                   ? SshAuthResult::SUCCESS
                   : SshAuthResult::FAILURE;
    }

    bool SshAuthPublickey::verify_signature(const std::vector<uint8_t> & session_id,
                                            const std::string & username,
                                            const std::string & algorithm,
                                            const std::vector<uint8_t> & public_key_blob,
                                            const std::vector<uint8_t> & signature)
    {
        (void)username;

        if (!crypto_)
            return false;

        ByteBuffer signed_data;
        SshMessageCodec::write_string(signed_data, session_id);
        SshMessageCodec::write_uint8(signed_data, static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_REQUEST));
        SshMessageCodec::write_string(signed_data, username);
        SshMessageCodec::write_string(signed_data, SSH_SERVICE_CONNECTION);
        SshMessageCodec::write_string(signed_data, "publickey");
        SshMessageCodec::write_boolean(signed_data, true);
        SshMessageCodec::write_string(signed_data, algorithm);
        SshMessageCodec::write_string(signed_data, std::string(public_key_blob.begin(), public_key_blob.end()));

        return crypto_->verify_signature(public_key_blob, signature,
                                         reinterpret_cast<const uint8_t *>(signed_data.read_ptr()),
                                         signed_data.readable_bytes(),
                                         algorithm);
    }
}
