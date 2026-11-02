// End-to-end in-memory tests: the C++ tftp::Server is tested using tools/tftp_test_client.py.

#include "test_utils.h"
#include "tftp/server.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace tftp_test;

class ServerTest : public ::testing::Test {
protected:
    static constexpr const char* kDownloadFile     = "test.bin";
    static constexpr size_t      kDownloadSize     = 50000;
    static constexpr size_t      kUploadSize       = 10000;
    static constexpr size_t      kServerMaxBlksize = 1428;

    static void SetUpTestSuite()
    {
        s_serverData = makeTestData(kDownloadSize);

        s_server = std::make_unique<tftp::Server>(tftp::ServerConfig{0, 1, 512});
        s_server->setReadHandler([](const std::string& filename) {
            if (filename != kDownloadFile) throw std::runtime_error("File not found");
            return s_serverData;
        });
        s_server->setWriteHandler([](const std::string&, const std::vector<uint8_t>& data) {
            s_uploaded = data;
        });

        s_serverThread = std::thread([] { s_server->run(); });
    }

    static void TearDownTestSuite()
    {
        s_server->stop();
        s_serverThread.join();
        s_server.reset();
    }

    void SetUp() override { s_uploaded.clear(); }

    /** Invoke: python3 tftp_test_client.py 127.0.0.1 -p <port> <cliArgs...> */
    static ProcessResult runClient(const std::vector<std::string>& cliArgs)
    {
        std::vector<std::string> cmd = {
            PYTHON_EXE, "-u", TFTP_TEST_CLIENT_SCRIPT,
            "127.0.0.1", "-p", std::to_string(s_server->getPort()),
            "--server-max-blksize", std::to_string(kServerMaxBlksize),
            "-t", "1"
        };
        cmd.insert(cmd.end(), cliArgs.begin(), cliArgs.end());
        return runProcess(cmd);
    }

    /** Download kDownloadFile via tftp_test_client.py and verify in memory. */
    static void expectGet(const std::vector<std::string>& extraFlags = {})
    {
        std::vector<std::string> args = {"get", kDownloadFile, "--size", std::to_string(kDownloadSize)};
        args.insert(args.end(), extraFlags.begin(), extraFlags.end());

        auto res = runClient(args);
        EXPECT_EQ(res.exitCode, 0) << res.output;
    }

    /** Upload 'size' bytes in memory via tftp_test_client.py and verify what the server received. */
    static void expectPut(size_t size, const std::vector<std::string>& extraFlags = {})
    {
        std::vector<std::string> args = {"put", "upload.bin", "--size", std::to_string(size)};
        args.insert(args.end(), extraFlags.begin(), extraFlags.end());

        auto res = runClient(args);
        ASSERT_EQ(res.exitCode, 0) << res.output;
        EXPECT_EQ(s_uploaded, makeTestData(size));
    }

    static inline std::unique_ptr<tftp::Server> s_server;
    static inline std::thread                   s_serverThread;
    static inline std::vector<uint8_t>          s_serverData;
    static inline std::vector<uint8_t>          s_uploaded;
};

// --- RRQ (server sends) -----------------------------------------------------

TEST_F(ServerTest, RrqWithoutOptions)               { expectGet(); }
TEST_F(ServerTest, RrqBlksize1024)                  { expectGet({"--blksize", "1024"}); }
TEST_F(ServerTest, RrqBlksize1428)                  { expectGet({"--blksize", "1428"}); }
TEST_F(ServerTest, RrqBlksizeExplicit512)           { expectGet({"--blksize", "512"}); }
TEST_F(ServerTest, RrqBlksizeExactMultiple)         { expectGet({"--blksize", "1000"}); }
TEST_F(ServerTest, RrqBlksizeMinimum)               { expectGet({"--blksize", "8"}); }
TEST_F(ServerTest, RrqBlksizeMaximumClamped)        { expectGet({"--blksize", "65464"}); }
TEST_F(ServerTest, RrqBlksizeAboveRfcMaximum)       { expectGet({"--blksize", "70000"}); }
TEST_F(ServerTest, RrqTsize)                        { expectGet({"--tsize"}); }
TEST_F(ServerTest, RrqTimeout1)                     { expectGet({"--timeout", "1"}); }
TEST_F(ServerTest, RrqTimeout5)                     { expectGet({"--timeout", "5"}); }
TEST_F(ServerTest, RrqAllOptions)                   { expectGet({"--blksize", "1428", "--tsize", "--timeout", "3"}); }
TEST_F(ServerTest, RrqUppercaseOptionNames)         { expectGet({"-O", "BLKSIZE=1024", "-O", "TSIZE=0"}); }
TEST_F(ServerTest, RrqUnknownOptionOnly)            { expectGet({"-O", "foobar=1"}); }
TEST_F(ServerTest, RrqUnknownOptionWithBlksize)     { expectGet({"-O", "foobar=1", "--blksize", "1024"}); }
TEST_F(ServerTest, RrqInvalidBlksizeIgnored)        { expectGet({"-O", "blksize=4", "-O", "blksize=-5"}); }
TEST_F(ServerTest, RrqInvalidTimeoutNotClamped)     { expectGet({"-O", "timeout=0", "-O", "timeout=500"}); }

TEST_F(ServerTest, RrqMissingFileReturnsError1)
{
    auto res = runClient({"get", "does_not_exist.bin", "--expect-error", "1"});
    EXPECT_EQ(res.exitCode, 0) << res.output;
}

// --- WRQ (server receives) --------------------------------------------------

TEST_F(ServerTest, WrqWithoutOptions)               { expectPut(kUploadSize); }
TEST_F(ServerTest, WrqBlksizeAndTsize)              { expectPut(kUploadSize, {"--blksize", "1024", "--tsize"}); }
TEST_F(ServerTest, WrqAllOptions)                   { expectPut(kUploadSize, {"--blksize", "1428", "--tsize", "--timeout", "3"}); }
TEST_F(ServerTest, WrqExactBlockMultiple)           { expectPut(4096, {"--blksize", "1024", "--tsize"}); }
TEST_F(ServerTest, WrqEmptyFile)                    { expectPut(0, {"--tsize"}); }
