#include "tftp/socket.h"

#include <stdexcept>
#include <unistd.h>
#include <cstring>
#include <poll.h>

namespace tftp {

// Largest possible UDP payload. Negotiated block sizes (RFC 2348) can go up
// to 65464 bytes, so the receive buffer must be able to hold a full datagram.
static constexpr size_t MAX_DATAGRAM_SIZE = 65536;


UDPSocket::UDPSocket(uint16_t port)
    : m_sockFd(-1)
{
    m_sockFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sockFd < 0) 
    {
        throw std::runtime_error("Failed to create socket");
    }

    if (port > 0)
    {
        bindSocket(port);
    }
    else
    {
        // if port is 0 we let the OS decide which port to bind to and query
        // the port afterwards with getPort()
        bindSocket(0);
    }
}

UDPSocket::~UDPSocket()
{
    if (m_sockFd >= 0)
    {
        close(m_sockFd);
    }
}

UDPSocket::UDPSocket(UDPSocket&& other) noexcept
    : m_sockFd(other.m_sockFd), m_addr(other.m_addr)
{
    other.m_sockFd = -1;
}

UDPSocket& UDPSocket::operator=(UDPSocket&& other) noexcept
{
    if (this != &other)
    {
        if (m_sockFd >= 0)
        {
            close(m_sockFd);
        }
        m_sockFd        = other.m_sockFd;
        m_addr          = other.m_addr;
        other.m_sockFd  = -1;
    }

    return *this;
}

void UDPSocket::bindSocket(uint16_t port)
{
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin_family       = AF_INET;
    m_addr.sin_addr.s_addr  = INADDR_ANY;
    m_addr.sin_port         = htons(port);

    if ( bind(m_sockFd, reinterpret_cast<const sockaddr*>(&m_addr), sizeof(m_addr)) < 0)
    {
        throw std::runtime_error("Failed to bind socket");
    }
}

bool UDPSocket::sendTo(const std::vector<uint8_t>& data, const struct sockaddr_in& dest) {
    if (m_sockFd < 0) 
    {
        return false;
    }

    ssize_t sent = sendto(m_sockFd, data.data(), data.size(), 0, 
                          reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));

    // return 1 if sent bytes equals data.size()
    return sent == (ssize_t)data.size();
}

bool UDPSocket::recvFrom(std::vector<uint8_t>& buffer, struct sockaddr_in& sender, int timeoutSec) 
{
    if (m_sockFd < 0)
    {
        return false;
    }

    socklen_t addrLen = sizeof(sender);
    buffer.resize(MAX_DATAGRAM_SIZE); 

    if (timeoutSec >= 0) 
    {
        struct timeval tv;
        tv.tv_sec = timeoutSec;
        tv.tv_usec = 0;
        setsockopt(m_sockFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ssize_t len = recvfrom(m_sockFd, buffer.data(), buffer.size(), 0,
                           reinterpret_cast<sockaddr*>(&sender), &addrLen);
    
    if (len < 0) return false;
    
    buffer.resize(len);
    return true;
}

bool UDPSocket::waitReadable(int timeoutMs) const
{
    if (m_sockFd < 0)
    {
        return false;
    }

    struct pollfd pfd;
    pfd.fd      = m_sockFd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    return poll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & POLLIN);
}

uint16_t UDPSocket::getPort() const 
{
    if (m_sockFd < 0) 
    {
        return 0;
    }
    struct sockaddr_in sin;

    socklen_t len = sizeof(sin);
    if (getsockname(m_sockFd, (struct sockaddr *)&sin, &len) == -1)
    {
        return 0;
    }

    return ntohs(sin.sin_port);
}



}