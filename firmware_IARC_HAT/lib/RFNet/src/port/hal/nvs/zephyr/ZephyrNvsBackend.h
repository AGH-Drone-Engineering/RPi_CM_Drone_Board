#pragma once
#include "../INVBackend.h"
#include "../../../Logger.h"

#if defined(__ZEPHYR__)
#include <zephyr/fs/nvs.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

class ZephyrNvsBackend : public INVBackend {
public:
    ZephyrNvsBackend() = default;

    void begin() override {
        _fs.flash_device = FIXED_PARTITION_DEVICE(storage_partition);
        _fs.offset       = FIXED_PARTITION_OFFSET(storage_partition);
        struct flash_pages_info info;
        // info is left uninitialized on failure.
        const int rc = flash_get_page_info_by_offs(_fs.flash_device, _fs.offset, &info);
        if (rc != 0) {
            LOG_E("ZephyrNvs", "begin: flash_get_page_info_by_offs failed (%d) — NV disabled; "
                  "seq/duty will NOT persist across reboot", rc);
            _ready = false;
            return;
        }
        _fs.sector_size  = info.size;
        _fs.sector_count = 2;   // Zephyr NVS minimum sector count.
        _ready = (nvs_mount(&_fs) == 0);
        if (!_ready) {
            LOG_E("ZephyrNvs", "begin: nvs_mount failed — NV disabled; "
                  "seq/duty will NOT persist across reboot");
        }
    }

    bool isPersistent() const override { return _ready; }

    bool read(uint16_t id, void* out, size_t len) override {
        if (!_ready) return false;
        const ssize_t n = nvs_read(&_fs, id, out, len);   // <0 = not found / error
        return n >= 0 && (size_t)n == len;
    }

    bool write(uint16_t id, const void* in, size_t len) override {
        if (!_ready) return false;
        // nvs_write returns bytes written, 0 if unchanged, or a negative errno.
        return nvs_write(&_fs, id, in, len) >= 0;
    }

private:
    nvs_fs _fs{};
    bool   _ready = false;
};
#endif
