#pragma once

// Minimal in-memory helpers shared by the TFTP test suite.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <sys/wait.h>

namespace tftp_test {

#ifndef PYTHON_EXE
#define PYTHON_EXE "python3"
#endif
#ifndef TFTP_TEST_CLIENT_SCRIPT
#define TFTP_TEST_CLIENT_SCRIPT "tools/tftp_test_client.py"
#endif
#ifndef TFTP_TEST_SERVER_SCRIPT
#define TFTP_TEST_SERVER_SCRIPT "tools/tftp_test_server.py"
#endif

/**
 * Generate deterministic in-memory test bytes of the given size.
 * Matches make_test_data() in tftp_test_client.py and tftp_test_server.py.
 */
inline std::vector<uint8_t> makeTestData(size_t size)
{
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i)
    {
        out[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    }
    return out;
}

/** Build a single POSIX shell command line from an argument vector. */
inline std::string buildCommand(const std::vector<std::string>& args)
{
    std::string cmd;
    for (const auto& arg : args)
    {
        if (!cmd.empty()) cmd += ' ';
        cmd += '\'';
        for (char c : arg) cmd += (c == '\'') ? "'\\''" : std::string(1, c);
        cmd += '\'';
    }
    return cmd + " 2>&1";
}

struct ProcessResult {
    int         exitCode = -1;
    std::string output;
};

/** Run a command to completion and return its exit code and output in memory. */
inline ProcessResult runProcess(const std::vector<std::string>& args)
{
    ProcessResult res;
    FILE* pipe = popen(buildCommand(args).c_str(), "r");
    if (!pipe) return res;

    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), pipe)) res.output.append(buf, n);

    int status   = pclose(pipe);
    res.exitCode = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return res;
}

/** Background child process with line-by-line stdout reading (used for tftp_test_server.py). */
class BackgroundProcess {
public:
    explicit BackgroundProcess(const std::vector<std::string>& args)
        : m_pipe(popen(buildCommand(args).c_str(), "r"))
    {
    }

    ~BackgroundProcess() { wait(); }

    std::optional<std::string> waitForLine(const std::string& prefix)
    {
        if (!m_pipe) return std::nullopt;
        char line[1024];
        while (fgets(line, sizeof(line), m_pipe))
        {
            std::string s(line);
            m_result.output += s;
            if (s.rfind(prefix, 0) == 0)
            {
                s.erase(0, prefix.size());
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                return s;
            }
        }
        return std::nullopt;
    }

    ProcessResult wait()
    {
        if (m_pipe)
        {
            char buf[4096];
            while (size_t n = fread(buf, 1, sizeof(buf), m_pipe)) m_result.output.append(buf, n);
            int status        = pclose(m_pipe);
            m_result.exitCode = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
            m_pipe            = nullptr;
        }
        return m_result;
    }

private:
    FILE*         m_pipe = nullptr;
    ProcessResult m_result;
};

} // namespace tftp_test
