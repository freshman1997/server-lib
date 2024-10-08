#ifndef __SOCKET_H__
#define __SOCKET_H__

namespace net
{
    class InetAddress;
    
    class Socket
    {
    public:
        explicit Socket(const char *ip, int port, bool udp = false, int fd = -1);
        ~Socket();

        bool bind();

        bool listen();

        int accept(struct sockaddr_in &peer_addr);

        bool connect();

        void set_no_deylay(bool on);

        void set_reuse(bool on);

        void set_keep_alive(bool on);

        void set_none_block(bool on);

        int get_fd() const
        {
            return fd_;
        }

        bool valid()
        {
            return fd_ > 0;
        }

        InetAddress * get_address() 
        {
            return addr;
        }

        void set_id(int id)
        {
            id_= id;
        }

        int get_id() const
        {
            return id_;
        }

    private:
        int fd_;
        int id_;
        InetAddress *addr;
    };
}
#endif
