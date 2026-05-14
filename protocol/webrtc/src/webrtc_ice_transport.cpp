#include "webrtc_ice_transport.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace
{

using namespace yuan::net::webrtc;

std::string to_lower_copy(const std::string &value)
{
    std::string out = value;
    for (char &ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool parse_u32(const std::string &text, uint32_t &out)
{
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

bool parse_u16(const std::string &text, uint16_t &out)
{
    uint32_t value = 0;
    if (!parse_u32(text, value) || value > 65535u) {
        return false;
    }
    out = static_cast<uint16_t>(value);
    return true;
}

std::string make_mock_transaction_id(uint64_t now_ms)
{
    static uint64_t counter = 1;
    return std::string("mock-stun-") + std::to_string(now_ms) + "-" + std::to_string(counter++);
}

std::vector<std::string> tokenize_candidate(const std::string &candidate_text)
{
    std::vector<std::string> tokens;
    std::istringstream iss(candidate_text);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

void ensure_default_pair_selector(std::shared_ptr<IcePairSelector> &selector)
{
    if (!selector) {
        selector = std::make_shared<HighestPriorityIcePairSelector>();
    }
}

IceCandidate normalize_candidate(const IceCandidate &candidate)
{
    IceCandidate parsed = candidate;
    if (!parsed.candidate.empty()
        && (parsed.foundation.empty() || parsed.transport.empty() || parsed.ip.empty() || parsed.port == 0)) {
        (void) parse_ice_candidate_sdp(parsed.candidate, parsed);
    }
    return parsed;
}

} // namespace

namespace yuan::net::webrtc
{

bool parse_ice_candidate_sdp(const std::string &candidate_text, IceCandidate &out_candidate)
{
    if (candidate_text.empty()) {
        return false;
    }

    const std::vector<std::string> tokens = tokenize_candidate(candidate_text);
    if (tokens.size() < 8) {
        return false;
    }

    const std::string foundation_prefix = "candidate:";
    if (tokens[0].rfind(foundation_prefix, 0) != 0 || tokens[0].size() <= foundation_prefix.size()) {
        return false;
    }

    IceCandidate parsed = out_candidate;
    parsed.candidate = candidate_text;
    parsed.foundation = tokens[0].substr(foundation_prefix.size());
    if (!parse_u32(tokens[1], parsed.component)) {
        return false;
    }
    parsed.transport = to_lower_copy(tokens[2]);
    if (!parse_u32(tokens[3], parsed.priority)) {
        return false;
    }
    parsed.ip = tokens[4];
    if (!parse_u16(tokens[5], parsed.port)) {
        return false;
    }
    if (tokens[6] != "typ") {
        return false;
    }
    parsed.type = to_lower_copy(tokens[7]);

    for (std::size_t i = 8; i + 1 < tokens.size(); i += 2) {
        const std::string key = to_lower_copy(tokens[i]);
        const std::string &value = tokens[i + 1];
        if (key == "raddr") {
            parsed.related_address = value;
        } else if (key == "rport") {
            uint16_t related_port = 0;
            if (!parse_u16(value, related_port)) {
                return false;
            }
            parsed.related_port = related_port;
        }
    }

    out_candidate = std::move(parsed);
    return true;
}

IceCandidateEndpoint to_candidate_endpoint(const IceCandidate &candidate)
{
    const IceCandidate parsed = normalize_candidate(candidate);
    IceCandidateEndpoint endpoint;
    endpoint.foundation = parsed.foundation;
    endpoint.component = parsed.component;
    endpoint.transport = parsed.transport;
    endpoint.priority = parsed.priority;
    endpoint.ip = parsed.ip;
    endpoint.port = parsed.port;
    endpoint.type = parsed.type;
    endpoint.related_address = parsed.related_address;
    endpoint.related_port = parsed.related_port;
    return endpoint;
}

bool HighestPriorityIcePairSelector::select(
    const std::vector<IceCandidateEndpoint> &locals,
    const std::vector<IceCandidateEndpoint> &remotes,
    IceCandidatePair &out_pair) const
{
    if (locals.empty() || remotes.empty()) {
        return false;
    }

    const IceCandidateEndpoint *best_local = &locals.front();
    for (const auto &local : locals) {
        if (local.priority > best_local->priority) {
            best_local = &local;
        }
    }
    const IceCandidateEndpoint *best_remote = &remotes.front();
    for (const auto &remote : remotes) {
        if (remote.priority > best_remote->priority) {
            best_remote = &remote;
        }
    }

    out_pair.local = *best_local;
    out_pair.remote = *best_remote;
    out_pair.reason = IceSelectedPairReason::highest_priority;
    out_pair.reason_text = "highest_priority";
    out_pair.nomination_transaction_id.clear();
    return true;
}

void MockIceProvider::set_config(const MockIceProviderConfig &config)
{
    config_ = config;
    if (config_.gather_delay_ms == 0) {
        config_.gather_delay_ms = 1;
    }
    if (config_.checklist_delay_ms == 0) {
        config_.checklist_delay_ms = 1;
    }
    if (config_.stun_response_delay_ms == 0) {
        config_.stun_response_delay_ms = 1;
    }
    if (config_.stun_timeout_ms == 0) {
        config_.stun_timeout_ms = 1;
    }
    if (config_.stun_retry_interval_ms == 0) {
        config_.stun_retry_interval_ms = 1;
    }
}

MockIceProviderConfig MockIceProvider::config() const
{
    return config_;
}

void MockIceProvider::start_stun_transaction(uint64_t now_ms)
{
    StunTransaction tx;
    tx.transaction_id = make_mock_transaction_id(now_ms);
    tx.state = StunTransactionState::request_sent;
    tx.started_at_ms = now_ms;
    tx.last_request_at_ms = now_ms;
    tx.completed_at_ms = 0;
    tx.request_count = 1;
    tx.retransmit_count = 0;
    tx.request_priority = 1862270975u;
    tx.request_use_candidate = false;
    tx.error.clear();
    tx.response_code.clear();
    tx.mapped_address.clear();
    tx.mapped_port = 0;
    stun_transactions_.push_back(tx);
    active_stun_indices_.push_back(stun_transactions_.size() - 1);
}

void MockIceProvider::reset()
{
    gathering_state_ = IceGatheringState::new_;
    checklist_state_ = IceChecklistState::idle;
    nomination_state_ = IceNominationState::none;
    gathering_started_at_ms_ = 0;
    checklist_started_at_ms_ = 0;
    local_candidates_.clear();
    remote_candidates_.clear();
    nominated_pair_.reset();
    last_error_.clear();
    stun_transactions_.clear();
    active_stun_indices_.clear();
}

void MockIceProvider::on_remote_candidate(const IceCandidate &candidate, uint64_t now_ms)
{
    IceCandidate parsed = normalize_candidate(candidate);
    if (parsed.candidate.empty()) {
        return;
    }
    remote_candidates_.push_back(parsed);
    if (checklist_state_ == IceChecklistState::running) {
        start_stun_transaction(now_ms);
    }
    if (checklist_state_ == IceChecklistState::idle) {
        start_checklist(now_ms);
    }
}

void MockIceProvider::start_gathering(uint64_t now_ms)
{
    if (gathering_state_ == IceGatheringState::complete) {
        return;
    }
    gathering_state_ = IceGatheringState::gathering;
    gathering_started_at_ms_ = now_ms;
}

void MockIceProvider::start_checklist(uint64_t now_ms)
{
    if (checklist_state_ == IceChecklistState::completed || checklist_state_ == IceChecklistState::failed) {
        return;
    }
    checklist_state_ = IceChecklistState::running;
    nomination_state_ = IceNominationState::in_progress;
    checklist_started_at_ms_ = now_ms;
    start_stun_transaction(now_ms);
}

void MockIceProvider::poll(uint64_t now_ms)
{
    if (gathering_state_ == IceGatheringState::gathering) {
        const uint64_t elapsed = now_ms >= gathering_started_at_ms_ ? (now_ms - gathering_started_at_ms_) : 0;
        if (elapsed >= config_.gather_delay_ms) {
            local_candidates_.clear();

            IceCandidate host;
            host.candidate = "candidate:1 1 udp 2130706431 192.168.0.2 54000 typ host";
            (void) parse_ice_candidate_sdp(host.candidate, host);
            local_candidates_.push_back(host);

            if (config_.emit_srflx_candidate) {
                IceCandidate srflx;
                srflx.candidate = "candidate:2 1 udp 1686052607 203.0.113.5 62000 typ srflx raddr 192.168.0.2 rport 54000";
                (void) parse_ice_candidate_sdp(srflx.candidate, srflx);
                local_candidates_.push_back(srflx);
            }

            gathering_state_ = IceGatheringState::complete;
        }
    }

    for (std::size_t i = 0; i < active_stun_indices_.size();) {
        const std::size_t tx_index = active_stun_indices_[i];
        if (tx_index >= stun_transactions_.size()) {
            active_stun_indices_.erase(active_stun_indices_.begin() + i);
            continue;
        }

        StunTransaction &tx = stun_transactions_[tx_index];
        if (tx.state != StunTransactionState::request_sent) {
            active_stun_indices_.erase(active_stun_indices_.begin() + i);
            continue;
        }

        const uint64_t total_elapsed = now_ms >= tx.started_at_ms ? (now_ms - tx.started_at_ms) : 0;
        const uint64_t retry_elapsed = now_ms >= tx.last_request_at_ms ? (now_ms - tx.last_request_at_ms) : 0;

        if (config_.stun_retry_interval_ms > 0 && tx.retransmit_count < config_.stun_max_retransmits) {
            const uint64_t due_retries = retry_elapsed / config_.stun_retry_interval_ms;
            if (due_retries > 0) {
                const uint64_t remaining = static_cast<uint64_t>(config_.stun_max_retransmits - tx.retransmit_count);
                const uint64_t apply = std::min(due_retries, remaining);
                tx.retransmit_count += static_cast<uint32_t>(apply);
                tx.request_count += static_cast<uint32_t>(apply);
                tx.last_request_at_ms += (apply * config_.stun_retry_interval_ms);
            }
        }

        if (config_.force_stun_timeout && total_elapsed >= config_.stun_timeout_ms) {
            tx.state = StunTransactionState::timed_out;
            tx.completed_at_ms = now_ms;
            tx.error = "stun_timeout";
            tx.response_code = "timeout";
            active_stun_indices_.erase(active_stun_indices_.begin() + i);
            continue;
        }

        if (!config_.force_stun_timeout && total_elapsed >= config_.stun_response_delay_ms) {
            tx.state = StunTransactionState::response_received;
            tx.completed_at_ms = now_ms;
            tx.error.clear();
            tx.response_code = "success";
            tx.mapped_address = "203.0.113.5";
            tx.mapped_port = static_cast<uint16_t>(62000 + (tx_index % 10));
            active_stun_indices_.erase(active_stun_indices_.begin() + i);
            continue;
        }

        ++i;
    }

    if (checklist_state_ == IceChecklistState::running) {
        const uint64_t elapsed = now_ms >= checklist_started_at_ms_ ? (now_ms - checklist_started_at_ms_) : 0;
        if (elapsed >= config_.checklist_delay_ms) {
            bool has_stun_timeout = false;
            bool has_stun_response = false;
            for (const auto &tx : stun_transactions_) {
                if (tx.state == StunTransactionState::timed_out) {
                    has_stun_timeout = true;
                }
                if (tx.state == StunTransactionState::response_received) {
                    has_stun_response = true;
                }
            }

            if (has_stun_timeout && !has_stun_response) {
                checklist_state_ = IceChecklistState::failed;
                nomination_state_ = IceNominationState::failed;
                nominated_pair_.reset();
                last_error_ = "provider_stun_timeout";
            } else if (remote_candidates_.empty() || local_candidates_.empty()) {
                checklist_state_ = IceChecklistState::failed;
                nomination_state_ = IceNominationState::failed;
                nominated_pair_.reset();
                last_error_ = "provider_no_viable_pair";
            } else {
                checklist_state_ = IceChecklistState::completed;
                nomination_state_ = IceNominationState::nominated;
                IceCandidatePair pair;
                pair.local = to_candidate_endpoint(local_candidates_.front());
                pair.remote = to_candidate_endpoint(remote_candidates_.front());
                pair.reason = IceSelectedPairReason::nominated_by_provider;
                pair.reason_text = "provider_nominated";
                pair.nomination_transaction_id = has_last_stun_transaction() ? last_stun_transaction().transaction_id : std::string{};
                nominated_pair_ = pair;
                last_error_.clear();
            }
        }
    }
}

IceGatheringState MockIceProvider::gathering_state() const
{
    return gathering_state_;
}

IceChecklistState MockIceProvider::checklist_state() const
{
    return checklist_state_;
}

IceNominationState MockIceProvider::nomination_state() const
{
    return nomination_state_;
}

std::vector<IceCandidate> MockIceProvider::local_candidates() const
{
    return local_candidates_;
}

std::vector<IceCandidate> MockIceProvider::remote_candidates() const
{
    return remote_candidates_;
}

bool MockIceProvider::has_nominated_pair() const
{
    return nominated_pair_.has_value();
}

IceCandidatePair MockIceProvider::nominated_pair() const
{
    return nominated_pair_.value_or(IceCandidatePair{});
}

std::string MockIceProvider::last_error() const
{
    return last_error_;
}

std::vector<StunTransaction> MockIceProvider::stun_transactions() const
{
    return stun_transactions_;
}

bool MockIceProvider::has_last_stun_transaction() const
{
    return !stun_transactions_.empty();
}

StunTransaction MockIceProvider::last_stun_transaction() const
{
    if (stun_transactions_.empty()) {
        return {};
    }
    return stun_transactions_.back();
}

void ScaffoldIceAdapter::reset()
{
    gathering_state_ = IceGatheringState::new_;
    checklist_state_ = IceChecklistState::idle;
    nomination_state_ = IceNominationState::none;
    gathering_started_at_ms_ = 0;
    checklist_started_at_ms_ = 0;
    local_candidates_.clear();
    remote_candidates_.clear();
    nominated_pair_.reset();
    last_error_.clear();
    stun_transactions_.clear();
    last_stun_transaction_.reset();
    if (provider_) {
        provider_->reset();
    }
}

void ScaffoldIceAdapter::set_config(const IceAdapterConfig &config)
{
    config_ = config;
    if (config_.gather_delay_ms == 0) {
        config_.gather_delay_ms = 1;
    }
    if (config_.checklist_delay_ms == 0) {
        config_.checklist_delay_ms = 1;
    }
}

IceAdapterConfig ScaffoldIceAdapter::config() const
{
    return config_;
}

void ScaffoldIceAdapter::on_remote_candidate(const IceCandidate &candidate, uint64_t now_ms)
{
    const IceCandidate parsed = normalize_candidate(candidate);
    if (parsed.candidate.empty()) {
        return;
    }
    remote_candidates_.push_back(parsed);
    if (provider_) {
        provider_->on_remote_candidate(parsed, now_ms);
    }
    if (checklist_state_ == IceChecklistState::idle) {
        start_checklist(now_ms);
    }
}

void ScaffoldIceAdapter::start_gathering(uint64_t now_ms)
{
    if (provider_) {
        provider_->start_gathering(now_ms);
        gathering_state_ = provider_->gathering_state();
        return;
    }
    if (gathering_state_ == IceGatheringState::complete) {
        return;
    }
    gathering_state_ = IceGatheringState::gathering;
    gathering_started_at_ms_ = now_ms;
}

void ScaffoldIceAdapter::start_checklist(uint64_t now_ms)
{
    if (provider_) {
        provider_->start_checklist(now_ms);
        checklist_state_ = provider_->checklist_state();
        nomination_state_ = provider_->nomination_state();
        return;
    }
    if (checklist_state_ == IceChecklistState::completed || checklist_state_ == IceChecklistState::failed) {
        return;
    }
    checklist_state_ = IceChecklistState::running;
    nomination_state_ = IceNominationState::in_progress;
    checklist_started_at_ms_ = now_ms;
    StunTransaction tx;
    tx.transaction_id = make_mock_transaction_id(now_ms);
    tx.state = StunTransactionState::request_sent;
    tx.started_at_ms = now_ms;
    tx.last_request_at_ms = now_ms;
    tx.request_count = 1;
    tx.retransmit_count = 0;
    tx.request_priority = 1862270975u;
    tx.request_use_candidate = false;
    tx.response_code.clear();
    stun_transactions_.push_back(tx);
    last_stun_transaction_ = tx;
}

void ScaffoldIceAdapter::poll(uint64_t now_ms)
{
    if (provider_) {
        provider_->poll(now_ms);
        gathering_state_ = provider_->gathering_state();
        checklist_state_ = provider_->checklist_state();
        nomination_state_ = provider_->nomination_state();
        last_error_ = provider_->last_error();
        const auto provider_locals = provider_->local_candidates();
        if (!provider_locals.empty()) {
            local_candidates_ = provider_locals;
        }
        const auto provider_remotes = provider_->remote_candidates();
        if (!provider_remotes.empty()) {
            remote_candidates_ = provider_remotes;
        }
        if (provider_->has_nominated_pair()) {
            nominated_pair_ = provider_->nominated_pair();
        }
        stun_transactions_ = provider_->stun_transactions();
        if (provider_->has_last_stun_transaction()) {
            last_stun_transaction_ = provider_->last_stun_transaction();
        } else {
            last_stun_transaction_.reset();
        }
        return;
    }

    if (gathering_state_ == IceGatheringState::gathering) {
        const uint64_t elapsed = now_ms >= gathering_started_at_ms_ ? (now_ms - gathering_started_at_ms_) : 0;
        if (elapsed >= config_.gather_delay_ms) {
            local_candidates_.clear();
            IceCandidate c1;
            c1.candidate = "candidate:1 1 udp 2130706431 192.168.0.2 54000 typ host";
            (void) parse_ice_candidate_sdp(c1.candidate, c1);
            local_candidates_.push_back(c1);
            if (config_.gather_two_host_candidates) {
                IceCandidate c2;
                c2.candidate = "candidate:2 1 udp 2130706430 10.0.0.2 54002 typ host";
                (void) parse_ice_candidate_sdp(c2.candidate, c2);
                local_candidates_.push_back(c2);
            }
            gathering_state_ = IceGatheringState::complete;
        }
    }

    if (checklist_state_ == IceChecklistState::running) {
        const uint64_t elapsed = now_ms >= checklist_started_at_ms_ ? (now_ms - checklist_started_at_ms_) : 0;
        if (last_stun_transaction_ && last_stun_transaction_->state == StunTransactionState::request_sent
            && elapsed >= (config_.checklist_delay_ms / 2)) {
            last_stun_transaction_->state = StunTransactionState::response_received;
            last_stun_transaction_->completed_at_ms = now_ms;
            last_stun_transaction_->response_code = "success";
            last_stun_transaction_->mapped_address = "203.0.113.7";
            last_stun_transaction_->mapped_port = 62002;
            if (!stun_transactions_.empty()) {
                stun_transactions_.back() = *last_stun_transaction_;
            }
        }
        if (elapsed >= config_.checklist_delay_ms) {
            if (remote_candidates_.empty()) {
                checklist_state_ = IceChecklistState::failed;
                nomination_state_ = IceNominationState::failed;
                last_error_ = "no_remote_candidates";
                nominated_pair_.reset();
            } else {
                checklist_state_ = IceChecklistState::completed;
                nomination_state_ = IceNominationState::nominated;
                IceCandidatePair pair;
                pair.local = to_candidate_endpoint(local_candidates_.empty() ? remote_candidates_.front() : local_candidates_.front());
                pair.remote = to_candidate_endpoint(remote_candidates_.front());
                pair.reason = IceSelectedPairReason::nominated_by_provider;
                pair.reason_text = "scaffold_nominated";
                pair.nomination_transaction_id = has_last_stun_transaction() ? last_stun_transaction().transaction_id : std::string{};
                nominated_pair_ = pair;
                last_error_.clear();
            }
        }
    }
}

IceGatheringState ScaffoldIceAdapter::gathering_state() const
{
    return gathering_state_;
}

IceChecklistState ScaffoldIceAdapter::checklist_state() const
{
    return checklist_state_;
}

IceNominationState ScaffoldIceAdapter::nomination_state() const
{
    return nomination_state_;
}

std::vector<IceCandidate> ScaffoldIceAdapter::local_candidates() const
{
    return local_candidates_;
}

std::vector<IceCandidate> ScaffoldIceAdapter::remote_candidates() const
{
    return remote_candidates_;
}

bool ScaffoldIceAdapter::has_nominated_pair() const
{
    return nominated_pair_.has_value();
}

IceCandidatePair ScaffoldIceAdapter::nominated_pair() const
{
    return nominated_pair_.value_or(IceCandidatePair{});
}

std::string ScaffoldIceAdapter::last_error() const
{
    return last_error_;
}

std::vector<StunTransaction> ScaffoldIceAdapter::stun_transactions() const
{
    return stun_transactions_;
}

bool ScaffoldIceAdapter::has_last_stun_transaction() const
{
    return last_stun_transaction_.has_value();
}

StunTransaction ScaffoldIceAdapter::last_stun_transaction() const
{
    return last_stun_transaction_.value_or(StunTransaction{});
}

void ScaffoldIceAdapter::set_provider(std::shared_ptr<IceTransportProvider> provider)
{
    provider_ = std::move(provider);
}

std::shared_ptr<IceTransportProvider> ScaffoldIceAdapter::provider() const
{
    return provider_;
}

void MockIceTransport::set_config(const MockIceTransportConfig &config)
{
    config_ = config;
    if (config_.connect_delay_ms == 0) {
        config_.connect_delay_ms = 1;
    }
    ensure_default_pair_selector(pair_selector_);
}

MockIceTransportConfig MockIceTransport::config() const
{
    return config_;
}

void MockIceTransport::reset()
{
    state_ = IceTransportState::new_;
    checking_started_at_ms_ = 0;
    remote_candidate_count_ = 0;
    local_candidates_.clear();
    remote_candidates_.clear();
    selected_pair_.reset();
    gathering_state_ = IceGatheringState::new_;
    checklist_state_ = IceChecklistState::idle;
    nomination_state_ = IceNominationState::none;
    last_error_.clear();
    stun_transactions_.clear();
    last_stun_transaction_.reset();
    ensure_default_pair_selector(pair_selector_);
    if (!adapter_) {
        adapter_ = std::make_shared<ScaffoldIceAdapter>();
    }
    adapter_->reset();
}

void MockIceTransport::set_state(IceTransportState state)
{
    state_ = state;
    if (state_ != IceTransportState::connected) {
        selected_pair_.reset();
    }
}

IceTransportState MockIceTransport::state() const
{
    return state_;
}

void MockIceTransport::on_remote_candidate(const IceCandidate &candidate, uint64_t now_ms)
{
    const IceCandidate parsed = normalize_candidate(candidate);
    if (parsed.candidate.empty()) {
        return;
    }
    ++remote_candidate_count_;
    remote_candidates_.push_back(parsed);
    if (!adapter_) {
        adapter_ = std::make_shared<ScaffoldIceAdapter>();
    }
    adapter_->on_remote_candidate(parsed, now_ms);
    if (state_ == IceTransportState::new_ && config_.auto_start_on_candidate) {
        start_checking(now_ms);
    }
}

void MockIceTransport::start_gathering(uint64_t now_ms)
{
    if (!adapter_) {
        adapter_ = std::make_shared<ScaffoldIceAdapter>();
    }
    adapter_->start_gathering(now_ms);
    gathering_state_ = adapter_->gathering_state();
}

void MockIceTransport::start_checking(uint64_t now_ms)
{
    if (state_ == IceTransportState::connected || state_ == IceTransportState::failed) {
        return;
    }
    state_ = IceTransportState::checking;
    checking_started_at_ms_ = now_ms;
    if (!adapter_) {
        adapter_ = std::make_shared<ScaffoldIceAdapter>();
    }
    adapter_->start_checklist(now_ms);
    checklist_state_ = adapter_->checklist_state();
    nomination_state_ = adapter_->nomination_state();
}

void MockIceTransport::poll(uint64_t now_ms)
{
    if (!adapter_) {
        adapter_ = std::make_shared<ScaffoldIceAdapter>();
    }

    adapter_->poll(now_ms);
    gathering_state_ = adapter_->gathering_state();
    checklist_state_ = adapter_->checklist_state();
    nomination_state_ = adapter_->nomination_state();
    last_error_ = adapter_->last_error();
    stun_transactions_ = adapter_->stun_transactions();
    if (adapter_->has_last_stun_transaction()) {
        last_stun_transaction_ = adapter_->last_stun_transaction();
    } else {
        last_stun_transaction_.reset();
    }

    const auto gathered = adapter_->local_candidates();
    if (local_candidates_.empty() && !gathered.empty()) {
        local_candidates_ = gathered;
    }
    const auto adapter_remotes = adapter_->remote_candidates();
    if (!adapter_remotes.empty()) {
        remote_candidates_ = adapter_remotes;
        remote_candidate_count_ = remote_candidates_.size();
    }

    if (adapter_->has_nominated_pair()) {
        IceCandidatePair pair = adapter_->nominated_pair();
        if (!local_candidates_.empty()) {
            pair.local = to_candidate_endpoint(local_candidates_.front());
        }
        if (!remote_candidates_.empty()) {
            pair.remote = to_candidate_endpoint(remote_candidates_.front());
        }
        selected_pair_ = pair;
    }

    if (state_ != IceTransportState::checking) {
        return;
    }

    if (checklist_state_ == IceChecklistState::failed) {
        state_ = IceTransportState::failed;
        if (last_error_.empty()) {
            last_error_ = "ice_checklist_failed";
        }
        return;
    }

    const uint64_t elapsed = now_ms >= checking_started_at_ms_ ? (now_ms - checking_started_at_ms_) : 0;
    if (remote_candidate_count_ > 0 && elapsed >= config_.connect_delay_ms) {
        state_ = IceTransportState::connected;
        if (!selected_pair_ && !local_candidates_.empty() && !remote_candidates_.empty()) {
            std::vector<IceCandidateEndpoint> local_endpoints;
            for (const auto &c : local_candidates_) {
                local_endpoints.push_back(to_candidate_endpoint(c));
            }
            std::vector<IceCandidateEndpoint> remote_endpoints;
            for (const auto &c : remote_candidates_) {
                remote_endpoints.push_back(to_candidate_endpoint(c));
            }

            IceCandidatePair pair;
            ensure_default_pair_selector(pair_selector_);
            if (pair_selector_->select(local_endpoints, remote_endpoints, pair)) {
                selected_pair_ = pair;
            }
        }
        return;
    }

    if (remote_candidate_count_ == 0 && elapsed >= config_.fail_timeout_ms) {
        state_ = IceTransportState::failed;
        if (last_error_.empty()) {
            last_error_ = "ice_timeout_no_remote_candidate";
        }
    }
}

std::size_t MockIceTransport::remote_candidate_count() const
{
    return remote_candidate_count_;
}

void MockIceTransport::set_local_candidates(const std::vector<IceCandidate> &candidates)
{
    local_candidates_.clear();
    local_candidates_.reserve(candidates.size());
    for (const auto &c : candidates) {
        local_candidates_.push_back(normalize_candidate(c));
    }
}

std::vector<IceCandidate> MockIceTransport::local_candidates() const
{
    return local_candidates_;
}

std::vector<IceCandidate> MockIceTransport::remote_candidates() const
{
    return remote_candidates_;
}

bool MockIceTransport::has_selected_pair() const
{
    return selected_pair_.has_value();
}

IceCandidatePair MockIceTransport::selected_pair() const
{
    return selected_pair_.value_or(IceCandidatePair{});
}

void MockIceTransport::set_pair_selector(std::shared_ptr<IcePairSelector> selector)
{
    pair_selector_ = selector ? std::move(selector) : std::make_shared<HighestPriorityIcePairSelector>();
}

IceGatheringState MockIceTransport::gathering_state() const
{
    return gathering_state_;
}

IceChecklistState MockIceTransport::checklist_state() const
{
    return checklist_state_;
}

IceNominationState MockIceTransport::nomination_state() const
{
    return nomination_state_;
}

std::string MockIceTransport::last_error() const
{
    return last_error_;
}

std::vector<StunTransaction> MockIceTransport::stun_transactions() const
{
    return stun_transactions_;
}

bool MockIceTransport::has_last_stun_transaction() const
{
    return last_stun_transaction_.has_value();
}

StunTransaction MockIceTransport::last_stun_transaction() const
{
    return last_stun_transaction_.value_or(StunTransaction{});
}

void MockIceTransport::set_adapter(std::shared_ptr<IceTransportAdapter> adapter)
{
    adapter_ = adapter ? std::move(adapter) : std::make_shared<ScaffoldIceAdapter>();
    gathering_state_ = adapter_->gathering_state();
    checklist_state_ = adapter_->checklist_state();
    nomination_state_ = adapter_->nomination_state();
    stun_transactions_ = adapter_->stun_transactions();
    if (adapter_->has_last_stun_transaction()) {
        last_stun_transaction_ = adapter_->last_stun_transaction();
    } else {
        last_stun_transaction_.reset();
    }
}

void MockIceTransport::set_provider(std::shared_ptr<IceTransportProvider> provider)
{
    if (!adapter_) {
        adapter_ = std::make_shared<ScaffoldIceAdapter>();
    }
    adapter_->set_provider(std::move(provider));
}

void MockIceTransport::set_nomination_from_signal(
    IceNominationState state,
    IceSelectedPairReason reason,
    const std::string &reason_text,
    const std::string &nomination_transaction_id)
{
    nomination_state_ = state;
    if (state == IceNominationState::nominated) {
        if (!selected_pair_) {
            IceCandidatePair pair;
            if (!local_candidates_.empty()) {
                pair.local = to_candidate_endpoint(local_candidates_.front());
            }
            if (!remote_candidates_.empty()) {
                pair.remote = to_candidate_endpoint(remote_candidates_.front());
            }
            selected_pair_ = pair;
        }
        selected_pair_->reason = reason;
        selected_pair_->reason_text = reason_text.empty() ? "forced_by_signal" : reason_text;
        selected_pair_->nomination_transaction_id = nomination_transaction_id;

        bool found = false;
        if (!nomination_transaction_id.empty()) {
            for (const auto &tx : stun_transactions_) {
                if (tx.transaction_id == nomination_transaction_id) {
                    found = true;
                    break;
                }
            }
        }
        if (!nomination_transaction_id.empty() && !found) {
            last_error_ = "nomination_transaction_not_found";
        }
    } else if (state == IceNominationState::failed) {
        selected_pair_.reset();
        if (last_error_.empty()) {
            last_error_ = "nomination_failed_by_signal";
        }
    }
}

} // namespace yuan::net::webrtc
