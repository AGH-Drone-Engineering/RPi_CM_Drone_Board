#include "BasicUart.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

speed_t toTermiosSpeed(uint32_t baudrate)
{
    switch (baudrate) {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:
            throw std::runtime_error("BasicUart: unsupported baud rate " + std::to_string(baudrate));
    }
}

tcflag_t toTermiosDataBits(uint8_t dataBits)
{
    switch (dataBits) {
        case 5: return CS5;
        case 6: return CS6;
        case 7: return CS7;
        case 8: return CS8;
        default:
            throw std::runtime_error("BasicUart: unsupported data bit count " + std::to_string(dataBits));
    }
}

} // namespace

BasicUart::BasicUart(const std::string& device, uint32_t baudrate, uint8_t dataBits,
                      Parity parity, uint8_t stopBits, bool flowControl)
{
    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0) {
        throw std::runtime_error("BasicUart: failed to open " + device + ": " + std::strerror(errno));
    }

    termios opt{};
    if (::tcgetattr(fd_, &opt) != 0) {
        ::close(fd_);
        throw std::runtime_error("BasicUart: tcgetattr failed on " + device + ": " + std::strerror(errno));
    }

    speed_t speed = toTermiosSpeed(baudrate);
    ::cfsetispeed(&opt, speed);
    ::cfsetospeed(&opt, speed);

    ::cfmakeraw(&opt);

    opt.c_cflag &= ~CSIZE;
    opt.c_cflag |= toTermiosDataBits(dataBits);

    switch (parity) {
        case Parity::None:
            opt.c_cflag &= ~PARENB;
            break;
        case Parity::Even:
            opt.c_cflag |= PARENB;
            opt.c_cflag &= ~PARODD;
            break;
        case Parity::Odd:
            opt.c_cflag |= PARENB;
            opt.c_cflag |= PARODD;
            break;
    }

    if (stopBits == 2) {
        opt.c_cflag |= CSTOPB;
    } else {
        opt.c_cflag &= ~CSTOPB;
    }

    if (flowControl) {
        opt.c_cflag |= CRTSCTS;
    } else {
        opt.c_cflag &= ~CRTSCTS;
    }

    opt.c_cflag |= (CLOCAL | CREAD);

    // Block until at least one byte is available, then return immediately
    // with whatever else has already arrived alongside it.
    opt.c_cc[VMIN] = 1;
    opt.c_cc[VTIME] = 0;

    if (::tcsetattr(fd_, TCSANOW, &opt) != 0) {
        ::close(fd_);
        throw std::runtime_error("BasicUart: tcsetattr failed on " + device + ": " + std::strerror(errno));
    }

    ::tcflush(fd_, TCIOFLUSH);
}

BasicUart::~BasicUart()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

BasicUart::BasicUart(BasicUart&& other) noexcept
    : fd_(std::exchange(other.fd_, -1))
{
}

BasicUart& BasicUart::operator=(BasicUart&& other) noexcept
{
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

bool BasicUart::write(const std::vector<uint8_t>& data)
{
    size_t total = 0;
    while (total < data.size()) {
        ssize_t written = ::write(fd_, data.data() + total, data.size() - total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        total += static_cast<size_t>(written);
    }
    return true;
}

std::vector<uint8_t> BasicUart::read()
{
    uint8_t buffer[512];
    ssize_t bytesRead = ::read(fd_, buffer, sizeof(buffer));
    if (bytesRead <= 0) {
        return {};
    }
    return std::vector<uint8_t>(buffer, buffer + bytesRead);
}

std::vector<uint8_t> BasicUart::read(uint32_t timeoutMs)
{
    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    int ret = ::poll(&pfd, 1, static_cast<int>(timeoutMs));
    if (ret <= 0) {
        return {}; // timeout or poll error
    }
    return read();
}
