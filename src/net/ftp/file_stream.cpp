#include "net/ftp/file_stream.h"
#include <fstream>
#include <ios>

namespace net::ftp 
{
    
    FtpFileStream::FtpFileStream() : connected_(false), mode_(StreamMode::Receiver), session_(nullptr), file_stream_(nullptr)
    {
    }

    FtpFileStream::~FtpFileStream()
    {
        if (file_stream_) {
            file_stream_->close();
            delete file_stream_;
        }
    }

    void FtpFileStream::on_connected(Connection *conn)
    {
        connected_ = true;
    }

    void FtpFileStream::on_error(Connection *conn)
    {

    }

    void FtpFileStream::on_read(Connection *conn)
    {

    }

    void FtpFileStream::on_write(Connection *conn)
    {

    }

    void FtpFileStream::on_close(Connection *conn)
    {
        connected_ = false;
    }

    void FtpFileStream::open_file_stream(const std::string &filepath)
    {
        if (filepath.empty()) {
            return;
        }

        file_stream_ = new std::fstream;
        if (mode_ == StreamMode::Receiver) {
            file_stream_->open(filepath, std::ios_base::out);
        } else {
            file_stream_->open(filepath, std::ios_base::in);
        }

        if (!file_stream_->good()) {
            file_stream_->close();
            delete file_stream_;
            file_stream_ = nullptr;
        }
    }
}