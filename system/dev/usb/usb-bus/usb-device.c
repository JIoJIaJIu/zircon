// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/usb/usb.h>
#include <zircon/hw/usb-audio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "usb-bus.h"
#include "usb-device.h"
#include "usb-interface.h"
#include "util.h"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))

// By default we create devices for the interfaces on the first configuration.
// This table allows us to specify a different configuration for certain devices
// based on their VID and PID.
//
// TODO(voydanoff) Find a better way of handling this. For example, we could query to see
// if any interfaces on the first configuration have drivers that can bind to them.
// If not, then we could try the other configurations automatically instead of having
// this hard coded list of VID/PID pairs
typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint8_t configuration;
} usb_config_override_t;

static const usb_config_override_t config_overrides[] = {
    { 0x0bda, 0x8153, 2 },  // Realtek ethernet dongle has CDC interface on configuration 2
    { 0, 0, 0 },
};

// This thread is for calling the usb request completion callback for requests received from our
// client. We do this on a separate thread because it is unsafe to call out on our own completion
// callback, which is called on the main thread of the USB HCI driver.
static int callback_thread(void* arg) {
    usb_device_t* dev = (usb_device_t *)arg;
    bool done = false;

    while (!done) {
        // wait for new usb requests to complete or for signal to exit this thread
        sync_completion_wait(&dev->callback_thread_completion, ZX_TIME_INFINITE);

        mtx_lock(&dev->callback_lock);

        sync_completion_reset(&dev->callback_thread_completion);
        done = dev->callback_thread_stop;

        // copy completed requests to a temp list so we can process them outside of our lock
        list_node_t temp_list = LIST_INITIAL_VALUE(temp_list);
        list_move(&dev->completed_reqs, &temp_list);

        mtx_unlock(&dev->callback_lock);

        // call completion callbacks outside of the lock
        usb_request_t* req;
        while ((req = list_remove_head_type(&temp_list, usb_request_t, node))) {
            usb_request_complete(req, req->response.status, req->response.actual);
        }
    }

    return 0;
}

static void start_callback_thread(usb_device_t* dev) {
    // TODO(voydanoff) Once we have a way of knowing when a driver has bound to us, move the thread
    // start there so we don't have to start a thread unless we know we will need it.
    thrd_create_with_name(&dev->callback_thread, callback_thread, dev, "usb-device-callback-thread");
}

static void stop_callback_thread(usb_device_t* dev) {
    mtx_lock(&dev->callback_lock);
    dev->callback_thread_stop = true;
    mtx_unlock(&dev->callback_lock);

    sync_completion_signal(&dev->callback_thread_completion);
    thrd_join(dev->callback_thread, NULL);
}

// usb request completion for the requests passed down to the HCI driver
static void request_complete(usb_request_t* req, void* cookie) {
    usb_device_t* dev = cookie;

    mtx_lock(&dev->callback_lock);
    // move original request to completed_reqs list so it can be completed on the callback_thread
    req->complete_cb = req->saved_complete_cb;
    req->cookie = req->saved_cookie;
    list_add_tail(&dev->completed_reqs, &req->node);
    mtx_unlock(&dev->callback_lock);
    sync_completion_signal(&dev->callback_thread_completion);
}

static zx_status_t usb_device_add_interfaces(usb_device_t* parent,
                                             usb_configuration_descriptor_t* config);

void usb_device_set_hub_interface(usb_device_t* dev, usb_hub_interface_t* hub_intf) {
    mtx_lock(&dev->interface_mutex);
    dev->isHub = true;
    if (hub_intf) {
        memcpy(&dev->hub_intf, hub_intf, sizeof(dev->hub_intf));
    }
    mtx_unlock(&dev->interface_mutex);
}

static usb_configuration_descriptor_t* get_config_desc(usb_device_t* dev, int config) {
    int num_configurations = dev->device_desc.bNumConfigurations;
    for (int i = 0; i < num_configurations; i++) {
        usb_configuration_descriptor_t* desc = dev->config_descs[i];
        if (desc->bConfigurationValue == config) {
            return desc;
        }
    }
    return NULL;
}

static void usb_device_remove_interfaces(usb_device_t* dev) {
    mtx_lock(&dev->interface_mutex);

    usb_interface_t* intf;
    while ((intf = list_remove_head_type(&dev->children, usb_interface_t, node)) != NULL) {
        device_remove(intf->zxdev);
    }

    mtx_unlock(&dev->interface_mutex);
}

zx_status_t usb_device_claim_interface(usb_device_t* dev, uint8_t interface_id) {
    mtx_lock(&dev->interface_mutex);

    interface_status_t status = dev->interface_statuses[interface_id];
    if (status == CLAIMED) {
        // The interface has already been claimed by a different interface.
        mtx_unlock(&dev->interface_mutex);
        return ZX_ERR_ALREADY_BOUND;
    } else if (status == CHILD_DEVICE) {
        bool removed = usb_device_remove_interface_by_id_locked(dev, interface_id);
        if (!removed) {
            mtx_unlock(&dev->interface_mutex);
            return ZX_ERR_BAD_STATE;
        }
    }
    dev->interface_statuses[interface_id] = CLAIMED;

    mtx_unlock(&dev->interface_mutex);

    return ZX_OK;
}

/*
zx_status_t usb_device_set_configuration(usb_device_t* dev, int config) {
    int num_configurations = dev->device_desc.bNumConfigurations;
    usb_configuration_descriptor_t* config_desc = NULL;
    int config_index = -1;

    // validate config and get the new current_config_index
    for (int i = 0; i < num_configurations; i++) {
        usb_configuration_descriptor_t* desc = dev->config_descs[i];
        if (desc->bConfigurationValue == config) {
            config_desc = desc;
            config_index = i;
            break;
        }
    }
    if (!config_desc) return ZX_ERR_INVALID_ARGS;

    // set configuration
    zx_status_t status = usb_util_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                             USB_REQ_SET_CONFIGURATION, config, 0, NULL, 0);
    if (status < 0) {
        zxlogf(ERROR, "usb_device_set_configuration: USB_REQ_SET_CONFIGURATION failed\n");
        return status;
    }

    dev->current_config_index = config_index;

    // tear down and recreate the subdevices for our interfaces
    usb_device_remove_interfaces(dev);
    memset(dev->interface_statuses, 0,
           config_desc->bNumInterfaces * sizeof(interface_status_t));
    return usb_device_add_interfaces(dev, config_desc);
}
*/

static zx_status_t usb_device_ioctl(void* ctx, uint32_t op, const void* in_buf, size_t in_len,
                                    void* out_buf, size_t out_len, size_t* out_actual) {
    usb_device_t* dev = ctx;

    switch (op) {
    case IOCTL_USB_GET_DEVICE_TYPE: {
        int* reply = out_buf;
        if (out_len < sizeof(*reply)) return ZX_ERR_BUFFER_TOO_SMALL;
        *reply = USB_DEVICE_TYPE_DEVICE;
        *out_actual = sizeof(*reply);
        return ZX_OK;
    }
    case IOCTL_USB_GET_DEVICE_SPEED: {
        int* reply = out_buf;
        if (out_len < sizeof(*reply)) return ZX_ERR_BUFFER_TOO_SMALL;
        *reply = dev->speed;
        *out_actual = sizeof(*reply);
        return ZX_OK;
    }
    case IOCTL_USB_GET_DEVICE_DESC: {
        usb_device_descriptor_t* descriptor = &dev->device_desc;
        if (out_len < sizeof(*descriptor)) return ZX_ERR_BUFFER_TOO_SMALL;
        memcpy(out_buf, descriptor, sizeof(*descriptor));
        *out_actual = sizeof(*descriptor);
        return ZX_OK;
    }
    case IOCTL_USB_GET_CONFIG_DESC_SIZE: {
        if (in_len != sizeof(int)) return ZX_ERR_INVALID_ARGS;
        int config = *((int *)in_buf);
        int* reply = out_buf;
        usb_configuration_descriptor_t* descriptor = get_config_desc(dev, config);
        if (!descriptor) {
            return ZX_ERR_INVALID_ARGS;
        }
        *reply = le16toh(descriptor->wTotalLength);
        *out_actual = sizeof(*reply);
        return ZX_OK;
    }
    case IOCTL_USB_GET_DESCRIPTORS_SIZE: {
        usb_configuration_descriptor_t* descriptor = dev->config_descs[dev->current_config_index];
        int* reply = out_buf;
        if (out_len < sizeof(*reply)) return ZX_ERR_BUFFER_TOO_SMALL;
        *reply = le16toh(descriptor->wTotalLength);
        *out_actual = sizeof(*reply);
        return ZX_OK;
    }
    case IOCTL_USB_GET_CONFIG_DESC: {
        if (in_len != sizeof(int)) return ZX_ERR_INVALID_ARGS;
        int config = *((int *)in_buf);
        usb_configuration_descriptor_t* descriptor = get_config_desc(dev, config);
        if (!descriptor) {
            return ZX_ERR_INVALID_ARGS;
        }
        size_t desc_length = le16toh(descriptor->wTotalLength);
        if (out_len < desc_length) return ZX_ERR_BUFFER_TOO_SMALL;
        memcpy(out_buf, descriptor, desc_length);
        *out_actual = desc_length;
        return ZX_OK;
    }
    case IOCTL_USB_GET_DESCRIPTORS: {
        usb_configuration_descriptor_t* descriptor = dev->config_descs[dev->current_config_index];
        size_t desc_length = le16toh(descriptor->wTotalLength);
        if (out_len < desc_length) return ZX_ERR_BUFFER_TOO_SMALL;
        memcpy(out_buf, descriptor, desc_length);
        *out_actual = desc_length;
        return ZX_OK;
    }
    case IOCTL_USB_GET_STRING_DESC: {
        if (in_len != sizeof(usb_ioctl_get_string_desc_req_t)) return ZX_ERR_INVALID_ARGS;
        if (out_len < sizeof(usb_ioctl_get_string_desc_resp_t)) return ZX_ERR_INVALID_ARGS;

        const usb_ioctl_get_string_desc_req_t* req =
            (const usb_ioctl_get_string_desc_req_t*)(in_buf);
        usb_ioctl_get_string_desc_resp_t* resp = (usb_ioctl_get_string_desc_resp_t*)(out_buf);
        resp->lang_id = req->lang_id;

        const size_t max_space = out_len - sizeof(*resp);
        size_t encoded_len = max_space;

        memset(out_buf, 0, out_len);
        zx_status_t result = usb_util_get_string_descriptor(dev, req->desc_id, &resp->lang_id,
                                                            resp->data, &encoded_len);
        if (result < 0) {
            return result;
        }

        ZX_DEBUG_ASSERT(encoded_len <= UINT16_MAX);
        resp->data_len = (uint16_t)(encoded_len);

        *out_actual = MAX(out_len, sizeof(*resp) + encoded_len);
        return ZX_OK;
    }
    case IOCTL_USB_SET_INTERFACE: {
        if (in_len != 2 * sizeof(int)) return ZX_ERR_INVALID_ARGS;
        int* args = (int *)in_buf;
        return usb_device_set_interface(dev, args[0], args[1]);
    }
    case IOCTL_USB_GET_CURRENT_FRAME: {
        uint64_t* reply = out_buf;
        if (out_len < sizeof(*reply)) return ZX_ERR_BUFFER_TOO_SMALL;
        *reply = usb_hci_get_current_frame(&dev->hci);
        *out_actual = sizeof(*reply);
        return ZX_OK;
    }
    case IOCTL_USB_GET_DEVICE_ID: {
        uint64_t* reply = out_buf;
        if (out_len < sizeof(*reply)) return ZX_ERR_BUFFER_TOO_SMALL;
        *reply = dev->device_id;
        *out_actual = sizeof(*reply);
        return ZX_OK;
    }
    case IOCTL_USB_GET_DEVICE_HUB_ID: {
        uint64_t* reply = out_buf;
        if (out_len < sizeof(*reply)) return ZX_ERR_BUFFER_TOO_SMALL;
        *reply = dev->hub_id;
        *out_actual = sizeof(*reply);
        return ZX_OK;
    }
    case IOCTL_USB_GET_CONFIGURATION: {
        int* reply = out_buf;
        if (out_len != sizeof(*reply)) return ZX_ERR_INVALID_ARGS;
        usb_configuration_descriptor_t* descriptor = dev->config_descs[dev->current_config_index];
        *reply = descriptor->bConfigurationValue;
        *out_actual = sizeof(*reply);
        return ZX_OK;
    }
    case IOCTL_USB_SET_CONFIGURATION: {
        if (in_len != sizeof(int)) return ZX_ERR_INVALID_ARGS;
        int config = *((int *)in_buf);
        zxlogf(TRACE, "IOCTL_USB_SET_CONFIGURATION %d\n", config);
        return usb_device_set_configuration(dev, config);
    }
    default:
        return ZX_ERR_NOT_SUPPORTED;
    }
}

static void usb_device_unbind(void* ctx) {
    usb_device_t* dev = ctx;
    usb_device_remove_interfaces(dev);
    device_remove(dev->zxdev);
}

static void usb_device_release(void* ctx) {
    usb_device_t* dev = ctx;

    stop_callback_thread(dev);

    if (dev->config_descs) {
        int num_configurations = dev->device_desc.bNumConfigurations;
        for (int i = 0; i < num_configurations; i++) {
            if (dev->config_descs[i]) free(dev->config_descs[i]);
        }
        free(dev->config_descs);
    }
    free((void*)dev->lang_ids);
    free(dev->interface_statuses);
    free(dev);
}

static zx_protocol_device_t usb_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .ioctl = usb_device_ioctl,
    .release = usb_device_release,
};

#define NEXT_DESCRIPTOR(header) ((usb_descriptor_header_t*)((void*)header + header->bLength))

static zx_status_t usb_device_add_interfaces(usb_device_t* parent,
                                             usb_configuration_descriptor_t* config) {
    usb_device_descriptor_t* device_desc = &parent->device_desc;
    zx_status_t result = ZX_OK;

    // Iterate through interfaces in first configuration and create devices for them
    usb_descriptor_header_t* header = NEXT_DESCRIPTOR(config);
    usb_descriptor_header_t* end = (usb_descriptor_header_t*)((void*)config + le16toh(config->wTotalLength));

    while (header < end) {
        if (header->bDescriptorType == USB_DT_INTERFACE_ASSOCIATION) {
            usb_interface_assoc_descriptor_t* assoc_desc = (usb_interface_assoc_descriptor_t*)header;
            int interface_count = assoc_desc->bInterfaceCount;

            // find end of this interface association
            usb_descriptor_header_t* next = NEXT_DESCRIPTOR(assoc_desc);
            while (next < end) {
                if (next->bDescriptorType == USB_DT_INTERFACE_ASSOCIATION) {
                    break;
                } else if (next->bDescriptorType == USB_DT_INTERFACE) {
                    usb_interface_descriptor_t* test_intf = (usb_interface_descriptor_t*)next;

                    if (test_intf->bAlternateSetting == 0) {
                        if (interface_count == 0) {
                            break;
                        }
                        interface_count--;
                    }
                }
                next = NEXT_DESCRIPTOR(next);
            }

            size_t length = (void *)next - (void *)assoc_desc;
            usb_interface_assoc_descriptor_t* assoc_copy = malloc(length);
            if (!assoc_copy) return ZX_ERR_NO_MEMORY;
            memcpy(assoc_copy, assoc_desc, length);

            zx_status_t status = usb_device_add_interface_association(parent, device_desc, assoc_copy, length);
            if (status != ZX_OK) {
                result = status;
            }

            header = next;
        } else if (header->bDescriptorType == USB_DT_INTERFACE) {
            usb_interface_descriptor_t* intf_desc = (usb_interface_descriptor_t*)header;
            // find end of current interface descriptor
            usb_descriptor_header_t* next = NEXT_DESCRIPTOR(intf_desc);
            while (next < end) {
                if (next->bDescriptorType == USB_DT_INTERFACE) {
                    usb_interface_descriptor_t* test_intf = (usb_interface_descriptor_t*)next;
                    // Iterate until we find the next top-level interface
                    // Include alternate interfaces in the current interface
                    if (test_intf->bAlternateSetting == 0) {
                        break;
                    }
                }
                next = NEXT_DESCRIPTOR(next);
            }

            // Only create a child device if no child interface has claimed this interface.
            mtx_lock(&parent->interface_mutex);
            interface_status_t intf_status = parent->interface_statuses[intf_desc->bInterfaceNumber];
            mtx_unlock(&parent->interface_mutex);

            size_t length = (void *)next - (void *)intf_desc;
            if (intf_status == AVAILABLE) {
                usb_interface_descriptor_t* intf_copy = malloc(length);
                if (!intf_copy) return ZX_ERR_NO_MEMORY;
                memcpy(intf_copy, intf_desc, length);
                zx_status_t status = usb_device_add_interface(parent, device_desc,
                                                              intf_copy, length);
                if (status != ZX_OK) {
                    result = status;
                }
                // The interface may have been claimed in the meanwhile, so we need to
                // check the interface status again.
                mtx_lock(&parent->interface_mutex);
                if (parent->interface_statuses[intf_desc->bInterfaceNumber] == CLAIMED) {
                    bool removed = usb_device_remove_interface_by_id_locked(parent,
                                                                            intf_desc->bInterfaceNumber);
                    if (!removed) {
                        mtx_unlock(&parent->interface_mutex);
                        return ZX_ERR_BAD_STATE;
                    }
                } else {
                    parent->interface_statuses[intf_desc->bInterfaceNumber] = CHILD_DEVICE;
                }
                mtx_unlock(&parent->interface_mutex);
            }
            header = next;
        } else {
            header = NEXT_DESCRIPTOR(header);
        }
    }

    return result;
}


#define NEXT_DESCRIPTOR(header) ((usb_descriptor_header_t*)((void*)header + header->bLength))


static zx_status_t usb_device_req_alloc(void* ctx, usb_request_t** out, uint64_t data_size,
                                           uint8_t ep_address) {
    usb_device_t* dev = ctx;

    return usb_request_alloc(out, dev->bus->bti_handle, data_size, ep_address);
}

static zx_status_t usb_device_req_alloc_vmo(void* ctx, usb_request_t** out,
                                               zx_handle_t vmo_handle, uint64_t vmo_offset,
                                               uint64_t length, uint8_t ep_address) {
    usb_device_t* dev = ctx;

    return usb_request_alloc_vmo(out, dev->bus->bti_handle, vmo_handle, vmo_offset, length,
                                 ep_address);
}

static zx_status_t usb_device_req_init(void* ctx, usb_request_t* req, zx_handle_t vmo_handle,
                                          uint64_t vmo_offset, uint64_t length,
                                          uint8_t ep_address) {
    usb_device_t* dev = ctx;

    return usb_request_init(req, dev->bus->bti_handle, vmo_handle, vmo_offset, length, ep_address);
}

static ssize_t usb_device_req_copy_from(void* ctx, usb_request_t* req, void* data,
                                          size_t length, size_t offset) {
    return usb_request_copyfrom(req, data, length, offset);
}

static ssize_t usb_device_req_copy_to(void* ctx, usb_request_t* req, const void* data,
                                        size_t length, size_t offset) {
    return usb_request_copyto(req, data, length, offset);
}

static zx_status_t usb_device_req_mmap(void* ctx, usb_request_t* req, void** data) {
    return usb_request_mmap(req, data);
}

static zx_status_t usb_device_req_cacheop(void* ctx, usb_request_t* req, uint32_t op,
                                             size_t offset, size_t length) {
    return usb_request_cacheop(req, op, offset, length);
}

static zx_status_t usb_device_req_cache_flush(void* ctx, usb_request_t* req,
                                                 size_t offset, size_t length) {
    return usb_request_cache_flush(req, offset, length);
}

static zx_status_t usb_device_req_cache_flush_invalidate(void* ctx, usb_request_t* req,
                                                            zx_off_t offset, size_t length) {
    return usb_request_cache_flush_invalidate(req, offset, length);
}

static zx_status_t usb_device_req_physmap(void* ctx, usb_request_t* req) {
    return usb_request_physmap(req);
}

static void usb_device_req_release(void* ctx, usb_request_t* req) {
    usb_request_release(req);
}

static void usb_device_req_complete(void* ctx, usb_request_t* req,
                                       zx_status_t status, zx_off_t actual) {
    usb_request_complete(req, status, actual);
}

static void usb_device_req_phys_iter_init(void* ctx, phys_iter_t* iter, usb_request_t* req,
                                             size_t max_length) {
    usb_request_phys_iter_init(iter, req, max_length);
}

static void usb_control_complete(usb_request_t* req, void* cookie) {
    sync_completion_signal((sync_completion_t*)cookie);
}

zx_status_t usb_device_control(void* ctx, uint8_t request_type, uint8_t request, uint16_t value,
                               uint16_t index, void* data, size_t length, zx_time_t timeout,
                               size_t* out_length) {
    usb_device_t* dev = ctx;

    usb_request_t* req = NULL;
    bool use_free_list = length == 0;
    if (use_free_list) {
        req = usb_request_pool_get(&dev->free_reqs, length);
    }

    if (req == NULL) {
        zx_status_t status = usb_request_alloc(&req, dev->bus->bti_handle, length, 0);
        if (status != ZX_OK) {
            return status;
        }
    }

    // fill in protocol data
    usb_setup_t* setup = &req->setup;
    setup->bmRequestType = request_type;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = length;

    bool out = !!((request_type & USB_DIR_MASK) == USB_DIR_OUT);
    if (length > 0 && out) {
        usb_request_copyto(req, data, length, 0);
    }

    sync_completion_t completion = SYNC_COMPLETION_INIT;

    req->header.device_id = dev->device_id;
    req->header.length = length;
    req->complete_cb = usb_control_complete;
    req->cookie = &completion;
    // We call this directly instead of via hci_queue, as it's safe to call our
    // own completion callback, and prevents clients getting into odd deadlocks.
    usb_hci_request_queue(&dev->hci, req);
    zx_status_t status = sync_completion_wait(&completion, timeout);

    if (status == ZX_OK) {
        status = req->response.status;
    } else if (status == ZX_ERR_TIMED_OUT) {
        // cancel transactions and wait for request to be completed
        sync_completion_reset(&completion);
        status = usb_hci_cancel_all(&dev->hci, dev->device_id, 0);
        if (status == ZX_OK) {
            sync_completion_wait(&completion, ZX_TIME_INFINITE);
            status = ZX_ERR_TIMED_OUT;
        }
    }
    if (status == ZX_OK) {
        if (out_length != NULL) {
            *out_length = req->response.actual;
        }

        if (length > 0 && !out) {
            usb_request_copyfrom(req, data, req->response.actual, 0);
        }
    }

    if (use_free_list) {
        usb_request_pool_add(&dev->free_reqs, req);
    } else {
        usb_request_release(req);
    }
    return status;
}

void usb_device_request_queue(void* ctx, usb_request_t* req) {
    usb_device_t* dev = ctx;

    req->header.device_id = dev->device_id;
    // save the existing callback and cookie, so we can replace them
    // with our own before passing the request to the HCI driver.
    req->saved_complete_cb = req->complete_cb;
    req->saved_cookie = req->cookie;

    req->complete_cb = request_complete;
    // set device as the cookie so we can get at it in request_complete()
    req->cookie = dev;

    usb_hci_request_queue(&dev->hci, req);
}

static usb_speed_t usb_device_get_speed(void* ctx) {
    usb_device_t* dev = ctx;
    return dev->speed;
}

zx_status_t usb_device_set_interface(void* ctx, uint8_t interface_number, uint8_t alt_setting) {
    usb_device_t* dev = ctx;

    mtx_lock(&dev->interface_mutex);
    usb_interface_t* intf;
    list_for_every_entry(&dev->children, intf, usb_interface_t, node) {
        if (usb_interface_contains_interface(intf, interface_number)) {
            mtx_unlock(&dev->interface_mutex);
            return usb_interface_set_alt_setting(intf, interface_number, alt_setting);
        }
    }
    mtx_unlock(&dev->interface_mutex);
    return ZX_ERR_INVALID_ARGS;
}

zx_status_t usb_device_set_configuration(void* ctx, uint8_t config) {
    usb_device_t* dev = ctx;
    int num_configurations = dev->device_desc.bNumConfigurations;
    usb_configuration_descriptor_t* config_desc = NULL;
    int config_index = -1;

    // validate config and get the new current_config_index
    for (int i = 0; i < num_configurations; i++) {
        usb_configuration_descriptor_t* desc = dev->config_descs[i];
        if (desc->bConfigurationValue == config) {
            config_desc = desc;
            config_index = i;
            break;
        }
    }
    if (!config_desc) return ZX_ERR_INVALID_ARGS;

    // set configuration
    zx_status_t status = usb_util_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                          USB_REQ_SET_CONFIGURATION, config, 0, NULL, 0);
    if (status < 0) {
        zxlogf(ERROR, "usb_device_set_configuration: USB_REQ_SET_CONFIGURATION failed\n");
        return status;
    }

    dev->current_config_index = config_index;

    // tear down and recreate the subdevices for our interfaces
    usb_device_remove_interfaces(dev);
    memset(dev->interface_statuses, 0,
           config_desc->bNumInterfaces * sizeof(interface_status_t));
    return usb_device_add_interfaces(dev, config_desc);
}

static zx_status_t usb_device_reset_endpoint(void* ctx, uint8_t ep_address) {
    usb_device_t* dev = ctx;
    return usb_hci_reset_endpoint(&dev->hci, dev->device_id, ep_address);
}

static size_t usb_device_get_max_transfer_size(void* ctx, uint8_t ep_address) {
    usb_device_t* dev = ctx;
    return usb_hci_get_max_transfer_size(&dev->hci, dev->device_id, ep_address);
}

static uint32_t _usb_device_get_device_id(void* ctx) {
    usb_device_t* dev = ctx;
    return dev->device_id;
}

static void usb_device_get_device_descriptor(void* ctx, usb_device_descriptor_t* out_desc) {
    usb_device_t* dev = ctx;
    memcpy(out_desc, &dev->device_desc, sizeof(usb_device_descriptor_t));
}

static zx_status_t usb_device_get_descriptor_list(void* ctx, void** out_descriptors,
                                                  size_t* out_length) {
    usb_device_t* dev = ctx;
    usb_configuration_descriptor_t* config_desc = dev->config_descs[dev->current_config_index];
    size_t length = le16toh(config_desc->wTotalLength);

    void* descriptors = malloc(length);
    if (!descriptors) {
        *out_descriptors = NULL;
        *out_length = 0;
        return ZX_ERR_NO_MEMORY;
    }
    memcpy(descriptors, config_desc, length);
    *out_descriptors = descriptors;
    *out_length = length;
    return ZX_OK;
}

static zx_status_t usb_device_get_additional_descriptor_list(void* ctx, void** out_descriptors,
                                                                size_t* out_length) {
    //???
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t usb_device_get_string_descriptor(void* ctx, uint8_t desc_id, uint16_t* inout_lang_id,
                                             uint8_t* buf, size_t* inout_buflen) {
    usb_device_t* dev = ctx;
    return usb_util_get_string_descriptor(dev, desc_id, inout_lang_id, buf, inout_buflen);
}

static zx_status_t usb_device_claim_device_interface(void* ctx,
                                                     usb_interface_descriptor_t* claim_intf,
                                                     size_t claim_length) {
    //???
    return ZX_ERR_NOT_SUPPORTED;
}

static zx_status_t usb_device_cancel_all(void* ctx, uint8_t ep_address) {
    usb_device_t* dev = ctx;
    return usb_hci_cancel_all(&dev->hci, dev->device_id, ep_address);
}

static usb_protocol_ops_t _usb_protocol = {
    .req_alloc = usb_device_req_alloc,
    .req_alloc_vmo = usb_device_req_alloc_vmo,
    .req_init = usb_device_req_init,
    .req_copy_from = usb_device_req_copy_from,
    .req_copy_to = usb_device_req_copy_to,
    .req_mmap = usb_device_req_mmap,
    .req_cacheop = usb_device_req_cacheop,
    .req_cache_flush = usb_device_req_cache_flush,
    .req_cache_flush_invalidate = usb_device_req_cache_flush_invalidate,
    .req_physmap = usb_device_req_physmap,
    .req_release = usb_device_req_release,
    .req_complete = usb_device_req_complete,
    .req_phys_iter_init = usb_device_req_phys_iter_init,
    .control = usb_device_control,
    .request_queue = usb_device_request_queue,
    .get_speed = usb_device_get_speed,
    .set_interface = usb_device_set_interface,
    .set_configuration = usb_device_set_configuration,
    .reset_endpoint = usb_device_reset_endpoint,
    .get_max_transfer_size = usb_device_get_max_transfer_size,
    .get_device_id = _usb_device_get_device_id,
    .get_device_descriptor = usb_device_get_device_descriptor,
    .get_descriptor_list = usb_device_get_descriptor_list,
    .get_additional_descriptor_list = usb_device_get_additional_descriptor_list,
    .get_string_descriptor = usb_device_get_string_descriptor,
    .claim_interface = usb_device_claim_device_interface,
    .cancel_all = usb_device_cancel_all,
};

zx_status_t usb_device_add(usb_bus_t* bus, uint32_t device_id, uint32_t hub_id,
                           usb_speed_t speed, usb_device_t** out_device) {

    usb_device_t* dev = calloc(1, sizeof(usb_device_t));
    if (!dev) {
        return ZX_ERR_NO_MEMORY;
    }

    // Needed for usb_util_control requests.
    memcpy(&dev->hci, &bus->hci, sizeof(usb_hci_protocol_t));
    dev->bus = bus;
    dev->device_id = device_id;
    mtx_init(&dev->callback_lock, mtx_plain);
    sync_completion_reset(&dev->callback_thread_completion);
    list_initialize(&dev->completed_reqs);
    usb_request_pool_init(&dev->free_reqs);

    // read device descriptor
    usb_device_descriptor_t* device_desc = &dev->device_desc;
    zx_status_t status = usb_util_get_descriptor(dev, USB_DT_DEVICE, 0, 0, device_desc,
                                                 sizeof(*device_desc));
    if (status != sizeof(*device_desc)) {
        zxlogf(ERROR, "usb_device_add: usb_util_get_descriptor failed\n");
        free(dev);
        return status;
    }

    int num_configurations = device_desc->bNumConfigurations;
    usb_configuration_descriptor_t** configs = calloc(num_configurations,
                                                      sizeof(usb_configuration_descriptor_t*));
    if (!configs) {
        status = ZX_ERR_NO_MEMORY;
        goto error_exit;
    }

    for (int config = 0; config < num_configurations; config++) {
        // read configuration descriptor header to determine size
        usb_configuration_descriptor_t config_desc_header;
        status = usb_util_get_descriptor(dev, USB_DT_CONFIG, config, 0, &config_desc_header,
                                         sizeof(config_desc_header));
        if (status != sizeof(config_desc_header)) {
            zxlogf(ERROR, "usb_device_add: usb_util_get_descriptor failed\n");
            goto error_exit;
        }
        uint16_t config_desc_size = letoh16(config_desc_header.wTotalLength);
        usb_configuration_descriptor_t* config_desc = malloc(config_desc_size);
        if (!config_desc) {
            status = ZX_ERR_NO_MEMORY;
            goto error_exit;
        }
        configs[config] = config_desc;

        // read full configuration descriptor
        status = usb_util_get_descriptor(dev, USB_DT_CONFIG, config, 0, config_desc,
                                         config_desc_size);
         if (status != config_desc_size) {
            zxlogf(ERROR, "usb_device_add: usb_util_get_descriptor failed\n");
            goto error_exit;
        }
    }

    // we will create devices for interfaces on the first configuration by default
    uint8_t configuration = 1;
    const usb_config_override_t* override = config_overrides;
    while (override->configuration) {
        if (override->vid == le16toh(device_desc->idVendor) &&
            override->pid == le16toh(device_desc->idProduct)) {
            configuration = override->configuration;
            break;
        }
        override++;
    }
    if (configuration > num_configurations) {
        zxlogf(ERROR, "usb_device_add: override configuration number out of range\n");
        return ZX_ERR_INTERNAL;
    }
    dev->current_config_index = configuration - 1;

    // set configuration
    status = usb_util_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                              USB_REQ_SET_CONFIGURATION,
                              configs[dev->current_config_index]->bConfigurationValue, 0, NULL, 0);
    if (status < 0) {
        zxlogf(ERROR, "usb_device_add: USB_REQ_SET_CONFIGURATION failed\n");
        goto error_exit;
    }

    zxlogf(INFO, "* found USB device (0x%04x:0x%04x, USB %x.%x) config %u\n",
            device_desc->idVendor, device_desc->idProduct, device_desc->bcdUSB >> 8,
            device_desc->bcdUSB & 0xff, configuration);

    list_initialize(&dev->children);
    dev->hci_zxdev = bus->hci_zxdev;
    dev->hub_id = hub_id;
    dev->speed = speed;
    dev->config_descs = configs;

    usb_configuration_descriptor_t* cur_config = configs[dev->current_config_index];

    mtx_init(&dev->interface_mutex, mtx_plain);
    dev->interface_statuses = calloc(cur_config->bNumInterfaces,
                                     sizeof(interface_status_t));
    if (!dev->interface_statuses) {
        status = ZX_ERR_NO_MEMORY;
        goto error_exit;
    }

    // callback thread must be started before device_add() since it will recursively
    // bind other drivers to us before it returns.
    start_callback_thread(dev);

    char name[16];
    snprintf(name, sizeof(name), "%03d", device_id);

    zx_device_prop_t props[] = {
        { BIND_PROTOCOL, 0, ZX_PROTOCOL_USB },
        { BIND_USB_VID, 0, device_desc->idVendor },
        { BIND_USB_PID, 0, device_desc->idProduct },
        { BIND_USB_CLASS, 0, device_desc->bDeviceClass },
        { BIND_USB_SUBCLASS, 0, device_desc->bDeviceSubClass },
        { BIND_USB_PROTOCOL, 0, device_desc->bDeviceProtocol },
    };

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = name,
        .ctx = dev,
        .ops = &usb_device_proto,
        .proto_id = ZX_PROTOCOL_USB,
        .proto_ops = &_usb_protocol,
        .props = props,
        .prop_count = countof(props),
        // Do not allow binding to root of a composite device.
        // Clients will bind to the child interfaces instead.
        .flags = DEVICE_ADD_NON_BINDABLE,
    };

    status = device_add(bus->zxdev, &args, &dev->zxdev);
    if (status == ZX_OK) {
        *out_device = dev;
    } else {
        stop_callback_thread(dev);
        goto error_exit;
    }

    return usb_device_add_interfaces(dev, cur_config);

error_exit:
    if (configs) {
        for (int i = 0; i < num_configurations; i++) {
            if (configs[i]) free(configs[i]);
        }
        free(configs);
    }
    free(dev->interface_statuses);
    free(dev);
    return status;
}
