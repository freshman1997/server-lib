#include "filesync_config.h"
#include "filesync_common.h"
#include "filesync_protocol.h"
#include "filesync_scan.h"
#include "filesync_transfer.h"

#include "buffer/byte_buffer.h"
#include "coroutine/runtime_view.hpp"
#include "coroutine/task.h"
#include "net/runtime/network_runtime.h"
#include "net/session/stream_client_session.h"
#include "net/session/stream_server_session.h"

#include <functional>
#include <iostream>
#include <memory>
#include <unordered_map>

namespace filesync::peer {

void send(yuan::net::ConnectionContext& context, const std::string& text) {
    if (!context.try_append_output(text)) throw std::runtime_error("filesync output buffer full");
    context.flush();
}

void send(yuan::net::StreamClientSession& session, const std::string& text) {
    auto context = session.context();
    if (!context.try_append_output(text)) throw std::runtime_error("filesync output buffer full");
    session.flush();
}

class ContextLineWriter final : public transfer::LineWriter {
public:
    explicit ContextLineWriter(yuan::net::ConnectionContext& value) : context_(value) {}
    void write_line(const std::string& line) override { pending_ += line + "\n"; if (pending_.size() >= limits::line_flush_size) flush(); }
    void flush() override { if (!pending_.empty()) { if (!context_.try_append_output(pending_)) throw std::runtime_error("filesync output buffer full"); context_.flush(); pending_.clear(); } }
private:
    yuan::net::ConnectionContext& context_;
    std::string pending_;
};

class SessionLineWriter final : public transfer::LineWriter {
public:
    explicit SessionLineWriter(yuan::net::StreamClientSession& value) : session_(value) {}
    void write_line(const std::string& line) override { pending_ += line + "\n"; if (pending_.size() >= limits::line_flush_size) flush(); }
    void flush() override { if (!pending_.empty()) { if (!session_.context().try_append_output(pending_)) throw std::runtime_error("filesync output buffer full"); session_.flush(); pending_.clear(); } }
private:
    yuan::net::StreamClientSession& session_;
    std::string pending_;
};

class Service {
public:
    explicit Service(model::Config config) : config_(std::move(config)) {}

    bool start() {
        server_.set_connected_callback(on_connected);
        server_.set_read_callback(std::bind(&Service::on_read, this, std::placeholders::_1));
        server_.set_write_callback(std::bind(&Service::on_write, this, std::placeholders::_1));
        server_.set_close_callback(std::bind(&Service::on_close, this, std::placeholders::_1));
        server_.set_error_callback(std::bind(&Service::on_close, this, std::placeholders::_1));
        if (!server_.bind(config_.listen_host, config_.listen_port, runtime_)) return false;
        if (!config_.peer_host.empty()) {
            runtime_.schedule(0, std::bind(&Service::start_outbound, this));
            if (config_.sync_mode != "send_only") {
                timer_ = runtime_.schedule_periodic(static_cast<std::uint32_t>(config_.scan_interval_ms),
                    static_cast<std::uint32_t>(config_.scan_interval_ms), std::bind(&Service::start_outbound, this), -1);
            }
        }
        std::cout << "filesync v4 listening on " << config_.listen_host << ':' << config_.listen_port << '\n';
        runtime_.run();
        return true;
    }

private:
    enum class State { Manifest, Files, Need, Ack };
    struct Inbound { State state = State::Manifest; std::string buffer; model::Manifest manifest; std::vector<model::NeedRequest> requests; std::vector<std::string> need_lines; transfer::Receiver receiver; std::unique_ptr<transfer::FileSender> sender; std::size_t expected = 0; std::size_t entries = 0; bool hello_received = false; bool manifest_started = false; bool need_started = false; explicit Inbound(const model::Config& c) : receiver(c) {} };

    static void on_connected(yuan::net::ConnectionContext& context) {
        context.set_max_packet_size(limits::network_packet_size);
        context.native_handle()->set_max_output_buffer_size(limits::network_output_buffer_size);
    }
    void on_close(yuan::net::ConnectionContext& context) { inbound_.erase(context.connection_id()); }

    void pump_inbound_transfer(yuan::net::ConnectionContext& context, Inbound& state) {
        if (!state.sender) return;
        std::string output;
        std::string line;
        while (output.size() < limits::transfer_batch_size && state.sender->next_line(line)) {
            output += line;
            output.push_back('\n');
        }
        if (!output.empty()) {
            if (!context.try_append_output(output)) throw std::runtime_error("filesync output buffer full");
            context.flush();
            if (context.output_readable_bytes() == 0) {
                runtime_.schedule(0, std::bind(&Service::resume_inbound_transfer, this, context));
            }
            return;
        }
        std::cout << "filesync send complete full=" << state.sender->full_count()
                  << " delta=" << state.sender->delta_count() << std::endl;
        state.sender.reset();
        state.state = State::Ack;
    }

    void resume_inbound_transfer(yuan::net::ConnectionContext context) {
        on_write(context);
    }

    void on_write(yuan::net::ConnectionContext& context) {
        const auto found = inbound_.find(context.connection_id());
        if (found == inbound_.end() || !found->second.sender) return;
        try {
            pump_inbound_transfer(context, found->second);
        } catch (const std::exception& error) {
            std::cerr << "inbound filesync send error: " << error.what() << '\n';
            context.close();
        }
    }

    static std::string input(yuan::net::ConnectionContext& context) {
        const auto buffer = context.take_input_byte_buffer();
        const auto span = buffer.readable_span();
        return std::string(span.begin(), span.end());
    }

    void on_read(yuan::net::ConnectionContext& context) {
        auto found = inbound_.find(context.connection_id());
        if (found == inbound_.end()) found = inbound_.try_emplace(context.connection_id(), config_).first;
        auto& state = found->second;
        state.buffer += input(context);
        if (state.buffer.size() > protocol::max_control_buffer) {
            std::cerr << "inbound filesync frame exceeds limit\n";
            context.close();
            return;
        }
        while (true) {
            const auto end = state.buffer.find('\n');
            if (end == std::string::npos) return;
            if (end > protocol::max_line_size) { context.close(); return; }
            std::string frame = state.buffer.substr(0, end);
            state.buffer.erase(0, end + 1);
            try {
                if (state.state == State::Manifest) {
                    if (frame.rfind("HELLO ", 0) == 0) { const auto p = split_line(frame); if (state.hello_received || p.size() != 3 || p[0] != "HELLO" || p[1] != "filesync/4" || unquote_token(p[2]) != config_.token) throw std::runtime_error("unauthorized peer"); state.hello_received = true; }
                    else if (frame.rfind("MANIFEST_BEGIN ", 0) == 0) { if (!state.hello_received || state.manifest_started) throw std::runtime_error("invalid manifest start"); state.manifest_started = true; state.manifest.clear(); state.expected = protocol::number(frame.substr(15)); if (state.expected > limits::max_manifest_entries) throw std::runtime_error("manifest is too large"); state.entries = 0; }
                    else if (frame == "MANIFEST_END") { if (!state.hello_received || !state.manifest_started || state.entries != state.expected) throw std::runtime_error("manifest count mismatch"); ContextLineWriter writer(context); transfer::send_need(config_, state.manifest, writer); writer.flush(); state.state = State::Files; }
                    else { std::string path; auto value = protocol::parse_manifest_entry(frame, path); if (!state.manifest.emplace(path, value).second) throw std::runtime_error("duplicate manifest path"); ++state.entries; }
                } else if (state.state == State::Files) {
                    if (state.receiver.apply_line(frame)) {
                        state.manifest = scan::paths(config_);
                        config::save_state(config_, state.manifest);
                        if (config_.sync_mode == "receive_only") {
                            send(context, "READY\n");
                            state.state = State::Ack;
                            state.receiver.reset();
                            continue;
                        }
                        ContextLineWriter manifest_writer(context);
                        manifest_writer.write_line("HELLO filesync/4 " + quote_token(config_.token));
                        manifest_writer.write_line("MANIFEST_BEGIN " + std::to_string(state.manifest.size()));
                        for (const auto& entry : state.manifest) manifest_writer.write_line(protocol::encode_manifest_entry(entry.first, entry.second));
                        manifest_writer.write_line("MANIFEST_END"); manifest_writer.flush();
                        std::cout << "filesync sync round complete inbound entries=" << state.receiver.completed() << std::endl;
                        state.state = State::Need;
                        state.receiver.reset();
                    }
                } else if (state.state == State::Need) {
                    if (frame.rfind("NEED_BEGIN ", 0) == 0) { if (state.need_started) throw std::runtime_error("invalid need start"); state.need_started = true; state.requests.clear(); state.need_lines = {"NEED " + frame.substr(11)}; state.expected = protocol::number(frame.substr(11)); if (state.expected > limits::max_transfer_entries) throw std::runtime_error("need is too large"); }
                    else if (frame == "NEED_END") {
                        state.need_lines.push_back("END");
                        if (!state.need_started) throw std::runtime_error("need has no header");
                        state.requests = protocol::parse_need(state.need_lines);
                        state.manifest = scan::paths(config_);
                        state.sender = std::make_unique<transfer::FileSender>(config_, state.manifest, state.requests);
                        pump_inbound_transfer(context, state);
                    }
                    else { state.need_lines.push_back(frame); }
                } else {
                    if (frame != "OK") throw std::runtime_error("invalid sync acknowledgement");
                    send(context, "READY\n");
                    state.state = State::Manifest;
                    state.hello_received = false;
                    state.manifest_started = false;
                    state.need_started = false;
                }
                    } catch (const std::exception& error) {
                std::cerr << "inbound filesync error: " << error.what() << '\n';
                context.close();
                return;
            }
        }
    }

    yuan::coroutine::Task<std::string> line(yuan::net::StreamClientSession& session, std::string& buffer) {
        while (true) {
            const auto end = buffer.find('\n');
            if (end != std::string::npos) {
                if (end > limits::max_line_size) throw std::runtime_error("sync line exceeds limit");
                std::string result = buffer.substr(0, end);
                buffer.erase(0, end + 1);
                co_return result;
            }
            if (buffer.size() > protocol::max_control_buffer) throw std::runtime_error("sync control buffer exceeds limit");
            auto packet = co_await session.read_async();
            const auto span = packet.readable_span();
            if (span.empty()) throw std::runtime_error("peer closed");
            buffer.append(span.begin(), span.end());
        }
    }

    yuan::coroutine::Task<void> receive_files(yuan::net::StreamClientSession& session, std::string& buffer) {
        transfer::Receiver receiver(config_);
        while (true) {
            const auto value = co_await line(session, buffer);
            if (receiver.apply_line(value)) co_return;
        }
    }

    yuan::coroutine::Task<std::string> control_line(yuan::net::StreamClientSession& session, std::string& buffer) { co_return co_await line(session, buffer); }

    yuan::coroutine::Task<void> send_files(yuan::net::StreamClientSession& session,
                                            const model::Manifest& manifest,
                                            const std::vector<model::NeedRequest>& requests) {
        transfer::FileSender sender(config_, manifest, requests);
        std::string line_value;
        while (true) {
            std::string output;
            while (output.size() < limits::transfer_batch_size && sender.next_line(line_value)) {
                output += line_value;
                output.push_back('\n');
            }
            if (output.empty()) break;
            auto context = session.context();
            if (!context.try_append_output(output)) throw std::runtime_error("filesync output buffer full");
            const auto result = co_await session.runtime_view().flush(context.shared_handle());
            if (result.status != yuan::coroutine::IoStatus::success) {
                throw std::runtime_error("filesync output flush failed");
            }
            co_await session.runtime_view().schedule();
        }
        std::cout << "filesync send complete full=" << sender.full_count()
                  << " delta=" << sender.delta_count() << std::endl;
    }

    yuan::coroutine::Task<void> outbound() {
        if (running_) co_return;
        running_ = true;
        try {
            if (!client_) client_ = std::make_shared<yuan::net::StreamClientSession>();
            if (!client_->is_connected() && !co_await client_->connect_async(runtime_.runtime_view().raw(), config_.peer_host, config_.peer_port, 0)) throw std::runtime_error("connect failed");
            client_->context().set_max_packet_size(limits::network_packet_size);
            client_->context().native_handle()->set_max_output_buffer_size(limits::network_output_buffer_size);
            const auto manifest = scan::paths(config_);
            SessionLineWriter manifest_writer(*client_);
            manifest_writer.write_line("HELLO filesync/4 " + quote_token(config_.token));
            manifest_writer.write_line("MANIFEST_BEGIN " + std::to_string(manifest.size()));
            for (const auto& entry : manifest) manifest_writer.write_line(protocol::encode_manifest_entry(entry.first, entry.second));
            manifest_writer.write_line("MANIFEST_END"); manifest_writer.flush();
            std::string buffer;
            std::vector<model::NeedRequest> requests;
            std::vector<std::string> need_lines;
            while (true) {
                const auto value = co_await line(*client_, buffer);
                if (value == "NEED_END") break;
                if (value.rfind("NEED_BEGIN ", 0) == 0) { need_lines = {"NEED " + value.substr(11)}; continue; }
                need_lines.push_back(value);
            }
            need_lines.push_back("END");
            requests = protocol::parse_need(need_lines);
            co_await send_files(*client_, manifest, requests);
            if (config_.sync_mode == "send_only") {
                if (co_await line(*client_, buffer) != "READY") throw std::runtime_error("peer did not finish receive-only sync");
                auto context = client_->context();
                if (!context.try_append_output("OK\n")) throw std::runtime_error("filesync output buffer full");
                const auto flush_result = co_await client_->runtime_view().flush(context.shared_handle());
                if (flush_result.status != yuan::coroutine::IoStatus::success) throw std::runtime_error("filesync final acknowledgement failed");
                config::save_state(config_, manifest);
                std::cout << "filesync send-only sync complete" << std::endl;
                running_ = false;
                runtime_.stop();
                co_return;
            }
            model::Manifest reverse; std::size_t reverse_expected = 0; std::size_t reverse_entries = 0;
            while (true) {
                const auto value = co_await line(*client_, buffer);
                if (value == "MANIFEST_END") {
                    if (reverse_entries != reverse_expected) throw std::runtime_error("reverse manifest count mismatch");
                    break;
                }
                if (value.rfind("HELLO ", 0) == 0) continue;
                if (value.rfind("MANIFEST_BEGIN ", 0) == 0) { reverse_expected = protocol::number(value.substr(15)); continue; }
                std::string path;
                auto entry = protocol::parse_manifest_entry(value, path);
                if (!reverse.emplace(path, entry).second) throw std::runtime_error("duplicate reverse manifest path");
                ++reverse_entries;
            }
            SessionLineWriter need_writer(*client_); transfer::send_need(config_, reverse, need_writer); need_writer.flush();
            co_await receive_files(*client_, buffer);
            send(*client_, "OK\n");
            if (co_await line(*client_, buffer) != "READY") throw std::runtime_error("peer did not finish");
            config::save_state(config_, scan::paths(config_));
            std::cout << "filesync sync round complete outbound" << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "outbound filesync error: " << error.what() << '\n';
            if (client_) client_->close();
        }
        running_ = false;
        co_return;
    }

    void start_outbound() {
        if (task_ && !task_->done()) return;
        task_.reset();
        task_ = std::make_unique<yuan::coroutine::Task<void>>(outbound());
        task_->resume();
    }

    model::Config config_;
    yuan::net::NetworkRuntime runtime_;
    yuan::net::StreamServerSession server_;
    std::unordered_map<uintptr_t, Inbound> inbound_;
    std::shared_ptr<yuan::net::StreamClientSession> client_;
    std::unique_ptr<yuan::coroutine::Task<void>> task_;
    yuan::timer::TimerHandle timer_;
    bool running_ = false;
};
}

class ReleaseFileSyncPeerApp {
public:
    int start(int argc, char** argv) {
#ifdef FILESYNC_DEFAULT_CONFIG
        const std::filesystem::path default_config = FILESYNC_DEFAULT_CONFIG;
#else
        const std::filesystem::path default_config = "peer_config.json";
#endif
        try { return filesync::peer::Service(filesync::config::load(argc > 1 ? argv[1] : default_config)).start() ? 0 : 1; }
        catch (const std::exception& error) { std::cerr << "filesync error: " << error.what() << '\n'; return 1; }
    }
};

#ifndef FILESYNC_PEER_TESTING
int main(int argc, char** argv) { return ReleaseFileSyncPeerApp().start(argc, argv); }
#endif
