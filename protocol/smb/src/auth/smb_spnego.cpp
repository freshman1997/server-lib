#include "auth/smb_spnego.h"
#include <cstring>

namespace yuan::net::smb
{
    static const uint8_t SPNEGO_OID[] = { 0x06, 0x06, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x02 };
    static const uint8_t NTLMSSP_OID[] = { 0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0A };

    static bool contains_ntlmssp_oid(const std::vector<uint8_t> &token)
    {
        if (token.size() < sizeof(NTLMSSP_OID)) {
            return false;
        }
        for (size_t i = 0; i + sizeof(NTLMSSP_OID) <= token.size(); ++i) {
            if (std::memcmp(token.data() + i, NTLMSSP_OID, sizeof(NTLMSSP_OID)) == 0) {
                return true;
            }
        }
        return false;
    }

    static bool read_der_length(const std::vector<uint8_t> &token, size_t &offset, size_t &length)
    {
        if (offset >= token.size()) {
            return false;
        }

        const uint8_t first = token[offset++];
        if ((first & 0x80) == 0) {
            length = first;
            return offset + length <= token.size();
        }

        const size_t count = first & 0x7F;
        if (count == 0 || count > sizeof(size_t) || offset + count > token.size()) {
            return false;
        }

        length = 0;
        for (size_t i = 0; i < count; ++i) {
            length = (length << 8) | token[offset++];
        }
        return offset + length <= token.size();
    }

    static bool read_der_tlv(const std::vector<uint8_t> &token,
                             size_t &offset,
                             uint8_t &tag,
                             size_t &value_offset,
                             size_t &value_length)
    {
        if (offset >= token.size()) {
            return false;
        }

        tag = token[offset++];
        if (!read_der_length(token, offset, value_length)) {
            return false;
        }

        value_offset = offset;
        offset += value_length;
        return true;
    }

    static std::vector<uint8_t> extract_spnego_inner_token(const std::vector<uint8_t> &token)
    {
        size_t outer_offset = 0;
        uint8_t outer_tag = 0;
        size_t outer_value_offset = 0;
        size_t outer_value_length = 0;
        if (!read_der_tlv(token, outer_offset, outer_tag, outer_value_offset, outer_value_length)) {
            return {};
        }

        size_t current = outer_value_offset;
        size_t end = outer_value_offset + outer_value_length;

        if (outer_tag == 0x60) {
            uint8_t oid_tag = 0;
            size_t oid_value_offset = 0;
            size_t oid_value_length = 0;
            if (!read_der_tlv(token, current, oid_tag, oid_value_offset, oid_value_length) || oid_tag != 0x06) {
                return {};
            }
            static const uint8_t spnego_oid_value[] = { 0x2B, 0x06, 0x01, 0x05, 0x05, 0x02 };
            if (oid_value_length != sizeof(spnego_oid_value) ||
                std::memcmp(token.data() + oid_value_offset, spnego_oid_value, oid_value_length) != 0) {
                return {};
            }

            if (!read_der_tlv(token, current, outer_tag, outer_value_offset, outer_value_length)) {
                return {};
            }
            end = outer_value_offset + outer_value_length;
        }

        if (outer_tag != 0x30 && outer_tag != 0xA0 && outer_tag != 0xA1) {
            return {};
        }

        if (outer_tag == 0xA0 || outer_tag == 0xA1) {
            current = outer_value_offset;
            end = outer_value_offset + outer_value_length;
            if (!read_der_tlv(token, current, outer_tag, outer_value_offset, outer_value_length) || outer_tag != 0x30) {
                return {};
            }
            current = outer_value_offset;
            end = outer_value_offset + outer_value_length;
        }

        while (current < end) {
            uint8_t field_tag = 0;
            size_t field_value_offset = 0;
            size_t field_value_length = 0;
            if (!read_der_tlv(token, current, field_tag, field_value_offset, field_value_length)) {
                return {};
            }

            if (field_tag != 0xA2) {
                continue;
            }

            size_t inner_offset = field_value_offset;
            uint8_t octet_tag = 0;
            size_t octet_value_offset = 0;
            size_t octet_value_length = 0;
            if (!read_der_tlv(token, inner_offset, octet_tag, octet_value_offset, octet_value_length) || octet_tag != 0x04) {
                return {};
            }

            return std::vector<uint8_t>(token.begin() + static_cast<std::ptrdiff_t>(octet_value_offset),
                                        token.begin() + static_cast<std::ptrdiff_t>(octet_value_offset + octet_value_length));
        }

        return {};
    }

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

    void SmbSpnegoAuth::set_password_lookup(std::function<std::optional<std::string>(const std::string &, const std::string &)> lookup)
    {
        ntlm_auth_->set_password_lookup(std::move(lookup));
    }

    void SmbSpnegoAuth::set_nt_hash_lookup(std::function<std::optional<std::string>(const std::string &, const std::string &)> lookup)
    {
        ntlm_auth_->set_nt_hash_lookup(std::move(lookup));
    }

    std::vector<uint8_t> SmbSpnegoAuth::extract_mech_token_for_test(const std::vector<uint8_t> & token)
    {
        return extract_mech_token(token);
    }

    std::vector<uint8_t> SmbSpnegoAuth::extract_mech_token(const std::vector<uint8_t> & spnego_token)
    {
        if (auto extracted = extract_spnego_inner_token(spnego_token); !extracted.empty()) {
            return extracted;
        }

        const uint8_t ntlmssp_sig[] = { 'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0' };
        for (size_t i = 0; i + 8 <= spnego_token.size(); i++) {
            if (std::memcmp(spnego_token.data() + i, ntlmssp_sig, 8) == 0) {
                size_t start = i;
                size_t end = spnego_token.size();
                return std::vector<uint8_t>(spnego_token.begin() + start, spnego_token.begin() + end);
            }
        }

        // Samba can send a NegTokenInit that advertises NTLMSSP and leaves mechToken
        // empty; treat that as the initial NTLM negotiate step.
        if (contains_ntlmssp_oid(spnego_token)) {
            return {};
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
        if (!ntlm_token.empty()) {
            mech_token.push_back(0x04);
            write_der_length(mech_token, ntlm_token.size());
            mech_token.insert(mech_token.end(), ntlm_token.begin(), ntlm_token.end());
        }

        if (is_neg_init) {
            std::vector<uint8_t> mech_type_list;
            mech_type_list.push_back(0x30);
            write_der_length(mech_type_list, sizeof(NTLMSSP_OID));
            mech_type_list.insert(mech_type_list.end(), NTLMSSP_OID, NTLMSSP_OID + sizeof(NTLMSSP_OID));

            std::vector<uint8_t> mech_types_wrapped;
            mech_types_wrapped.push_back(0xA0);
            write_der_length(mech_types_wrapped, mech_type_list.size());
            mech_types_wrapped.insert(mech_types_wrapped.end(), mech_type_list.begin(), mech_type_list.end());

            std::vector<uint8_t> seq_content;
            seq_content.insert(seq_content.end(), mech_types_wrapped.begin(), mech_types_wrapped.end());
            if (!mech_token.empty()) {
                std::vector<uint8_t> mech_token_wrapped;
                mech_token_wrapped.push_back(0xA2);
                write_der_length(mech_token_wrapped, mech_token.size());
                mech_token_wrapped.insert(mech_token_wrapped.end(), mech_token.begin(), mech_token.end());
                seq_content.insert(seq_content.end(), mech_token_wrapped.begin(), mech_token_wrapped.end());
            }

            std::vector<uint8_t> seq;
            seq.push_back(0x30);
            write_der_length(seq, seq_content.size());
            seq.insert(seq.end(), seq_content.begin(), seq_content.end());

            std::vector<uint8_t> result;
            result.push_back(0x60);
            write_der_length(result, sizeof(SPNEGO_OID) + seq.size());
            result.insert(result.end(), SPNEGO_OID, SPNEGO_OID + sizeof(SPNEGO_OID));
            result.insert(result.end(), seq.begin(), seq.end());
            return result;
        } else {
            std::vector<uint8_t> neg_result;
            neg_result.push_back(0x0A);
            neg_result.push_back(0x01);
            neg_result.push_back(success ? 0x00 : 0x01);

            std::vector<uint8_t> neg_result_wrapped;
            neg_result_wrapped.push_back(0xA0);
            write_der_length(neg_result_wrapped, neg_result.size());
            neg_result_wrapped.insert(neg_result_wrapped.end(), neg_result.begin(), neg_result.end());

            std::vector<uint8_t> supported_mech_wrapped;
            supported_mech_wrapped.push_back(0xA1);
            write_der_length(supported_mech_wrapped, sizeof(NTLMSSP_OID));
            supported_mech_wrapped.insert(supported_mech_wrapped.end(), NTLMSSP_OID, NTLMSSP_OID + sizeof(NTLMSSP_OID));

            std::vector<uint8_t> seq_content;
            seq_content.insert(seq_content.end(), neg_result_wrapped.begin(), neg_result_wrapped.end());
            seq_content.insert(seq_content.end(), supported_mech_wrapped.begin(), supported_mech_wrapped.end());
            if (!mech_token.empty()) {
                std::vector<uint8_t> resp_token_wrapped;
                resp_token_wrapped.push_back(0xA2);
                write_der_length(resp_token_wrapped, mech_token.size());
                resp_token_wrapped.insert(resp_token_wrapped.end(), mech_token.begin(), mech_token.end());
                seq_content.insert(seq_content.end(), resp_token_wrapped.begin(), resp_token_wrapped.end());
            }

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
        if (token.empty()) {
            return wrap_ntlm_token({}, true, true);
        }

        auto ntlm_token = extract_mech_token(token);
        if (ntlm_token.empty() && contains_ntlmssp_oid(token)) {
            ntlm_token = {
                'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0',
                0x01, 0x00, 0x00, 0x00,
                0x07, 0x82, 0x08, 0xA0
            };
        }
        auto response = ntlm_auth_->process_inbound_token(ntlm_token);

        if (ntlm_auth_->state() == AuthState::authenticated) {
            return wrap_ntlm_token(response, false, true);
        }

        if (response.empty()) {
            return {};
        }

        if (ntlm_auth_->state() == AuthState::challenge_sent) {
            return wrap_ntlm_token(response, false, false);
        }

        return wrap_ntlm_token(response, false, true);
    }
}
