#pragma once

#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

class BasicUart
{
public:
    BasicUart(const std::string& device, uint32_t baudrate);
    ~BasicUart();

    bool write(const std::vector<uint8_t>& data);
    std::vector<uint8_t> read();
}


// BasicUart implementation

BasicUart::BasicUart(const std::string& device, uint32_t baudrate)
{
    termios opt{};

}

BasicUart::~BasicUart()
{
    // Close UART file descriptor
}

bool BasicUart::write(const std::vector<uint8_t>& data)
{
    // Write data to UART
}

std::vector<uint8_t> BasicUart::read()
{
    // Read data from UART and return as vector
}