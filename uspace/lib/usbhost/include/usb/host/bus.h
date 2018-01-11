/*
 * Copyright (c) 2017 Ondrej Hlavaty <aearsis@eideo.cz>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/** @addtogroup libusbhost
 * @{
 */
/** @file
 * Virtual base for usb bus implementations.
 *
 * The purpose of this structure is to keep information about connected devices
 * and endpoints, manage available bandwidth and the toggle bit flipping.
 *
 * The generic implementation is provided for USB 1 and 2 in usb2_bus.c. Some
 * details in [OUE]HCI are solved through overriding some functions. XHCI does
 * not need the bookkeeping functionality, because addresses are managed by HC
 * itself.
 */
#ifndef LIBUSBHOST_HOST_BUS_H
#define LIBUSBHOST_HOST_BUS_H

#include <assert.h>
#include <fibril_synch.h>
#include <stdbool.h>
#include <usb/host/hcd.h>
#include <usb/request.h>
#include <usb/usb.h>
#include <usbhc_iface.h>

typedef struct hcd hcd_t;
typedef struct endpoint endpoint_t;
typedef struct bus bus_t;
typedef struct ddf_fun ddf_fun_t;
typedef struct usb_transfer_batch usb_transfer_batch_t;

typedef struct device {
	/* Device tree keeping */
	link_t link;
	list_t devices;
	fibril_mutex_t guard;

	/* Associated DDF function, if any */
	ddf_fun_t *fun;

	/* Invalid for the roothub device */
	unsigned port;
	struct device *hub;

	/* Transaction translator */
	usb_tt_address_t tt;

	/* The following are not set by the library */
	usb_speed_t speed;
	usb_address_t address;
	endpoint_t *endpoints [USB_ENDPOINT_MAX];

	/* Managing bus */
	bus_t *bus;

	/** True if the device can add new endpoints and schedule transfers. */
	volatile bool online;

	/* This structure is meant to be extended by overriding. */
} device_t;

typedef struct bus_ops bus_ops_t;

/**
 * Operations structure serving as an interface of hc driver for the library
 * (and the rest of the system).
 */
struct bus_ops {
	/* Undefined operations will be delegated to parent ops */
	const bus_ops_t *parent;

	/* Global operations on the bus */
	void (*interrupt)(bus_t *, uint32_t);
	int (*status)(bus_t *, uint32_t *);

	/* Operations on device */
	int (*device_enumerate)(device_t *);
	void (*device_remove)(device_t *);
	int (*device_online)(device_t *);			/**< Optional */
	int (*device_offline)(device_t *);			/**< Optional */
	endpoint_t *(*endpoint_create)(device_t *, const usb_endpoint_descriptors_t *);

	/* Operations on endpoint */
	int (*endpoint_register)(endpoint_t *);
	void (*endpoint_unregister)(endpoint_t *);
	void (*endpoint_destroy)(endpoint_t *);			/**< Optional */
	void (*endpoint_toggle_reset)(endpoint_t *);		/**< Optional */
	ssize_t (*endpoint_count_bw) (endpoint_t *, size_t);	/**< Optional */
	usb_transfer_batch_t *(*batch_create)(endpoint_t *);	/**< Optional */

	/* Operations on batch */
	int (*batch_schedule)(usb_transfer_batch_t *);
	void (*batch_destroy)(usb_transfer_batch_t *);		/**< Optional */
};

/**
 * Use this macro to lookup virtual function.
 */
#define BUS_OPS_LOOKUP(start, fn) ({ bus_ops_t const * ops = (start); while (ops && ops->fn == NULL) ops = ops->parent; ops; })

/** Endpoint management structure */
typedef struct bus {
	/* Synchronization of ops */
	fibril_mutex_t guard;

	/* Size of the device_t extended structure */
	size_t device_size;

	/* Do not call directly, ops are synchronized. */
	const bus_ops_t *ops;

	/* Reserving default address - USB_SPEED_MAX when free. */
	usb_speed_t default_address_speed;

	/* This structure is meant to be extended by overriding. */
} bus_t;

void bus_init(bus_t *, size_t);
int bus_device_init(device_t *, bus_t *);

int bus_device_set_default_name(device_t *);

int bus_device_enumerate(device_t *);
void bus_device_remove(device_t *);

int bus_device_online(device_t *);
int bus_device_offline(device_t *);

int bus_device_send_batch(device_t *, usb_target_t,
    usb_direction_t direction, char *, size_t, uint64_t,
    usbhc_iface_transfer_callback_t, void *, const char *);

ssize_t bus_device_send_batch_sync(device_t *, usb_target_t,
    usb_direction_t direction, char *, size_t, uint64_t,
    const char *);

int bus_endpoint_add(device_t *, const usb_endpoint_descriptors_t *, endpoint_t **);
endpoint_t *bus_find_endpoint(device_t *, usb_endpoint_t);
int bus_endpoint_remove(endpoint_t *);

int bus_reserve_default_address(bus_t *, usb_speed_t);
void bus_release_default_address(bus_t *);

#endif
/**
 * @}
 */
