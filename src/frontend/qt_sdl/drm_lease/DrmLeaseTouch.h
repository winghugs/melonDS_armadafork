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

#ifndef DRMLEASETOUCH_H
#define DRMLEASETOUCH_H

#include <atomic>
#include <string>
#include <thread>

class DrmLeaseScreen;

// Feeds a touchscreen evdev device to a DrmLeaseScreen as normalized panel
// coordinates; no windowing system delivers input for a leased panel. The
// device is grabbed so the compositor stops seeing it.
class DrmLeaseTouch
{
public:
    // devicePath: an evdev node path, an evdev device name, or "auto".
    DrmLeaseTouch(DrmLeaseScreen& screen, std::string devicePath);
    ~DrmLeaseTouch();

    DrmLeaseTouch(const DrmLeaseTouch&) = delete;
    DrmLeaseTouch& operator=(const DrmLeaseTouch&) = delete;

private:
    void run();
    int openDevice();
    void readLoop(int fd);

    DrmLeaseScreen& screen;
    std::string devicePath;
    bool swapXY = false;
    bool invertX = false;
    bool invertY = false;
    std::atomic<bool> stopRequested{false};
    std::thread thread;
};

#endif // DRMLEASETOUCH_H
