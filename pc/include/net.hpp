#pragma once
#include <cstdint>
#include <string>
#include <stdexcept>
#include <chrono>
#include <csignal>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket bad_socket = INVALID_SOCKET;
inline void close_socket(Socket s) { closesocket(s); }
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
using Socket = int;
constexpr Socket bad_socket = -1;
inline void close_socket(Socket s) { close(s); }
#endif
inline uint64_t now_ms()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Network
{
    Network()
    {
#ifdef _WIN32
        WSADATA w{};
        if (WSAStartup(MAKEWORD(2, 2), &w))
            throw std::runtime_error("WSAStartup failed");
#else
        signal(SIGPIPE, SIG_IGN);
#endif
    }
    ~Network()
    {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};
struct OwnedSocket
{
    Socket s = bad_socket;
    explicit OwnedSocket(Socket value = bad_socket) : s(value) {}
    ~OwnedSocket()
    {
        if (s != bad_socket)
            close_socket(s);
    }
    OwnedSocket(const OwnedSocket &) = delete;
    OwnedSocket &operator=(const OwnedSocket &) = delete;
    operator Socket() const { return s; }
};
inline sockaddr_in address(const std::string &ip, uint16_t port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1)
        throw std::runtime_error("Invalid IPv4 address: " + ip);
    return a;
}
inline std::string ip_string(const sockaddr_in &a)
{
    char b[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &a.sin_addr, b, sizeof(b));
    return b;
}
inline void nonblocking(Socket s)
{
#ifdef _WIN32
    u_long v = 1;
    if (ioctlsocket(s, FIONBIO, &v))
        throw std::runtime_error("ioctlsocket failed");
#else
    if (fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) < 0)
        throw std::runtime_error("fcntl failed");
#endif
}
inline bool ready(Socket s, bool write, int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval t{ms / 1000, (ms % 1000) * 1000};
    return select(int(s + 1), write ? nullptr : &set, write ? &set : nullptr, nullptr, &t) > 0;
}
inline Socket bind_socket(const std::string &ip, uint16_t port, bool tcp)
{
    OwnedSocket s(socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0));
    if (s.s == bad_socket)
        throw std::runtime_error("socket failed");
#ifndef _WIN32
    if (tcp)
    {
        int yes = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    }
#endif
    auto a = address(ip, port);
    if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)))
        throw std::runtime_error("Cannot bind " + ip + ":" + std::to_string(port));
    if (tcp && listen(s, 16))
        throw std::runtime_error("listen failed");
    nonblocking(s);
    Socket result = s.s;
    s.s = bad_socket;
    return result;
}
inline bool send_all(Socket s, const uint8_t *p, size_t n, uint64_t deadline)
{
    while (n)
    {
        if (now_ms() >= deadline)
            return false;
        if (!ready(s, true, 50))
            continue;
        int sent = send(s, reinterpret_cast<const char *>(p), int(n), 0);
        if (sent <= 0)
            return false;
        p += sent;
        n -= size_t(sent);
    }
    return true;
}
inline bool send_text(Socket s, const std::string &text, uint64_t deadline) { return send_all(s, reinterpret_cast<const uint8_t *>(text.data()), text.size(), deadline); }
