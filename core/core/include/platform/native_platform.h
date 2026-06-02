#ifndef YUAN_PLATFORM_NATIVE_PLATFORM_H
#define YUAN_PLATFORM_NATIVE_PLATFORM_H

#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#endif

namespace yuan::platform
{
    // Cross-platform normalized network/system error categories.
    // These values are intended for control flow decisions (retry/close/fail),
    // not for external protocol serialization.
    enum class NativeError
    {
        // No error.
        none,
        // Operation interrupted by signal/cancel and can usually be retried.
        interrupted,
        // Non-blocking operation would block right now.
        would_block,
        // Async/connect operation has started and is still in progress.
        in_progress,
        // Async/connect operation is already in progress.
        already,
        // Socket/connection is not connected.
        not_connected,
        // Peer reset the connection.
        connection_reset,
        // Connection aborted locally or by the stack.
        connection_aborted,
        // Peer actively refused the connection.
        connection_refused,
        // Operation timed out.
        timed_out,
        // Local address/port already in use.
        address_in_use,
        // Local address is not available on this host.
        address_not_available,
        // Network is unreachable.
        network_unreachable,
        // Host is unreachable.
        host_unreachable,
        // Datagram/message too large for transport.
        message_too_large,
        // No buffer space available in kernel/stack.
        no_buffer_space,
        // Invalid argument or invalid state for this syscall.
        invalid_argument,
        // Permission denied by OS/security policy.
        permission_denied,
        // Process/socket/file descriptor limit reached.
        too_many_open_files,
        // File descriptor/handle is not a socket.
        not_socket,
        // Error not covered by known mapping table.
        unknown,
    };

    // Returns the last socket-related native error code.
    // Windows: WSAGetLastError()
    // POSIX: errno
    inline int GetLastNativeError() noexcept
    {
#ifdef _WIN32
        return ::WSAGetLastError();
#else
        return errno;
#endif
    }

    // Returns the last generic OS/system error code.
    // Windows: GetLastError()
    // POSIX: errno
    inline int GetLastSystemError() noexcept
    {
#ifdef _WIN32
        return static_cast<int>(::GetLastError());
#else
        return errno;
#endif
    }

    // Converts a native error code to human-readable text.
    // Windows uses FormatMessageA; POSIX uses strerror.
    inline std::string DescribeNativeError(int err)
    {
#ifdef _WIN32
        LPSTR buffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD len = ::FormatMessageA(
            flags,
            nullptr,
            static_cast<DWORD>(err),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPSTR>(&buffer),
            0,
            nullptr);
        if (len == 0 || buffer == nullptr) {
            return "WSA error " + std::to_string(err);
        }

        std::string text(buffer, len);
        ::LocalFree(buffer);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) {
            text.pop_back();
        }
        return text;
#else
        return std::strerror(err);
#endif
    }

    // Maps platform-specific error code to NativeError.
    // Use this to avoid scattering errno/WSA* checks in call sites.
    inline NativeError ClassifyNativeError(int err) noexcept
    {
        if (err == 0) {
            return NativeError::none;
        }

#ifdef _WIN32
        switch (err) {
        case WSAEINTR:
            return NativeError::interrupted;
        case WSAEWOULDBLOCK:
            return NativeError::would_block;
        case WSAEINPROGRESS:
            return NativeError::in_progress;
        case WSAEALREADY:
            return NativeError::already;
        case WSAENOTCONN:
            return NativeError::not_connected;
        case WSAECONNRESET:
            return NativeError::connection_reset;
        case WSAECONNABORTED:
            return NativeError::connection_aborted;
        case WSAECONNREFUSED:
            return NativeError::connection_refused;
        case WSAETIMEDOUT:
            return NativeError::timed_out;
        case WSAEADDRINUSE:
            return NativeError::address_in_use;
        case WSAEADDRNOTAVAIL:
            return NativeError::address_not_available;
        case WSAENETUNREACH:
            return NativeError::network_unreachable;
        case WSAEHOSTUNREACH:
            return NativeError::host_unreachable;
        case WSAEMSGSIZE:
            return NativeError::message_too_large;
        case WSAENOBUFS:
            return NativeError::no_buffer_space;
        case WSAEINVAL:
            return NativeError::invalid_argument;
        case WSAEACCES:
            return NativeError::permission_denied;
        case WSAEMFILE:
            return NativeError::too_many_open_files;
        case WSAENOTSOCK:
            return NativeError::not_socket;
        default:
            return NativeError::unknown;
        }
#else
        switch (err) {
        case EINTR:
            return NativeError::interrupted;
#if defined(EWOULDBLOCK) && defined(EAGAIN) && (EWOULDBLOCK != EAGAIN)
        case EWOULDBLOCK:
#endif
        case EAGAIN:
            return NativeError::would_block;
        case EINPROGRESS:
            return NativeError::in_progress;
        case EALREADY:
            return NativeError::already;
        case ENOTCONN:
            return NativeError::not_connected;
        case ECONNRESET:
            return NativeError::connection_reset;
        case ECONNABORTED:
            return NativeError::connection_aborted;
        case ECONNREFUSED:
            return NativeError::connection_refused;
        case ETIMEDOUT:
            return NativeError::timed_out;
        case EADDRINUSE:
            return NativeError::address_in_use;
        case EADDRNOTAVAIL:
            return NativeError::address_not_available;
        case ENETUNREACH:
            return NativeError::network_unreachable;
        case EHOSTUNREACH:
            return NativeError::host_unreachable;
        case EMSGSIZE:
            return NativeError::message_too_large;
        case ENOBUFS:
            return NativeError::no_buffer_space;
        case EINVAL:
            return NativeError::invalid_argument;
        case EACCES:
            return NativeError::permission_denied;
        case EMFILE:
            return NativeError::too_many_open_files;
        case ENOTSOCK:
            return NativeError::not_socket;
        default:
            return NativeError::unknown;
        }
#endif
    }

    // True if this error is usually safe to retry immediately in non-blocking I/O.
    inline bool IsNativeRetryableError(int err) noexcept
    {
        const auto kind = ClassifyNativeError(err);
        return kind == NativeError::interrupted || kind == NativeError::would_block || kind == NativeError::in_progress ||
               kind == NativeError::already;
    }

    class NativePlatformGuard
    {
    public:
        NativePlatformGuard()
        {
#ifdef _WIN32
            WSADATA wsa{};
            initialized_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
            initialized_ = true;
#endif
        }

        ~NativePlatformGuard()
        {
#ifdef _WIN32
            if (initialized_) {
                WSACleanup();
            }
#endif
        }

        bool ok() const noexcept
        {
            return initialized_;
        }

    private:
        bool initialized_ = false;
    };
}

#endif
