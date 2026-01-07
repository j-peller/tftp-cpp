#pragma once
#ifndef PACKET_H
#define PACKET_H

#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <memory>

namespace tftp {

// TFTP Opcodes (RFC 1350 + RFC 2347)
enum class Opcode : uint16_t {
    RRQ   = 1,
    WRQ   = 2,
    DATA  = 3,
    ACK   = 4,
    ERROR = 5,
    OACK  = 6 // Crucial for ARINC 615A (Options Acknowledgment)
};

// Error Codes
enum class ErrorCode : uint16_t {
    NotDefined      = 0,
    FileNotFound    = 1,
    AccessViolation = 2,
    DiskFull        = 3,
    IllegalOp       = 4,
    UnknownTID      = 5,
    FileExists      = 6,
    NoSuchUser      = 7
};

/**
 * Base Packet class 
 */
class Packet {
public:
    virtual ~Packet() = default;
    virtual Opcode getOpcode() const = 0;
    
    // Serialize packet to buffer
    virtual std::vector<uint8_t> serialize() const = 0;

    // Factory method to parse raw bytes into a specific Packet subclass
    static std::unique_ptr<Packet> parse(const uint8_t* buffer, size_t len);
};

/**
 * Handles RRQ (Read) and WRQ (Write)
 * Includes Options support for ARINC 615A extensions.
 */
class RequestPacket final : public Packet {
public:
    RequestPacket(Opcode type, std::string filename, std::string mode = "octet") ;
    
    Opcode getOpcode() const override { return m_type; }
    std::vector<uint8_t> serialize() const override;
    
    //--- Getters
    const std::string& getFilename() const { return m_filename; }
    const std::string& getMode() const { return m_mode; }
    const std::map<std::string, std::string>& getOptions() const { return m_options; }

    void addOption(const std::string& key, const std::string& value) { m_options[key] = value; }

private:
    Opcode m_type;
    std::string m_filename;
    std::string m_mode;
    std::map<std::string, std::string> m_options;
};

/**
 * Data Packet
 */
class DataPacket final : public Packet {
public:
    DataPacket(uint16_t blockNum, const std::vector<uint8_t>& data);
    Opcode getOpcode() const override { return Opcode::DATA; }
    std::vector<uint8_t> serialize() const override;

    //--- Getters
    uint16_t getBlockNumber() const { return m_blockNumber; }
    const std::vector<uint8_t>& getData() const { return m_data; }

private:
    uint16_t m_blockNumber;
    std::vector<uint8_t> m_data;
};


/**
 * Acknowledgment Packet
 */
class AckPacket final : public Packet {
public:
    explicit AckPacket(uint16_t blockNum);
    Opcode getOpcode() const override { return Opcode::ACK; }
    std::vector<uint8_t> serialize() const override;

    //--- Getters
    uint16_t getBlockNumber() const { return m_blockNumber; }

private:
    uint16_t m_blockNumber;

};

/**
 * Error Packet
 */
class ErrorPacket final : public Packet {
public:
    ErrorPacket(ErrorCode code, std::string msg);
    Opcode getOpcode() const override { return Opcode::ERROR; }
    std::vector<uint8_t> serialize() const override;

    //--- Getters
    ErrorCode getErrorCode() const { return m_errorCode; }
    const std::string& getErrorMessage() const { return m_errorMessage; }

private:
    ErrorCode m_errorCode;
    std::string m_errorMessage;
};

/**
 * Options Acknowledgment Packet (OACK)
 */
class OackPacket final : public Packet {
public:
    Opcode getOpcode() const override { return Opcode::OACK; }
    std::vector<uint8_t> serialize() const override;
    void addOption(const std::string& key, const std::string& value);

    //--- Getters
    const std::map<std::string, std::string>& getOptions() const { return m_options; }
    
private:
    std::map<std::string, std::string> m_options;
};

}

#endif