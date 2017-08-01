/*
 * Copyright (C) 2015, 2016 Igalia S.L.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "nested-compositor.h"

#define WL_HIDE_DEPRECATED 1

#include "display.h"
#include "nc-renderer-host.h"
#include "nc-view-display.h"
#include "ivi-application-client-protocol.h"
#include "wayland-drm-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <unistd.h>
#include <unordered_map>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-server-core.h>
#include "wayland-drm-server-protocol.h"
#include <wayland-server-protocol.h>
#include <wpe/view-backend.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <cstring>
#include <fcntl.h>

namespace NC {
namespace Wayland {

class Popup;
class Buffer;

class ViewBackend: public NC::ViewDisplay::Client {
public:
    class Surface: public NC::ViewDisplay::Surface {
    public:
        Surface(struct wpe_view_backend* backend, ::Wayland::Display& display,
                struct wl_resource* resource, NC::ViewDisplay& view,
                NC::ViewDisplay::Surface::Type type);

        virtual ~Surface();

        struct ResizingData {
            struct wpe_view_backend* backend;
            uint32_t width;
            uint32_t height;
        };

        struct wl_surface* surface() { return m_surface; }

    protected:
        virtual void onSurfaceAttach(NC::ViewDisplay::Buffer* buffer) override;
        virtual void onSurfaceCommit(const NC::ViewDisplay::Surface::CommitState&) override;

    private:
        ::Wayland::Display& m_display;

        struct wl_surface* m_surface {nullptr};
        struct xdg_surface* m_xdgSurface {nullptr};
        struct wl_shell_surface* m_shellSurface {nullptr};
        struct ivi_surface* m_iviSurface {nullptr};
        struct wl_callback* m_callback {nullptr};

        struct wpe_view_backend* m_backend;

        ResizingData m_resizingData { nullptr, 0, 0 };
    };

    ViewBackend(struct wpe_view_backend*);
    virtual ~ViewBackend();

    virtual NC::ViewDisplay::Surface* createSurface(struct wl_resource*, NC::ViewDisplay&, NC::ViewDisplay::Surface::Type) override;

    void initialize();

    struct wpe_view_backend* backend() { return m_backend; }
    ::Wayland::Display& display() { return m_display; }

    Buffer* allocBuffer(uint32_t format, uint32_t width, uint32_t height);
    Popup* allocPopup(struct wpe_popup*, int32_t x, int32_t y);

    Surface* mainSurface()
    {
        return dynamic_cast<Surface*>(m_viewDisplay.mainSurface());
    }

private:
    ::Wayland::Display& m_display;
    struct wpe_view_backend* m_backend;

    struct wl_cursor_theme* m_cursorTheme {nullptr};


    struct {
        struct wl_global* drm;
    } m_server;

    NC::ViewDisplay m_viewDisplay;

    static const struct wl_drm_interface g_drmImplementation;
    static const struct wl_drm_listener g_drmListener;
    static const struct wl_buffer_interface g_drmBufferImplementation;
    static const struct wl_buffer_listener g_drmBufferListener;

    static void bindDrm(struct wl_client*, void*, uint32_t, uint32_t);

    static void destroyBuffer(struct wl_resource*);
    static void destroyDrm(struct wl_resource*);
};

class Popup {
public:
    Popup(ViewBackend&, struct wpe_popup*, int32_t x, int32_t y);

    ~Popup();

    static const struct wpe_popup_interface interface;

private:
    void attachBuffer(Buffer*);

    struct wpe_popup* m_popup;
    ViewBackend& m_parent;

    struct wl_surface* m_surface {nullptr};
    struct xdg_popup* m_xdgPopup {nullptr};
    struct wl_shell_surface* m_shellSurface {nullptr};
    struct wl_callback* m_frameCallback {nullptr};

    static const struct wl_callback_listener s_frameCallbackListener;
    static const struct wl_shell_surface_listener s_shellSurfaceListener;
    static const struct xdg_popup_listener s_xdgPopupListener;
};

class Buffer {
public:
    Buffer(ViewBackend& backend, int fd, void* data, size_t size, uint32_t format, uint32_t width, uint32_t height, uint32_t stride, struct wl_buffer* buffer)
        : m_backend(backend)
        , m_buffer(buffer)
        , m_fd(fd)
        , m_data(data)
        , m_size(size)
        , m_format(format)
        , m_width(width)
        , m_height(height)
        , m_stride(stride)
    { }

    ~Buffer()
    {
        wl_buffer_destroy(m_buffer);
        munmap(m_data, m_size);
        close(m_fd);
    }

    struct wl_buffer* buffer() const { return m_buffer; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }

    static const struct wpe_buffer_interface interface;

private:
    ViewBackend& m_backend;
    struct wl_buffer* m_buffer;
    int m_fd;
    void* m_data;
    size_t m_size;
    uint32_t m_format;
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_stride;
};

static const struct xdg_surface_listener g_xdgSurfaceListener = {
    // configure
    [](void* data, struct xdg_surface* surface, int32_t width, int32_t height, struct wl_array*, uint32_t serial)
    {
        if( width != 0 || height != 0 ) {
            auto* resizeData = static_cast<ViewBackend::Surface::ResizingData*>(data);
            wpe_view_backend_dispatch_set_size(resizeData->backend, std::max(0, width), std::max(0, height));
            resizeData->width = width;
            resizeData->height = height;
        }
        xdg_surface_ack_configure(surface, serial);
    },
    // delete
    [](void*, struct xdg_surface*) { },
};

static const struct wl_shell_surface_listener g_shellSurfaceListener = {
    // ping
    [](void* data, struct wl_shell_surface* surface, uint32_t serial)
    {
        wl_shell_surface_pong(surface, serial);
    },
    // configure
    [](void* data, struct wl_shell_surface* surface, uint32_t, int32_t width, int32_t height)
    {
        if( width != 0 || height != 0 ) {
            auto* resizeData = static_cast<ViewBackend::Surface::ResizingData*>(data);
            wpe_view_backend_dispatch_set_size(resizeData->backend, std::max(0, width), std::max(0, height));
            resizeData->width = width;
            resizeData->height = height;
        }
    },
    // popup_done
    [](void* data, struct wl_shell_surface* surface) { },
};

static const struct ivi_surface_listener g_iviSurfaceListener = {
    // configure
    [](void* data, struct ivi_surface*, int32_t width, int32_t height)
    {
        auto* resizeData = static_cast<ViewBackend::Surface::ResizingData*>(data);
        wpe_view_backend_dispatch_set_size(resizeData->backend, std::max(0, width), std::max(0, height));
        resizeData->width = width;
        resizeData->height = height;
    },
};

const struct wl_buffer_interface ViewBackend::g_drmBufferImplementation = {
    // destroy
    [](struct wl_client* client, struct wl_resource* resource)
    {
        auto* buffer = static_cast<struct wl_buffer*>(wl_resource_get_user_data(resource));
        wl_buffer_destroy(buffer);
        wl_resource_set_user_data(resource, nullptr);
    },
};

const struct wl_buffer_listener ViewBackend::g_drmBufferListener = {
    // release
    [](void* data, struct wl_buffer* buffer)
    {
        auto* resource = static_cast<struct wl_resource*>(data);
        wl_buffer_send_release(resource);
    },
};

void ViewBackend::destroyBuffer(struct wl_resource* resource)
{
    auto* buffer = static_cast<struct wl_buffer*>(wl_resource_get_user_data(resource));
    if (buffer)
        wl_buffer_destroy(buffer);
}

struct drm {
    struct wl_drm* drm;
    struct wl_display* display;
};

const struct wl_drm_interface ViewBackend::g_drmImplementation = {
    // authenticate
    [](struct wl_client* client, struct wl_resource* resource, uint32_t id)
    {
        auto* drm = static_cast<struct drm*>(wl_resource_get_user_data(resource));
        wl_drm_authenticate(drm->drm, id);
        wl_display_roundtrip(drm->display);
    },
    // create_buffer
	[](struct wl_client *client, struct wl_resource *resource, uint32_t id, uint32_t name,
            int32_t width, int32_t height, uint32_t stride, uint32_t format)
    {
        auto* drm = static_cast<struct drm*>(wl_resource_get_user_data(resource));

        struct wl_resource* buffer_resource = wl_resource_create(client, &wl_buffer_interface,
                1, id);

        if (!buffer_resource) {
            wl_resource_post_no_memory(resource);
            return;
        }

        struct wl_buffer* buffer = wl_drm_create_buffer(drm->drm, name, width,
                height, stride, format);

        wl_resource_set_implementation(buffer_resource, &g_drmBufferImplementation, buffer,
                destroyBuffer);

        wl_buffer_add_listener(buffer, &g_drmBufferListener, buffer_resource);
    },
    // create_planar_buffer
    [](struct wl_client *client, struct wl_resource *resource, uint32_t id, uint32_t name,
            int32_t width, int32_t height, uint32_t format, int32_t offset0, int32_t stride0,
            int32_t offset1, int32_t stride1, int32_t offset2, int32_t stride2)
    {
        auto* drm = static_cast<struct drm*>(wl_resource_get_user_data(resource));

        struct wl_resource* buffer_resource = wl_resource_create(client, &wl_buffer_interface,
                1, id);

        if (!buffer_resource) {
            wl_resource_post_no_memory(resource);
            return;
        }

        struct wl_buffer* buffer = wl_drm_create_planar_buffer(drm->drm, name, width, height, format,
                offset0, stride0, offset1, stride1, offset2, stride2);

        wl_resource_set_implementation(buffer_resource, &g_drmBufferImplementation, buffer,
                destroyBuffer);

        wl_buffer_add_listener(buffer, &g_drmBufferListener, buffer_resource);
    },
	// create_prime_buffer
    [](struct wl_client *client, struct wl_resource *resource, uint32_t id, int32_t name,
            int32_t width, int32_t height, uint32_t format, int32_t offset0, int32_t stride0,
            int32_t offset1, int32_t stride1, int32_t offset2, int32_t stride2)
    {
        auto* drm = static_cast<struct drm*>(wl_resource_get_user_data(resource));

        struct wl_resource* buffer_resource = wl_resource_create(client, &wl_buffer_interface,
                1, id);

        if (!buffer_resource) {
            wl_resource_post_no_memory(resource);
            return;
        }

        struct wl_buffer* buffer = wl_drm_create_prime_buffer(drm->drm, name, width, height, format,
                offset0, stride0, offset1, stride1, offset2, stride2);

        wl_resource_set_implementation(buffer_resource, &g_drmBufferImplementation, buffer,
                destroyBuffer);

        wl_buffer_add_listener(buffer, &g_drmBufferListener, buffer_resource);
    },
};

void ViewBackend::destroyDrm(struct wl_resource* resource)
{
    auto* drm = static_cast<struct drm*>(wl_resource_get_user_data(resource));
    wl_drm_destroy(drm->drm);
    delete drm;
}

void ViewBackend::bindDrm(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto& backend = *static_cast<ViewBackend*>(data);
    struct wl_resource* resource = wl_resource_create(client, &wl_drm_interface,
        version, id);

    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_drm* drm = static_cast<struct wl_drm*>(wl_registry_bind(backend.m_display.registry(),
                backend.m_display.interfaces().drm_name, &wl_drm_interface, version));

    wl_drm_add_listener(drm, &g_drmListener, resource);

    struct drm* drm_data = new struct drm;
    drm_data->drm = drm;
    drm_data->display = backend.m_display.display();

    wl_resource_set_implementation(resource, &g_drmImplementation, drm_data, destroyDrm);

    wl_display_roundtrip(backend.m_display.display());
}

const struct wl_drm_listener ViewBackend::g_drmListener = {
    // device
    [](void* data, struct wl_drm*, char const* name)
    {
        auto* resource = static_cast<struct wl_resource*>(data);
        wl_drm_send_device(resource, name);
    },
    // format
    [](void* data, struct wl_drm*, uint32_t format)
    {
        auto* resource = static_cast<struct wl_resource*>(data);
        wl_drm_send_format(resource, format);
    },
    // authenticated
    [](void* data, struct wl_drm*)
    {
        auto* resource = static_cast<struct wl_resource*>(data);
        wl_drm_send_authenticated(resource);
    },
    // capabilities
    [](void* data, struct wl_drm*, uint32_t capabilities)
    {
        auto* resource = static_cast<struct wl_resource*>(data);
        wl_drm_send_capabilities(resource, capabilities);
    },
};


ViewBackend::ViewBackend(struct wpe_view_backend* backend)
    : m_display(::Wayland::Display::singleton())
    , m_backend(backend)
    , m_viewDisplay(this)
{
    // Ensure the Pasteboard singleton is constructed early.
    // FIXME:
    // Pasteboard::Pasteboard::singleton();

    auto& server = NC::RendererHost::singleton();
    server.initialize();

    m_viewDisplay.initialize(server.display());

    m_server.drm = wl_global_create(server.display(), &wl_drm_interface, m_display.interfaces().drm_version,
            this, bindDrm);


    if (m_display.interfaces().shm)
        m_cursorTheme = wl_cursor_theme_load(NULL, 32, m_display.interfaces().shm);

    if (m_cursorTheme)
        m_display.setCursor(wl_cursor_theme_get_cursor(m_cursorTheme, "left_ptr"));
}

ViewBackend::~ViewBackend()
{
    m_backend = nullptr;

    m_display.setCursor(nullptr);

    if (m_cursorTheme)
        wl_cursor_theme_destroy(m_cursorTheme);
}

void ViewBackend::initialize()
{
}

NC::ViewDisplay::Surface* ViewBackend::createSurface(struct wl_resource* resource, NC::ViewDisplay& view, NC::ViewDisplay::Surface::Type type)
{
    return new Surface(m_backend, m_display, resource, view, type);
}

ViewBackend::Surface::Surface(struct wpe_view_backend* backend,
        ::Wayland::Display& display, struct wl_resource* resource,
        NC::ViewDisplay& view, NC::ViewDisplay::Surface::Type t)
    : NC::ViewDisplay::Surface(resource, view, t)
    , m_display(display)
    , m_backend(backend)
{
    m_resizingData.backend = m_backend;

    m_surface = wl_compositor_create_surface(m_display.interfaces().compositor);

    if (type() == NC::ViewDisplay::Surface::OnScreen) {
        if (m_display.interfaces().xdg) {
            m_xdgSurface = xdg_shell_get_xdg_surface(m_display.interfaces().xdg, m_surface);
            xdg_surface_add_listener(m_xdgSurface, &g_xdgSurfaceListener, &m_resizingData);
            xdg_surface_set_title(m_xdgSurface, "WPE");
        } else if (m_display.interfaces().shell) {
            m_shellSurface = wl_shell_get_shell_surface(m_display.interfaces().shell, m_surface);
            wl_shell_surface_add_listener(m_shellSurface, &g_shellSurfaceListener, &m_resizingData);
            wl_shell_surface_set_toplevel(m_shellSurface);
            wl_shell_surface_set_title(m_shellSurface, "WPE");
        }

        if (m_display.interfaces().ivi_application) {
            m_iviSurface = ivi_application_surface_create(m_display.interfaces().ivi_application,
                4200 + getpid(), // a unique identifier
                m_surface);
            ivi_surface_add_listener(m_iviSurface, &g_iviSurfaceListener, &m_resizingData);
        }

        m_display.registerInputClient(m_surface, wpe_view_backend_get_input(m_backend));
    }
}

ViewBackend::Surface::~Surface()
{
    if (type() == NC::ViewDisplay::Surface::OnScreen)
        m_display.unregisterInputClient(m_surface);

    if (m_iviSurface)
        ivi_surface_destroy(m_iviSurface);
    m_iviSurface = nullptr;
    if (m_xdgSurface)
        xdg_surface_destroy(m_xdgSurface);
    m_xdgSurface = nullptr;
    if (m_shellSurface)
        wl_shell_surface_destroy(m_shellSurface);
    m_shellSurface = nullptr;
    if (m_callback)
        wl_callback_destroy(m_callback);
    m_callback = nullptr;
    if (m_surface)
        wl_surface_destroy(m_surface);
    m_surface = nullptr;
}

void ViewBackend::Surface::onSurfaceAttach(NC::ViewDisplay::Buffer* buffer)
{
    struct wl_buffer* b = nullptr;
    int32_t x = 0;
    int32_t y = 0;

    if (buffer) {
        b = static_cast<struct wl_buffer*>(wl_resource_get_user_data(buffer->resource()));
        x = buffer->X();
        y = buffer->Y();
    }

    wl_surface_attach(m_surface, b, x, y);
}

void ViewBackend::Surface::onSurfaceCommit(const NC::ViewDisplay::Surface::CommitState& state)
{
    static const struct wl_callback_listener frameCallbackListener = {
        // done
        [](void* data, struct wl_callback*, uint32_t callback_data)
        {
            auto& surface = *static_cast<ViewBackend::Surface*>(data);
            wl_callback_destroy(surface.m_callback);
            surface.m_callback = nullptr;

            surface.frameComplete(callback_data);
        },
    };

    if (m_callback)
        wl_callback_destroy(m_callback);

    m_callback = wl_surface_frame(m_surface);
    wl_callback_add_listener(m_callback, &frameCallbackListener, this);

    if (state.damage)
        wl_surface_damage(m_surface, state.damage->X(), state.damage->Y(), state.damage->width(), state.damage->height());

    wl_surface_commit(m_surface);
}

Popup* ViewBackend::allocPopup(struct wpe_popup* popup, int32_t x, int32_t y)
{
    return new Popup(*this, popup, x, y);
}

Buffer* ViewBackend::allocBuffer(uint32_t format, uint32_t width, uint32_t height)
{
    static char const temp_file_template[] = "/WPE-shared-XXXXXX";
    size_t stride = width * 4;
    size_t size = height * stride;

    if (! m_display.interfaces().shm)
        return nullptr;

    char const* path = getenv("XDG_RUNTIME_DIR");
    if (!path)
        return nullptr;

    char* name = new char[strlen(path) + strlen(temp_file_template) + 1];
    if (!name)
        return nullptr;

    strcpy(name, path);
    strcat(name, temp_file_template);

    int fd = mkostemp(name, O_CLOEXEC);

    if (fd < 0) {
        delete [] name;
        return nullptr;
    }
    unlink(name);
    delete [] name;

    ftruncate(fd, size);

    void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    uint32_t wl_format = format;

    switch (format) {
    /* These two Wayland format numbers don't match the FourCC numbers, so convert
     * them specially */
    case WPE_FOURCC_XRGB8888:
        wl_format = WL_SHM_FORMAT_XRGB8888;
        break;
    case WPE_FOURCC_ARGB8888:
        wl_format = WL_SHM_FORMAT_ARGB8888;
        break;
    default:
        break;
    }

    struct wl_shm_pool* pool = wl_shm_create_pool(m_display.interfaces().shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, wl_format);
    wl_shm_pool_destroy(pool);

    return new Buffer(*this, fd, data, size, format, width, height, stride, buffer);
}

const struct wl_callback_listener Popup::s_frameCallbackListener = {
    // done
    [](void* data, struct wl_callback* callback, uint32_t callback_data)
    {
        auto* popup = static_cast<Popup*>(data);
        wl_callback_destroy(popup->m_frameCallback);
        popup->m_frameCallback = nullptr;

        wpe_popup_dispatch_frame_displayed(popup->m_popup);
    },
};

const struct wl_shell_surface_listener Popup::s_shellSurfaceListener = {
    // ping
    [](void* data, struct wl_shell_surface* surface, uint32_t serial)
    {
        wl_shell_surface_pong(surface, serial);
    },
    // configure
    [](void* data, struct wl_shell_surface* surface, uint32_t, int32_t width, int32_t height) { },
    // popup_done
    [](void* data, struct wl_shell_surface* surface)
    {
        auto* popup = static_cast<Popup*>(data);
        wl_shell_surface_destroy(popup->m_shellSurface);
        popup->m_shellSurface = nullptr;

        wpe_popup_dispatch_dismissed(popup->m_popup);
    },
};

const struct xdg_popup_listener Popup::s_xdgPopupListener = {
    // popup_done
    [](void* data, struct xdg_popup*)
    {
        auto* popup = static_cast<Popup*>(data);
        xdg_popup_destroy(popup->m_xdgPopup);
        popup->m_xdgPopup = nullptr;

        wpe_popup_dispatch_dismissed(popup->m_popup);
    },
};

Popup::Popup(ViewBackend& m_backend, struct wpe_popup* popup, int32_t x, int32_t y)
    : m_popup(popup)
    , m_parent(m_backend)
{
    auto* parent = m_parent.mainSurface()->surface();
    auto& display = m_parent.display();

    m_surface = wl_compositor_create_surface(display.interfaces().compositor);

    if (display.interfaces().xdg) {
        m_xdgPopup = xdg_shell_get_xdg_popup(display.interfaces().xdg, m_surface,
                parent, display.interfaces().seat, display.serial(), x, y);

        xdg_popup_add_listener(m_xdgPopup, &s_xdgPopupListener, this);
    } else if (display.interfaces().shell) {
        m_shellSurface = wl_shell_get_shell_surface(display.interfaces().shell, m_surface);

        wl_shell_surface_set_popup(m_shellSurface, display.interfaces().seat, display.serial(),
                parent, x, y, 0);

        wl_shell_surface_add_listener(m_shellSurface, &s_shellSurfaceListener, this);
    }

    display.registerInputClient(m_surface, wpe_popup_get_input(m_popup));
}

Popup::~Popup()
{
    if (m_xdgPopup)
        xdg_popup_destroy(m_xdgPopup);
    m_xdgPopup = nullptr;
    if (m_shellSurface)
        wl_shell_surface_destroy(m_shellSurface);
    m_shellSurface = nullptr;
    if (m_surface) {
        m_parent.display().unregisterInputClient(m_surface);
        wl_surface_destroy(m_surface);
    }
    m_surface = nullptr;
    if (m_frameCallback)
        wl_callback_destroy(m_frameCallback);
    m_frameCallback = nullptr;
}

void Popup::attachBuffer(Buffer* buffer)
{
    auto& display = m_parent.display();

    if (m_frameCallback)
        wl_callback_destroy(m_frameCallback);

    if (buffer) {
        m_frameCallback = wl_surface_frame(m_surface);
        wl_callback_add_listener(m_frameCallback, &s_frameCallbackListener, this);

        wl_surface_attach(m_surface, buffer->buffer(), 0, 0);
        wl_surface_damage(m_surface, 0, 0, buffer->width(), buffer->height());
    } else {
        wl_surface_attach(m_surface, NULL, 0, 0);
    }

    wl_surface_commit(m_surface);
}

const struct wpe_popup_interface Popup::interface = {
    // destroy
    [](void* data)
    {
        auto* popup = static_cast<Popup*>(data);
        delete popup;
    },
    // attach_buffer
    [](void* popup_data, void* buffer_data)
    {
        auto* popup = static_cast<Popup*>(popup_data);
        auto* buffer = static_cast<Buffer*>(buffer_data);

        popup->attachBuffer(buffer);
    },
};

const struct wpe_buffer_interface Buffer::interface = {
    // destroy
    [](void* data)
    {
        auto* buffer = static_cast<Buffer*>(data);
        delete buffer;
    },
    // get_info
    [](void* data, struct wpe_buffer_info* info)
    {
        auto* buffer = static_cast<Buffer*>(data);

        info->format = buffer->m_format;
        info->height = buffer->m_height;
        info->width = buffer->m_width;
        info->stride = buffer->m_stride;
        info->data = buffer->m_data;
    },
};


} // namespace Wayland
} // namespace NC

extern "C" {

struct wpe_view_backend_interface nc_view_backend_wayland_interface = {
    // create
    [](void*, struct wpe_view_backend* backend) -> void*
    {
        return new NC::Wayland::ViewBackend(backend);
    },
    // destroy
    [](void* data)
    {
        auto* backend = static_cast<NC::Wayland::ViewBackend*>(data);
        delete backend;
    },
    // initialize
    [](void* data) {
        auto* backend = static_cast<NC::Wayland::ViewBackend*>(data);
        backend->initialize();
    },
    // get_renderer_host_fd
    [](void* data) -> int
    {
        return -1;
    },
    // create_popup
    [](void* data, struct wpe_popup* popup, int32_t x, int32_t y) -> bool
    {
        auto* backend = static_cast<NC::Wayland::ViewBackend*>(data);

        auto* p = backend->allocPopup(popup, x, y);

        if (!p)
            return false;

        wpe_popup_set_interface(popup, &NC::Wayland::Popup::interface, static_cast<void*>(p));
        return true;
    },
    // alloc_buffer
    [](void* data, struct wpe_buffer* buffer, uint32_t format, uint32_t width, uint32_t height) -> bool
    {
        auto* backend = static_cast<NC::Wayland::ViewBackend*>(data);

        auto* b = backend->allocBuffer(format, width, height);

        if (!b)
            return false;

        wpe_buffer_set_interface(buffer, &NC::Wayland::Buffer::interface, static_cast<void*>(b));
        return true;
    },
    // get_display
    [](void* data) -> struct wl_display*
    {
        auto* backend = static_cast<NC::Wayland::ViewBackend*>(data);
        return backend->display().display();
    },
    // get_surface
    [](void* data) -> struct wl_surface*
    {
        auto* backend = static_cast<NC::Wayland::ViewBackend*>(data);
        auto* surface = backend->mainSurface();
        if (surface)
            return surface->surface();
        return nullptr;
    },
};

}
