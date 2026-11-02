#include "tftp/server.h"

#include <thread>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <optional>

namespace tftp {

namespace {

bool sameTid(const struct sockaddr_in& a, const struct sockaddr_in& b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// Strict decimal unsigned parser: rejects empty strings, signs, and non-digits.
std::optional<size_t> parseUnsigned(const std::string& str)
{
    if (str.empty() || str.size() > 19) return std::nullopt;
    unsigned long long val = 0;
    for (char c : str)
    {
        if (c < '0' || c > '9') return std::nullopt;
        val = val * 10 + static_cast<unsigned long long>(c - '0');
    }
    return static_cast<size_t>(val);
}

/**
 * Send pkt (OACK or DATA) and wait for ACK(expectedBlock).
 * - Retransmits pkt only on timeout (up to MAX_RETRIES), avoiding Sorcerer's
 *   Apprentice Syndrome on duplicate ACKs (RFC 1123 section 4.2.3.1).
 * - Aborts immediately if the client sends an ERROR packet (RFC 1350 / 2347).
 * - Replies with ERROR 5 (Unknown TID) to packets from foreign peers (RFC 1350).
 */
bool sendAndAwaitAck(UDPSocket& sock, const Packet& pkt, uint16_t expectedBlock,
                     const struct sockaddr_in& clientAddr, int timeoutSec, int maxRetries)
{
    using Clock = std::chrono::steady_clock;
    auto rawPkt = pkt.serialize();

    for (int attempt = 0; attempt < maxRetries; ++attempt)
    {
        if (!sock.sendTo(rawPkt, clientAddr))
        {
            return false;
        }

        auto deadline = Clock::now() + std::chrono::seconds(timeoutSec);
        while (true)
        {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now());
            if (remaining.count() <= 0 || !sock.waitReadable(static_cast<int>(remaining.count())))
            {
                break; // timeout -> retransmit on next attempt
            }

            std::vector<uint8_t> rxBuffer;
            struct sockaddr_in peer{};
            if (!sock.recvFrom(rxBuffer, peer, 0))
            {
                continue;
            }

            if (!sameTid(peer, clientAddr))
            {
                ErrorPacket err(ErrorCode::UnknownTID, "Unknown transfer ID");
                sock.sendTo(err.serialize(), peer);
                continue;
            }

            try
            {
                auto response = Packet::parse(rxBuffer.data(), rxBuffer.size());
                if (response->getOpcode() == Opcode::ERROR)
                {
                    return false; // Client aborted the transfer
                }
                if (response->getOpcode() == Opcode::ACK)
                {
                    auto* ack = static_cast<AckPacket*>(response.get());
                    if (ack->getBlockNumber() == expectedBlock)
                    {
                        return true;
                    }
                    // Duplicate ACK for an earlier block: ignore and keep waiting
                    // until deadline (RFC 1123 section 4.2.3.1).
                }
            }
            catch (...) {}
        }
    }

    return false;
}

} // namespace

// Default MTU-safe block size limit to avoid fragmentation
// 1500 (Ethernet) - 20 (IP) - 8 (UDP) - 4 (TFTP) - 40 (Safety/VLAN) = 1428
static constexpr size_t MIN_NEGOTIATED_BLKSIZE = 8;
static constexpr size_t MAX_NEGOTIATED_BLKSIZE = 1428;
static constexpr size_t DEFAULT_BLKSIZE = 512;
static constexpr int    MAX_RETRIES = 3;
static constexpr int    DEFAULT_TIMEOUT_SEC = 2;
static constexpr int    MIN_TIMEOUT_SEC = 1;
static constexpr int    MAX_TIMEOUT_SEC = 255;

Server::Server(const ServerConfig& config)
    : TftpBase(config.port), m_config(config)
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
        // Poll with a short timeout so that stop() is honoured promptly
        // instead of blocking forever in recvFrom().
        if (!m_socket.waitReadable(200))
        {
            continue;
        }

        // Use the base class socket directly
        if (m_socket.recvFrom(buffer, clientAddr)) 
        {
            {
                std::lock_guard<std::mutex> lock(m_sessionMutex);
                ++m_activeSessions;
            }

            std::thread([this, clientAddr, buffer]() {
                this->handleRequest(clientAddr, buffer);

                std::lock_guard<std::mutex> lock(m_sessionMutex);
                --m_activeSessions;
                m_sessionCv.notify_all();
            }).detach();
        }
    }

    // Sessions run detached and reference this object, so wait until all
    // of them have finished before returning (the Server may be destroyed next).
    std::unique_lock<std::mutex> lock(m_sessionMutex);
    m_sessionCv.wait(lock, [this]() { return m_activeSessions == 0; });
}

void Server::handleRequest(const struct sockaddr_in& clientAddr, const std::vector<uint8_t>& rawPacket)
{
    try
    {
        auto packet = Packet::parse(rawPacket.data(), rawPacket.size());

        if (packet->getOpcode() != Opcode::RRQ && packet->getOpcode() != Opcode::WRQ)
        {
            if (packet->getOpcode() != Opcode::ERROR)
            {
                UDPSocket errSocket(0);
                ErrorPacket err(ErrorCode::IllegalOp, "Expected RRQ or WRQ");
                errSocket.sendTo(err.serialize(), clientAddr);
            }
            return;
        }

        auto* reqPkt = static_cast<RequestPacket*>(packet.get());
        if (reqPkt->getMode() != "octet" && reqPkt->getMode() != "netascii")
        {
            UDPSocket errSocket(0);
            ErrorPacket err(ErrorCode::IllegalOp, "Unsupported transfer mode: " + reqPkt->getMode());
            errSocket.sendTo(err.serialize(), clientAddr);
            return;
        }

        // dispatch packet to correct handler
        if (packet->getOpcode() == Opcode::RRQ)
        {
            handleReadRequest(clientAddr, reqPkt);
        }
        else
        {
            handleWriteRequest(clientAddr, reqPkt);
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
    size_t blockSize  = m_config.blockSize;
    int    timeoutSec = m_config.timeoutSec;
    bool   useOack    = false;
    auto   filename = pkt->getFilename();
    
    std::cout << "Handling RRQ for: " << filename << " on local TID: " << transferSocket.getPort() << std::endl;

    if (!m_onRead) 
    {
        ErrorPacket err(ErrorCode::NotDefined, "Read not implemented");
        transferSocket.sendTo(err.serialize(), clientAddr);
        return;
    }

    std::vector<uint8_t> fileContent;
    try {
        fileContent = m_onRead(filename);
    } catch (...) {
        ErrorPacket err(ErrorCode::FileNotFound, "File not found");
        transferSocket.sendTo(err.serialize(), clientAddr);
        return;
    }

    // RFC 2348 Blksize (valid requests are >= 8; server may clamp to its maximum)
    auto oack = std::make_unique<OackPacket>();
    auto options = pkt->getOptions();
    if (options.count("blksize")) 
    {
        auto requestedSize = parseUnsigned(options.at("blksize"));
        if (requestedSize && *requestedSize >= MIN_NEGOTIATED_BLKSIZE)
        {
            blockSize = std::min(*requestedSize, MAX_NEGOTIATED_BLKSIZE);
            oack->addOption("blksize", std::to_string(blockSize));
            useOack = true;
        }
    }

    // RFC 2349 TIMEOUT Option (must be in [1, 255] and echoed verbatim, never clamped)
    if (options.count("timeout"))
    {
        auto requestedTimeout = parseUnsigned(options.at("timeout"));
        if (requestedTimeout && *requestedTimeout >= MIN_TIMEOUT_SEC && *requestedTimeout <= MAX_TIMEOUT_SEC)
        {
            timeoutSec = static_cast<int>(*requestedTimeout);
            oack->addOption("timeout", std::to_string(timeoutSec));
            useOack = true;
        }
    }

    // RFC 2349 TSIZE Option
    if (options.count("tsize"))
    {
        // In Read Request, a size of "0" is specified in the request
        // and the size of the file, in octets, is returned in the OACK
        auto requestedTsize = parseUnsigned(options.at("tsize"));
        if (requestedTsize && *requestedTsize == 0)
        {
            oack->addOption("tsize", std::to_string(fileContent.size()));
            useOack = true;
        }
    }

    // If options were negotiated, send OACK and wait for ACK 0 (with retries)
    if (useOack)
    {
        if (!sendAndAwaitAck(transferSocket, *oack, 0, clientAddr, timeoutSec, MAX_RETRIES))
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
        if (!sendAndAwaitAck(transferSocket, dataPkt, blockNumber, clientAddr, timeoutSec, MAX_RETRIES))
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
    auto options  = pkt->getOptions();
    auto filename = pkt->getFilename();
    
    // DEBUG: Print all options received from the client
    std::cout << "[DEBUG] WRQ for " << filename << " Options count: " << options.size() << std::endl;
    for (auto const& [key, val] : options) {
        std::cout << "  - " << key << ": " << val << std::endl;
    }

    size_t blockSize  = m_config.blockSize;
    int    timeoutSec = m_config.timeoutSec;
    size_t totalExpectedSize = 0;
    bool   useOack = false;

    // RFC 2348 Blksize (valid requests are >= 8; server may clamp to its maximum)
    auto oack = std::make_unique<OackPacket>();
    if (options.count("blksize")) 
    {
        auto requestedSize = parseUnsigned(options.at("blksize"));
        if (requestedSize && *requestedSize >= MIN_NEGOTIATED_BLKSIZE)
        {
            blockSize = std::min(*requestedSize, MAX_NEGOTIATED_BLKSIZE);
            oack->addOption("blksize", std::to_string(blockSize));
            useOack = true;
        }
    }

    // RFC 2349 TIMEOUT Option (must be in [1, 255] and echoed verbatim, never clamped)
    if (options.count("timeout"))
    {
        auto requestedTimeout = parseUnsigned(options.at("timeout"));
        if (requestedTimeout && *requestedTimeout >= MIN_TIMEOUT_SEC && *requestedTimeout <= MAX_TIMEOUT_SEC)
        {
            timeoutSec = static_cast<int>(*requestedTimeout);
            oack->addOption("timeout", std::to_string(timeoutSec));
            useOack = true;
        }
    }

    // RFC 2349 TSIZE Option
    // In Write Request packets, the size of the file, in octets, is specified 
    // in the request and echoed back in the OACK.  If the file is too large 
    // for the server to handle, it may abort the transfer with an Error packet (error code 3)
    if (options.count("tsize"))
    {
        auto requestedTsize = parseUnsigned(options.at("tsize"));
        if (requestedTsize)
        {
            totalExpectedSize = *requestedTsize;

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
    }

    if (!m_onWrite) 
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
                if (!sameTid(peer, clientAddr))
                {
                    ErrorPacket err(ErrorCode::UnknownTID, "Unknown transfer ID");
                    transferSocket.sendTo(err.serialize(), peer);
                    continue;
                }
                try {
                    auto packet = Packet::parse(rxBuffer.data(), rxBuffer.size());
                    if (packet->getOpcode() == Opcode::DATA) 
                    {
                        auto dataPkt = static_cast<DataPacket*>(packet.get());

                        uint16_t blockNum = dataPkt->getBlockNumber();
                        
                        if (blockNum == expectedBlock) 
                        {
                            // Correct block received
                            auto data = dataPkt->getData();

                            // Append data to file buffer
                            receivedFile.insert(receivedFile.end(), data.begin(), data.end());
                            
                            // Send ACK for this block
                            AckPacket ack(expectedBlock);
                            transferSocket.sendTo(ack.serialize(), clientAddr);
                            
                            // Check if this was the last packet
                            if (data.size() < blockSize) 
                            {
                                transferActive = false;
                            }
                            
                            // Move to next expected block
                            expectedBlock++;
                            receivedData = true;
                            break;
                        } 
                        else if (blockNum < expectedBlock) 
                        {
                            // Duplicate block (retry) - re-send ACK for previous block
                            AckPacket ack(blockNum);
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
    try {
        m_onWrite(filename, receivedFile);
        std::cout << "Write transfer for " << filename << " complete." << std::endl;
    } catch (const std::exception& e) {
        ErrorPacket err(ErrorCode::AccessViolation, e.what());
        transferSocket.sendTo(err.serialize(), clientAddr);
    }
}

} // namespace tftp