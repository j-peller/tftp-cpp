#include "tftp/server.h"

#include <thread>
#include <algorithm>
#include <iostream>
#include <filesystem>

namespace tftp {

// Default MTU-safe block size limit to avoid fragmentation
// 1500 (Ethernet) - 20 (IP) - 8 (UDP) - 4 (TFTP) - 40 (Safety/VLAN) = 1428
static constexpr size_t MIN_NEGOTIATED_BLKSIZE = 8;
static constexpr size_t MAX_NEGOTIATED_BLKSIZE = 1428;
static constexpr size_t DEFAULT_BLKSIZE = 512;
static constexpr int    MAX_RETRIES = 3;
static constexpr int    DEFAULT_TIMEOUT_SEC = 2;
static constexpr int    MIN_TIMEOUT_SEC = 1;
static constexpr int    MAX_TIMEOUT_SEC = 255;

Server::Server(uint16_t port)
    : TftpBase(port)
{

}


void Server::run()
{
    m_running = true;
    std::cout << "TFTP Server listenting on port " << m_socket.getPort() << std::endl;

    std::vector<uint8_t> buffer;
    struct sockaddr_in clientAddr;

    while (m_running)
    {
        // Use the base class socket directly
        if (m_socket.recvFrom(buffer, clientAddr)) 
        {
            std::thread([this, clientAddr, buffer]() {
                this->handleRequest(clientAddr, buffer);
            }).detach();
        }
    }
}

void Server::handleRequest(const struct sockaddr_in& clientAddr, const std::vector<uint8_t>& rawPacket)
{
    try
    {
        auto packet = Packet::parse(rawPacket.data(), rawPacket.size());

        // dispatch packet to correct handler
        if (packet->getOpcode() == Opcode::RRQ)
        {
            handleReadRequest(clientAddr, static_cast<RequestPacket*>(packet.get()));
        }
        else if (packet->getOpcode() == Opcode::WRQ)
        {
            handleWriteRequest(clientAddr, static_cast<RequestPacket*>(packet.get()));
        }
    }
    catch(const std::exception& e)
    {
        std::cerr << "Error parsing packet: " << e.what() << std::endl;
    }
}

void Server::handleReadRequest(const struct sockaddr_in& clientAddr, const RequestPacket* pkt)
{
    // 1. Create a TRANSFER SESSION (TID)
    // We create a local UDPSocket bound to an ephemeral port (0).
    // Per RFC 1350, the server MUST use a new port for the transfer so the 
    // main port remains available for other requests.
    UDPSocket transferSocket(0); 
    size_t blockSize  = DEFAULT_BLKSIZE;
    int    timeoutSec = DEFAULT_TIMEOUT_SEC;
    bool   useOack    = false;
    
    std::cout << "Handling RRQ for: " << pkt->filename << " on local TID: " << transferSocket.getPort() << std::endl;

    if (!onRead) 
    {
        ErrorPacket err(ErrorCode::NotDefined, "Read not implemented");
        transferSocket.sendTo(err.serialize(), clientAddr);
        return;
    }

    std::vector<uint8_t> fileContent;
    try {
        fileContent = onRead(pkt->filename);
    } catch (...) {
        ErrorPacket err(ErrorCode::FileNotFound, "File not found");
        transferSocket.sendTo(err.serialize(), clientAddr);
        return;
    }

    // RFC 2348 Blksize
    auto oack = std::make_unique<OackPacket>();
    if (pkt->options.count("blksize")) 
    {
        try {
            int requestedSize = std::stoi(pkt->options.at("blksize"));
            blockSize = std::clamp(static_cast<size_t>(requestedSize), static_cast<size_t>(MIN_NEGOTIATED_BLKSIZE), MAX_NEGOTIATED_BLKSIZE); // TFTP limits
            oack->addOption("blksize", std::to_string(blockSize));
            useOack = true;
        } catch (...) {}
    }

    // RFC 2349 TIMEOUT Option
    if (pkt->options.count("timeout"))
    {
        try {
            timeoutSec = std::stoi(pkt->options.at("timeout"));
            timeoutSec = std::clamp(timeoutSec, MIN_TIMEOUT_SEC, MAX_TIMEOUT_SEC); // TFTP limits
            oack->addOption("timeout", std::to_string(timeoutSec));
            useOack = true;
        } catch (...) {}
    }

    // RFC 2349 TSIZE Option
    if (pkt->options.count("tsize"))
    {
        // In Read Request, a size of "0" is specified in the request
        // and the size of the file, in octets, is returned in the OACK
        oack->addOption("tsize", std::to_string(fileContent.size()));
        useOack = true;
    }

    // If options were negotiated, send OACK
    if (useOack)
    {
        transferSocket.sendTo(oack->serialize(), clientAddr);
        std::vector<uint8_t> rxBuffer;
        struct sockaddr_in peer;
        bool confirmed = false;

        if (transferSocket.recvFrom(rxBuffer, peer, timeoutSec)) 
        {
            try {
                auto response = Packet::parse(rxBuffer.data(), rxBuffer.size());
                if (response->getOpcode() == Opcode::ACK) 
                {
                    // Client confirms OACK with ACK block 0
                    auto ack = static_cast<AckPacket*>(response.get());
                    if (ack->blockNumber == 0) 
                    {
                       confirmed = true;
                    }
                }
            } catch(...) {}
        }

        if (!confirmed)
        {
            std::cerr << "OACK negotiation failed or timed out." << std::endl;
            return;
        }
    }

    // Send Data Loop
    uint16_t blockNumber = 1;
    size_t offset        = 0;
    size_t total         = fileContent.size();

    while (offset <= total)
    {
        size_t chunk = std::min(blockSize, total - offset);
        std::vector<uint8_t> chunkData(fileContent.begin() + offset, fileContent.begin() + offset + chunk);
        
        DataPacket dataPkt(blockNumber, chunkData);
        if (!transferSocket.sendTo(dataPkt.serialize(), clientAddr)) break;

        std::vector<uint8_t> rxBuffer;
        struct sockaddr_in peer;
        bool acked = false;
        int attempts = 0;

        while(attempts < MAX_RETRIES) 
        {
            // Re-using the socket wrapper logic on the transfer-specific socket
            if (transferSocket.recvFrom(rxBuffer, peer, timeoutSec)) { 
                try {
                    auto response = Packet::parse(rxBuffer.data(), rxBuffer.size());
                    if (response->getOpcode() == Opcode::ACK) 
                    {
                        auto ack = static_cast<AckPacket*>(response.get());
                        if (ack->blockNumber == blockNumber) 
                        {
                           acked = true;
                           break;
                        }
                    }
                } catch(...) {}
            }

            // Retransmit if we didn't get the right ACK in time
            attempts++;
            if (!acked && attempts < MAX_RETRIES) 
            {
                std::cout << "Retransmitting block " << blockNumber << std::endl;
                transferSocket.sendTo(dataPkt.serialize(), clientAddr);
            }
        }

        if (!acked) 
        {
            std::cerr << "Failed to receive ACK for block " << blockNumber << std::endl;
            break;
        }

        // chunk < blocksize marks an EOF...
        if (chunk < blockSize) 
        {
            // Last packet sent
            break;
        }

        // Continue with next block
        offset += chunk;
        blockNumber++;
    }
}

void Server::handleWriteRequest(const struct sockaddr_in& clientAddr, const RequestPacket* pkt) 
{
    // 1. Create a TRANSFER SESSION (TID)
    UDPSocket transferSocket(0);
    
    // DEBUG: Print all options received from the client
    std::cout << "[DEBUG] WRQ for " << pkt->filename << " Options count: " << pkt->options.size() << std::endl;
    for (auto const& [key, val] : pkt->options) {
        std::cout << "  - " << key << ": " << val << std::endl;
    }

    size_t blockSize  = DEFAULT_BLKSIZE; // Default block size
    int    timeoutSec = DEFAULT_TIMEOUT_SEC;
    size_t totalExpectedSize = 0;
    bool   useOack = false;

    // RFC 2348 Blksize
    auto oack = std::make_unique<OackPacket>();
    if (pkt->options.count("blksize")) 
    {
        try {
            int requestedSize = std::stoi(pkt->options.at("blksize"));
            blockSize = std::clamp(static_cast<size_t>(requestedSize), static_cast<size_t>(8), MAX_NEGOTIATED_BLKSIZE); // TFTP limits
            oack->addOption("blksize", std::to_string(blockSize));
            useOack = true;
        } catch (...) {}
    }

    // RFC 2349 TIMEOUT Option
    if (pkt->options.count("timeout"))
    {
        try {
            timeoutSec = std::stoi(pkt->options.at("timeout"));
            timeoutSec = std::clamp(timeoutSec, MIN_TIMEOUT_SEC, MAX_TIMEOUT_SEC); // TFTP limits
            oack->addOption("timeout", std::to_string(timeoutSec));
            useOack = true;
        } catch (...) {}
    }

    // RFC 2349 TSIZE Option
    // In Write Request packets, the size of the file, in octets, is specified 
    // in the request and echoed back in the OACK.  If the file is too large 
    // for the server to handle, it may abort the transfer with an Error packet (error code 3)
    if (pkt->options.count("tsize"))
    {
        // Extract the expected size from the WRQ options
        totalExpectedSize = std::stoul(pkt->options.at("tsize"));

        // Legacy applications may not send any tsize option, therefore skipping this check
        // and probably allowing to exhaust the filesystem. TODO
        std::error_code ec;
        auto spaceInfo = std::filesystem::space(std::filesystem::current_path(), ec);
        // check if file is too large for the server to handle 
        if (!ec && totalExpectedSize > spaceInfo.available) 
        {
            ErrorPacket err(ErrorCode::DiskFull, "Not enough space on filesystem");
            transferSocket.sendTo(err.serialize(), clientAddr);
            return;
        }

        // Echo back the tsize in the OACK
        oack->addOption("tsize", std::to_string(totalExpectedSize));
        useOack = true;
    }

    if (!onWrite) 
    {
        ErrorPacket err(ErrorCode::NotDefined, "Write not implemented");
        transferSocket.sendTo(err.serialize(), clientAddr);
        return;
    }

    // Respond with OACK if options were negotiated
    bool success = false;
    if (useOack) 
    {
        success = transferSocket.sendTo(oack->serialize(), clientAddr);
    }
    else
    {
        AckPacket initialAck(0);
        success = transferSocket.sendTo(initialAck.serialize(), clientAddr);
    }

    if (!success) 
    {
        return;
    }

    std::vector<uint8_t> receivedFile;
    uint16_t expectedBlock = 1;
    bool transferActive = true;

    while (transferActive) 
    {
        std::vector<uint8_t> rxBuffer;
        struct sockaddr_in peer;
        bool receivedData = false;
        int attempts = 0;

        while (attempts < MAX_RETRIES) 
        {
            if (transferSocket.recvFrom(rxBuffer, peer, timeoutSec)) 
            {
                try {
                    auto packet = Packet::parse(rxBuffer.data(), rxBuffer.size());
                    if (packet->getOpcode() == Opcode::DATA) 
                    {
                        auto dataPkt = static_cast<DataPacket*>(packet.get());
                        
                        if (dataPkt->blockNumber == expectedBlock) 
                        {
                            // Correct block received
                            receivedFile.insert(receivedFile.end(), dataPkt->data.begin(), dataPkt->data.end());
                            
                            // Send ACK for this block
                            AckPacket ack(expectedBlock);
                            transferSocket.sendTo(ack.serialize(), clientAddr);
                            
                            // Check if this was the last packet
                            if (dataPkt->data.size() < blockSize) 
                            {
                                transferActive = false;
                            }
                            
                            expectedBlock++;
                            receivedData = true;
                            break;
                        } 
                        else if (dataPkt->blockNumber < expectedBlock) 
                        {
                            // Duplicate block (retry) - re-send ACK for previous block
                            AckPacket ack(dataPkt->blockNumber);
                            transferSocket.sendTo(ack.serialize(), clientAddr);
                        }
                    } 
                    else if (packet->getOpcode() == Opcode::ERROR) 
                    {
                        std::cerr << "Client sent error during WRQ" << std::endl;
                        return;
                    }
                } catch (...) {}
            }
            else
            {
                attempts++;
                if (attempts < MAX_RETRIES) 
                {
                    AckPacket lastAck(expectedBlock - 1);
                    transferSocket.sendTo(lastAck.serialize(), clientAddr);
                    std::cout << "Timeout waiting for block " << expectedBlock 
                              << " - Retransmitting ACK for block " << (expectedBlock -1) << std::endl;
                }
            }
        }

        if (!receivedData && transferActive) 
        {
            std::cerr << "Write transfer timed out waiting for block " << expectedBlock << std::endl;
            return;
        }
    }

    // 3. Finalize Write (User Logic)
    try 
    {
        onWrite(pkt->filename, receivedFile);
        std::cout << "Write transfer for " << pkt->filename << " complete." << std::endl;
    } 
    catch (const std::exception& e) 
    {
        ErrorPacket err(ErrorCode::AccessViolation, e.what());
        transferSocket.sendTo(err.serialize(), clientAddr);
    }
}



} // namespace tftp