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

#ifndef DRMLEASECLIENT_H
#define DRMLEASECLIENT_H

#include <functional>
#include <string>
#include <vector>

#include "types.h"

struct wl_display;
struct wl_registry;
struct wp_drm_lease_device_v1;
struct wp_drm_lease_connector_v1;
struct wp_drm_lease_v1;

// Client for the drm-lease-v1 Wayland protocol. Connects to the compositor
// on a dedicated wl_display, leases a connector and hands out the lease fd.
class DrmLeaseClient
{
public:
    DrmLeaseClient();
    ~DrmLeaseClient();

    DrmLeaseClient(const DrmLeaseClient&) = delete;
    DrmLeaseClient& operator=(const DrmLeaseClient&) = delete;

    // One registry roundtrip; leases nothing.
    static bool compositorOffersLease();

    // Empty connectorName takes the first eligible offer; internalOnly limits
    // that to DSI/eDP/LVDS/DPI so auto-detection never grabs a headset. Blocks.
    bool acquire(const std::string& connectorName, bool internalOnly);

    // Non-blocking; call regularly so a compositor finished event is seen.
    void pumpEvents();

    int getLeaseFd() const { return leaseFd; }
    melonDS::u32 getConnectorId() const { return leasedConnectorId; }
    bool isFinished() const { return leaseFinished; }

    struct ConnectorOffer
    {
        wp_drm_lease_connector_v1* handle = nullptr;
        std::string name;
        std::string description;
        melonDS::u32 connectorId = 0;
        bool done = false;
    };

private:
    friend struct DrmLeaseClientCallbacks;

    bool connect();

    // Returns the final condition state; false also on connection failure.
    bool dispatchUntil(const std::function<bool()>& condition, int timeoutMs);

    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wp_drm_lease_device_v1* device = nullptr;
    wp_drm_lease_v1* lease = nullptr;

    std::vector<ConnectorOffer> connectors;
    int enumFd = -1;
    int leaseFd = -1;
    melonDS::u32 leasedConnectorId = 0;
    bool deviceDone = false;
    bool leaseFinished = false;
    bool pumpDead = false;
};

#endif // DRMLEASECLIENT_H
