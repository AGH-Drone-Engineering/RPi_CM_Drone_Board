#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class Parity : uint8_t
{
    None,
    Even,
    Odd,
};

// Thin RAII wrapper around a POSIX serial device (open/configure/read/write/close).
class BasicUart
{
public:
    BasicUart(const std::string& device,
              uint32_t baudrate = 115200,
              uint8_t dataBits = 8,
              Parity parity = Parity::None,
              uint8_t stopBits = 1,
              bool flowControl = false);
    ~BasicUart();

    BasicUart(const BasicUart&) = delete;
    BasicUart& operator=(const BasicUart&) = delete;
    BasicUart(BasicUart&& other) noexcept;
    BasicUart& operator=(BasicUart&& other) noexcept;

    bool write(const std::vector<uint8_t>& data);
    std::vector<uint8_t> read();

    // Waits up to timeoutMs for data to arrive, then reads whatever is available.
    // Returns {} on timeout (or a read error).
    std::vector<uint8_t> read(uint32_t timeoutMs);

private:
    int fd_;
};
