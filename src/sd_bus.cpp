#include "sd_bus.h"

#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace sd_bus
{
    namespace
    {
        SemaphoreHandle_t g_sdBusMutex = nullptr;

        void ensureMutex()
        {
            if (g_sdBusMutex != nullptr)
            {
                return;
            }
            g_sdBusMutex = xSemaphoreCreateRecursiveMutex();
        }

        bool takeRecursive(uint32_t timeoutMs)
        {
            ensureMutex();
            if (g_sdBusMutex == nullptr)
            {
                return false;
            }

            const TickType_t sliceTicks = pdMS_TO_TICKS(SD_BUS_LOCK_WAIT_SLICE_MS > 0 ? SD_BUS_LOCK_WAIT_SLICE_MS : 1);
            uint32_t waitedMs = 0;
            while (true)
            {
                if (xSemaphoreTakeRecursive(g_sdBusMutex, sliceTicks) == pdTRUE)
                {
                    return true;
                }
                (void)esp_task_wdt_reset();
                if (timeoutMs == 0)
                {
                    return false;
                }
                waitedMs += SD_BUS_LOCK_WAIT_SLICE_MS;
                if (waitedMs >= timeoutMs)
                {
                    return false;
                }
            }
        }
    } // namespace

    void init()
    {
        ensureMutex();
    }

    bool lock(uint32_t timeoutMs)
    {
        return takeRecursive(timeoutMs);
    }

    void unlock()
    {
        if (g_sdBusMutex != nullptr)
        {
            xSemaphoreGiveRecursive(g_sdBusMutex);
        }
    }

    Guard::Guard(uint32_t timeoutMs)
        : locked_(takeRecursive(timeoutMs))
    {
    }

    Guard::~Guard()
    {
        if (locked_ && g_sdBusMutex != nullptr)
        {
            xSemaphoreGiveRecursive(g_sdBusMutex);
        }
    }

    SdFile::SdFile(File file)
        : file_(file)
    {
    }

    SdFile::SdFile(const SdFile &other)
        : file_(other.file_)
    {
    }

    SdFile &SdFile::operator=(const SdFile &other)
    {
        if (this != &other)
        {
            file_ = other.file_;
        }
        return *this;
    }

    SdFile &SdFile::operator=(File file)
    {
        file_ = file;
        return *this;
    }

    SdFile::operator bool() const
    {
        return static_cast<bool>(file_);
    }

    int SdFile::available()
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return 0;
        }
        return file_.available();
    }

    int SdFile::read()
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return -1;
        }
        return file_.read();
    }

    int SdFile::peek()
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return -1;
        }
        return file_.peek();
    }

    void SdFile::flush()
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return;
        }
        file_.flush();
    }

    size_t SdFile::write(uint8_t c)
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return 0;
        }
        return file_.write(c);
    }

    size_t SdFile::write(const uint8_t *buf, size_t size)
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return 0;
        }
        return file_.write(buf, size);
    }

    size_t SdFile::read(uint8_t *buf, size_t size)
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return 0;
        }
        return file_.read(buf, size);
    }

    bool SdFile::seek(uint32_t pos)
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return false;
        }
        return file_.seek(pos);
    }

    void SdFile::close()
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return;
        }
        file_.close();
    }

    size_t SdFile::size()
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return 0;
        }
        return file_.size();
    }

    bool SdFile::isDirectory()
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return false;
        }
        return file_.isDirectory();
    }

    const char *SdFile::name()
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return "";
        }
        return file_.name();
    }

    SdFile SdFile::openNextFile(const char *mode)
    {
        Guard g;
        if (!g.locked() || !file_)
        {
            return SdFile();
        }
        return SdFile(file_.openNextFile(mode));
    }

    bool exists(const char *path)
    {
        Guard g;
        if (!g.locked() || path == nullptr)
        {
            return false;
        }
        return SD_MMC.exists(path);
    }

    bool exists(const String &path)
    {
        return exists(path.c_str());
    }

    bool mkdir(const char *path)
    {
        Guard g;
        if (!g.locked() || path == nullptr)
        {
            return false;
        }
        return SD_MMC.mkdir(path);
    }

    bool mkdir(const String &path)
    {
        return mkdir(path.c_str());
    }

    bool rename(const char *pathFrom, const char *pathTo)
    {
        Guard g;
        if (!g.locked() || pathFrom == nullptr || pathTo == nullptr)
        {
            return false;
        }
        return SD_MMC.rename(pathFrom, pathTo);
    }

    bool rename(const String &pathFrom, const String &pathTo)
    {
        return rename(pathFrom.c_str(), pathTo.c_str());
    }

    bool remove(const char *path)
    {
        Guard g;
        if (!g.locked() || path == nullptr)
        {
            return false;
        }
        return SD_MMC.remove(path);
    }

    bool remove(const String &path)
    {
        return remove(path.c_str());
    }

    bool rmdir(const char *path)
    {
        Guard g;
        if (!g.locked() || path == nullptr)
        {
            return false;
        }
        return SD_MMC.rmdir(path);
    }

    bool rmdir(const String &path)
    {
        return rmdir(path.c_str());
    }

    SdFile open(const char *path, const char *mode)
    {
        Guard g;
        if (!g.locked() || path == nullptr)
        {
            return SdFile();
        }
        return SdFile(SD_MMC.open(path, mode));
    }

    SdFile open(const String &path, const char *mode)
    {
        return open(path.c_str(), mode);
    }

    uint8_t cardType()
    {
        Guard g;
        if (!g.locked())
        {
            return CARD_NONE;
        }
        return SD_MMC.cardType();
    }

    uint64_t totalBytes()
    {
        Guard g;
        if (!g.locked())
        {
            return 0;
        }
        return SD_MMC.totalBytes();
    }

    uint64_t usedBytes()
    {
        Guard g;
        if (!g.locked())
        {
            return 0;
        }
        return SD_MMC.usedBytes();
    }

    bool mount(const char *mountpoint, bool mode1bit, bool formatIfMountFailed, uint32_t frequency, uint8_t maxOpenFiles)
    {
        Guard g(SD_BUS_MOUNT_LOCK_TIMEOUT_MS);
        if (!g.locked())
        {
            return false;
        }
        return SD_MMC.begin(mountpoint, mode1bit, formatIfMountFailed, frequency, maxOpenFiles);
    }

    void unmount()
    {
        Guard g(SD_BUS_MOUNT_LOCK_TIMEOUT_MS);
        if (!g.locked())
        {
            return;
        }
        SD_MMC.end();
    }

}
