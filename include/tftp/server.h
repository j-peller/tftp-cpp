
#pragma once

#ifndef SERVER_H
#define SERVER_H

#include "tftp.h"
#include <functional>

namespace tftp {

using ReadHandler = std::function<std::vector<uint8_t>(const std::string& filename)>;
using WriteHandler = std::function<void(const std::string& filename, const std::vector<uint8_t>& data)>;

class Server : public TftpBase {
private:
    ReadHandler onRead;
    WriteHandler onWrite;

    void handleRequest(const struct sockaddr_in& clientAddr, const std::vector<uint8_t>& rawPacket);
    void handleReadRequest(const struct sockaddr_in& clientAddr, const RequestPacket* pkt);
    void handleWriteRequest(const struct sockaddr_in& clientAddr, const RequestPacket* pkt);

public:
    explicit Server(uint16_t port = 69);
    
    void setReadHandler(ReadHandler h) { onRead = h; }
    void setWriteHandler(WriteHandler h) { onWrite = h; }

    void run(); 
};

}


#endif