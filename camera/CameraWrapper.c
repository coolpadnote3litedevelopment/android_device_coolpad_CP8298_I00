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

/*
 * Coolpad's HAL1 set_callbacks takes an ArcSoft display callback between
 * get_memory and the cookie. Pass NULL there so the HAL skips it and gets
 * the real cookie.
 */

#define LOG_TAG "CameraWrapper"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>

#include <cutils/log.h>
#include <hardware/camera.h>
#include <hardware/hardware.h>

typedef void (*vendor_set_callbacks_t)(struct camera_device *dev,
        camera_notify_callback notify_cb, camera_data_callback data_cb,
        camera_data_timestamp_callback data_cb_timestamp,
        camera_request_memory get_memory, void *arc_display_cb, void *user);

static camera_module_t *vendor_module;
static camera_device_ops_t wrapper_ops;
static vendor_set_callbacks_t vendor_set_callbacks;
static pthread_mutex_t ops_lock = PTHREAD_MUTEX_INITIALIZER;

static void wrapper_set_callbacks(struct camera_device *dev,
        camera_notify_callback notify_cb, camera_data_callback data_cb,
        camera_data_timestamp_callback data_cb_timestamp,
        camera_request_memory get_memory, void *user)
{
    vendor_set_callbacks(dev, notify_cb, data_cb, data_cb_timestamp,
            get_memory, NULL, user);
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
