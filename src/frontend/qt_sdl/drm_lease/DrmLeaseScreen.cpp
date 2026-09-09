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

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <random>
#include <drm_fourcc.h>
#include <gbm.h>
#include <poll.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "DrmLeaseClient.h"
#include "DrmLeaseScreen.h"
#include "DrmLeaseTouch.h"
#include "EmuInstance.h"
#include "OpenGLSupport.h"
#include "Platform.h"

using namespace melonDS;
using Platform::Log;
using Platform::LogLevel;

// EGL_EXT_image_dma_buf_import(_modifiers) and GL_OES_EGL_image; the glad
// EGL loader only carries core EGL.
#define EGL_LINUX_DMA_BUF_EXT              0x3270
#define EGL_LINUX_DRM_FOURCC_EXT           0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT          0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT      0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT       0x3274
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
typedef void (APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;

static const char* kLeaseScreenVS = R"(#version 140

uniform vec2 uScreenSize;
uniform mat2x3 uTransform;

in vec2 vPosition;
in vec2 vTexcoord;

smooth out vec2 fTexcoord;

void main()
{
    vec4 fpos;

    fpos.xy = vec3(vPosition, 1.0) * uTransform;

    // Rendering into a scanout buffer: row 0 is the top line, so no y flip.
    fpos.xy = ((fpos.xy * 2.0) / uScreenSize) - 1.0;
    fpos.z = 0.0;
    fpos.w = 1.0;

    gl_Position = fpos;
    fTexcoord = vTexcoord;
}
)";

static const char* kLeaseScreenFS = R"(#version 140

uniform sampler2DArray ScreenTex;

smooth in vec2 fTexcoord;

out vec4 oColor;

void main()
{
    vec4 pixel = texture(ScreenTex, vec3(fTexcoord, 1.0));

    oColor = vec4(pixel.rgb, 1.0);
}
)";

// Content rotation from the connector's "panel orientation" property, or -1
// when absent. Degrees are clockwise, matching melonDS screen rotation.
static int readPanelOrientation(int fd, u32 connectorId)
{
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, connectorId, DRM_MODE_OBJECT_CONNECTOR);
    if (!props)
        return -1;

    int rotation = -1;
    for (u32 i = 0; i < props->count_props && rotation < 0; i++)
    {
        drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop)
            continue;
        if (strcmp(prop->name, "panel orientation") == 0)
        {
            for (int j = 0; j < prop->count_enums; j++)
            {
                if (prop->enums[j].value != props->prop_values[i])
                    continue;
                const char* name = prop->enums[j].name;
                if (strcmp(name, "Normal") == 0)
                    rotation = 0;
                else if (strcmp(name, "Upside Down") == 0)
                    rotation = 180;
                else if (strcmp(name, "Left Side Up") == 0)
                    rotation = 270;
                else if (strcmp(name, "Right Side Up") == 0)
                    rotation = 90;
                break;
            }
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return rotation;
}

bool DrmLeaseScreen::enabled()
{
    const char* env = getenv("MELONDS_DRM_LEASE");
    return !(env && strcmp(env, "0") == 0);
}

bool DrmLeaseScreen::available()
{
    static const bool offered = enabled() && DrmLeaseClient::compositorOffersLease();
    return offered;
}

DrmLeaseScreen::DrmLeaseScreen(EmuInstance* inst) : emuInstance(inst)
{
}

DrmLeaseScreen::~DrmLeaseScreen()
{
    // The touch thread must stop before the layout it reads goes away.
    touch.reset();
    deinitOpenGL();
    client.reset();
}

bool DrmLeaseScreen::initialize(const std::string& connectorName, int rotation,
                                const std::string& touchDevice, bool internalOnly)
{
    client = std::make_unique<DrmLeaseClient>();
    if (!client->acquire(connectorName, internalOnly))
    {
        client.reset();
        return false;
    }
    leaseFd = client->getLeaseFd();
    connectorId = client->getConnectorId();

    if (!setupKMS())
    {
        client.reset();
        return false;
    }

    if (rotation < 0)
    {
        rotation = readPanelOrientation(leaseFd, connectorId);
        if (rotation < 0)
        {
            rotation = 0;
            Log(LogLevel::Info, "drm-lease: connector reports no panel orientation, assuming unrotated; "
                                "set MELONDS_DRM_LEASE_ROTATION if the output is sideways\n");
        }
        else
        {
            Log(LogLevel::Info, "drm-lease: panel orientation property gives rotation %d\n", rotation);
        }
    }
    screenRotation = ((rotation % 360) + 360) % 360 / 90;

    auto& cfg = emuInstance->getMainWindow()->getWindowConfig();
    filter = cfg.GetBool("ScreenFilter");
    bool screenSwap = cfg.GetBool("ScreenSwap");
    auto sizingMode = screenSwap ? screenSizing_BotOnly : screenSizing_TopOnly;

    layout.Setup(mode.hdisplay, mode.vdisplay,
                 screenLayout_Natural,
                 static_cast<ScreenRotation>(screenRotation),
                 sizingMode,
                 0,
                 cfg.GetBool("IntegerScaling"),
                 false,
                 1.0f, 1.0f);
    float matrices[kMaxScreenTransforms][6];
    int kinds[kMaxScreenTransforms];
    int numScreens = layout.GetScreenTransforms(matrices[0], kinds);
    for (int i = 0; i < numScreens; i++)
    {
        if (kinds[i] == 1)
            memcpy(screenMatrix, matrices[i], sizeof(screenMatrix));
    }

    if (!touchDevice.empty())
        touch = std::make_unique<DrmLeaseTouch>(*this, touchDevice);

    Log(LogLevel::Info, "drm-lease: secondary output ready, %ux%u@%u rotation %d\n",
        mode.hdisplay, mode.vdisplay, mode.vrefresh, screenRotation * 90);
    return true;
}

bool DrmLeaseScreen::findProperty(u32 objectId, u32 objectType, const char* name, u32& propId)
{
    drmModeObjectProperties* props = drmModeObjectGetProperties(leaseFd, objectId, objectType);
    if (!props)
        return false;
    propId = 0;
    for (u32 i = 0; i < props->count_props && !propId; i++)
    {
        drmModePropertyRes* prop = drmModeGetProperty(leaseFd, props->props[i]);
        if (!prop)
            continue;
        if (strcmp(prop->name, name) == 0)
            propId = prop->prop_id;
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    if (!propId)
        Log(LogLevel::Error, "drm-lease: object %u has no '%s' property\n", objectId, name);
    return propId != 0;
}

// Atomic only: legacy SETCRTC requires the CRTC's own primary plane to be
// leased, and the lease may carry a different plane for that CRTC.
bool DrmLeaseScreen::setupKMS()
{
    drmSetClientCap(leaseFd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (drmSetClientCap(leaseFd, DRM_CLIENT_CAP_ATOMIC, 1))
    {
        Log(LogLevel::Error, "drm-lease: atomic modesetting is not available on the lease fd\n");
        return false;
    }

    drmModeConnector* connector = drmModeGetConnector(leaseFd, connectorId);
    if (!connector || connector->count_modes < 1)
    {
        Log(LogLevel::Error, "drm-lease: failed to read a mode from the leased connector\n");
        if (connector)
            drmModeFreeConnector(connector);
        return false;
    }

    mode = connector->modes[0];
    for (int i = 0; i < connector->count_modes; i++)
    {
        if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED)
        {
            mode = connector->modes[i];
            break;
        }
    }

    drmModeRes* res = drmModeGetResources(leaseFd);
    if (!res)
    {
        Log(LogLevel::Error, "drm-lease: drmModeGetResources failed on the lease fd\n");
        drmModeFreeConnector(connector);
        return false;
    }

    // possible_crtcs is remapped by the kernel to the lessee's CRTC list.
    crtcId = 0;
    int crtcIndex = -1;
    for (int e = 0; e < connector->count_encoders && !crtcId; e++)
    {
        drmModeEncoder* encoder = drmModeGetEncoder(leaseFd, connector->encoders[e]);
        if (!encoder)
            continue;
        for (int c = 0; c < res->count_crtcs; c++)
        {
            if (encoder->possible_crtcs & (1u << c))
            {
                crtcId = res->crtcs[c];
                crtcIndex = c;
                break;
            }
        }
        drmModeFreeEncoder(encoder);
    }
    if (!crtcId && res->count_crtcs == 1)
    {
        crtcId = res->crtcs[0];
        crtcIndex = 0;
    }

    drmModeFreeResources(res);
    drmModeFreeConnector(connector);

    if (!crtcId)
    {
        Log(LogLevel::Error, "drm-lease: no leased CRTC can drive connector %u\n", connectorId);
        return false;
    }

    planeId = 0;
    drmModePlaneRes* planes = drmModeGetPlaneResources(leaseFd);
    if (!planes)
    {
        Log(LogLevel::Error, "drm-lease: drmModeGetPlaneResources failed on the lease fd\n");
        return false;
    }
    for (u32 i = 0; i < planes->count_planes && !planeId; i++)
    {
        drmModePlane* plane = drmModeGetPlane(leaseFd, planes->planes[i]);
        if (!plane)
            continue;
        if (plane->possible_crtcs & (1u << crtcIndex))
        {
            for (u32 f = 0; f < plane->count_formats; f++)
            {
                if (plane->formats[f] == DRM_FORMAT_XRGB8888)
                {
                    planeId = plane->plane_id;
                    break;
                }
            }
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(planes);
    if (!planeId)
    {
        Log(LogLevel::Error, "drm-lease: no leased XRGB8888 plane for CRTC %u\n", crtcId);
        return false;
    }

    if (!findProperty(connectorId, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", connectorCrtcIdProp) ||
        !findProperty(crtcId, DRM_MODE_OBJECT_CRTC, "MODE_ID", crtcModeIdProp) ||
        !findProperty(crtcId, DRM_MODE_OBJECT_CRTC, "ACTIVE", crtcActiveProp) ||
        !findProperty(planeId, DRM_MODE_OBJECT_PLANE, "FB_ID", planeProps.fbId) ||
        !findProperty(planeId, DRM_MODE_OBJECT_PLANE, "CRTC_ID", planeProps.crtcId) ||
        !findProperty(planeId, DRM_MODE_OBJECT_PLANE, "SRC_X", planeProps.srcX) ||
        !findProperty(planeId, DRM_MODE_OBJECT_PLANE, "SRC_Y", planeProps.srcY) ||
        !findProperty(planeId, DRM_MODE_OBJECT_PLANE, "SRC_W", planeProps.srcW) ||
        !findProperty(planeId, DRM_MODE_OBJECT_PLANE, "SRC_H", planeProps.srcH) ||
        !findProperty(planeId, DRM_MODE_OBJECT_PLANE, "CRTC_X", planeProps.crtcX) ||
        !findProperty(planeId, DRM_MODE_OBJECT_PLANE, "CRTC_Y", planeProps.crtcY) ||
        !findProperty(planeId, DRM_MODE_OBJECT_PLANE, "CRTC_W", planeProps.crtcW) ||
        !findProperty(planeId, DRM_MODE_OBJECT_PLANE, "CRTC_H", planeProps.crtcH))
        return false;

    if (drmModeCreatePropertyBlob(leaseFd, &mode, sizeof(mode), &modeBlobId))
    {
        Log(LogLevel::Error, "drm-lease: drmModeCreatePropertyBlob failed: %s\n", strerror(errno));
        return false;
    }

    Log(LogLevel::Info, "drm-lease: connector %u crtc %u plane %u\n", connectorId, crtcId, planeId);
    return true;
}

// fbId 0 turns the output off.
bool DrmLeaseScreen::commit(u32 fbId, bool modeset)
{
    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req)
        return false;

    const bool on = fbId != 0;
    if (modeset)
    {
        drmModeAtomicAddProperty(req, connectorId, connectorCrtcIdProp, on ? crtcId : 0);
        drmModeAtomicAddProperty(req, crtcId, crtcModeIdProp, on ? modeBlobId : 0);
        drmModeAtomicAddProperty(req, crtcId, crtcActiveProp, on ? 1 : 0);
        drmModeAtomicAddProperty(req, planeId, planeProps.crtcId, on ? crtcId : 0);
        drmModeAtomicAddProperty(req, planeId, planeProps.srcX, 0);
        drmModeAtomicAddProperty(req, planeId, planeProps.srcY, 0);
        drmModeAtomicAddProperty(req, planeId, planeProps.srcW, on ? (u64)mode.hdisplay << 16 : 0);
        drmModeAtomicAddProperty(req, planeId, planeProps.srcH, on ? (u64)mode.vdisplay << 16 : 0);
        drmModeAtomicAddProperty(req, planeId, planeProps.crtcX, 0);
        drmModeAtomicAddProperty(req, planeId, planeProps.crtcY, 0);
        drmModeAtomicAddProperty(req, planeId, planeProps.crtcW, on ? mode.hdisplay : 0);
        drmModeAtomicAddProperty(req, planeId, planeProps.crtcH, on ? mode.vdisplay : 0);
    }
    drmModeAtomicAddProperty(req, planeId, planeProps.fbId, fbId);

    // The lease fd is shared with the compositor, so its event queue can hold
    // flips from an earlier holder; tag flips with a process-unique sequence, not a pointer.
    static std::atomic<uintptr_t> nextCookie{static_cast<uintptr_t>(std::random_device{}())};
    flipCookie = ++nextCookie;
    const u32 flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET
                              : (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK);
    const int ret = drmModeAtomicCommit(leaseFd, req, flags, reinterpret_cast<void*>(flipCookie));
    drmModeAtomicFree(req);
    if (ret)
    {
        Log(LogLevel::Error, "drm-lease: atomic %s failed: %s\n", modeset ? "modeset" : "flip", strerror(-ret));
        return false;
    }
    return true;
}

bool DrmLeaseScreen::createBuffer(EGLDisplay display, Buffer& buf)
{
    buf.bo = gbm_bo_create(gbm, mode.hdisplay, mode.vdisplay, GBM_FORMAT_XRGB8888,
                           GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!buf.bo)
    {
        Log(LogLevel::Error, "drm-lease: gbm_bo_create failed: %s\n", strerror(errno));
        return false;
    }

    const u32 handles[4] = {gbm_bo_get_handle(buf.bo).u32};
    const u32 strides[4] = {gbm_bo_get_stride(buf.bo)};
    const u32 offsets[4] = {0};
    const u64 modifier = gbm_bo_get_modifier(buf.bo);
    int ret;
    if (modifier != DRM_FORMAT_MOD_INVALID)
    {
        const u64 modifiers[4] = {modifier};
        ret = drmModeAddFB2WithModifiers(leaseFd, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888,
                                         handles, strides, offsets, modifiers, &buf.fbId, DRM_MODE_FB_MODIFIERS);
    }
    else
    {
        ret = drmModeAddFB2(leaseFd, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888,
                            handles, strides, offsets, &buf.fbId, 0);
    }
    if (ret)
    {
        Log(LogLevel::Error, "drm-lease: drmModeAddFB2 failed: %s\n", strerror(-ret));
        return false;
    }

    const int dmabuf = gbm_bo_get_fd(buf.bo);
    if (dmabuf < 0)
    {
        Log(LogLevel::Error, "drm-lease: gbm_bo_get_fd failed\n");
        return false;
    }
    EGLAttrib attribs[32];
    int n = 0;
    attribs[n++] = EGL_WIDTH;                     attribs[n++] = mode.hdisplay;
    attribs[n++] = EGL_HEIGHT;                    attribs[n++] = mode.vdisplay;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;      attribs[n++] = DRM_FORMAT_XRGB8888;
    attribs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;     attribs[n++] = dmabuf;
    attribs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attribs[n++] = 0;
    attribs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attribs[n++] = strides[0];
    if (modifier != DRM_FORMAT_MOD_INVALID && !hasModifierImport)
    {
        Log(LogLevel::Error, "drm-lease: buffer has an explicit modifier but EGL cannot import modifiers\n");
        close(dmabuf);
        return false;
    }
    if (modifier != DRM_FORMAT_MOD_INVALID)
    {
        attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attribs[n++] = static_cast<EGLAttrib>(modifier & 0xFFFFFFFFu);
        attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attribs[n++] = static_cast<EGLAttrib>(modifier >> 32);
    }
    attribs[n++] = EGL_NONE;
    buf.image = eglCreateImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    close(dmabuf);
    if (buf.image == EGL_NO_IMAGE)
    {
        Log(LogLevel::Error, "drm-lease: eglCreateImage failed: 0x%x\n", eglGetError());
        return false;
    }

    glGenTextures(1, &buf.texture);
    glBindTexture(GL_TEXTURE_2D, buf.texture);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, buf.image);
    glGenFramebuffers(1, &buf.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, buf.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buf.texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        Log(LogLevel::Error, "drm-lease: scanout framebuffer incomplete: 0x%x\n", status);
        return false;
    }
    return true;
}

void DrmLeaseScreen::destroyBuffer(Buffer& buf)
{
    if (buf.fbo)
        glDeleteFramebuffers(1, &buf.fbo);
    if (buf.texture)
        glDeleteTextures(1, &buf.texture);
    if (buf.image != EGL_NO_IMAGE)
        eglDestroyImage(eglGetCurrentDisplay(), buf.image);
    if (buf.fbId)
        drmModeRmFB(leaseFd, buf.fbId);
    if (buf.bo)
        gbm_bo_destroy(buf.bo);
    buf = {};
}

bool DrmLeaseScreen::initOpenGL()
{
    if (glInited)
        return true;

    // The scanout buffers are imported as dma-bufs, which needs the main
    // context to be EGL rather than GLX.
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT)
    {
        Log(LogLevel::Error, "drm-lease: the OpenGL context is not EGL, cannot drive the leased output\n");
        return false;
    }
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_EXT_image_dma_buf_import"))
    {
        Log(LogLevel::Error, "drm-lease: EGL_EXT_image_dma_buf_import is not supported\n");
        return false;
    }
    hasModifierImport = strstr(extensions, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;
    glEGLImageTargetTexture2DOES_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!glEGLImageTargetTexture2DOES_)
    {
        Log(LogLevel::Error, "drm-lease: GL_OES_EGL_image is not supported\n");
        return false;
    }

    drainEvents();

    gbm = gbm_create_device(leaseFd);
    if (!gbm)
    {
        Log(LogLevel::Error, "drm-lease: gbm_create_device failed on the lease fd\n");
        return false;
    }

    for (auto& buf : buffers)
    {
        if (!createBuffer(display, buf))
        {
            deinitOpenGL();
            return false;
        }
    }

    if (!OpenGL::CompileVertexFragmentProgram(shaderProgram,
                                              kLeaseScreenVS, kLeaseScreenFS,
                                              "DrmLeaseScreenShader",
                                              {{"vPosition", 0}, {"vTexcoord", 1}},
                                              {{"oColor", 0}}))
    {
        deinitOpenGL();
        return false;
    }
    glUseProgram(shaderProgram);
    glUniform1i(glGetUniformLocation(shaderProgram, "ScreenTex"), 0);
    screenSizeULoc = glGetUniformLocation(shaderProgram, "uScreenSize");
    transformULoc = glGetUniformLocation(shaderProgram, "uTransform");

    const float vertices[] =
    {
        0.f,   0.f,    0.f, 0.f,
        0.f,   192.f,  0.f, 1.f,
        256.f, 192.f,  1.f, 1.f,
        0.f,   0.f,    0.f, 0.f,
        256.f, 192.f,  1.f, 1.f,
        256.f, 0.f,    1.f, 0.f,
    };

    glGenBuffers(1, &vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*4, (void*)(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*4, (void*)(2*4));

    // Upload target for the software renderer; layer 1 is the bottom screen.
    glGenTextures(1, &screenTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, screenTexture);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, 256, 192, 2, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

    glInited = true;
    return true;
}

void DrmLeaseScreen::deinitOpenGL()
{
    if (glInited)
    {
        waitFlip(100);
        // Blank the panel rather than leave the last frame on it.
        if (modeSet)
            commit(0, true);
        modeSet = false;
        glDeleteTextures(1, &screenTexture);
        glDeleteVertexArrays(1, &vertexArray);
        glDeleteBuffers(1, &vertexBuffer);
        glDeleteProgram(shaderProgram);
    }
    for (auto& buf : buffers)
        destroyBuffer(buf);
    if (gbm)
    {
        gbm_device_destroy(gbm);
        gbm = nullptr;
    }
    if (modeBlobId)
    {
        drmModeDestroyPropertyBlob(leaseFd, modeBlobId);
        modeBlobId = 0;
    }
    glInited = false;
}

static thread_local uintptr_t completedCookie = 0;

static void pageFlipHandler(int fd, unsigned int sequence, unsigned int sec, unsigned int usec, void* data)
{
    completedCookie = reinterpret_cast<uintptr_t>(data);
}

// False if a flip is still pending after timeoutMs.
bool DrmLeaseScreen::waitFlip(int timeoutMs)
{
    while (flipPending)
    {
        pollfd pfd{.fd = leaseFd, .events = POLLIN, .revents = 0};
        const int ret = poll(&pfd, 1, timeoutMs);
        if (ret < 0 && errno == EINTR)
            continue;
        if (ret <= 0)
            return false;
        drmEventContext ctx{};
        ctx.version = 2;
        ctx.page_flip_handler = pageFlipHandler;
        completedCookie = 0;
        if (drmHandleEvent(leaseFd, &ctx) < 0)
        {
            dead = true;
            return false;
        }
        if (completedCookie == flipCookie)
            flipPending = false;
    }
    return true;
}

void DrmLeaseScreen::drainEvents()
{
    drmEventContext ctx{};
    ctx.version = 2;
    ctx.page_flip_handler = pageFlipHandler;
    for (;;)
    {
        pollfd pfd{.fd = leaseFd, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
            return;
        if (drmHandleEvent(leaseFd, &ctx) < 0)
            return;
    }
}

void DrmLeaseScreen::drawScreen()
{
    if (!glInited || dead)
        return;

    client->pumpEvents();
    if (client->isFinished())
    {
        dead = true;
        return;
    }

    // Scanout objects live in the main window's context.
    emuInstance->makeCurrentGL();

    // A flip still pending after a short wait means the panel is slower than
    // the main window; drop the frame rather than stall emulation.
    if (!waitFlip(2))
        return;

    Buffer& buf = buffers[nextBuffer];
    const int w = mode.hdisplay;
    const int h = mode.vdisplay;

    glBindFramebuffer(GL_FRAMEBUFFER, buf.fbo);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, w, h);

    if (emuInstance->emuIsActive())
    {
        auto nds = emuInstance->getNDS();

        glUseProgram(shaderProgram);
        glUniform2f(screenSizeULoc, w, h);

        void* topbuf; void* bottombuf;
        if (nds->GPU.GetFramebuffers(&topbuf, &bottombuf))
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, screenTexture);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 256, 192, 1, GL_BGRA,
                            GL_UNSIGNED_BYTE, bottombuf);
        }
        else
        {
            GLuint texid = *(GLuint*)topbuf;

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, texid);
        }

        GLint filt = filter ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, filt);

        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        glBindVertexArray(vertexArray);
        glUniformMatrix2x3fv(transformULoc, 1, GL_TRUE, screenMatrix);
        glDrawArrays(GL_TRIANGLES, 0, 2 * 3);
        glUseProgram(0);
    }

    // The display engine reads the buffer directly, so the render must be
    // complete before it is flipped in.
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!commit(buf.fbId, !modeSet))
    {
        dead = true;
        return;
    }
    if (modeSet)
        flipPending = true;
    modeSet = true;
    nextBuffer ^= 1;
}

bool DrmLeaseScreen::touchToScreen(float nx, float ny, bool clamp, int& x, int& y)
{
    x = static_cast<int>(nx * mode.hdisplay);
    y = static_cast<int>(ny * mode.vdisplay);
    return layout.GetTouchCoords(x, y, clamp);
}

bool DrmLeaseScreen::touchPressed(float nx, float ny)
{
    if (!emuInstance->emuIsActive())
        return false;
    int x, y;
    if (!touchToScreen(nx, ny, false, x, y))
        return false;
    touching = true;
    emuInstance->touchScreen(x, y);
    return true;
}

void DrmLeaseScreen::touchMoved(float nx, float ny)
{
    if (!touching || !emuInstance->emuIsActive())
        return;
    int x, y;
    if (touchToScreen(nx, ny, true, x, y))
        emuInstance->touchScreen(x, y);
}

void DrmLeaseScreen::touchReleased()
{
    if (!touching)
        return;
    touching = false;
    emuInstance->releaseScreen();
}
