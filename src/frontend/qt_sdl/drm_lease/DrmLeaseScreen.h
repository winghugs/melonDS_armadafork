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

#ifndef DRMLEASESCREEN_H
#define DRMLEASESCREEN_H

#include <memory>
#include <string>
#include <xf86drmMode.h>

#include "glad/glad.h"
#include "glad/glad_egl.h"
#include "ScreenLayout.h"
#include "types.h"

class DrmLeaseClient;
class DrmLeaseTouch;
class EmuInstance;
struct gbm_device;
struct gbm_bo;

// Bottom screen on a DRM lease instead of a Qt window: GBM scanout buffers
// imported into the main GL context as EGL images, page flipped on the leased
// CRTC, touch read directly from evdev.
class DrmLeaseScreen
{
public:
    explicit DrmLeaseScreen(EmuInstance* inst);
    ~DrmLeaseScreen();

    DrmLeaseScreen(const DrmLeaseScreen&) = delete;
    DrmLeaseScreen& operator=(const DrmLeaseScreen&) = delete;

    // MELONDS_DRM_LEASE=0 disables the feature.
    static bool enabled();
    // enabled() and the compositor offers leases; cached.
    static bool available();

    // rotation is clockwise degrees (0/90/180/270), or -1 to read the panel
    // orientation; connectorName/internalOnly as in DrmLeaseClient::acquire.
    bool initialize(const std::string& connectorName, int rotation,
                    const std::string& touchDevice, bool internalOnly);

    // Emu thread, main GL context current.
    bool initOpenGL();
    bool isDead() const { return dead; }
    void deinitOpenGL();
    void drawScreen();

    // Touch thread; coordinates are normalized to the panel.
    bool touchPressed(float nx, float ny);
    void touchMoved(float nx, float ny);
    void touchReleased();

private:
    struct Buffer
    {
        gbm_bo* bo = nullptr;
        melonDS::u32 fbId = 0;
        EGLImage image = EGL_NO_IMAGE;
        GLuint texture = 0;
        GLuint fbo = 0;
    };

    struct PlaneProps
    {
        melonDS::u32 fbId, crtcId, srcX, srcY, srcW, srcH, crtcX, crtcY, crtcW, crtcH;
    };

    bool setupKMS();
    bool findProperty(melonDS::u32 objectId, melonDS::u32 objectType, const char* name, melonDS::u32& propId);
    bool commit(melonDS::u32 fbId, bool modeset);
    bool createBuffer(EGLDisplay display, Buffer& buf);
    void destroyBuffer(Buffer& buf);
    bool waitFlip(int timeoutMs);
    void drainEvents();
    bool touchToScreen(float nx, float ny, bool clamp, int& x, int& y);

    EmuInstance* emuInstance;
    std::unique_ptr<DrmLeaseClient> client;
    std::unique_ptr<DrmLeaseTouch> touch;

    int leaseFd = -1;
    melonDS::u32 connectorId = 0;
    melonDS::u32 crtcId = 0;
    melonDS::u32 planeId = 0;
    melonDS::u32 modeBlobId = 0;
    melonDS::u32 connectorCrtcIdProp = 0;
    melonDS::u32 crtcModeIdProp = 0, crtcActiveProp = 0;
    PlaneProps planeProps{};
    drmModeModeInfo mode{};
    int screenRotation = 0;
    bool filter = false;

    ScreenLayout layout;
    float screenMatrix[6]{};
    bool touching = false;

    gbm_device* gbm = nullptr;
    Buffer buffers[2];
    int nextBuffer = 0;
    bool modeSet = false;
    bool flipPending = false;
    uintptr_t flipCookie = 0;
    bool dead = false;
    bool glInited = false;
    bool hasModifierImport = false;

    GLuint shaderProgram = 0;
    GLint transformULoc = -1, screenSizeULoc = -1;
    GLuint vertexBuffer = 0, vertexArray = 0;
    GLuint screenTexture = 0;
};

#endif // DRMLEASESCREEN_H
