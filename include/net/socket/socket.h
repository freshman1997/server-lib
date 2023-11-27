#ifndef __SOCKET_H__
#define __SOCKET_H__
#include <string>

class Socket
{
public:
    explicit Socket(int fd) : fd_(fd) {}

    void bind();

private:
    const int fd_;
};

#endif
