#include "plugins/livision/livision.h"
#include "plugins/livision/livision_shm.h"

#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/logging.h"

#include "gl_renderer.h"
#include "texture_registry.h"


static struct livision_internal {
    bool initialized;
    struct flutterpi* flutterpi;
    EGLDisplay display;
    EGLContext context;
    struct texture* texture[FB_COUNT];
    struct texture_frame frame;

    // shm 相关
    struct livision_ctl_t* controller;   // 共享内存控制器，包含信号量等
    uint8_t* fb[4];                     // 存储mmap后的framebuffer

    // 渲染线程
    pthread_t thread;
    bool stopRender;
} internal;

static bool create_egl_context()
{
    // create egl context
    struct gl_renderer* renderer = flutterpi_get_gl_renderer(internal.flutterpi);

    internal.display = gl_renderer_get_egl_display(renderer);
    if (internal.display == EGL_NO_DISPLAY) {
        return false;
    }

    internal.context = gl_renderer_create_context(renderer);
    if (internal.context == EGL_NO_CONTEXT) {
        return false;
    }

    return true;
}

static void destroy_egl_context()
{
    eglDestroyContext(internal.display, internal.context);
}

#define GL_RGBA8 0x8058
#include <pthread.h>
static void push_framebuffer(GLuint texture, int id)
{
    GLuint format;
    struct livision_fb_header_t* header = &internal.controller->fbs[id];
    if (header->bpp == 3) {
        format = GL_RGB;
    }
    else {
        format = GL_RGBA;
    }
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexImage2D(GL_TEXTURE_2D, 0, format, header->width, header->height,
        0, GL_RGBA, GL_UNSIGNED_BYTE, internal.fb[id]);

    // push the frame to flutter
    internal.frame.gl.name = texture;
    texture_push_frame(
        internal.texture[id],
        &internal.frame
    );
}

void* thread_render(void*)
{
    bool first_loop = false;
    GLuint textures[FB_COUNT];

    if (!create_egl_context()) {
        LOG_ERROR("Failed to create EGL context\n");
        return NULL;
    }
    eglMakeCurrent(internal.display, EGL_NO_SURFACE, EGL_NO_SURFACE, internal.context);

    glGenTextures(FB_COUNT, textures);

    memset(&internal.frame, 0, sizeof(internal.frame));
    internal.frame.gl.target = GL_TEXTURE_2D;
    internal.frame.gl.format = GL_RGBA8;
    // other fields are all zero

    while (1) {
        eglMakeCurrent(internal.display, EGL_NO_SURFACE, EGL_NO_SURFACE, internal.context);
        for (int i = 0; i < FB_COUNT; ++i) {
            if (internal.controller->fbs[i].frame_valid || first_loop) {
                push_framebuffer(textures[i], i);
                internal.controller->fbs[i].frame_valid = 0;
            }
        }
        sem_post(&internal.controller->mutex);
        glBindTexture(GL_TEXTURE_2D, 0);
        eglMakeCurrent(internal.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        while (1) {
            bool has_new_frame = false;
            sem_wait(&internal.controller->signal);
            if (internal.stopRender) break;
            sem_wait(&internal.controller->mutex);
            for (int i = 0; i < FB_COUNT; ++i) {
                if (internal.controller->fbs[i].frame_valid) {
                    has_new_frame = true;
                    break;
                }
            }
            if (has_new_frame || internal.stopRender) break;
            sem_post(&internal.controller->mutex);
        }
        if (internal.stopRender) break;
        first_loop = true;
    }
    eglMakeCurrent(internal.display, EGL_NO_SURFACE, EGL_NO_SURFACE, internal.context);
    glDeleteTextures(FB_COUNT, textures);
    eglMakeCurrent(internal.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    destroy_egl_context();
    return NULL;
}

static int on_get_texture(struct platch_obj* object, FlutterPlatformMessageResponseHandle* response_handle)
{
    (void)object;

    if (internal.initialized == false) {
        internal.controller = map_shm_ctl();
        for (int i = 0; i < 4; ++i) {
            internal.texture[i] = flutterpi_create_texture(internal.flutterpi);
            internal.fb[i] = map_framebuffer(i);
            if (internal.texture == NULL || internal.fb[i] == NULL) {
                return platch_respond_error_std(response_handle, "gl-error", "Failed to initialize", &STDNULL);
            }
        }

        internal.stopRender = false;
        pthread_create(&internal.thread, NULL, thread_render, NULL);

        internal.initialized = true;
    }
    int64_t textures[FB_COUNT];
    for (int i = 0; i < FB_COUNT; ++i) {
        textures[i] = texture_get_id(internal.texture[i]);
    }
    int ok = platch_respond_success_std(
        response_handle,
        &(struct std_value) {
        .type = kStdInt64Array,
            .size = FB_COUNT,
            .int64array = (int64_t*)textures,
    }
    );

    return ok;
}

static int on_receive(char* channel, struct platch_obj* object, FlutterPlatformMessageResponseHandle* response_handle)
{
    (void)channel;

    const char* method;
    method = object->method;

    if (streq(method, "get_texture")) {
        return on_get_texture(object, response_handle);
    }

    return platch_respond_not_implemented(response_handle);
}

enum plugin_init_result livision_init(struct flutterpi* flutterpi, void** userdata_out)
{
    int ok;

    internal.flutterpi = flutterpi;

    ok = plugin_registry_set_receiver_locked(LIVISION_CHANNEL, kStandardMethodCall, on_receive);
    if (ok != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    *userdata_out = NULL;

    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void livision_deinit(struct flutterpi* flutterpi, void* userdata)
{
    (void)userdata;

    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), LIVISION_CHANNEL);

    if (internal.initialized == false) return;
    internal.stopRender = true;
    sem_post(&internal.controller->signal);
    pthread_join(internal.thread, NULL);
    for (int i = 0; i < 4; ++i) {
        unmap_framebuffer(internal.fb[i]);
        internal.fb[i] = NULL;
    }
    unmap_shm_ctl(internal.controller);
    internal.controller = NULL;
}

FLUTTERPI_PLUGIN("LiVision plugin", livision_plugin, livision_init, livision_deinit)
