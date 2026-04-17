#include "auth/smb_spnego.h"
#include <cstring>

namespace yuan::net::smb
{
    static const uint8_t NTLMSSP_OID[] = { 0x06, 0x0B, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0A };

    SmbSpnegoAuth::SmbSpnegoAuth(const std::string & server_name, const std::string & domain_name)
        : ntlm_auth_(std::make_unique<SmbNtlmAuth>(server_name, domain_name))
    {
    }

    AuthState SmbSpnegoAuth::state() const
    {
        return ntlm_auth_->state();
    }

    const AuthResult &SmbSpnegoAuth::result() const
    {
        return ntlm_auth_->result();
    }

    bool SmbSpnegoAuth::is_complete() const
    {
        return ntlm_auth_->is_complete();
    }

    void SmbSpnegoAuth::set_credentials_db(std::function<bool(const std::string &, const std::string &, const std::string &)> validator)
    {
        ntlm_auth_->set_credentials_db(std::move(validator));
    }

    std::vector<uint8_t> SmbSpnegoAuth::extract_mech_token(const std::vector<uint8_t> & spnego_token)
    {
        const uint8_t ntlmssp_sig[] = { 'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0' };
        for (size_t i = 0; i + 8 <= spnego_token.size(); i++) {
            if (std::memcmp(spnego_token.data() + i, ntlmssp_sig, 8) == 0) {
                size_t start = i;
                size_t end = spnego_token.size();
                return std::vector<uint8_t>(spnego_token.begin() + start, spnego_token.begin() + end);
            }
        }
        return spnego_token;
    }

    static void write_der_length(std::vector<uint8_t> & out, size_t len)
    {
        if (len < 128) {
            out.push_back(static_cast<uint8_t>(len));
        } else if (len < 256) {
            out.push_back(0x81);
            out.push_back(static_cast<uint8_t>(len));
        } else {
            out.push_back(0x82);
            out.push_back(static_cast<uint8_t>(len >> 8));
            out.push_back(static_cast<uint8_t>(len & 0xFF));
        }
    }

    std::vector<uint8_t> SmbSpnegoAuth::wrap_ntlm_token(const std::vector<uint8_t> & ntlm_token, bool is_neg_init, bool success)
    {
        std::vector<uint8_t> mech_token;
        mech_token.push_back(0x04);
        write_der_length(mech_token, ntlm_token.size());
        mech_token.insert(mech_token.end(), ntlm_token.begin(), ntlm_token.end());

        if (is_neg_init) {
            std::vector<uint8_t> mech_type_list;
            mech_type_list.push_back(0x30);
            write_der_length(mech_type_list, sizeof(NTLMSSP_OID));
            mech_type_list.insert(mech_type_list.end(), NTLMSSP_OID, NTLMSSP_OID + sizeof(NTLMSSP_OID));

            std::vector<uint8_t> mech_types_wrapped;
            mech_types_wrapped.push_back(0xA0);
            write_der_length(mech_types_wrapped, mech_type_list.size());
            mech_types_wrapped.insert(mech_types_wrapped.end(), mech_type_list.begin(), mech_type_list.end());

            std::vector<uint8_t> mech_token_wrapped;
            mech_token_wrapped.push_back(0xA2);
            write_der_length(mech_token_wrapped, mech_token.size());
            mech_token_wrapped.insert(mech_token_wrapped.end(), mech_token.begin(), mech_token.end());

            std::vector<uint8_t> seq_content;
            seq_content.insert(seq_content.end(), mech_types_wrapped.begin(), mech_types_wrapped.end());
            seq_content.insert(seq_content.end(), mech_token_wrapped.begin(), mech_token_wrapped.end());

            std::vector<uint8_t> seq;
            seq.push_back(0x30);
            write_der_length(seq, seq_content.size());
            seq.insert(seq.end(), seq_content.begin(), seq_content.end());

            std::vector<uint8_t> result;
            result.push_back(0xA0);
            write_der_length(result, seq.size());
            result.insert(result.end(), seq.begin(), seq.end());
            return result;
        } else {
            std::vector<uint8_t> neg_result;
            neg_result.push_back(0x02);
            neg_result.push_back(0x01);
            neg_result.push_back(success ? 0x00 : 0x01);

            std::vector<uint8_t> neg_result_wrapped;
            neg_result_wrapped.push_back(0xA0);
            write_der_length(neg_result_wrapped, neg_result.size());
            neg_result_wrapped.insert(neg_result_wrapped.end(), neg_result.begin(), neg_result.end());

            std::vector<uint8_t> supported_mech_wrapped;
            supported_mech_wrapped.push_back(0xA2);
            write_der_length(supported_mech_wrapped, sizeof(NTLMSSP_OID));
            supported_mech_wrapped.insert(supported_mech_wrapped.end(), NTLMSSP_OID, NTLMSSP_OID + sizeof(NTLMSSP_OID));

            std::vector<uint8_t> resp_token_wrapped;
            resp_token_wrapped.push_back(0xA3);
            write_der_length(resp_token_wrapped, mech_token.size());
            resp_token_wrapped.insert(resp_token_wrapped.end(), mech_token.begin(), mech_token.end());

            std::vector<uint8_t> seq_content;
            seq_content.insert(seq_content.end(), neg_result_wrapped.begin(), neg_result_wrapped.end());
            seq_content.insert(seq_content.end(), supported_mech_wrapped.begin(), supported_mech_wrapped.end());
            seq_content.insert(seq_content.end(), resp_token_wrapped.begin(), resp_token_wrapped.end());

            std::vector<uint8_t> seq;
            seq.push_back(0x30);
            write_der_length(seq, seq_content.size());
            seq.insert(seq.end(), seq_content.begin(), seq_content.end());

            std::vector<uint8_t> result;
            result.push_back(0xA1);
            write_der_length(result, seq.size());
            result.insert(result.end(), seq.begin(), seq.end());
            return result;
        }
    }

    std::vector<uint8_t> SmbSpnegoAuth::process_inbound_token(const std::vector<uint8_t> & token)
    {
        auto ntlm_token = extract_mech_token(token);
        auto response = ntlm_auth_->process_inbound_token(ntlm_token);

        if (response.empty()) {
            return {};
        }

        bool is_init = (ntlm_auth_->state() == AuthState::challenge_sent);
        return wrap_ntlm_token(response, is_init, true);
    }
}
