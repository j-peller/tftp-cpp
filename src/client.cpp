#include "tftp/client.h"

#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace tftp {

// RFC 2348: valid block sizes are 8 - 65464 octets
static constexpr size_t MIN_BLKSIZE     = 8;
static constexpr size_t MAX_BLKSIZE     = 65464;
static constexpr size_t DEFAULT_BLKSIZE = 512;
// RFC 2349: valid timeouts are 1 - 255 seconds
static constexpr int    MIN_TIMEOUT_SEC = 1;
static constexpr int    MAX_TIMEOUT_SEC = 255;

namespace {

bool sameHost(const struct sockaddr_in& a, const struct sockaddr_in& b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr;
}

bool sameTid(const struct sockaddr_in& a, const struct sockaddr_in& b)
{
    return sameHost(a, b) && a.sin_port == b.sin_port;
}

// Strict decimal parser: rejects empty strings, signs and trailing garbage.
std::optional<unsigned long long> parseNumber(const std::string& str)
{
    if (str.empty() || str.size() > 19) return std::nullopt;
    unsigned long long value = 0;
    for (char c : str)
    {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<unsigned long long>(c - '0');
    }
    return value;
}

} // namespace

Client::Client(const ClientConfig& config)
    : TftpBase(0), m_config(config)
{
    if (m_config.timeoutSec < MIN_TIMEOUT_SEC)
    {
        // A timeout of 0 would block forever in UDPSocket::recvFrom()
        m_config.timeoutSec = MIN_TIMEOUT_SEC;
    }

    std::memset(&m_serverAddr, 0, sizeof(m_serverAddr));
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port   = htons(m_config.serverPort);
    if (inet_pton(AF_INET, m_config.serverAddress.c_str(), &m_serverAddr.sin_addr) != 1)
    {
        throw std::invalid_argument("Invalid server address: " + m_config.serverAddress);
    }
}

Client::OptionMap Client::buildOptions(const TransferOptions& options, size_t writeSize, bool isWrite) const
{
    OptionMap requested;
    if (options.blockSize)
    {
        requested["blksize"] = std::to_string(*options.blockSize);
    }
    if (options.timeoutSec)
    {
        requested["timeout"] = std::to_string(*options.timeoutSec);
    }
    if (options.transferSize)
    {
        // RFC 2349: RRQ sends "0" and the server answers with the file size,
        // WRQ announces the size of the file that is going to be uploaded.
        requested["tsize"] = isWrite ? std::to_string(writeSize) : "0";
    }
    return requested;
}

bool Client::applyOack(const OackPacket& oack, const OptionMap& requested, bool isWrite,
                       Session& session, TransferResult& result)
{
    auto reject = [&](const std::string& msg) {
        sendError(session, ErrorCode::OptionNegotiation, msg);
        fail(result, ErrorCode::OptionNegotiation, msg);
        return false;
    };

    size_t blockSize  = DEFAULT_BLKSIZE;
    int    timeoutSec = session.timeoutSec;

    for (const auto& [key, value] : oack.getOptions())
    {
        // RFC 2347: the server must not answer with options the client did not request
        auto req = requested.find(key);
        if (req == requested.end())
        {
            return reject("Unrequested option in OACK: " + key);
        }

        auto number = parseNumber(value);
        if (!number)
        {
            return reject("Invalid value for option " + key + ": " + value);
        }

        if (key == "blksize")
        {
            // RFC 2348: server may only answer with a value <= the requested one
            if (*number < MIN_BLKSIZE || *number > *parseNumber(req->second) || *number > MAX_BLKSIZE)
            {
                return reject("Invalid blksize in OACK: " + value);
            }
            blockSize = static_cast<size_t>(*number);
        }
        else if (key == "timeout")
        {
            // RFC 2349: server must echo the requested timeout
            if (value != req->second || *number < MIN_TIMEOUT_SEC || *number > MAX_TIMEOUT_SEC)
            {
                return reject("Invalid timeout in OACK: " + value);
            }
            timeoutSec = static_cast<int>(*number);
        }
        else if (key == "tsize")
        {
            // RFC 2349: for WRQ the server echoes the announced size
            if (isWrite && value != req->second)
            {
                return reject("Invalid tsize in OACK: " + value);
            }
            result.transferSize = static_cast<size_t>(*number);
        }
    }

    session.blockSize        = blockSize;
    session.timeoutSec       = timeoutSec;
    result.blockSize         = blockSize;
    result.negotiatedOptions = oack.getOptions();
    return true;
}

std::unique_ptr<Packet> Client::exchange(const Packet& pkt, Session& session,
                                         const std::function<bool(const Packet&)>& accept)
{
    using Clock = std::chrono::steady_clock;

    for (int attempt = 0; attempt <= m_config.maxRetries; ++attempt)
    {
        const auto& dest = session.peerKnown ? session.peer : m_serverAddr;
        if (!sendPacket(pkt, dest))
        {
            return nullptr;
        }

        auto deadline = Clock::now() + std::chrono::seconds(session.timeoutSec);

        while (true)
        {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now());
            if (remaining.count() <= 0 || !m_socket.waitReadable(static_cast<int>(remaining.count())))
            {
                break; // timeout -> retransmit pkt
            }

            struct sockaddr_in sender{};
            auto reply = receivePacket(sender, 0);
            if (!reply)
            {
                continue; // malformed packet -> keep waiting until deadline
            }

            if (!sameHost(sender, m_serverAddr))
            {
                continue;
            }

            if (!session.peerKnown)
            {
                // RFC 1350: the server answers from a new port (its TID) which
                // is used for the rest of the transfer.
                session.peer      = sender;
                session.peerKnown = true;
            }
            else if (!sameTid(sender, session.peer))
            {
                ErrorPacket err(ErrorCode::UnknownTID, "Unknown transfer ID");
                sendPacket(err, sender);
                continue;
            }

            if (accept && !accept(*reply))
            {
                // Stale/duplicate packet: do not resend immediately (Sorcerer's
                // Apprentice Syndrome), just keep waiting until deadline.
                continue;
            }

            return reply;
        }
    }

    return nullptr;
}

TransferResult Client::fail(TransferResult& result, ErrorCode code, const std::string& msg) const
{
    result.success      = false;
    result.errorCode    = code;
    result.errorMessage = msg;
    return result;
}

void Client::sendError(Session& session, ErrorCode code, const std::string& msg)
{
    if (session.peerKnown)
    {
        sendPacket(ErrorPacket(code, msg), session.peer);
    }
}

TransferResult Client::get(const std::string& filename, std::vector<uint8_t>& data,
                           const TransferOptions& options)
{
    TransferResult result;
    Session session;
    session.timeoutSec = m_config.timeoutSec;
    data.clear();

    auto requested = buildOptions(options, 0, false);
    RequestPacket rrq(Opcode::RRQ, filename, "octet");
    for (const auto& [key, value] : requested)
    {
        rrq.addOption(key, value);
    }

    std::unique_ptr<Packet> lastSent = std::make_unique<RequestPacket>(rrq);
    auto reply = exchange(*lastSent, session);

    bool     negotiated = false;
    uint16_t expected   = 1;

    while (true)
    {
        if (!reply)
        {
            return fail(result, ErrorCode::NotDefined, "Timed out waiting for server");
        }

        switch (reply->getOpcode())
        {
            case Opcode::ERROR: {
                auto err = static_cast<ErrorPacket*>(reply.get());
                return fail(result, err->getErrorCode(), err->getErrorMessage());
            }

            case Opcode::OACK: {
                if (expected != 1)
                {
                    sendError(session, ErrorCode::IllegalOp, "Unexpected OACK");
                    return fail(result, ErrorCode::IllegalOp, "Unexpected OACK during transfer");
                }
                if (!negotiated)
                {
                    if (!applyOack(*static_cast<OackPacket*>(reply.get()), requested, false, session, result))
                    {
                        return result;
                    }
                    negotiated = true;
                    lastSent   = std::make_unique<AckPacket>(0);
                }
                // Acknowledge the OACK (again, if it was retransmitted)
                reply = exchange(*lastSent, session);
                break;
            }

            case Opcode::DATA: {
                auto dataPkt   = static_cast<DataPacket*>(reply.get());
                uint16_t block = dataPkt->getBlockNumber();

                if (block == expected)
                {
                    const auto& payload = dataPkt->getData();
                    data.insert(data.end(), payload.begin(), payload.end());
                    result.bytesTransferred += payload.size();
                    lastSent = std::make_unique<AckPacket>(block);

                    if (payload.size() < session.blockSize)
                    {
                        // Final block: send the last ACK and finish
                        sendPacket(*lastSent, session.peer);
                        if (result.transferSize && *result.transferSize != result.bytesTransferred)
                        {
                            return fail(result, ErrorCode::NotDefined,
                                        "Received " + std::to_string(result.bytesTransferred) +
                                        " bytes but tsize announced " + std::to_string(*result.transferSize));
                        }
                        result.success = true;
                        return result;
                    }

                    ++expected;
                    reply = exchange(*lastSent, session);
                }
                else if (block == static_cast<uint16_t>(expected - 1))
                {
                    // Duplicate block (our ACK got lost): acknowledge again
                    reply = exchange(*lastSent, session);
                }
                else
                {
                    // Unexpected block: re-send last ACK and keep waiting
                    reply = exchange(*lastSent, session);
                }
                break;
            }

            default:
                sendError(session, ErrorCode::IllegalOp, "Unexpected packet");
                return fail(result, ErrorCode::IllegalOp, "Unexpected packet during download");
        }
    }
}

TransferResult Client::put(const std::string& filename, const std::vector<uint8_t>& data,
                           const TransferOptions& options)
{
    TransferResult result;
    Session session;
    session.timeoutSec = m_config.timeoutSec;

    auto requested = buildOptions(options, data.size(), true);
    RequestPacket wrq(Opcode::WRQ, filename, "octet");
    for (const auto& [key, value] : requested)
    {
        wrq.addOption(key, value);
    }

    auto reply = exchange(wrq, session);
    if (!reply)
    {
        return fail(result, ErrorCode::NotDefined, "Timed out waiting for server");
    }

    switch (reply->getOpcode())
    {
        case Opcode::ERROR: {
            auto err = static_cast<ErrorPacket*>(reply.get());
            return fail(result, err->getErrorCode(), err->getErrorMessage());
        }
        case Opcode::OACK:
            if (!applyOack(*static_cast<OackPacket*>(reply.get()), requested, true, session, result))
            {
                return result;
            }
            break;
        case Opcode::ACK:
            if (static_cast<AckPacket*>(reply.get())->getBlockNumber() == 0)
            {
                break; // Server does not support options (RFC 1350)
            }
            [[fallthrough]];
        default:
            sendError(session, ErrorCode::IllegalOp, "Expected OACK or ACK 0");
            return fail(result, ErrorCode::IllegalOp, "Expected OACK or ACK 0");
    }

    uint16_t block  = 1;
    size_t   offset = 0;

    while (true)
    {
        size_t chunk = std::min(session.blockSize, data.size() - offset);
        DataPacket dataPkt(block, std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + chunk));

        // Accept only the ACK for the current block (or an ERROR from the server).
        // Stale ACKs for earlier blocks are ignored until the attempt times out
        // and retransmits dataPkt (avoids Sorcerer's Apprentice Syndrome).
        reply = exchange(dataPkt, session, [block](const Packet& p) {
            if (p.getOpcode() == Opcode::ERROR) return true;
            if (p.getOpcode() == Opcode::ACK)
            {
                return static_cast<const AckPacket&>(p).getBlockNumber() == block;
            }
            return false;
        });

        if (!reply)
        {
            return fail(result, ErrorCode::NotDefined,
                        "Timed out waiting for ACK of block " + std::to_string(block));
        }

        if (reply->getOpcode() == Opcode::ERROR)
        {
            auto err = static_cast<ErrorPacket*>(reply.get());
            return fail(result, err->getErrorCode(), err->getErrorMessage());
        }

        offset += chunk;
        result.bytesTransferred += chunk;

        // A block shorter than blksize (possibly empty) marks the end of the transfer
        if (chunk < session.blockSize)
        {
            result.success = true;
            return result;
        }
        ++block;
    }
}

} // namespace tftp
