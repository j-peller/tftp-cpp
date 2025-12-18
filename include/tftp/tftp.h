#pragma once

#ifndef TFTP_H
#define TFTP_H

#include "socket.h"
#include "packet.h"
#include <atomic>
#include <memory>

namespace tftp {

/**
 * Common base for Server and Client.
 * Manages the socket and basic IO operations.
 */
class TftpBase {
protected:
    UDPSocket m_socket;
    std::atomic<bool> m_running;

public:
    explicit TftpBase(uint16_t port = 0) : m_socket(port), m_running(false) {}
    virtual ~TftpBase() = default;

    // Common Control
    virtual void stop() { m_running = false; }
    uint16_t getPort() const { return m_socket.getPort(); }

protected:
    // Helper to send a specific packet type
    bool sendPacket(const Packet& pkt, const struct sockaddr_in& dest) {
        return m_socket.sendTo(pkt.serialize(), dest);
    }

    // Helper to receive and parse a packet safely
    std::unique_ptr<Packet> receivePacket(struct sockaddr_in& sender, int timeoutSec = 2) {
        std::vector<uint8_t> buf;
        if (m_socket.recvFrom(buf, sender, timeoutSec)) {
            try 
            {
                return Packet::parse(buf.data(), buf.size());
            } 
            catch (...) 
            {
                // Malformed packet, could log here
            }
        }
        return nullptr;
    }
};

}

#endif