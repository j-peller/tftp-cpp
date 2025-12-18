#include "tftp/server.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <iomanip>

/**
 * Utility to create a large test file if it doesn't exist.
 * This helps in verifying the 'blksize' negotiation and transfer speed.
 */
void ensureLargeTestFile(const std::string& filename, size_t sizeMb) {
    if (std::filesystem::exists(filename)) return;

    std::cout << "[Setup] Creating " << sizeMb << "MB test file: " << filename << "..." << std::endl;
    std::ofstream file(filename, std::ios::binary);
    std::vector<char> dummyData(1024 * 1024, 'A'); // 1MB chunk
    for (size_t i = 0; i < sizeMb; ++i) {
        file.write(dummyData.data(), dummyData.size());
    }
    std::cout << "[Setup] Done." << std::endl;
}

// Helper to read a file from disk
std::vector<uint8_t> readFile(const std::string& filename) {
    if (filename == "dummy.txt") {
        std::string content = "Hello from Custom C++ TFTP Server! This is the short file content.";
        return std::vector<uint8_t>(content.begin(), content.end());
    }
    
    std::ifstream file(filename, std::ios::binary);
    if (!file) throw std::runtime_error("File not found: " + filename);
    
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
}

// Helper to write data to disk
void writeFile(const std::string& filename, const std::vector<uint8_t>& data) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open file for writing: " + filename);    
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
}

int main(int argc, char* argv[]) {
    try {
        uint16_t port = 6969;
        if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));

        // Create a 10MB file for better measurement granularity
        ensureLargeTestFile("large_test.bin", 10);

        tftp::Server server(port);

        // Handler responsible for reading the file from disk
        server.setReadHandler([](const std::string& fname) -> std::vector<uint8_t> {
            std::cout << "[CALLBACK] Starting RRQ for: " << fname << std::endl;
            auto data = readFile(fname);
            return data;
        });

        // Handler responsible for writing the file to disk
        server.setWriteHandler([](const std::string& fname, const std::vector<uint8_t>& data) {
            std::cout << "[CALLBACK] Starting WRQ for: " << fname << std::endl;
            writeFile(fname, data);
        });

        std::cout << "==================================================" << std::endl;
        std::cout << "  TFTP Server (Modern C++ / RFC 2347 Support)     " << std::endl;
        std::cout << "  Listening on Port: " << port << std::endl;
        std::cout << "==================================================" << std::endl;
        std::cout << "To verify throughput with different blocksizes:" << std::endl;
        std::cout << "  1. Connect: tftp 127.0.0.1 " << port << std::endl;
        std::cout << "  2. (Optional) Set blksize: tftp> blksize 1428" << std::endl;
        std::cout << "  3. Transfer: tftp> get large_test.bin" << std::endl;
        std::cout << "==================================================" << std::endl;
        
        server.run();
    } 
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}