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
    RRQ = 1,
    WRQ = 2,
    DATA = 3,
    ACK = 4,
    ERROR = 5,
    OACK = 6 // Crucial for ARINC 615A (Options Acknowledgment)
};

// Error Codes
enum class ErrorCode : uint16_t {
    NotDefined = 0,
    FileNotFound = 1,
    AccessViolation = 2,
    DiskFull = 3,
    IllegalOp = 4,
    UnknownTID = 5,
    FileExists = 6,
    NoSuchUser = 7
};

/**
 * Base Packet class. 
 * Designed to be serializable to raw bytes for the network.
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
class RequestPacket : public Packet {
public:
    Opcode type;
    std::string filename;
    std::string mode;
    std::map<std::string, std::string> options;

    RequestPacket(Opcode type, std::string filename, std::string mode = "octet");
    
    Opcode getOpcode() const override { return type; }
    std::vector<uint8_t> serialize() const override;
    
    void addOption(const std::string& key, const std::string& value);
};

class DataPacket : public Packet {
public:
    uint16_t blockNumber;
    std::vector<uint8_t> data;

    DataPacket(uint16_t blockNum, const std::vector<uint8_t>& data);
    
    Opcode getOpcode() const override { return Opcode::DATA; }
    std::vector<uint8_t> serialize() const override;
};

class AckPacket : public Packet {
public:
    uint16_t blockNumber;

    explicit AckPacket(uint16_t blockNum);
    
    Opcode getOpcode() const override { return Opcode::ACK; }
    std::vector<uint8_t> serialize() const override;
};

class ErrorPacket : public Packet {
public:
    ErrorCode errorCode;
    std::string errorMessage;

    ErrorPacket(ErrorCode code, std::string msg);
    
    Opcode getOpcode() const override { return Opcode::ERROR; }
    std::vector<uint8_t> serialize() const override;
};

// OACK Packet for Option Negotiation (RFC 2347)
class OackPacket : public Packet {
public:
    std::map<std::string, std::string> options;

    Opcode getOpcode() const override { return Opcode::OACK; }
    std::vector<uint8_t> serialize() const override;
    void addOption(const std::string& key, const std::string& value);
};

}

#endif