#pragma once

#include <cstdint>
#include <string>

// Inter-process mutex over a serial device: only one loracom transaction runs
// on a given UART at a time. Without it two concurrent instances interleave
// their frames and each may consume the other's reply.
//
// The lock is an flock() on a dedicated descriptor for the device itself, not
// on a lock file in /run/lock or /tmp: anyone allowed to use the port can open
// it, so there are no lock-directory permissions to get wrong (bootstrap runs
// as root, scripts don't) and no stale files to clean up. The kernel releases
// it when the fd closes - including when the process is killed or crashes.
//
// The descriptor is separate from the one BasicUart opens, because BasicUart's
// constructor reconfigures the line and flushes it, which would discard bytes
// another instance is in the middle of receiving. The lock therefore has to be
// held before BasicUart is constructed, so it needs an fd of its own.
class DeviceLock
{
public:
    // Tries to take the lock immediately; if another instance holds it, waits
    // waitMs and retries, up to `retries` times. Throws std::system_error with
    // EBUSY once those are exhausted, or with the underlying errno if
    // open()/flock() fails outright.
    DeviceLock(const std::string& device, uint32_t waitMs, uint32_t retries);
    ~DeviceLock();

    DeviceLock(const DeviceLock&) = delete;
    DeviceLock& operator=(const DeviceLock&) = delete;
    DeviceLock(DeviceLock&&) = delete;
    DeviceLock& operator=(DeviceLock&&) = delete;

private:
    int fd_;
};
