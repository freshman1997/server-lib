#ifndef __SERVER_BIT_TORRENT_SERVICE_H__
#define __SERVER_BIT_TORRENT_SERVICE_H__

#include "bit_torrent_client.h"
#include "server_runtime_host.h"
#include "service.h"

#include <memory>
#include <string>

namespace yuan::server
{

    class BitTorrentService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit BitTorrentService(std::string torrent_file_path, std::string save_path = ".");
        ~BitTorrentService() override;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        yuan::net::bit_torrent::BitTorrentClient &client();
        const yuan::net::bit_torrent::BitTorrentClient &client() const;

    private:
        std::string torrent_file_path_;
        std::string save_path_;
        std::unique_ptr<yuan::net::bit_torrent::BitTorrentClient> client_;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
    };

} // namespace yuan::server

#endif
