#include "algorithm/ssh_compression.h"
#include <zlib.h>
#include <cstring>

namespace yuan::net::ssh
{
    class SshCompressionZlib : public SshCompression
    {
    public:
        explicit SshCompressionZlib(bool delayed)
            : delayed_(delayed)
        {
        }

        ~SshCompressionZlib() override
        {
            if (deflate_initialized_)
                deflateEnd(&deflate_stream_);
            if (inflate_initialized_)
                inflateEnd(&inflate_stream_);
        }

        std::string name() const override
        {
            if (delayed_)
                return "zlib@openssh.com";
            return "zlib";
        }

        bool init() override
        {
            int ret = inflateInit(&inflate_stream_);
            if (ret != Z_OK)
                return false;
            inflate_initialized_ = true;

            ret = deflateInit(&deflate_stream_, Z_DEFAULT_COMPRESSION);
            if (ret != Z_OK)
                return false;
            deflate_initialized_ = true;

            return true;
        }

        std::vector<uint8_t> compress(const uint8_t *data, size_t len) override
        {
            if (!deflate_initialized_)
                return {};

            size_t bound = deflateBound(&deflate_stream_, static_cast<uLong>(len));
            std::vector<uint8_t> out(bound);

            deflate_stream_.next_in = const_cast<Bytef *>(data);
            deflate_stream_.avail_in = static_cast<uInt>(len);
            deflate_stream_.next_out = out.data();
            deflate_stream_.avail_out = static_cast<uInt>(bound);
            deflate_stream_.total_out = 0;

            int ret = deflate(&deflate_stream_, Z_SYNC_FLUSH);
            if (ret != Z_OK && ret != Z_BUF_ERROR)
                return {};

            out.resize(deflate_stream_.total_out);
            return out;
        }

        std::vector<uint8_t> decompress(const uint8_t *data, size_t len) override
        {
            if (!inflate_initialized_)
                return {};

            size_t out_size = len * 4;
            if (out_size < 256)
                out_size = 256;
            std::vector<uint8_t> out(out_size);

            inflate_stream_.next_in = const_cast<Bytef *>(data);
            inflate_stream_.avail_in = static_cast<uInt>(len);
            inflate_stream_.next_out = out.data();
            inflate_stream_.avail_out = static_cast<uInt>(out_size);
            inflate_stream_.total_out = 0;

            int ret = inflate(&inflate_stream_, Z_SYNC_FLUSH);
            if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_STREAM_END)
                return {};

            out.resize(inflate_stream_.total_out);
            return out;
        }

    private:
        bool delayed_;
        z_stream deflate_stream_ = {};
        z_stream inflate_stream_ = {};
        bool deflate_initialized_ = false;
        bool inflate_initialized_ = false;
    };

    std::unique_ptr<SshCompression> create_compression_zlib()
    {
        return std::make_unique<SshCompressionZlib>(false);
    }

    std::unique_ptr<SshCompression> create_compression_zlib_openssh()
    {
        return std::make_unique<SshCompressionZlib>(true);
    }
}
