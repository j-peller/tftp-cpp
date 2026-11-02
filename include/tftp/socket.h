#pragma once
#ifndef SOCKET_H
#define SOCKET_H

#include <cstdint>
#include <vector>
#include <netinet/in.h>

namespace tftp {

class UDPSocket {

public:
    explicit UDPSocket(uint16_t port = 0);
    ~UDPSocket();

    // Disable copy, enable move constructor
    UDPSocket(const UDPSocket&) = delete;
    UDPSocket& operator=(const UDPSocket&) = delete;
    UDPSocket(UDPSocket&& other) noexcept;
    UDPSocket& operator=(UDPSocket&& other) noexcept;

    void bindSocket(uint16_t port);
    bool sendTo(const std::vector<uint8_t>& data, const struct sockaddr_in& destAddr);
    bool recvFrom(std::vector<uint8_t>& buffer, struct sockaddr_in& sender, int timeoutSec = 0);

    /**
     * Wait until a datagram is ready to be read.
     * Returns true if data is available within timeoutMs milliseconds.
     */
    bool waitReadable(int timeoutMs) const;

    //--- Getters
    uint16_t getPort() const;


public:
    int m_sockFd;
    struct sockaddr_in m_addr;

};


}

#endif