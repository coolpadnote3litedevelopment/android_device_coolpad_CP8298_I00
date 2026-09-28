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

#define LOG_TAG "KeymasterWrapper"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <cutils/log.h>
#include <hardware/hardware.h>
#include <hardware/keymaster1.h>

#define VENDOR_MODULE "/system/lib64/hw/keystore.vendor.mt6735.so"

typedef struct {
    keymaster1_device_t device;
    keymaster1_device_t *vendor;
} wrapper_device_t;

static keymaster1_device_t *vendor(const keymaster1_device_t *dev) {
    return ((const wrapper_device_t *)dev)->vendor;
}

/* The TEE keymaster rejects tags it does not know with KM_ERROR_INVALID_TAG. */
static bool unsupported(keymaster_tag_t tag) {
    return tag == KM_TAG_MIN_SECONDS_BETWEEN_OPS ||
           tag == KM_TAG_BLOB_USAGE_REQUIREMENTS;
}

static keymaster_key_param_set_t *filter(const keymaster_key_param_set_t *in, const char *op) {
    keymaster_key_param_set_t *out = calloc(1, sizeof(*out));
    if (!out)
        return NULL;
    out->params = calloc(in->length ? in->length : 1, sizeof(*out->params));
    if (!out->params) {
        free(out);
        return NULL;
    }
    for (size_t i = 0; i < in->length; i++) {
        if (unsupported(in->params[i].tag)) {
            ALOGI("%s: dropping tag 0x%08x", op, in->params[i].tag);
            continue;
        }
        out->params[out->length++] = in->params[i];
    }
    return out;
}

static void release(keymaster_key_param_set_t *set) {
    if (set) {
        free(set->params);
        free(set);
    }
}

static keymaster_error_t get_supported_algorithms(const keymaster1_device_t *dev,
        keymaster_algorithm_t **algorithms, size_t *count) {
    return vendor(dev)->get_supported_algorithms(vendor(dev), algorithms, count);
}

static keymaster_error_t get_supported_block_modes(const keymaster1_device_t *dev,
        keymaster_algorithm_t algorithm, keymaster_purpose_t purpose,
        keymaster_block_mode_t **modes, size_t *count) {
    return vendor(dev)->get_supported_block_modes(vendor(dev), algorithm, purpose, modes, count);
}

static keymaster_error_t get_supported_padding_modes(const keymaster1_device_t *dev,
        keymaster_algorithm_t algorithm, keymaster_purpose_t purpose,
        keymaster_padding_t **modes, size_t *count) {
    return vendor(dev)->get_supported_padding_modes(vendor(dev), algorithm, purpose, modes, count);
}

static keymaster_error_t get_supported_digests(const keymaster1_device_t *dev,
        keymaster_algorithm_t algorithm, keymaster_purpose_t purpose,
        keymaster_digest_t **digests, size_t *count) {
    return vendor(dev)->get_supported_digests(vendor(dev), algorithm, purpose, digests, count);
}

static keymaster_error_t get_supported_import_formats(const keymaster1_device_t *dev,
        keymaster_algorithm_t algorithm, keymaster_key_format_t **formats, size_t *count) {
    return vendor(dev)->get_supported_import_formats(vendor(dev), algorithm, formats, count);
}

static keymaster_error_t get_supported_export_formats(const keymaster1_device_t *dev,
        keymaster_algorithm_t algorithm, keymaster_key_format_t **formats, size_t *count) {
    return vendor(dev)->get_supported_export_formats(vendor(dev), algorithm, formats, count);
}

static keymaster_error_t add_rng_entropy(const keymaster1_device_t *dev,
        const uint8_t *data, size_t length) {
    return vendor(dev)->add_rng_entropy(vendor(dev), data, length);
}

static keymaster_error_t generate_key(const keymaster1_device_t *dev,
        const keymaster_key_param_set_t *params, keymaster_key_blob_t *key_blob,
        keymaster_key_characteristics_t **characteristics) {
    keymaster_key_param_set_t *set = filter(params, "generate_key");
    if (!set)
        return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    keymaster_error_t ret = vendor(dev)->generate_key(vendor(dev), set, key_blob, characteristics);
    if (ret != KM_ERROR_OK) {
        ALOGE("generate_key failed %d", ret);
        for (size_t i = 0; i < set->length; i++)
            ALOGE("  tag 0x%08x", set->params[i].tag);
    }
    release(set);
    return ret;
}

static keymaster_error_t get_key_characteristics(const keymaster1_device_t *dev,
        const keymaster_key_blob_t *key_blob, const keymaster_blob_t *client_id,
        const keymaster_blob_t *app_data, keymaster_key_characteristics_t **characteristics) {
    return vendor(dev)->get_key_characteristics(vendor(dev), key_blob, client_id, app_data,
            characteristics);
}

static keymaster_error_t import_key(const keymaster1_device_t *dev,
        const keymaster_key_param_set_t *params, keymaster_key_format_t key_format,
        const keymaster_blob_t *key_data, keymaster_key_blob_t *key_blob,
        keymaster_key_characteristics_t **characteristics) {
    keymaster_key_param_set_t *set = filter(params, "import_key");
    if (!set)
        return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    keymaster_error_t ret = vendor(dev)->import_key(vendor(dev), set, key_format, key_data,
            key_blob, characteristics);
    release(set);
    return ret;
}

static keymaster_error_t export_key(const keymaster1_device_t *dev,
        keymaster_key_format_t export_format, const keymaster_key_blob_t *key_to_export,
        const keymaster_blob_t *client_id, const keymaster_blob_t *app_data,
        keymaster_blob_t *export_data) {
    return vendor(dev)->export_key(vendor(dev), export_format, key_to_export, client_id,
            app_data, export_data);
}

static keymaster_error_t delete_key(const keymaster1_device_t *dev,
        const keymaster_key_blob_t *key) {
    if (!vendor(dev)->delete_key)
        return KM_ERROR_UNIMPLEMENTED;
    return vendor(dev)->delete_key(vendor(dev), key);
}

static keymaster_error_t delete_all_keys(const keymaster1_device_t *dev) {
    if (!vendor(dev)->delete_all_keys)
        return KM_ERROR_UNIMPLEMENTED;
    return vendor(dev)->delete_all_keys(vendor(dev));
}

static keymaster_error_t begin(const keymaster1_device_t *dev, keymaster_purpose_t purpose,
        const keymaster_key_blob_t *key, const keymaster_key_param_set_t *in_params,
        keymaster_key_param_set_t *out_params, keymaster_operation_handle_t *operation_handle) {
    return vendor(dev)->begin(vendor(dev), purpose, key, in_params, out_params, operation_handle);
}

static keymaster_error_t update(const keymaster1_device_t *dev,
        keymaster_operation_handle_t operation_handle, const keymaster_key_param_set_t *in_params,
        const keymaster_blob_t *input, size_t *input_consumed,
        keymaster_key_param_set_t *out_params, keymaster_blob_t *output) {
    return vendor(dev)->update(vendor(dev), operation_handle, in_params, input, input_consumed,
            out_params, output);
}

static keymaster_error_t finish(const keymaster1_device_t *dev,
        keymaster_operation_handle_t operation_handle, const keymaster_key_param_set_t *in_params,
        const keymaster_blob_t *signature, keymaster_key_param_set_t *out_params,
        keymaster_blob_t *output) {
    return vendor(dev)->finish(vendor(dev), operation_handle, in_params, signature, out_params,
            output);
}

static keymaster_error_t abort_op(const keymaster1_device_t *dev,
        keymaster_operation_handle_t operation_handle) {
    return vendor(dev)->abort(vendor(dev), operation_handle);
}

static int device_close(hw_device_t *device) {
    wrapper_device_t *wrapper = (wrapper_device_t *)device;
    int ret = wrapper->vendor->common.close(&wrapper->vendor->common);
    free(wrapper);
    return ret;
}

static int device_open(const hw_module_t *module, const char *name, hw_device_t **device) {
    if (strcmp(name, KEYSTORE_KEYMASTER))
        return -EINVAL;

    void *handle = dlopen(VENDOR_MODULE, RTLD_NOW);
    if (!handle) {
        ALOGE("failed to load %s: %s", VENDOR_MODULE, dlerror());
        return -EINVAL;
    }
    hw_module_t *vendor_module = dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR);
    if (!vendor_module) {
        ALOGE("no module info in %s", VENDOR_MODULE);
        return -EINVAL;
    }

    wrapper_device_t *wrapper = calloc(1, sizeof(*wrapper));
    if (!wrapper)
        return -ENOMEM;

    int ret = vendor_module->methods->open(vendor_module, name,
            (hw_device_t **)&wrapper->vendor);
    if (ret) {
        ALOGE("vendor keymaster open failed %d", ret);
        free(wrapper);
        return ret;
    }

    keymaster1_device_t *dev = &wrapper->device;
    dev->common = wrapper->vendor->common;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = device_close;
    dev->context = wrapper->vendor->context;
    dev->flags = wrapper->vendor->flags;
    dev->get_supported_algorithms = get_supported_algorithms;
    dev->get_supported_block_modes = get_supported_block_modes;
    dev->get_supported_padding_modes = get_supported_padding_modes;
    dev->get_supported_digests = get_supported_digests;
    dev->get_supported_import_formats = get_supported_import_formats;
    dev->get_supported_export_formats = get_supported_export_formats;
    dev->add_rng_entropy = add_rng_entropy;
    dev->generate_key = generate_key;
    dev->get_key_characteristics = get_key_characteristics;
    dev->import_key = import_key;
    dev->export_key = export_key;
    dev->delete_key = delete_key;
    dev->delete_all_keys = delete_all_keys;
    dev->begin = begin;
    dev->update = update;
    dev->finish = finish;
    dev->abort = abort_op;

    *device = &dev->common;
    return 0;
}

static struct hw_module_methods_t module_methods = {
    .open = device_open,
};

struct keystore_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = KEYMASTER_MODULE_API_VERSION_1_0,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = KEYSTORE_HARDWARE_MODULE_ID,
        .name = "CP8298_I00 Keymaster Wrapper",
        .author = "The LineageOS Project",
        .methods = &module_methods,
    },
};
