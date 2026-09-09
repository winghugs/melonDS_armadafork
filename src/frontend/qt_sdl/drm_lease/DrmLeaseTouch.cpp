/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "DrmLeaseScreen.h"
#include "DrmLeaseTouch.h"
#include "Platform.h"

using namespace melonDS;
using Platform::Log;
using Platform::LogLevel;

static bool hasBit(const u8* bits, unsigned bit)
{
    return bits[bit / 8] & (1 << (bit % 8));
}

static bool isDirectTouchDevice(int fd)
{
    u8 props[INPUT_PROP_MAX / 8 + 1]{};
    u8 absBits[ABS_MAX / 8 + 1]{};
    if (ioctl(fd, EVIOCGPROP(sizeof(props)), props) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) < 0)
        return false;
    return hasBit(props, INPUT_PROP_DIRECT) && hasBit(absBits, ABS_MT_POSITION_X);
}

// Empty name: the only direct-touch device, or the unique one named like the
// bottom screen; several ambiguous devices are refused rather than guessed.
static std::optional<std::string> findDirectTouchDevice(const std::string& name)
{
    std::vector<std::pair<std::string, std::string>> candidates; // path, evdev name
    DIR* dir = opendir("/dev/input");
    if (!dir)
        return std::nullopt;
    while (dirent* entry = readdir(dir))
    {
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;
        const std::string path = std::string("/dev/input/") + entry->d_name;
        const int fd = openat(AT_FDCWD, path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        char deviceName[80]{};
        if (isDirectTouchDevice(fd))
        {
            ioctl(fd, EVIOCGNAME(sizeof(deviceName) - 1), deviceName);
            candidates.emplace_back(path, deviceName);
        }
        close(fd);
    }
    closedir(dir);

    if (!name.empty())
    {
        for (const auto& [path, devName] : candidates)
        {
            if (devName == name)
                return path;
        }
        return std::nullopt;
    }
    if (candidates.size() == 1)
        return candidates.front().first;
    std::optional<std::string> bottom;
    for (const auto& [path, devName] : candidates)
    {
        if (devName.find("bottom") != std::string::npos)
        {
            if (bottom)
            {
                bottom.reset();
                break;
            }
            bottom = path;
        }
    }
    if (bottom)
        return bottom;
    Log(LogLevel::Error,
        "drm-lease touch: %zu touch devices found and none is unambiguous, set MELONDS_DRM_LEASE_TOUCH explicitly\n",
        candidates.size());
    return std::nullopt;
}

static bool envFlag(const char* name)
{
    const char* value = getenv(name);
    return value && value[0] != '\0' && value[0] != '0';
}

DrmLeaseTouch::DrmLeaseTouch(DrmLeaseScreen& screen, std::string devicePath)
    : screen(screen), devicePath(std::move(devicePath))
{
    // Digitizer orientation quirks: swap is applied before inversion.
    swapXY = envFlag("MELONDS_DRM_LEASE_TOUCH_SWAP_XY");
    invertX = envFlag("MELONDS_DRM_LEASE_TOUCH_INVERT_X");
    invertY = envFlag("MELONDS_DRM_LEASE_TOUCH_INVERT_Y");
    thread = std::thread([this] { run(); });
}

DrmLeaseTouch::~DrmLeaseTouch()
{
    stopRequested = true;
    thread.join();
}

int DrmLeaseTouch::openDevice()
{
    std::string path = devicePath;
    if (path.empty() || path[0] != '/')
    {
        const auto found = findDirectTouchDevice(path == "auto" ? "" : path);
        if (!found)
            return -1;
        path = *found;
    }

    // Steam's overlay interposes open() to hide input devices from games, but
    // not openat(); the leased panel's digitizer has no other route into the emulator.
    const int fd = openat(AT_FDCWD, path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    if (ioctl(fd, EVIOCGRAB, 1) < 0)
    {
        Log(LogLevel::Error, "drm-lease touch: EVIOCGRAB on %s failed: %s\n", path.c_str(), strerror(errno));
        close(fd);
        return -1;
    }

    char name[64]{};
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    Log(LogLevel::Info, "drm-lease touch: grabbed %s ('%s')\n", path.c_str(), name);
    return fd;
}

void DrmLeaseTouch::run()
{
    while (!stopRequested)
    {
        const int fd = openDevice();
        if (fd >= 0)
        {
            readLoop(fd);
            ioctl(fd, EVIOCGRAB, 0);
            close(fd);
            screen.touchReleased();
        }
        // Device missing, unusable or not yet powered (e.g. across suspend); retry.
        for (int i = 0; i < 20 && !stopRequested; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void DrmLeaseTouch::readLoop(int fd)
{
    input_absinfo absX{};
    input_absinfo absY{};
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absX) < 0 ||
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &absY) < 0)
    {
        Log(LogLevel::Error, "drm-lease touch: failed to read axis ranges\n");
        return;
    }
    const float rangeX = static_cast<float>(absX.maximum - absX.minimum);
    const float rangeY = static_cast<float>(absY.maximum - absY.minimum);
    if (rangeX <= 0.0f || rangeY <= 0.0f)
    {
        Log(LogLevel::Error, "drm-lease touch: degenerate axis ranges\n");
        return;
    }

    // Multitouch protocol B, slot 0 only: the DS touchscreen is single-touch.
    int slot = 0;
    int rawX = absX.value;
    int rawY = absY.value;
    int trackingId = -1;
    bool touching = false;

    while (!stopRequested)
    {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        const int ret = poll(&pfd, 1, 500);
        if (ret < 0 && errno != EINTR)
            return;
        if (ret <= 0 || !(pfd.revents & POLLIN))
        {
            if (pfd.revents & (POLLERR | POLLHUP))
                return;
            continue;
        }

        input_event events[64];
        const ssize_t bytes = read(fd, events, sizeof(events));
        if (bytes < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            Log(LogLevel::Warn, "drm-lease touch: read failed: %s\n", strerror(errno));
            return;
        }

        const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
        for (size_t i = 0; i < count; i++)
        {
            const input_event& ev = events[i];
            switch (ev.type)
            {
            case EV_ABS:
                switch (ev.code)
                {
                case ABS_MT_SLOT:
                    slot = ev.value;
                    break;
                case ABS_MT_TRACKING_ID:
                    if (slot == 0) trackingId = ev.value;
                    break;
                case ABS_MT_POSITION_X:
                    if (slot == 0) rawX = ev.value;
                    break;
                case ABS_MT_POSITION_Y:
                    if (slot == 0) rawY = ev.value;
                    break;
                }
                break;
            case EV_SYN:
                {
                    if (ev.code != SYN_REPORT)
                        break;
                    const bool nowTouching = trackingId >= 0;
                    float nx = static_cast<float>(rawX - absX.minimum) / rangeX;
                    float ny = static_cast<float>(rawY - absY.minimum) / rangeY;
                    if (swapXY)
                        std::swap(nx, ny);
                    if (invertX)
                        nx = 1.0f - nx;
                    if (invertY)
                        ny = 1.0f - ny;

                    if (nowTouching && !touching)
                        touching = screen.touchPressed(nx, ny);
                    else if (nowTouching && touching)
                        screen.touchMoved(nx, ny);
                    else if (!nowTouching && touching)
                    {
                        screen.touchReleased();
                        touching = false;
                    }
                }
                break;
            }
        }
    }
}
