#ifndef __NET_WEBRTC_ICE_TRANSPORT_H__
#define __NET_WEBRTC_ICE_TRANSPORT_H__

#include "webrtc_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::webrtc
{

struct IceCandidateEndpoint
{
    std::string foundation;
    uint32_t component = 1;
    std::string transport;
    uint32_t priority = 0;
    std::string ip;
    uint16_t port = 0;
    std::string type;
    std::string related_address;
    uint16_t related_port = 0;
};

bool parse_ice_candidate_sdp(const std::string &candidate_text, IceCandidate &out_candidate);
IceCandidateEndpoint to_candidate_endpoint(const IceCandidate &candidate);

struct IceAdapterConfig
{
    uint64_t gather_delay_ms = 20;
    uint64_t checklist_delay_ms = 60;
    bool gather_two_host_candidates = true;
};

struct IceCandidatePair
{
    IceCandidateEndpoint local;
    IceCandidateEndpoint remote;
    IceSelectedPairReason reason = IceSelectedPairReason::none;
    std::string reason_text;
    std::string nomination_transaction_id;
};

struct StunTransaction
{
    std::string transaction_id;
    StunTransactionState state = StunTransactionState::new_;
    uint64_t started_at_ms = 0;
    uint64_t last_request_at_ms = 0;
    uint64_t completed_at_ms = 0;
    uint32_t request_count = 0;
    uint32_t retransmit_count = 0;
    uint32_t request_priority = 0;
    bool request_use_candidate = false;
    std::string error;
    std::string response_code;
    std::string mapped_address;
    uint16_t mapped_port = 0;
};

class IceTransportProvider
{
public:
    virtual ~IceTransportProvider() = default;
    virtual void reset() = 0;
    virtual void on_remote_candidate(const IceCandidate &candidate, uint64_t now_ms) = 0;
    virtual void start_gathering(uint64_t now_ms) = 0;
    virtual void start_checklist(uint64_t now_ms) = 0;
    virtual void poll(uint64_t now_ms) = 0;
    virtual IceGatheringState gathering_state() const = 0;
    virtual IceChecklistState checklist_state() const = 0;
    virtual IceNominationState nomination_state() const = 0;
    virtual std::vector<IceCandidate> local_candidates() const = 0;
    virtual std::vector<IceCandidate> remote_candidates() const = 0;
    virtual bool has_nominated_pair() const = 0;
    virtual IceCandidatePair nominated_pair() const = 0;
    virtual std::string last_error() const = 0;
    virtual std::vector<StunTransaction> stun_transactions() const = 0;
    virtual bool has_last_stun_transaction() const = 0;
    virtual StunTransaction last_stun_transaction() const = 0;
};

class IceTransportAdapter
{
public:
    virtual ~IceTransportAdapter() = default;
    virtual void reset() = 0;
    virtual void set_config(const IceAdapterConfig &config) = 0;
    virtual IceAdapterConfig config() const = 0;
    virtual void on_remote_candidate(const IceCandidate &candidate, uint64_t now_ms) = 0;
    virtual void start_gathering(uint64_t now_ms) = 0;
    virtual void start_checklist(uint64_t now_ms) = 0;
    virtual void poll(uint64_t now_ms) = 0;
    virtual IceGatheringState gathering_state() const = 0;
    virtual IceChecklistState checklist_state() const = 0;
    virtual IceNominationState nomination_state() const = 0;
    virtual std::vector<IceCandidate> local_candidates() const = 0;
    virtual std::vector<IceCandidate> remote_candidates() const = 0;
    virtual bool has_nominated_pair() const = 0;
    virtual IceCandidatePair nominated_pair() const = 0;
    virtual std::string last_error() const = 0;
    virtual std::vector<StunTransaction> stun_transactions() const = 0;
    virtual bool has_last_stun_transaction() const = 0;
    virtual StunTransaction last_stun_transaction() const = 0;
    virtual void set_provider(std::shared_ptr<IceTransportProvider> provider) = 0;
    virtual std::shared_ptr<IceTransportProvider> provider() const = 0;
};

class ScaffoldIceAdapter final : public IceTransportAdapter
{
public:
    void reset() override;
    void set_config(const IceAdapterConfig &config) override;
    IceAdapterConfig config() const override;
    void on_remote_candidate(const IceCandidate &candidate, uint64_t now_ms) override;
    void start_gathering(uint64_t now_ms) override;
    void start_checklist(uint64_t now_ms) override;
    void poll(uint64_t now_ms) override;
    IceGatheringState gathering_state() const override;
    IceChecklistState checklist_state() const override;
    IceNominationState nomination_state() const override;
    std::vector<IceCandidate> local_candidates() const override;
    std::vector<IceCandidate> remote_candidates() const override;
    bool has_nominated_pair() const override;
    IceCandidatePair nominated_pair() const override;
    std::string last_error() const override;
    std::vector<StunTransaction> stun_transactions() const override;
    bool has_last_stun_transaction() const override;
    StunTransaction last_stun_transaction() const override;
    void set_provider(std::shared_ptr<IceTransportProvider> provider) override;
    std::shared_ptr<IceTransportProvider> provider() const override;

private:
    IceAdapterConfig config_;
    IceGatheringState gathering_state_ = IceGatheringState::new_;
    IceChecklistState checklist_state_ = IceChecklistState::idle;
    uint64_t gathering_started_at_ms_ = 0;
    uint64_t checklist_started_at_ms_ = 0;
    std::vector<IceCandidate> local_candidates_;
    std::vector<IceCandidate> remote_candidates_;
    IceNominationState nomination_state_ = IceNominationState::none;
    std::optional<IceCandidatePair> nominated_pair_;
    std::string last_error_;
    std::vector<StunTransaction> stun_transactions_;
    std::optional<StunTransaction> last_stun_transaction_;
    std::shared_ptr<IceTransportProvider> provider_;
};

class IcePairSelector
{
public:
    virtual ~IcePairSelector() = default;
    virtual bool select(
        const std::vector<IceCandidateEndpoint> &locals,
        const std::vector<IceCandidateEndpoint> &remotes,
        IceCandidatePair &out_pair) const = 0;
};

class HighestPriorityIcePairSelector : public IcePairSelector
{
public:
    bool select(
        const std::vector<IceCandidateEndpoint> &locals,
        const std::vector<IceCandidateEndpoint> &remotes,
        IceCandidatePair &out_pair) const override;
};

class IceTransportEngine
{
public:
    virtual ~IceTransportEngine() = default;

    virtual void reset() = 0;
    virtual void set_state(IceTransportState state) = 0;
    virtual IceTransportState state() const = 0;
    virtual void on_remote_candidate(const IceCandidate &candidate, uint64_t now_ms) = 0;
    virtual void start_gathering(uint64_t now_ms)
    {
        (void) now_ms;
    }
    virtual void start_checking(uint64_t now_ms) = 0;
    virtual void poll(uint64_t now_ms) = 0;
    virtual std::size_t remote_candidate_count() const = 0;
    virtual IceGatheringState gathering_state() const
    {
        return IceGatheringState::new_;
    }
    virtual IceChecklistState checklist_state() const
    {
        return IceChecklistState::idle;
    }
    virtual IceNominationState nomination_state() const
    {
        return IceNominationState::none;
    }
    virtual std::string last_error() const
    {
        return {};
    }
    virtual std::vector<StunTransaction> stun_transactions() const
    {
        return {};
    }
    virtual bool has_last_stun_transaction() const
    {
        return false;
    }
    virtual StunTransaction last_stun_transaction() const
    {
        return {};
    }
    virtual void set_adapter(std::shared_ptr<IceTransportAdapter> adapter)
    {
        (void) adapter;
    }
    virtual void set_provider(std::shared_ptr<IceTransportProvider> provider)
    {
        (void) provider;
    }
    virtual void set_local_candidates(const std::vector<IceCandidate> &candidates)
    {
        (void) candidates;
    }
    virtual std::vector<IceCandidate> local_candidates() const
    {
        return {};
    }
    virtual std::vector<IceCandidate> remote_candidates() const
    {
        return {};
    }
    virtual bool has_selected_pair() const
    {
        return false;
    }
    virtual IceCandidatePair selected_pair() const
    {
        return {};
    }
    virtual void set_pair_selector(std::shared_ptr<IcePairSelector> selector)
    {
        (void) selector;
    }
    virtual void set_nomination_from_signal(
        IceNominationState state,
        IceSelectedPairReason reason,
        const std::string &reason_text,
        const std::string &nomination_transaction_id)
    {
        (void) state;
        (void) reason;
        (void) reason_text;
        (void) nomination_transaction_id;
    }
};

struct MockIceTransportConfig
{
    uint64_t connect_delay_ms = 80;
    uint64_t fail_timeout_ms = 1000;
    bool auto_start_on_candidate = true;
};

struct MockIceProviderConfig
{
    uint64_t gather_delay_ms = 25;
    uint64_t checklist_delay_ms = 80;
    uint64_t stun_response_delay_ms = 30;
    uint64_t stun_timeout_ms = 120;
    uint64_t stun_retry_interval_ms = 20;
    uint32_t stun_max_retransmits = 2;
    bool emit_srflx_candidate = true;
    bool force_stun_timeout = false;
};

class MockIceProvider final : public IceTransportProvider
{
public:
    void set_config(const MockIceProviderConfig &config);
    MockIceProviderConfig config() const;
    void reset() override;
    void on_remote_candidate(const IceCandidate &candidate, uint64_t now_ms) override;
    void start_gathering(uint64_t now_ms) override;
    void start_checklist(uint64_t now_ms) override;
    void poll(uint64_t now_ms) override;
    IceGatheringState gathering_state() const override;
    IceChecklistState checklist_state() const override;
    IceNominationState nomination_state() const override;
    std::vector<IceCandidate> local_candidates() const override;
    std::vector<IceCandidate> remote_candidates() const override;
    bool has_nominated_pair() const override;
    IceCandidatePair nominated_pair() const override;
    std::string last_error() const override;
    std::vector<StunTransaction> stun_transactions() const override;
    bool has_last_stun_transaction() const override;
    StunTransaction last_stun_transaction() const override;

private:
    void start_stun_transaction(uint64_t now_ms);

    MockIceProviderConfig config_;
    IceGatheringState gathering_state_ = IceGatheringState::new_;
    IceChecklistState checklist_state_ = IceChecklistState::idle;
    IceNominationState nomination_state_ = IceNominationState::none;
    uint64_t gathering_started_at_ms_ = 0;
    uint64_t checklist_started_at_ms_ = 0;
    std::vector<IceCandidate> local_candidates_;
    std::vector<IceCandidate> remote_candidates_;
    std::optional<IceCandidatePair> nominated_pair_;
    std::string last_error_;
    std::vector<StunTransaction> stun_transactions_;
    std::vector<std::size_t> active_stun_indices_;
};

class MockIceTransport : public IceTransportEngine
{
public:
    void set_config(const MockIceTransportConfig &config);
    MockIceTransportConfig config() const;

    void reset() override;
    void set_state(IceTransportState state) override;
    IceTransportState state() const override;

    void on_remote_candidate(const IceCandidate &candidate, uint64_t now_ms) override;
    void start_checking(uint64_t now_ms) override;
    void poll(uint64_t now_ms) override;

    std::size_t remote_candidate_count() const override;
    void set_local_candidates(const std::vector<IceCandidate> &candidates) override;
    std::vector<IceCandidate> local_candidates() const override;
    std::vector<IceCandidate> remote_candidates() const override;
    bool has_selected_pair() const override;
    IceCandidatePair selected_pair() const override;
    void set_pair_selector(std::shared_ptr<IcePairSelector> selector) override;
    void start_gathering(uint64_t now_ms) override;
    IceGatheringState gathering_state() const override;
    IceChecklistState checklist_state() const override;
    IceNominationState nomination_state() const override;
    std::string last_error() const override;
    std::vector<StunTransaction> stun_transactions() const override;
    bool has_last_stun_transaction() const override;
    StunTransaction last_stun_transaction() const override;
    void set_adapter(std::shared_ptr<IceTransportAdapter> adapter) override;
    void set_provider(std::shared_ptr<IceTransportProvider> provider) override;
    void set_nomination_from_signal(
        IceNominationState state,
        IceSelectedPairReason reason,
        const std::string &reason_text,
        const std::string &nomination_transaction_id) override;

private:
    MockIceTransportConfig config_;
    IceTransportState state_ = IceTransportState::new_;
    uint64_t checking_started_at_ms_ = 0;
    std::size_t remote_candidate_count_ = 0;
    std::vector<IceCandidate> local_candidates_;
    std::vector<IceCandidate> remote_candidates_;
    std::optional<IceCandidatePair> selected_pair_;
    std::shared_ptr<IcePairSelector> pair_selector_;
    std::shared_ptr<IceTransportAdapter> adapter_;
    IceGatheringState gathering_state_ = IceGatheringState::new_;
    IceChecklistState checklist_state_ = IceChecklistState::idle;
    IceNominationState nomination_state_ = IceNominationState::none;
    std::string last_error_;
    std::vector<StunTransaction> stun_transactions_;
    std::optional<StunTransaction> last_stun_transaction_;
};

} // namespace yuan::net::webrtc

#endif
