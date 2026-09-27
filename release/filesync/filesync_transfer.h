#pragma once

#include "filesync_model.h"
#include <fstream>
#include <memory>

namespace filesync::transfer {

class LineWriter {
public:
    virtual ~LineWriter() = default;
    virtual void write_line(const std::string& line) = 0;
    virtual void flush() {}
};
void send_need(const model::Config& config, const model::Manifest& remote, LineWriter& writer);
void send_files(const model::Config& config, const model::Manifest& manifest,
                const std::vector<model::NeedRequest>& requests, LineWriter& writer);

class FileSender {
public:
    FileSender(const model::Config& config, const model::Manifest& manifest,
               const std::vector<model::NeedRequest>& requests);
    ~FileSender();
    FileSender(FileSender&&) noexcept;
    FileSender& operator=(FileSender&&) noexcept;
    FileSender(const FileSender&) = delete;
    FileSender& operator=(const FileSender&) = delete;

    bool next_line(std::string& line);
    std::size_t full_count() const;
    std::size_t delta_count() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class Receiver {
public:
    explicit Receiver(const model::Config& config);
    ~Receiver();
    bool apply_line(const std::string& line);
    std::size_t completed() const;
    void reset();

private:
    enum class Kind { None, Full, Delta };
    const model::Config& config_;
    Kind kind_ = Kind::None;
    std::string path_;
    model::FileState expected_;
    std::filesystem::path target_;
    std::filesystem::path temp_;
    std::filesystem::path basis_;
    std::ofstream output_;
    std::uintmax_t received_ = 0;
    std::size_t completed_ = 0;
    std::size_t expected_entries_ = 0;
    bool batch_started_ = false;
    void finish();
    void begin(const std::vector<std::string>& parts, bool delta);
    void clear_file();
    void abort_file();
};

}
