#include "content/parsers/chunked_content_parser.h"

#include "base/time.h"
#include "content/types.h"
#include "packet.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <system_error>

namespace yuan::net::http
{
    ChunkedContentParser::ChunkedContentParser() = default;

    ChunkedContentParser::~ChunkedContentParser()
    {
        reset();
    }

    void ChunkedContentParser::reset()
    {
        if (file_stream_) {
            file_stream_->flush();
            file_stream_->close();
            file_stream_.reset();
        }

        phase_ = ParsePhase::read_size;
        total_bytes_ = 0;
        remain_bytes_ = 0;
        size_buf_.clear();
        pending_buffer_.clear();
        pending_buffer_.shrink_to_fit();
        mem_buffer_.clear();
        mem_buffer_.shrink_to_fit();
        tmp_file_path_.clear();
        trailer_checksum_.clear();
    }

    std::size_t ChunkedContentParser::get_content_length()
    {
        return total_bytes_;
    }

    bool ChunkedContentParser::can_parse(ContentType contentType)
    {
        return contentType == ContentType::chunked;
    }

    ChunkState ChunkedContentParser::parse_chunked(HttpPacket *packet)
    {
        if (!packet) {
            return ChunkState::internal_error_;
        }

        auto incoming = packet->take_body_buffer();
        if (!incoming.empty()) {
            pending_buffer_.append(incoming);
        }

        if (pending_buffer_.empty()) {
            return ChunkState::need_more_;
        }

        ChunkState state = feed_data();

        if (state == ChunkState::completed_) {
            if (file_stream_) {
                file_stream_->flush();
            } else {
                packet->replace_body_buffer(mem_buffer_.copy_readable());
            }
            packet->set_chunked_checksum(std::move(trailer_checksum_));
        } else if (state == ChunkState::internal_error_ || state == ChunkState::invalid_chunk_) {
            if (file_stream_) {
                file_stream_->close();
                file_stream_.reset();
            }
            if (!tmp_file_path_.empty()) {
                std::error_code ec;
                std::filesystem::remove(tmp_file_path_, ec);
                tmp_file_path_.clear();
            }
        }

        return state;
    }

    ChunkState ChunkedContentParser::feed_data()
    {
        const char *pos = pending_buffer_.read_ptr();
        const char *data_end = pending_buffer_.read_ptr() + pending_buffer_.readable_bytes();

        while (pos < data_end && phase_ != ParsePhase::done && phase_ != ParsePhase::error) {
            switch (phase_) {
            case ParsePhase::read_size: {
                char ch = *pos++;
                if (ch == '\r') {
                    if (pos < data_end && *pos == '\n') {
                        ++pos;

                        std::size_t chunk_size = 0;
                        auto [ptr, ec] = std::from_chars(
                            size_buf_.data(),
                            size_buf_.data() + size_buf_.size(),
                            chunk_size,
                            16);

                        if (ec != std::errc{} || ptr != size_buf_.data() + size_buf_.size() || chunk_size > max_chunk_size_) {
                            phase_ = ParsePhase::error;
                            break;
                        }

                        size_buf_.clear();
                        remain_bytes_ = chunk_size;
                        phase_ = (chunk_size == 0) ? ParsePhase::read_trailer : ParsePhase::read_data;
                    } else {
                        --pos;
                        pending_buffer_.consume(static_cast<std::size_t>(pos - pending_buffer_.read_ptr()));
                        pending_buffer_.compact();
                        return ChunkState::need_more_;
                    }
                } else if (std::isxdigit(static_cast<unsigned char>(ch)) || ch == ' ') {
                    size_buf_.push_back(ch);
                } else if (ch == ';') {
                    while (pos < data_end && *pos != '\r') {
                        ++pos;
                    }
                    if (pos >= data_end) {
                        --pos;
                        pending_buffer_.consume(static_cast<std::size_t>(pos - pending_buffer_.read_ptr()));
                        pending_buffer_.compact();
                        return ChunkState::need_more_;
                    }
                } else {
                    phase_ = ParsePhase::error;
                }
                break;
            }
            case ParsePhase::read_data: {
                const std::size_t avail = static_cast<std::size_t>(data_end - pos);
                const std::size_t to_read = (std::min)(avail, remain_bytes_);

                if (file_stream_) {
                    file_stream_->write(pos, static_cast<std::streamsize>(to_read));
                    if (!file_stream_->good()) {
                        phase_ = ParsePhase::error;
                        break;
                    }
                } else {
                    mem_buffer_.append(pos, to_read);
                }

                total_bytes_ += to_read;
                remain_bytes_ -= to_read;
                pos += to_read;

                if (remain_bytes_ == 0) {
                    phase_ = ParsePhase::read_data_crlf;
                }

                if (!file_stream_ && total_bytes_ > disk_spill_size_ && exceed_save_file_) {
                    tmp_file_path_ = "___tmp___" + std::to_string(yuan::base::time::now());
                    file_stream_ = std::make_unique<std::fstream>(
                        tmp_file_path_,
                        std::ios_base::out | std::ios_base::binary | std::ios_base::app);

                    if (!file_stream_->is_open() || !file_stream_->good()) {
                        file_stream_.reset();
                        phase_ = ParsePhase::error;
                        break;
                    }

                    if (!mem_buffer_.empty()) {
                        const auto span = mem_buffer_.readable_span();
                        file_stream_->write(span.data(), static_cast<std::streamsize>(span.size()));
                        mem_buffer_.clear();
                    }
                }
                break;
            }
            case ParsePhase::read_data_crlf: {
                if (pos + 1 < data_end && pos[0] == '\r' && pos[1] == '\n') {
                    pos += 2;
                    phase_ = ParsePhase::read_size;
                } else if (*pos == '\r') {
                    pending_buffer_.consume(static_cast<std::size_t>(pos - pending_buffer_.read_ptr()));
                    pending_buffer_.compact();
                    return ChunkState::need_more_;
                } else {
                    phase_ = ParsePhase::error;
                }
                break;
            }
            case ParsePhase::read_trailer: {
                if (pos + 1 < data_end && pos[0] == '\r' && pos[1] == '\n') {
                    pos += 2;
                    phase_ = ParsePhase::done;
                } else {
                    if (*pos != '\r' && *pos != '\n') {
                        trailer_checksum_.push_back(static_cast<char>(
                            std::tolower(static_cast<unsigned char>(*pos))));
                    }
                    ++pos;
                }
                break;
            }
            default:
                phase_ = ParsePhase::error;
                break;
            }
        }

        pending_buffer_.consume(static_cast<std::size_t>(pos - pending_buffer_.read_ptr()));
        pending_buffer_.compact();

        if (phase_ == ParsePhase::error) {
            return ChunkState::invalid_chunk_;
        }
        if (phase_ == ParsePhase::done) {
            return ChunkState::completed_;
        }
        return ChunkState::need_more_;
    }

    bool ChunkedContentParser::parse(HttpPacket *packet)
    {
        ChunkState state = parse_chunked(packet);

        if (state == ChunkState::need_more_) {
            packet->set_body_state(BodyState::partial);
        }

        if (state == ChunkState::completed_) {
            auto cc = std::make_unique<ChunkedContent>();
            cc->tmp_file = tmp_file_path_;
            cc->total_bytes = total_bytes_;
            cc->trailer_checksum = trailer_checksum_;
            packet->set_body_content(new Content(ContentType::chunked, cc.release()));
            packet->set_body_state(BodyState::fully);
        }

        return state == ChunkState::completed_;
    }
}
