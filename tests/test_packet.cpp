// Unit tests for tftp::Packet serialization/parsing and C++ Client <-> Server
// loopback transfers.

#include "test_utils.h"
#include "tftp/client.h"
#include "tftp/packet.h"
#include "tftp/server.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace tftp;

TEST(PacketTest, RequestPacketRoundTrip)
{
    RequestPacket req(Opcode::RRQ, "firmware.bin", "octet");
    req.addOption("blksize", "1428");
    req.addOption("tsize", "0");

    auto raw    = req.serialize();
    auto parsed = Packet::parse(raw.data(), raw.size());
    ASSERT_NE(parsed, nullptr);
    ASSERT_EQ(parsed->getOpcode(), Opcode::RRQ);

    auto* out = static_cast<RequestPacket*>(parsed.get());
    EXPECT_EQ(out->getFilename(), "firmware.bin");
    EXPECT_EQ(out->getMode(), "octet");
    ASSERT_EQ(out->getOptions().size(), 2u);
    EXPECT_EQ(out->getOptions().at("blksize"), "1428");
    EXPECT_EQ(out->getOptions().at("tsize"), "0");
}

TEST(PacketTest, OptionNamesNormalizedToLowercase)
{
    RequestPacket req(Opcode::WRQ, "test.bin", "OCTET");
    req.addOption("BLKSIZE", "1024");
    req.addOption("TSize", "4096");

    auto raw    = req.serialize();
    auto parsed = Packet::parse(raw.data(), raw.size());
    auto* out   = static_cast<RequestPacket*>(parsed.get());

    EXPECT_EQ(out->getMode(), "octet");
    EXPECT_EQ(out->getOptions().at("blksize"), "1024");
    EXPECT_EQ(out->getOptions().at("tsize"), "4096");
}

TEST(PacketTest, DataAckErrorOackRoundTrip)
{
    DataPacket data(42, {1, 2, 3, 4, 5});
    auto dRaw    = data.serialize();
    auto dParsed = Packet::parse(dRaw.data(), dRaw.size());
    ASSERT_EQ(dParsed->getOpcode(), Opcode::DATA);
    EXPECT_EQ(static_cast<DataPacket*>(dParsed.get())->getBlockNumber(), 42);
    EXPECT_EQ(static_cast<DataPacket*>(dParsed.get())->getData(), (std::vector<uint8_t>{1, 2, 3, 4, 5}));

    AckPacket ack(7);
    auto aRaw    = ack.serialize();
    auto aParsed = Packet::parse(aRaw.data(), aRaw.size());
    ASSERT_EQ(aParsed->getOpcode(), Opcode::ACK);
    EXPECT_EQ(static_cast<AckPacket*>(aParsed.get())->getBlockNumber(), 7);

    ErrorPacket err(ErrorCode::FileNotFound, "not here");
    auto eRaw    = err.serialize();
    auto eParsed = Packet::parse(eRaw.data(), eRaw.size());
    ASSERT_EQ(eParsed->getOpcode(), Opcode::ERROR);
    EXPECT_EQ(static_cast<ErrorPacket*>(eParsed.get())->getErrorCode(), ErrorCode::FileNotFound);
    EXPECT_EQ(static_cast<ErrorPacket*>(eParsed.get())->getErrorMessage(), "not here");

    OackPacket oack;
    oack.addOption("BLKSIZE", "1024");
    auto oRaw    = oack.serialize();
    auto oParsed = Packet::parse(oRaw.data(), oRaw.size());
    ASSERT_EQ(oParsed->getOpcode(), Opcode::OACK);
    EXPECT_EQ(static_cast<OackPacket*>(oParsed.get())->getOptions().at("blksize"), "1024");
}

TEST(PacketTest, MalformedPacketsThrow)
{
    uint8_t empty[]   = {0};
    uint8_t badOp[]   = {0, 99, 0, 0};
    uint8_t shortDt[] = {0, 3, 0};

    EXPECT_THROW(Packet::parse(empty, 1), std::runtime_error);
    EXPECT_THROW(Packet::parse(badOp, sizeof(badOp)), std::runtime_error);
    EXPECT_THROW(Packet::parse(shortDt, sizeof(shortDt)), std::runtime_error);
}

TEST(LoopbackTest, CppClientAndServerRoundTrip)
{
    std::vector<uint8_t> stored;

    Server server({0, 1, 512});
    server.setReadHandler([&](const std::string&) { return stored; });
    server.setWriteHandler([&](const std::string&, const std::vector<uint8_t>& d) { stored = d; });
    std::thread srvThread([&] { server.run(); });

    Client client({"127.0.0.1", server.getPort(), 1, 3});
    TransferOptions opts{1428, 1, true};

    auto uploaded = tftp_test::makeTestData(33333);
    ASSERT_TRUE(client.put("test.bin", uploaded, opts));

    std::vector<uint8_t> downloaded;
    auto getRes = client.get("test.bin", downloaded, opts);
    ASSERT_TRUE(getRes) << getRes.errorMessage;
    EXPECT_EQ(downloaded, uploaded);

    server.stop();
    srvThread.join();
}
