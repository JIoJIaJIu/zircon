// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/usb/usb.h>
#include <zircon/hw/usb-audio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "usb-composite.h"
#include "usb-interface.h"

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

static zx_status_t usb_device_add_interfaces(usb_device_t* parent,
                                             usb_configuration_descriptor_t* config);

zx_status_t usb_device_set_interface(usb_device_t* device, uint8_t interface_id,
                                     uint8_t alt_setting) {
    mtx_lock(&device->interface_mutex);
    usb_interface_t* intf;
    list_for_every_entry(&device->children, intf, usb_interface_t, node) {
        if (usb_interface_contains_interface(intf, interface_id)) {
            mtx_unlock(&device->interface_mutex);
            return usb_interface_set_alt_setting(intf, interface_id, alt_setting);
        }
    }
    mtx_unlock(&device->interface_mutex);
    return ZX_ERR_INVALID_ARGS;
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
    zx_status_t status = usb_control(&dev->usb, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                     USB_REQ_SET_CONFIGURATION, config, 0, NULL, 0, ZX_TIME_INFINITE, NULL);
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

static void usb_device_unbind(void* ctx) {
    usb_device_t* dev = ctx;
    usb_device_remove_interfaces(dev);
    device_remove(dev->zxdev);
}

static void usb_device_release(void* ctx) {
    usb_device_t* dev = ctx;

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

static zx_status_t usb_composite_bind(void* ctx, zx_device_t* parent) {
    usb_protocol_t usb;
    zx_status_t status = device_get_protocol(parent, ZX_PROTOCOL_USB_DEVICE, &usb);
    if (status != ZX_OK) {
        return status;
    }

    usb_device_t* dev = calloc(1, sizeof(usb_device_t));
    if (!dev) {
        return ZX_ERR_NO_MEMORY;
    }
    memcpy(&dev->usb, &usb, sizeof(dev->usb));

    dev->device_id = usb_get_device_id(&usb);

    usb_device_descriptor_t device_desc;
    usb_get_device_descriptor(&usb, &device_desc);
    memcpy(&dev->device_desc, &device_desc, sizeof(dev->device_desc));

    int num_configurations = device_desc.bNumConfigurations;
    usb_configuration_descriptor_t** configs = calloc(num_configurations,
                                                      sizeof(usb_configuration_descriptor_t*));
    if (!configs) {
        status = ZX_ERR_NO_MEMORY;
        goto error_exit;
    }

    for (int config = 0; config < num_configurations; config++) {
        // read configuration descriptor header to determine size
        usb_configuration_descriptor_t config_desc_header;
        size_t actual;
        status = usb_get_descriptor(&usb, USB_DT_CONFIG, config, 0, &config_desc_header,
                                    sizeof(config_desc_header), ZX_TIME_INFINITE, &actual);
        if (status != ZX_OK) {
            zxlogf(ERROR, "usb_device_add: usb_get_descriptor failed\n");
            goto error_exit;
        }
        if (actual != sizeof(config_desc_header)) {
            status = ZX_ERR_IO;
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
        status = usb_get_descriptor(&usb, USB_DT_CONFIG, config, 0, config_desc, config_desc_size,
                                    ZX_TIME_INFINITE, &actual);
         if (status != ZX_OK) {
            zxlogf(ERROR, "usb_device_add: usb_get_descriptor failed\n");
            goto error_exit;
        }
        if (actual != config_desc_size) {
            status = ZX_ERR_IO;
            goto error_exit;
        }
    }

    // we will create devices for interfaces on the first configuration by default
    uint8_t configuration = 1;
    const usb_config_override_t* override = config_overrides;
    while (override->configuration) {
        if (override->vid == le16toh(device_desc.idVendor) &&
            override->pid == le16toh(device_desc.idProduct)) {
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
    status = usb_set_configuration(&usb, configs[dev->current_config_index]->bConfigurationValue);
    if (status < 0) {
        zxlogf(ERROR, "usb_device_add: usb_set_configuration failed\n");
        goto error_exit;
    }

    zxlogf(INFO, "* found USB device (0x%04x:0x%04x, USB %x.%x) config %u\n",
            device_desc.idVendor, device_desc.idProduct, device_desc.bcdUSB >> 8,
            device_desc.bcdUSB & 0xff, configuration);

    list_initialize(&dev->children);
    dev->config_descs = configs;

    usb_configuration_descriptor_t* cur_config = configs[dev->current_config_index];

    mtx_init(&dev->interface_mutex, mtx_plain);
    dev->interface_statuses = calloc(cur_config->bNumInterfaces,
                                     sizeof(interface_status_t));
    if (!dev->interface_statuses) {
        status = ZX_ERR_NO_MEMORY;
        goto error_exit;
    }

    char name[16];
    snprintf(name, sizeof(name), "%03d", dev->device_id);

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = name,
        .ctx = dev,
        .ops = &usb_device_proto,
        .flags = DEVICE_ADD_NON_BINDABLE,
    };

    status = device_add(parent, &args, &dev->zxdev);
    if (status == ZX_OK) {
        return usb_device_add_interfaces(dev, cur_config);
    }

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

static zx_driver_ops_t usb_composite_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = usb_composite_bind,
};

ZIRCON_DRIVER_BEGIN(usb_composite, usb_composite_driver_ops, "zircon", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_USB_DEVICE),
ZIRCON_DRIVER_END(usb_composite)
