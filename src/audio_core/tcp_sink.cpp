// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "audio_core/tcp_sink.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "audio_core/audio_types.h"

namespace AudioCore {

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;

bool EnsureSocketSupport() {
    static const bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

void CloseSocket(SocketHandle& socket) {
    if (socket != InvalidSocket) {
        closesocket(socket);
        socket = InvalidSocket;
    }
}

bool SetNonBlocking(SocketHandle socket) {
    u_long non_blocking = 1;
    return ioctlsocket(socket, FIONBIO, &non_blocking) == 0;
}

int GetSocketError() {
    return WSAGetLastError();
}

bool IsWouldBlockError(int error) {
    return error == WSAEWOULDBLOCK;
}
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle InvalidSocket = -1;

bool EnsureSocketSupport() {
    return true;
}

void CloseSocket(SocketHandle& socket) {
    if (socket != InvalidSocket) {
        close(socket);
        socket = InvalidSocket;
    }
}

bool SetNonBlocking(SocketHandle socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool ConfigureClientSocket(SocketHandle socket) {
#ifdef SO_NOSIGPIPE
    int enable = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) != 0) {
        return false;
    }
#endif
    return true;
}

int GetSocketError() {
    return errno;
}

bool IsWouldBlockError(int error) {
    return error == EWOULDBLOCK || error == EAGAIN;
}
#endif

#ifdef _WIN32
bool ConfigureClientSocket(SocketHandle) {
    return true;
}
#endif

int SendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

} // namespace

struct TCPSink::Impl {
    SocketHandle listener = InvalidSocket;
    SocketHandle client = InvalidSocket;
};

TCPSink::TCPSink(std::string port_text) : impl(new Impl) {
    if (!EnsureSocketSupport()) {
        std::fprintf(stderr, "Error: failed to initialize TCP audio socket support\n");
        return;
    }

    char* end = nullptr;
    const unsigned long port_value = std::strtoul(port_text.c_str(), &end, 10);
    if (end == port_text.c_str() || *end != '\0' || port_value > 65535UL) {
        std::fprintf(stderr, "Error: invalid TCP audio port '%s'\n", port_text.c_str());
        return;
    }

    impl->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl->listener == InvalidSocket) {
        std::fprintf(stderr, "Error: failed to create TCP audio listener\n");
        return;
    }

    int enable = 1;
    setsockopt(impl->listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enable),
               sizeof(enable));

    if (!SetNonBlocking(impl->listener)) {
        std::fprintf(stderr, "Error: failed to set TCP audio listener non-blocking mode\n");
        CloseSocket(impl->listener);
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u16>(port_value));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(impl->listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "Error: failed to bind TCP audio listener on 127.0.0.1:%lu\n",
                     port_value);
        CloseSocket(impl->listener);
        return;
    }

    if (listen(impl->listener, 1) != 0) {
        std::fprintf(stderr, "Error: failed to listen on TCP audio listener\n");
        CloseSocket(impl->listener);
        return;
    }

    sockaddr_in bound_address{};
    SocketLength bound_address_size = sizeof(bound_address);
    if (getsockname(impl->listener, reinterpret_cast<sockaddr*>(&bound_address),
                    &bound_address_size) == 0) {
        std::fprintf(stderr, "azahar_audio_listener 127.0.0.1 %u %d 2 16\n",
                     ntohs(bound_address.sin_port), native_sample_rate);
    }
}

TCPSink::~TCPSink() {
    CloseSocket(impl->client);
    CloseSocket(impl->listener);
    delete impl;
}

unsigned int TCPSink::GetNativeSampleRate() const {
    return native_sample_rate;
}

void TCPSink::PushSamples(const void* data, std::size_t num_samples) {
    if (impl->listener == InvalidSocket) {
        return;
    }

    if (impl->client == InvalidSocket) {
        sockaddr_in client_address{};
        SocketLength client_address_size = sizeof(client_address);
        SocketHandle accepted = accept(impl->listener, reinterpret_cast<sockaddr*>(&client_address),
                                       &client_address_size);
        if (accepted != InvalidSocket) {
            if (!ConfigureClientSocket(accepted) || !SetNonBlocking(accepted)) {
                CloseSocket(accepted);
                return;
            }
            impl->client = accepted;
        } else if (!IsWouldBlockError(GetSocketError())) {
            return;
        }
    }

    if (impl->client == InvalidSocket) {
        return;
    }

    const auto total_bytes = static_cast<int>(num_samples * 2 * sizeof(s16));
    auto bytes_sent = 0;
    while (bytes_sent < total_bytes) {
        const auto sent =
            send(impl->client, static_cast<const char*>(data) + bytes_sent, total_bytes - bytes_sent,
                 SendFlags());
        if (sent > 0) {
            bytes_sent += sent;
            continue;
        }

        if (sent == 0) {
            CloseSocket(impl->client);
            return;
        }

        const int error = GetSocketError();
        if (error == EINTR) {
            continue;
        }
        if (IsWouldBlockError(error)) {
            return;
        }
        CloseSocket(impl->client);
        return;
    }
}

std::vector<std::string> ListTCPSinkDevices() {
    return std::vector<std::string>{};
}

} // namespace AudioCore
