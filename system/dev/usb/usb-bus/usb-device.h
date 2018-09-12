// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/protocol/usb-hci.h>
#include <ddk/protocol/usb-hub.h>
#include <ddk/usb-request/usb-request.h>
#include <lib/sync/completion.h>
#include <zircon/hw/usb.h>

#include <threads.h>
#include <stdatomic.h>

typedef enum {
    // The interface has not been claimed and no device has been created for it.
    AVAILABLE,
    // Another interface has claimed the interface.
    CLAIMED,
    // A child device has been created for the interface.
    CHILD_DEVICE
} interface_status_t;

typedef struct usb_bus usb_bus_t;

// Represents a USB top-level device
typedef struct usb_device {
    zx_device_t* zxdev;
    zx_device_t* hci_zxdev;
    usb_hci_protocol_t hci;
    usb_bus_t* bus;

    // ID assigned by host controller
    uint32_t device_id;
    // device_id of the hub we are attached to (or zero for root hub)
    uint32_t hub_id;
    usb_speed_t speed;

    // true if device is a hub
    bool isHub;
    // Interface to talk to the hub driver
    usb_hub_interface_t hub_intf;

    usb_device_descriptor_t device_desc;
    usb_configuration_descriptor_t** config_descs;
    int current_config_index;

    atomic_bool langids_fetched;
    atomic_uintptr_t lang_ids;

    mtx_t interface_mutex;
    // Array storing whether interfaces from 0 to bNumInterfaces-1
    // are available, claimed or is a child device.
    interface_status_t* interface_statuses;
    // list of child devices (for USB composite devices)
    list_node_t children;

    list_node_t node;

    // thread for calling client's usb request complete callback
    thrd_t callback_thread;
    bool callback_thread_stop;
    // completion used for signalling callback_thread
    sync_completion_t callback_thread_completion;
    // list of requests that need to have client's completion callback called
    list_node_t completed_reqs;
    // mutex that protects the callback_* members above
    mtx_t callback_lock;

    // pool of requests that can be reused
    usb_request_pool_t free_reqs;
} usb_device_t;

zx_status_t usb_device_add(usb_bus_t* bus, uint32_t device_id, uint32_t hub_id,
                           usb_speed_t speed, usb_device_t** out_device);

void usb_device_remove(usb_device_t* dev);
void usb_device_set_hub_interface(usb_device_t* dev, usb_hub_interface_t* hub_intf);

// Marks the interface as claimed, removing the device if it exists.
// Returns an error if the interface was already claimed by another interface.
zx_status_t usb_device_claim_interface(usb_device_t* dev, uint8_t interface_id);

// device protocol functions shared with usb_interface_t
zx_status_t usb_device_control(void* ctx, uint8_t request_type, uint8_t request, uint16_t value,
                               uint16_t index, void* data, size_t length, zx_time_t timeout,
                               size_t* out_length);
void usb_device_request_queue(void* ctx, usb_request_t* usb_request);
zx_status_t usb_device_set_interface(void* ctx, uint8_t interface_id, uint8_t alt_setting);
zx_status_t usb_device_set_configuration(void* ctx, uint8_t configuration);

