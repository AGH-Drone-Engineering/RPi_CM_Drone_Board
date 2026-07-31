#include "DeviceLock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace {

void sleepMs(uint32_t ms)
{
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(ms / 1000);
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;

    // Finishing the wait early only costs us one wasted lock attempt, so a
    // signal cutting the sleep short doesn't need to be resumed.
    ::nanosleep(&ts, nullptr);
}

} // namespace

DeviceLock::DeviceLock(const std::string& device, uint32_t waitMs, uint32_t retries)
{
    // O_NONBLOCK: opening a tty can otherwise block waiting for carrier. This
    // descriptor only ever holds the lock, it never transfers data, so it also
    // deliberately leaves the line settings alone.
    fd_ = ::open(device.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(),
            "DeviceLock: failed to open " + device);
    }

    for (uint32_t attempt = 0;; ++attempt) {
        int rc;
        do {
            rc = ::flock(fd_, LOCK_EX | LOCK_NB);
        } while (rc != 0 && errno == EINTR);

        if (rc == 0) {
            return;
        }

        // EWOULDBLOCK is the only "someone else holds it" answer; anything
        // else (EBADF, ENOLCK, ...) is a real failure that retrying won't fix.
        int err = errno;
        if (err != EWOULDBLOCK) {
            ::close(fd_);
            throw std::system_error(err, std::generic_category(),
                "DeviceLock: flock failed on " + device);
        }

        if (attempt >= retries) {
            ::close(fd_);
            throw std::system_error(EBUSY, std::generic_category(),
                "DeviceLock: " + device + " is in use by another loracom instance");
        }

        sleepMs(waitMs);
    }
}

DeviceLock::~DeviceLock()
{
    if (fd_ >= 0) {
        ::close(fd_); // releases the flock
    }
}
