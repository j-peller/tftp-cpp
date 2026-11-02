
#pragma once

#ifndef SERVER_H
#define SERVER_H

#include "tftp.h"
#include <functional>
#include <mutex>
#include <condition_variable>

namespace tftp {

using ReadHandler = std::function<std::vector<uint8_t>(const std::string& filename)>;
using WriteHandler = std::function<void(const std::string& filename, const std::vector<uint8_t>& data)>;

/**
 * Configuration structure for TFTP Server
 */
struct ServerConfig {
    uint16_t port;
    int      timeoutSec;
    size_t   blockSize;
};

/**
 * TFTP Server implementation.
 * Handles incoming read and write requests.
 */
class Server final : private TftpBase {
public:
    explicit Server(const ServerConfig& config = {69, 2, 512});

    //--- A Server is non copyable
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    
    //--- User Logic Injection
    void setReadHandler(ReadHandler h) { m_onRead = h; }
    void setWriteHandler(WriteHandler h) { m_onWrite = h; }

    /**
     * Start the server. This call is blocking until stop() is called.
     * Before returning, it waits for all active transfer sessions to finish.
     */
    void run(); 

    /** 
     * Inherited from TftpBase
    */
    using TftpBase::stop;
    using TftpBase::getPort;

private:
    void handleRequest(const struct sockaddr_in& clientAddr, const std::vector<uint8_t>& rawPacket);
    void handleReadRequest(const struct sockaddr_in& clientAddr, const RequestPacket* pkt);
    void handleWriteRequest(const struct sockaddr_in& clientAddr, const RequestPacket* pkt);

    ReadHandler  m_onRead;
    WriteHandler m_onWrite;
    ServerConfig m_config;

    //--- Tracking of detached transfer sessions (see run())
    std::mutex              m_sessionMutex;
    std::condition_variable m_sessionCv;
    size_t                  m_activeSessions = 0;
};

}


#endif