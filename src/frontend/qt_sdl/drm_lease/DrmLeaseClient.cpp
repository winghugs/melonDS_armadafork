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

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

#include "DrmLeaseClient.h"
#include "Platform.h"
#include "drm-lease-v1-client-protocol.h"

using namespace melonDS;
using Platform::Log;
using Platform::LogLevel;

struct DrmLeaseClientCallbacks
{
    static void registryGlobal(void* data, wl_registry* registry, u32 name, const char* interface, u32 version)
    {
        auto* client = static_cast<DrmLeaseClient*>(data);
        if (strcmp(interface, wp_drm_lease_device_v1_interface.name) == 0)
        {
            // One global per DRM node; only the first is used.
            if (client->device)
            {
                Log(LogLevel::Info, "drm-lease: ignoring additional lease device global\n");
                return;
            }
            client->device = static_cast<wp_drm_lease_device_v1*>(
                wl_registry_bind(registry, name, &wp_drm_lease_device_v1_interface, 1u));
        }
    }

    static void registryGlobalRemove(void* data, wl_registry* registry, u32 name) {}

    static void deviceDrmFd(void* data, wp_drm_lease_device_v1* device, s32 fd)
    {
        auto* client = static_cast<DrmLeaseClient*>(data);
        // Only needed for enumeration; the mode is read from the lease fd.
        client->enumFd = fd;
    }

    static void deviceConnector(void* data, wp_drm_lease_device_v1* device, wp_drm_lease_connector_v1* connector)
    {
        auto* client = static_cast<DrmLeaseClient*>(data);
        client->connectors.push_back({.handle = connector});
        wp_drm_lease_connector_v1_add_listener(connector, &connectorListener, data);
    }

    static void deviceDone(void* data, wp_drm_lease_device_v1* device)
    {
        static_cast<DrmLeaseClient*>(data)->deviceDone = true;
    }

    static void deviceReleased(void* data, wp_drm_lease_device_v1* device) {}

    static DrmLeaseClient::ConnectorOffer* findOffer(DrmLeaseClient* client, wp_drm_lease_connector_v1* connector)
    {
        for (auto& offer : client->connectors)
        {
            if (offer.handle == connector)
                return &offer;
        }
        return nullptr;
    }

    static void connectorName(void* data, wp_drm_lease_connector_v1* connector, const char* name)
    {
        auto* client = static_cast<DrmLeaseClient*>(data);
        if (auto* offer = findOffer(client, connector))
            offer->name = name;
    }

    static void connectorDescription(void* data, wp_drm_lease_connector_v1* connector, const char* description)
    {
        auto* client = static_cast<DrmLeaseClient*>(data);
        if (auto* offer = findOffer(client, connector))
            offer->description = description;
    }

    static void connectorConnectorId(void* data, wp_drm_lease_connector_v1* connector, u32 connectorId)
    {
        auto* client = static_cast<DrmLeaseClient*>(data);
        if (auto* offer = findOffer(client, connector))
            offer->connectorId = connectorId;
    }

    static void connectorDone(void* data, wp_drm_lease_connector_v1* connector)
    {
        auto* client = static_cast<DrmLeaseClient*>(data);
        if (auto* offer = findOffer(client, connector))
            offer->done = true;
    }

    static void connectorWithdrawn(void* data, wp_drm_lease_connector_v1* connector)
    {
        auto* client = static_cast<DrmLeaseClient*>(data);
        if (auto* offer = findOffer(client, connector))
        {
            offer->done = false;
            offer->connectorId = 0;
        }
    }

    static void leaseFd(void* data, wp_drm_lease_v1* lease, s32 leasedFd)
    {
        static_cast<DrmLeaseClient*>(data)->leaseFd = leasedFd;
    }

    static void leaseFinished(void* data, wp_drm_lease_v1* lease)
    {
        auto* client = static_cast<DrmLeaseClient*>(data);
        client->leaseFinished = true;
        Log(LogLevel::Warn, "drm-lease: lease finished by compositor\n");
    }

    static constexpr wl_registry_listener registryListener =
    {
        .global = registryGlobal,
        .global_remove = registryGlobalRemove,
    };

    static constexpr wp_drm_lease_device_v1_listener deviceListener =
    {
        .drm_fd = deviceDrmFd,
        .connector = deviceConnector,
        .done = deviceDone,
        .released = deviceReleased,
    };

    static constexpr wp_drm_lease_connector_v1_listener connectorListener =
    {
        .name = connectorName,
        .description = connectorDescription,
        .connector_id = connectorConnectorId,
        .done = connectorDone,
        .withdrawn = connectorWithdrawn,
    };

    static constexpr wp_drm_lease_v1_listener leaseListener =
    {
        .lease_fd = leaseFd,
        .finished = leaseFinished,
    };
};

DrmLeaseClient::DrmLeaseClient() = default;

DrmLeaseClient::~DrmLeaseClient()
{
    if (lease)
        wp_drm_lease_v1_destroy(lease);
    for (auto& offer : connectors)
        wp_drm_lease_connector_v1_destroy(offer.handle);
    if (device)
    {
        // No roundtrip: after a timeout the compositor may be stalled, and
        // disconnecting releases everything server-side anyway.
        wp_drm_lease_device_v1_release(device);
        wl_display_flush(display);
        wp_drm_lease_device_v1_destroy(device);
    }
    if (registry)
        wl_registry_destroy(registry);
    if (display)
        wl_display_disconnect(display);
    if (enumFd >= 0)
        close(enumFd);
    if (leaseFd >= 0)
        close(leaseFd);
}

bool DrmLeaseClient::connect()
{
    // Gamescope hides WAYLAND_DISPLAY from clients and exposes its socket as
    // GAMESCOPE_WAYLAND_DISPLAY; the default connection may reach an outer compositor.
    if (const char* gamescopeDisplay = getenv("GAMESCOPE_WAYLAND_DISPLAY"))
        display = wl_display_connect(gamescopeDisplay);
    if (!display)
        display = wl_display_connect(nullptr);
    if (!display)
        return false;

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &DrmLeaseClientCallbacks::registryListener, this);
    wl_display_roundtrip(display);
    return true;
}

bool DrmLeaseClient::compositorOffersLease()
{
    DrmLeaseClient probe;
    return probe.connect() && probe.device != nullptr;
}

bool DrmLeaseClient::dispatchUntil(const std::function<bool()>& condition, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition())
    {
        while (wl_display_prepare_read(display) != 0)
        {
            if (wl_display_dispatch_pending(display) < 0)
                return false;
        }
        // Draining the queue above may have satisfied the condition.
        if (condition())
        {
            wl_display_cancel_read(display);
            return true;
        }
        short pollEvents = POLLIN;
        if (wl_display_flush(display) < 0)
        {
            if (errno != EAGAIN)
            {
                wl_display_cancel_read(display);
                return false;
            }
            // Incomplete flush: the request the server must answer may still
            // be buffered, so wake on writability and retry.
            pollEvents |= POLLOUT;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            wl_display_cancel_read(display);
            return condition();
        }
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        pollfd pfd{.fd = wl_display_get_fd(display), .events = pollEvents, .revents = 0};
        const int ret = poll(&pfd, 1, std::max(remaining, 1));
        if (ret < 0 && errno != EINTR)
        {
            wl_display_cancel_read(display);
            return false;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            wl_display_cancel_read(display);
            return false;
        }
        if (ret > 0 && (pfd.revents & POLLIN))
        {
            if (wl_display_read_events(display) < 0)
                return false;
        }
        else
        {
            wl_display_cancel_read(display);
        }
        if (wl_display_dispatch_pending(display) < 0)
            return false;
    }
    return true;
}

void DrmLeaseClient::pumpEvents()
{
    if (!display || pumpDead)
        return;
    const auto fail = [this]
    {
        Log(LogLevel::Warn, "drm-lease: Wayland connection failed, event pump disabled\n");
        pumpDead = true;
    };
    while (wl_display_prepare_read(display) != 0)
    {
        if (wl_display_dispatch_pending(display) < 0)
            return fail();
    }
    if (wl_display_flush(display) < 0 && errno != EAGAIN)
    {
        wl_display_cancel_read(display);
        return fail();
    }
    pollfd pfd{.fd = wl_display_get_fd(display), .events = POLLIN, .revents = 0};
    const int ret = poll(&pfd, 1, 0);
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    {
        wl_display_cancel_read(display);
        return fail();
    }
    if (ret > 0 && (pfd.revents & POLLIN))
    {
        if (wl_display_read_events(display) < 0)
            return fail();
    }
    else
    {
        wl_display_cancel_read(display);
        if (ret < 0 && errno != EINTR)
            return fail();
    }
    if (wl_display_dispatch_pending(display) < 0)
        return fail();
}

static bool isInternalConnector(const std::string& name)
{
    for (const char* prefix : {"DSI-", "eDP-", "LVDS-", "DPI-"})
    {
        if (name.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}

bool DrmLeaseClient::acquire(const std::string& connectorName, bool internalOnly)
{
    if (!connect())
    {
        Log(LogLevel::Info, "drm-lease: no Wayland display, not using a leased output\n");
        return false;
    }

    if (!device)
    {
        Log(LogLevel::Info, "drm-lease: compositor does not offer wp_drm_lease_device_v1\n");
        return false;
    }

    wp_drm_lease_device_v1_add_listener(device, &DrmLeaseClientCallbacks::deviceListener, this);
    // Enumeration can lag binding by seconds while the compositor regains DRM master.
    if (!dispatchUntil([this] { return deviceDone; }, 5000))
    {
        Log(LogLevel::Info, "drm-lease: lease device never finished enumeration\n");
        return false;
    }

    const ConnectorOffer* chosen = nullptr;
    for (const auto& offer : connectors)
    {
        if (!offer.done)
            continue;
        if (!connectorName.empty())
        {
            if (offer.name == connectorName)
            {
                chosen = &offer;
                break;
            }
        }
        else if (!internalOnly || isInternalConnector(offer.name))
        {
            chosen = &offer;
            break;
        }
    }
    if (!chosen)
    {
        Log(LogLevel::Info, "drm-lease: no eligible connector offered (wanted '%s', %zu offered)\n",
            connectorName.empty() ? "any internal" : connectorName.c_str(), connectors.size());
        return false;
    }

    // Dispatching below may reallocate the offer vector or clear the offer via withdrawn.
    const std::string chosenName = chosen->name;
    const u32 chosenId = chosen->connectorId;
    wp_drm_lease_connector_v1* const chosenHandle = chosen->handle;
    chosen = nullptr;

    Log(LogLevel::Info, "drm-lease: requesting connector '%s' (id %u)\n", chosenName.c_str(), chosenId);

    wp_drm_lease_request_v1* request = wp_drm_lease_device_v1_create_lease_request(device);
    wp_drm_lease_request_v1_request_connector(request, chosenHandle);
    lease = wp_drm_lease_request_v1_submit(request);
    wp_drm_lease_v1_add_listener(lease, &DrmLeaseClientCallbacks::leaseListener, this);

    if (!dispatchUntil([this] { return leaseFd >= 0 || leaseFinished; }, 5000))
    {
        Log(LogLevel::Error, "drm-lease: no lease response from the compositor\n");
        return false;
    }

    if (leaseFinished || leaseFd < 0)
    {
        Log(LogLevel::Error, "drm-lease: compositor rejected the lease request\n");
        return false;
    }

    leasedConnectorId = chosenId;
    Log(LogLevel::Info, "drm-lease: acquired lease fd %d for connector %u\n", leaseFd, leasedConnectorId);
    return true;
}
