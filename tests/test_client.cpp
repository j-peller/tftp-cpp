// End-to-end in-memory tests: the C++ tftp::Client is tested against tools/tftp_test_server.py.

#include "test_utils.h"
#include "tftp/client.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace tftp_test;

class ClientTest : public ::testing::Test {
protected:
    static constexpr const char* kSampleFile = "sample.bin";
    static constexpr size_t      kSampleSize = 20000;

    void SetUp() override { m_sampleData = makeTestData(kSampleSize); }

    /** Launch tftp_test_server.py in memory on an ephemeral port and return a Client. */
    tftp::Client startServerAndMakeClient(const std::vector<std::string>& cliFlags = {})
    {
        std::vector<std::string> cmd = {
            PYTHON_EXE, "-u", TFTP_TEST_SERVER_SCRIPT,
            "-p", "0",
            "-f", kSampleFile,
            "-s", std::to_string(kSampleSize),
            "-t", "0.5",
            "--once"
        };
        cmd.insert(cmd.end(), cliFlags.begin(), cliFlags.end());
        m_server = std::make_unique<BackgroundProcess>(cmd);

        auto ready = m_server->waitForLine("READY ");
        if (!ready)
        {
            ADD_FAILURE() << "tftp_test_server.py failed to start:\n" << m_server->wait().output;
            return tftp::Client({"127.0.0.1", 0, 1, 1});
        }

        uint16_t port = static_cast<uint16_t>(std::stoi(*ready));
        return tftp::Client({"127.0.0.1", port, 1, 3});
    }

    /** Wait for tftp_test_server.py to exit and return its stdout SESSION JSON line. */
    std::string finishServerAndReadSession()
    {
        if (!m_server) return {};
        auto res = m_server->wait();
        EXPECT_EQ(res.exitCode, 0) << res.output;
        m_server.reset();
        return res.output;
    }

    std::vector<uint8_t>               m_sampleData;
    std::unique_ptr<BackgroundProcess> m_server;
};

// --- Client GET (RRQ) tests against Python Server ---------------------------

TEST_F(ClientTest, GetWithoutOptions)
{
    auto client = startServerAndMakeClient();
    std::vector<uint8_t> rx;

    auto res  = client.get(kSampleFile, rx);
    auto json = finishServerAndReadSession();

    ASSERT_TRUE(res) << res.errorMessage;
    EXPECT_TRUE(res.negotiatedOptions.empty());
    EXPECT_EQ(res.blockSize, 512u);
    EXPECT_EQ(rx, m_sampleData);
    EXPECT_NE(json.find("\"req_options\": {}"), std::string::npos) << json;
    EXPECT_NE(json.find("\"oack_options\": {}"), std::string::npos) << json;
}

TEST_F(ClientTest, GetWithBlksizeTsizeTimeout)
{
    auto client = startServerAndMakeClient();
    std::vector<uint8_t> rx;

    auto res  = client.get(kSampleFile, rx, {1428, 2, true});
    auto json = finishServerAndReadSession();

    ASSERT_TRUE(res) << res.errorMessage;
    EXPECT_EQ(res.blockSize, 1428u);
    EXPECT_EQ(res.transferSize, kSampleSize);
    EXPECT_EQ(rx, m_sampleData);
    EXPECT_NE(json.find("\"blksize\": 1428"), std::string::npos) << json;
}

TEST_F(ClientTest, GetRespectsServerClampedBlksize)
{
    auto client = startServerAndMakeClient({"--max-blksize", "1024"});
    std::vector<uint8_t> rx;

    auto res  = client.get(kSampleFile, rx, {4096, {}, false});
    auto json = finishServerAndReadSession();

    ASSERT_TRUE(res) << res.errorMessage;
    EXPECT_EQ(res.blockSize, 1024u);
    EXPECT_EQ(rx, m_sampleData);
    EXPECT_NE(json.find("\"blksize\": 1024"), std::string::npos) << json;
}

TEST_F(ClientTest, GetFallsBackWhenServerIgnoresOptions)
{
    auto client = startServerAndMakeClient({"--ignore-options"});
    std::vector<uint8_t> rx;

    auto res = client.get(kSampleFile, rx, {1428, {}, true});
    finishServerAndReadSession();

    ASSERT_TRUE(res) << res.errorMessage;
    EXPECT_TRUE(res.negotiatedOptions.empty());
    EXPECT_EQ(res.blockSize, 512u);
    EXPECT_EQ(rx, m_sampleData);
}

TEST_F(ClientTest, GetRejectsUnrequestedOptionInOack)
{
    auto client = startServerAndMakeClient({"--bad-oack"});
    std::vector<uint8_t> rx;

    auto res  = client.get(kSampleFile, rx, {1024, {}, false});
    auto json = finishServerAndReadSession();

    EXPECT_FALSE(res);
    EXPECT_EQ(res.errorCode, tftp::ErrorCode::OptionNegotiation);
    EXPECT_NE(json.find("[8,"), std::string::npos) << json;
}

TEST_F(ClientTest, GetReportsServerErrorFileNotFound)
{
    auto client = startServerAndMakeClient();
    std::vector<uint8_t> rx;

    auto res = client.get("missing.bin", rx);
    finishServerAndReadSession();

    EXPECT_FALSE(res);
    EXPECT_EQ(res.errorCode, tftp::ErrorCode::FileNotFound);
}

TEST_F(ClientTest, GetRecoversFromDroppedAck)
{
    auto client = startServerAndMakeClient({"--drop", "2"});
    std::vector<uint8_t> rx;

    auto res  = client.get(kSampleFile, rx, {1024, {}, false});
    auto json = finishServerAndReadSession();

    ASSERT_TRUE(res) << res.errorMessage;
    EXPECT_EQ(rx, m_sampleData);
    EXPECT_NE(json.find("\"dropped\": 1"), std::string::npos) << json;
}

// --- Client PUT (WRQ) tests against Python Server ---------------------------

TEST_F(ClientTest, PutWithoutOptions)
{
    auto client = startServerAndMakeClient();
    auto tx     = makeTestData(10000);

    auto res  = client.put("upload.bin", tx);
    auto json = finishServerAndReadSession();

    ASSERT_TRUE(res) << res.errorMessage;
    EXPECT_TRUE(res.negotiatedOptions.empty());
    EXPECT_NE(json.find("\"data_valid\": true"), std::string::npos) << json;
    EXPECT_NE(json.find("\"bytes\": 10000"), std::string::npos) << json;
    EXPECT_NE(json.find("\"req_options\": {}"), std::string::npos) << json;
}

TEST_F(ClientTest, PutWithBlksizeTsizeTimeout)
{
    auto client = startServerAndMakeClient();
    auto tx     = makeTestData(12345);

    auto res  = client.put("upload.bin", tx, {1428, 2, true});
    auto json = finishServerAndReadSession();

    ASSERT_TRUE(res) << res.errorMessage;
    EXPECT_EQ(res.blockSize, 1428u);
    EXPECT_EQ(res.transferSize, tx.size());
    EXPECT_NE(json.find("\"data_valid\": true"), std::string::npos) << json;
    EXPECT_NE(json.find("\"bytes\": 12345"), std::string::npos) << json;
    EXPECT_NE(json.find("\"tsize\": \"12345\""), std::string::npos) << json;
}

TEST_F(ClientTest, PutExactBlockMultiple)
{
    auto client = startServerAndMakeClient();
    auto tx     = makeTestData(4096); // 4 * 1024 -> requires final 0-byte DATA packet

    auto res  = client.put("upload.bin", tx, {1024, {}, false});
    auto json = finishServerAndReadSession();

    ASSERT_TRUE(res) << res.errorMessage;
    EXPECT_NE(json.find("\"data_valid\": true"), std::string::npos) << json;
    EXPECT_NE(json.find("\"blocks\": 5"), std::string::npos) << json;
    EXPECT_NE(json.find("\"bytes\": 4096"), std::string::npos) << json;
}

TEST_F(ClientTest, PutRejectsUnrequestedOptionInOack)
{
    auto client = startServerAndMakeClient({"--bad-oack"});
    auto tx     = makeTestData(2048);

    auto res  = client.put("upload.bin", tx, {1024, {}, false});
    auto json = finishServerAndReadSession();

    EXPECT_FALSE(res);
    EXPECT_EQ(res.errorCode, tftp::ErrorCode::OptionNegotiation);
    EXPECT_NE(json.find("[8,"), std::string::npos) << json;
}

TEST_F(ClientTest, PutRecoversFromDroppedDataPacket)
{
    auto client = startServerAndMakeClient({"--drop", "2"});
    auto tx     = makeTestData(5000);

    auto res  = client.put("upload.bin", tx, {1024, {}, false});
    auto json = finishServerAndReadSession();

    ASSERT_TRUE(res) << res.errorMessage;
    EXPECT_NE(json.find("\"data_valid\": true"), std::string::npos) << json;
    EXPECT_NE(json.find("\"bytes\": 5000"), std::string::npos) << json;
    EXPECT_NE(json.find("\"dropped\": 1"), std::string::npos) << json;
}
