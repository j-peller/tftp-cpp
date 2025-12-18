#include "tftp/server.h"

#include <thread>
#include <algorithm>
#include <iostream>

namespace tftp {

// Default MTU-safe block size limit to avoid fragmentation
// 1500 (Ethernet) - 20 (IP) - 8 (UDP) - 4 (TFTP) - 40 (Safety/VLAN) = 1428
static constexpr size_t MAX_NEGOTIATED_BLKSIZE = 1428;
static constexpr int    MAX_RETRIES = 3;
static constexpr int    DEFAULT_TIMEOUT_SEC = 2;

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
    
    std::cout << "Handling RRQ for: " << pkt->filename << " on local TID: " << transferSocket.getPort() << std::endl;

    size_t blockSize = 512; // Default block size
    bool useOack = false;

    // RFC 2347 Negotiation
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

    if (!onRead) 
    {
        ErrorPacket err(ErrorCode::NotDefined, "Read not implemented");
        transferSocket.sendTo(err.serialize(), clientAddr);
        return;
    }

    std::vector<uint8_t> fileContent;
    try 
    {
        fileContent = onRead(pkt->filename);
    } 
    catch (...) 
    {
        ErrorPacket err(ErrorCode::FileNotFound, "File not found");
        transferSocket.sendTo(err.serialize(), clientAddr);
        return;
    }

    // If options were negotiated, send OACK
    if (useOack)
    {
        transferSocket.sendTo(oack->serialize(), clientAddr);

        std::vector<uint8_t> rxBuffer;
        struct sockaddr_in peer;
        bool confirmed = false;

        if (transferSocket.recvFrom(rxBuffer, peer, 2)) 
        {
            try 
            {
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
            } 
            catch(...) {}
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
            if (transferSocket.recvFrom(rxBuffer, peer, DEFAULT_TIMEOUT_SEC)) { 
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
    
    std::cout << "Handling WRQ for: " << pkt->filename << " on local TID: " << transferSocket.getPort() << std::endl;

    size_t blockSize = 512; // Default block size
    bool useOack = false;

    // RFC 2347 Negotiation
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
        success =transferSocket.sendTo(initialAck.serialize(), clientAddr);
    }

    if (!success) 
    {
        return;
    }

    std::vector<uint8_t> receivedFile;
    uint16_t expectedBlock = 1;
    bool transferActive = true;
    int attempts = 0;

    while (transferActive) 
    {
        std::vector<uint8_t> rxBuffer;
        struct sockaddr_in peer;
        int retries = 3;
        bool receivedData = false;

        while (attempts < MAX_RETRIES) 
        {
            // TODO: 5s timeout 
            if (transferSocket.recvFrom(rxBuffer, peer, DEFAULT_TIMEOUT_SEC)) 
            {
                try 
                {
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
                } 
                catch (...) {}
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