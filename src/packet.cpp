#include "tftp/packet.h"

namespace tftp {

// Helper: Append uint16 in Network Byte Order (Big Endian)
void appendU16(std::vector<uint8_t>& buf, uint16_t val) 
{
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back(val & 0xFF);
}

// Helper: Append String + Null Terminator
void appendString(std::vector<uint8_t>& buf, const std::string& str) 
{
    buf.insert(buf.end(), str.begin(), str.end());
    buf.push_back(0x00);
}

// Helper: Read String from vector buffer
std::string readString(const std::vector<uint8_t>& buffer, size_t& offset) 
{
    std::string str;
    size_t len = buffer.size();
    while (offset < len && buffer[offset] != 0x00) {
        str += static_cast<char>(buffer[offset]);
        offset++;
    }
    if (offset < len) offset++; // skip null
    return str;
}

//--- Packet Factory
std::unique_ptr<Packet> Packet::parse(const uint8_t* buffer, size_t len) 
{
    if (len < 2) 
    {
        throw std::runtime_error("Packet too small for TFTP packet");
    }
    
    std::vector<uint8_t> vecBuffer(buffer, buffer + len);

    uint16_t opcode = (vecBuffer[0] << 8) | vecBuffer[1];
    Opcode op = static_cast<Opcode>(opcode);

    size_t offset = 2;

    switch (static_cast<Opcode>(opcode)) {
        case Opcode::RRQ:
        case Opcode::WRQ: {
            std::string filename = readString(vecBuffer, offset);
            std::string mode     = readString(vecBuffer, offset);
            auto pkt = std::make_unique<RequestPacket>(static_cast<Opcode>(opcode), filename, mode);

            // Parse options if any
            while (offset < len) 
            {
                std::string key = readString(vecBuffer, offset);
                if (key.empty()) break;
                std::string value = readString(vecBuffer, offset);
                pkt->addOption(key, value);
            }

            return pkt;
        }

        case Opcode::DATA: {
            if (len < 4) throw std::runtime_error("Data packet too short");
            uint16_t block = (vecBuffer[2] << 8) | vecBuffer[3];
            std::vector<uint8_t> data(vecBuffer.begin() + 4, vecBuffer.end());
            return std::make_unique<DataPacket>(block, data);
        }

        case Opcode::ACK: {
            if (len < 4) throw std::runtime_error("ACK packet too short");
            uint16_t block = (vecBuffer[2] << 8) | vecBuffer[3];
            return std::make_unique<AckPacket>(block);
        }

        case Opcode::ERROR: {
            uint16_t code = (vecBuffer[2] << 8) | vecBuffer[3];
            std::string msg = readString(vecBuffer, offset);
            return std::make_unique<ErrorPacket>(static_cast<ErrorCode>(code), msg);
        }

        case Opcode::OACK: {
            auto pkt = std::make_unique<OackPacket>();
            
            // Parse options if any
            while (offset < len) 
            {
                std::string key = readString(vecBuffer, offset);
                if (offset >= len) break;
                std::string value = readString(vecBuffer, offset);
                pkt->addOption(key, value);
            }
            return pkt;
        }

        default:
            throw std::runtime_error("Unsupported or unimplemented TFTP opcode");
    }
}

//--- Request Package
RequestPacket::RequestPacket(Opcode type, std::string filename, std::string mode)
    : m_type(type), m_filename(std::move(filename)), m_mode(std::move(mode)) 
{

}

std::vector<uint8_t> RequestPacket::serialize() const 
{
    std::vector<uint8_t> buf;
    appendU16(buf, static_cast<uint16_t>(m_type));
    appendString(buf, m_filename);
    appendString(buf, m_mode);

    for (const auto& [key, val] : m_options) 
    {
        appendString(buf, key);
        appendString(buf, val);
    }

    return buf;
}


//--- Data Package
DataPacket::DataPacket(uint16_t blockNum, const std::vector<uint8_t>& data)
    : m_blockNumber(blockNum), m_data(data) 
{

}

std::vector<uint8_t> DataPacket::serialize() const 
{
    std::vector<uint8_t> buf;
    appendU16(buf, static_cast<uint16_t>(Opcode::DATA));
    appendU16(buf, m_blockNumber);

    buf.insert(buf.end(), m_data.begin(), m_data.end());
    
    return buf;
}


//--- Ack Package
AckPacket::AckPacket(uint16_t blockNum) 
    : m_blockNumber(blockNum) 
{

}

std::vector<uint8_t> AckPacket::serialize() const 
{
    std::vector<uint8_t> buf;
    appendU16(buf, static_cast<uint16_t>(Opcode::ACK));
    appendU16(buf, m_blockNumber);

    return buf;
}


//--- Error Package
ErrorPacket::ErrorPacket(ErrorCode code, std::string msg)
    : m_errorCode(code), m_errorMessage(std::move(msg)) 
{

}

std::vector<uint8_t> ErrorPacket::serialize() const 
{
    std::vector<uint8_t> buf;
    appendU16(buf, static_cast<uint16_t>(Opcode::ERROR));
    appendU16(buf, static_cast<uint16_t>(m_errorCode));
    appendString(buf, m_errorMessage);

    return buf;
}


//--- OAck Package
void OackPacket::addOption(const std::string& key, const std::string& value) 
{
    m_options[key] = value;
}

std::vector<uint8_t> OackPacket::serialize() const 
{
    std::vector<uint8_t> buf;
    appendU16(buf, static_cast<uint16_t>(Opcode::OACK));

    for (const auto& [key, val] : m_options) 
    {
        appendString(buf, key);
        appendString(buf, val);
    }

    return buf;
}

} // namespace tftp