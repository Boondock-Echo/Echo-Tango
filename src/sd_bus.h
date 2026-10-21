#pragma once

#include <Arduino.h>
#include <FS.h>
#include <Stream.h>
#include <SD_MMC.h>
#include "config.h"

// Firmware-wide SD_MMC serialization. ESP32 SD_MMC is not task-safe; RecordTask,
// NetworkTask, logger, maintenance, MQTT, and CLI must not issue overlapping
// card operations. See docs/SD_BUS_LOCK.md.
//
// Lock order: domain mutexes (log, /pending, upload) first, SD bus last.
// Never take another mutex while holding the SD bus. Never hold the bus across
// Wi-Fi, long delays, or work that can log (logging may wait on this lock).

namespace sd_bus
{
    void init();

    bool lock(uint32_t timeoutMs = SD_BUS_LOCK_TIMEOUT_MS);
    void unlock();

    class Guard
    {
    public:
        explicit Guard(uint32_t timeoutMs = SD_BUS_LOCK_TIMEOUT_MS);
        ~Guard();
        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;
        bool locked() const { return locked_; }

    private:
        bool locked_ = false;
    };

    class SdFile : public Stream
    {
    public:
        SdFile() = default;
        explicit SdFile(File file);
        SdFile(const SdFile &other);
        SdFile &operator=(const SdFile &other);
        SdFile &operator=(File file);

        explicit operator bool() const;

        int available() override;
        int read() override;
        int peek() override;
        void flush() override;
        size_t write(uint8_t c) override;
        size_t write(const uint8_t *buf, size_t size) override;

        size_t read(uint8_t *buf, size_t size);
        bool seek(uint32_t pos);
        void close();
        size_t size();
        bool isDirectory();
        const char *name();
        SdFile openNextFile(const char *mode = FILE_READ);

    private:
        File file_;
    };

    bool exists(const char *path);
    bool exists(const String &path);
    bool mkdir(const char *path);
    bool mkdir(const String &path);
    bool rename(const char *pathFrom, const char *pathTo);
    bool rename(const String &pathFrom, const String &pathTo);
    bool remove(const char *path);
    bool remove(const String &path);
    bool rmdir(const char *path);
    bool rmdir(const String &path);

    SdFile open(const char *path, const char *mode = FILE_READ);
    SdFile open(const String &path, const char *mode = FILE_READ);

    uint8_t cardType();
    uint64_t totalBytes();
    uint64_t usedBytes();

    bool mount(const char *mountpoint, bool mode1bit, bool formatIfMountFailed, uint32_t frequency, uint8_t maxOpenFiles);
    void unmount();
}