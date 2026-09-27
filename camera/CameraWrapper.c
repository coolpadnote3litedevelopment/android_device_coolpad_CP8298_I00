/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "CameraWrapper"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <cutils/log.h>
#include <hardware/camera.h>
#include <hardware/hardware.h>
#include <nativebase/nativebase.h>

typedef void (*vendor_set_callbacks_t)(struct camera_device *dev,
        camera_notify_callback notify_cb, camera_data_callback data_cb,
        camera_data_timestamp_callback data_cb_timestamp,
        camera_request_memory get_memory, void *arc_display_cb, void *user);

#define MAX_PREVIEW_BUFFERS 32

/*
 * The M libcam.client takes every preview buffer_handle_t as the handle
 * member of an ANativeWindowBuffer and references it through its base.
 */
struct preview_buffer {
    ANativeWindowBuffer anb;
    buffer_handle_t *handle;
};

struct preview_window {
    preview_stream_ops_t ops;
    preview_stream_ops_t *window;
    int width;
    int height;
    int format;
    int usage;
    struct preview_buffer buffers[MAX_PREVIEW_BUFFERS];
};

struct notify_msg {
    struct notify_msg *next;
    camera_notify_callback cb;
    int32_t msg_type;
    int32_t ext1;
    int32_t ext2;
    void *user;
};

static camera_module_t *vendor_module;
static camera_device_ops_t wrapper_ops;
static vendor_set_callbacks_t vendor_set_callbacks;
static int (*vendor_set_preview_window)(struct camera_device *dev,
        struct preview_stream_ops *window);
static struct preview_window preview;
static pthread_mutex_t ops_lock = PTHREAD_MUTEX_INITIALIZER;

static camera_notify_callback client_notify_cb;
static camera_request_memory client_get_memory;
static void *client_user;
static pthread_mutex_t notify_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t notify_cond = PTHREAD_COND_INITIALIZER;
static struct notify_msg *notify_head, *notify_tail;
static pthread_t notify_thread;
static int notify_thread_started;

static void *notify_loop(void *arg __unused)
{
    struct notify_msg *m;

    for (;;) {
        pthread_mutex_lock(&notify_lock);
        while (!notify_head)
            pthread_cond_wait(&notify_cond, &notify_lock);
        m = notify_head;
        notify_head = m->next;
        if (!notify_head)
            notify_tail = NULL;
        pthread_mutex_unlock(&notify_lock);

        m->cb(m->msg_type, m->ext1, m->ext2, m->user);
        free(m);
    }

    return NULL;
}

static void wrapper_notify_cb(int32_t msg_type, int32_t ext1, int32_t ext2,
        void *user)
{
    struct notify_msg *m;
    camera_notify_callback cb = client_notify_cb;

    if (!cb)
        return;

    m = notify_thread_started ? malloc(sizeof(*m)) : NULL;
    if (!m) {
        cb(msg_type, ext1, ext2, user);
        return;
    }
    m->next = NULL;
    m->cb = cb;
    m->msg_type = msg_type;
    m->ext1 = ext1;
    m->ext2 = ext2;
    m->user = user;

    pthread_mutex_lock(&notify_lock);
    if (notify_tail)
        notify_tail->next = m;
    else
        notify_head = m;
    notify_tail = m;
    pthread_cond_signal(&notify_cond);
    pthread_mutex_unlock(&notify_lock);
}

static camera_memory_t *wrapper_get_memory(int fd, size_t buf_size,
        unsigned int num_bufs, void *user __unused)
{
    return client_get_memory(fd, buf_size, num_bufs, client_user);
}

static void wrapper_set_callbacks(struct camera_device *dev,
        camera_notify_callback notify_cb, camera_data_callback data_cb,
        camera_data_timestamp_callback data_cb_timestamp,
        camera_request_memory get_memory, void *user)
{
    pthread_mutex_lock(&notify_lock);
    client_notify_cb = notify_cb;
    client_get_memory = get_memory;
    client_user = user;
    if (!notify_thread_started &&
            !pthread_create(&notify_thread, NULL, notify_loop, NULL))
        notify_thread_started = 1;
    pthread_mutex_unlock(&notify_lock);

    vendor_set_callbacks(dev, notify_cb ? wrapper_notify_cb : NULL, data_cb,
            data_cb_timestamp, get_memory ? wrapper_get_memory : NULL, NULL, user);
}

static void anb_ref(struct android_native_base_t *base __unused)
{
}

static struct preview_window *to_preview(struct preview_stream_ops *w)
{
    return (struct preview_window *)w;
}

static buffer_handle_t *real_handle(struct preview_window *p, buffer_handle_t *buffer)
{
    struct preview_buffer *b;

    if (buffer < &p->buffers[0].anb.handle ||
            buffer > &p->buffers[MAX_PREVIEW_BUFFERS - 1].anb.handle)
        return buffer;
    b = (struct preview_buffer *)((char *)buffer - offsetof(struct preview_buffer, anb.handle));
    return b->handle;
}

static int preview_dequeue_buffer(struct preview_stream_ops *w,
        buffer_handle_t **buffer, int *stride)
{
    struct preview_window *p = to_preview(w);
    struct preview_buffer *b = NULL;
    int i, ret;

    ret = p->window->dequeue_buffer(p->window, buffer, stride);
    if (ret)
        return ret;

    for (i = 0; i < MAX_PREVIEW_BUFFERS; i++) {
        if (p->buffers[i].handle == *buffer || !p->buffers[i].handle) {
            b = &p->buffers[i];
            break;
        }
    }
    if (!b) {
        ALOGE("%s: out of preview buffer slots", __func__);
        p->window->cancel_buffer(p->window, *buffer);
        return -ENOMEM;
    }

    b->handle = *buffer;
    b->anb.common.magic = ANDROID_NATIVE_BUFFER_MAGIC;
    b->anb.common.version = sizeof(ANativeWindowBuffer);
    b->anb.common.incRef = anb_ref;
    b->anb.common.decRef = anb_ref;
    b->anb.width = p->width;
    b->anb.height = p->height;
    b->anb.stride = *stride;
    b->anb.format = p->format;
    b->anb.usage = p->usage;
    b->anb.handle = **buffer;
    *buffer = &b->anb.handle;
    return 0;
}

static int preview_enqueue_buffer(struct preview_stream_ops *w, buffer_handle_t *buffer)
{
    struct preview_window *p = to_preview(w);

    return p->window->enqueue_buffer(p->window, real_handle(p, buffer));
}

static int preview_cancel_buffer(struct preview_stream_ops *w, buffer_handle_t *buffer)
{
    struct preview_window *p = to_preview(w);

    return p->window->cancel_buffer(p->window, real_handle(p, buffer));
}

static int preview_set_buffer_count(struct preview_stream_ops *w, int count)
{
    struct preview_window *p = to_preview(w);

    return p->window->set_buffer_count(p->window, count);
}

static int preview_set_buffers_geometry(struct preview_stream_ops *w,
        int width, int height, int format)
{
    struct preview_window *p = to_preview(w);

    p->width = width;
    p->height = height;
    p->format = format;
    return p->window->set_buffers_geometry(p->window, width, height, format);
}

static int preview_set_crop(struct preview_stream_ops *w,
        int left, int top, int right, int bottom)
{
    struct preview_window *p = to_preview(w);

    return p->window->set_crop(p->window, left, top, right, bottom);
}

static int preview_set_usage(struct preview_stream_ops *w, int usage)
{
    struct preview_window *p = to_preview(w);

    p->usage = usage;
    return p->window->set_usage(p->window, usage);
}

static int preview_set_swap_interval(struct preview_stream_ops *w, int interval)
{
    struct preview_window *p = to_preview(w);

    return p->window->set_swap_interval(p->window, interval);
}

static int preview_get_min_undequeued_buffer_count(const struct preview_stream_ops *w,
        int *count)
{
    struct preview_window *p = to_preview((struct preview_stream_ops *)w);

    return p->window->get_min_undequeued_buffer_count(p->window, count);
}

static int preview_lock_buffer(struct preview_stream_ops *w, buffer_handle_t *buffer)
{
    struct preview_window *p = to_preview(w);

    return p->window->lock_buffer(p->window, real_handle(p, buffer));
}

static int preview_set_timestamp(struct preview_stream_ops *w, int64_t timestamp)
{
    struct preview_window *p = to_preview(w);

    return p->window->set_timestamp(p->window, timestamp);
}

static int wrapper_set_preview_window(struct camera_device *dev,
        struct preview_stream_ops *window)
{
    memset(&preview, 0, sizeof(preview));
    if (!window)
        return vendor_set_preview_window(dev, NULL);

    preview.window = window;
    preview.ops.dequeue_buffer = preview_dequeue_buffer;
    preview.ops.enqueue_buffer = preview_enqueue_buffer;
    preview.ops.cancel_buffer = preview_cancel_buffer;
    preview.ops.set_buffer_count = preview_set_buffer_count;
    preview.ops.set_buffers_geometry = preview_set_buffers_geometry;
    preview.ops.set_crop = preview_set_crop;
    preview.ops.set_usage = preview_set_usage;
    preview.ops.set_swap_interval = preview_set_swap_interval;
    preview.ops.get_min_undequeued_buffer_count = preview_get_min_undequeued_buffer_count;
    preview.ops.lock_buffer = preview_lock_buffer;
    preview.ops.set_timestamp = preview_set_timestamp;
    return vendor_set_preview_window(dev, &preview.ops);
}

static void wrap_device(hw_device_t *device)
{
    camera_device_t *camera = (camera_device_t *)device;

    if (device->version >= CAMERA_DEVICE_API_VERSION_2_0)
        return;

    pthread_mutex_lock(&ops_lock);
    if (!vendor_set_callbacks) {
        wrapper_ops = *camera->ops;
        vendor_set_callbacks = (vendor_set_callbacks_t)camera->ops->set_callbacks;
        wrapper_ops.set_callbacks = wrapper_set_callbacks;
        vendor_set_preview_window = camera->ops->set_preview_window;
        wrapper_ops.set_preview_window = wrapper_set_preview_window;
    }
    pthread_mutex_unlock(&ops_lock);

    camera->ops = &wrapper_ops;
}

static int wrapper_open(const hw_module_t *module __unused, const char *name,
        hw_device_t **device)
{
    int ret;

    if (!vendor_module)
        return -ENODEV;

    ret = vendor_module->common.methods->open(&vendor_module->common, name,
            device);
    if (!ret)
        wrap_device(*device);

    return ret;
}

static int wrapper_open_legacy(const hw_module_t *module __unused,
        const char *id, uint32_t hal_version, hw_device_t **device)
{
    int ret;

    ret = vendor_module->open_legacy(&vendor_module->common, id, hal_version,
            device);
    if (!ret)
        wrap_device(*device);

    return ret;
}

static int wrapper_get_number_of_cameras(void)
{
    return vendor_module ? vendor_module->get_number_of_cameras() : 0;
}

static int wrapper_get_camera_info(int camera_id, struct camera_info *info)
{
    if (!vendor_module)
        return -ENODEV;
    return vendor_module->get_camera_info(camera_id, info);
}

static int wrapper_set_callbacks_module(const camera_module_callbacks_t *callbacks)
{
    return vendor_module->set_callbacks(callbacks);
}

static void wrapper_get_vendor_tag_ops(vendor_tag_ops_t *ops)
{
    vendor_module->get_vendor_tag_ops(ops);
}

static int wrapper_set_torch_mode(const char *camera_id, bool enabled)
{
    return vendor_module->set_torch_mode(camera_id, enabled);
}

static int wrapper_init(void)
{
    return vendor_module->init();
}

static struct hw_module_methods_t wrapper_module_methods = {
    .open = wrapper_open,
};

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_1_0,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = CAMERA_HARDWARE_MODULE_ID,
        .name = "CP8298_I00 Camera Wrapper",
        .author = "The LineageOS Project",
        .methods = &wrapper_module_methods,
    },
    .get_number_of_cameras = wrapper_get_number_of_cameras,
    .get_camera_info = wrapper_get_camera_info,
};

__attribute__((constructor))
static void wrapper_load_vendor_module(void)
{
    camera_module_t *hmi = &HAL_MODULE_INFO_SYM;

    if (hw_get_module_by_class(CAMERA_HARDWARE_MODULE_ID, "vendor",
            (const hw_module_t **)&vendor_module)) {
        ALOGE("failed to load vendor camera module");
        vendor_module = NULL;
        return;
    }

    hmi->common.module_api_version = vendor_module->common.module_api_version;
    if (vendor_module->set_callbacks)
        hmi->set_callbacks = wrapper_set_callbacks_module;
    if (vendor_module->get_vendor_tag_ops)
        hmi->get_vendor_tag_ops = wrapper_get_vendor_tag_ops;
    if (vendor_module->open_legacy)
        hmi->open_legacy = wrapper_open_legacy;
    if (vendor_module->set_torch_mode)
        hmi->set_torch_mode = wrapper_set_torch_mode;
    if (vendor_module->init)
        hmi->init = wrapper_init;
}
