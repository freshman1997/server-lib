#ifndef __BIT_TORRENT_NAT_UPNP_MANAGER_H__
#define __BIT_TORRENT_NAT_UPNP_MANAGER_H__

#include "nat_config.h"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <functional>

namespace yuan::net
{
    class EventLoop;
}

namespace yuan::net::bit_torrent
{

// UPnP IGD (Internet Gateway Device) and NAT-PMP port mapping.
//
// Workflow:
//   1. UPnP: SSDP multicast discovery -> find IGD control URL
//   2. UPnP: SOAP AddPortMapping TCP <port> -> <mapped_port>
//   3. NAT-PMP: UDP to gateway 192.168.x.1:5351 -> map port
//   4. Returns external IP + mapped port on success
//
// Port mapping is renewed periodically before the lease expires.
class UpnpManager
{
public:
    using ResultCallback = std::function<void(bool success,
                                               const std::string &external_ip,
                                               uint16_t mapped_port)>;

    UpnpManager();
    ~UpnpManager();

    // Start port mapping (non-blocking, runs in background thread)
    void start(const NatConfig &config, uint16_t internal_port, ResultCallback cb);

    // Stop mapping (sends DeletePortMapping)
    void stop();

    bool is_mapped() const { return mapped_.load(); }
    std::string get_external_ip() const;
    uint16_t get_mapped_port() const;

private:
    // UPnP methods
    void discover_igd();
    bool send_ssdp_discover(int sock);
    void parse_ssdp_response(const std::string &response);
    bool fetch_igd_description(const std::string &location);
    bool parse_igd_services(const std::string &xml);
    bool soap_add_port_mapping();
    bool soap_delete_port_mapping();
    bool soap_get_external_ip();
    std::string http_request(const std::string &url, const std::string &soap_action,
                             const std::string &soap_body);

    // NAT-PMP / PCP methods
    void try_nat_pmp();
    bool nat_pmp_map_port();
    bool nat_pmp_get_external_ip();

    // Background renewal thread
    void renewal_loop();

    std::string get_local_gateway();
    std::string get_local_ip();

private:
    std::atomic<bool> running_;
    std::atomic<bool> mapped_;
    std::thread worker_;

    NatConfig config_;
    uint16_t internal_port_ = 0;

    // UPnP state
    std::string igd_control_url_;
    std::string igd_service_type_;
    std::string external_ip_;
    uint16_t mapped_port_ = 0;

    // NAT-PMP state
    std::string gateway_ip_;
    bool nat_pmp_available_ = false;
    uint32_t nat_pmp_lifetime_ = 0;

    ResultCallback result_cb_;
    mutable std::mutex mutex_;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_NAT_UPNP_MANAGER_H__
