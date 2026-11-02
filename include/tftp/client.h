
#pragma once

#ifndef CLIENT_H
#define CLIENT_H

#include "tftp.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tftp {

/**
 * Configuration structure for TFTP Client
 */
struct ClientConfig {
    std::string serverAddress = "127.0.0.1";
    uint16_t    serverPort    = 69;
    int         timeoutSec    = 2;   // receive timeout until options are negotiated (>= 1)
    int         maxRetries    = 3;   // retransmissions before a transfer is aborted
};

/**
 * Options requested for a single transfer (RFC 2347 / 2348 / 2349).
 * Unset options are not sent to the server.
 */
struct TransferOptions {
    std::optional<size_t> blockSize;           // RFC 2348 "blksize"
    std::optional<int>    timeoutSec;          // RFC 2349 "timeout"
    bool                  transferSize = false; // RFC 2349 "tsize"
};

/**
 * Outcome of a single transfer.
 */
struct TransferResult {
    bool        success   = false;
    ErrorCode   errorCode = ErrorCode::NotDefined;
    std::string errorMessage;

    std::map<std::string, std::string> negotiatedOptions; // options of the OACK (empty if none)
    size_t                blockSize        = 512;         // block size used for the transfer
    std::optional<size_t> transferSize;                   // "tsize" reported in the OACK
    size_t                bytesTransferred = 0;

    explicit operator bool() const { return success; }
};

/**
 * TFTP Client implementation.
 * Performs read (get) and write (put) requests against a TFTP server.
 */
class Client final : private TftpBase {
public:
    explicit Client(const ClientConfig& config);

    //--- A Client is non copyable
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /**
     * Download filename from the server (RRQ) into data.
     */
    TransferResult get(const std::string& filename, std::vector<uint8_t>& data,
                       const TransferOptions& options = {});

    /**
     * Upload data to the server as filename (WRQ).
     */
    TransferResult put(const std::string& filename, const std::vector<uint8_t>& data,
                       const TransferOptions& options = {});

    /**
     * Inherited from TftpBase
     */
    using TftpBase::getPort;

private:
    struct Session {
        struct sockaddr_in peer{};          // server transfer ID (address + port)
        bool               peerKnown  = false;
        size_t             blockSize  = 512;
        int                timeoutSec = 2;
    };

    using OptionMap = std::map<std::string, std::string>;

    OptionMap buildOptions(const TransferOptions& options, size_t writeSize, bool isWrite) const;
    bool applyOack(const OackPacket& oack, const OptionMap& requested, bool isWrite,
                   Session& session, TransferResult& result);
    std::unique_ptr<Packet> exchange(const Packet& pkt, Session& session,
                                     const std::function<bool(const Packet&)>& accept = nullptr);
    TransferResult fail(TransferResult& result, ErrorCode code, const std::string& msg) const;
    void sendError(Session& session, ErrorCode code, const std::string& msg);

    ClientConfig       m_config;
    struct sockaddr_in m_serverAddr{};
};

}

#endif
