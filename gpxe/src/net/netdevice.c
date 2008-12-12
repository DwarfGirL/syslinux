/*
 * Copyright (C) 2006 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <byteswap.h>
#include <string.h>
#include <errno.h>
#include <gpxe/if_ether.h>
#include <gpxe/iobuf.h>
#include <gpxe/tables.h>
#include <gpxe/process.h>
#include <gpxe/init.h>
#include <gpxe/device.h>
#include <gpxe/netdevice.h>

/** @file
 *
 * Network device management
 *
 */

/** Registered network-layer protocols */
static struct net_protocol net_protocols[0]
	__table_start ( struct net_protocol, net_protocols );
static struct net_protocol net_protocols_end[0]
	__table_end ( struct net_protocol, net_protocols );

/** List of network devices */
struct list_head net_devices = LIST_HEAD_INIT ( net_devices );

/**
 * Transmit raw packet via network device
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer
 * @ret rc		Return status code
 *
 * Transmits the packet via the specified network device.  This
 * function takes ownership of the I/O buffer.
 */
int netdev_tx ( struct net_device *netdev, struct io_buffer *iobuf ) {
	int rc;

	DBGC ( netdev, "NETDEV %p transmitting %p (%p+%zx)\n",
	       netdev, iobuf, iobuf->data, iob_len ( iobuf ) );

	list_add_tail ( &iobuf->list, &netdev->tx_queue );

	if ( ! ( netdev->state & NETDEV_OPEN ) ) {
		rc = -ENETUNREACH;
		goto err;
	}
		
	if ( ( rc = netdev->op->transmit ( netdev, iobuf ) ) != 0 )
		goto err;

	return 0;

 err:
	netdev_tx_complete_err ( netdev, iobuf, rc );
	return rc;
}

/**
 * Complete network transmission
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer
 * @v rc		Packet status code
 *
 * The packet must currently be in the network device's TX queue.
 */
void netdev_tx_complete_err ( struct net_device *netdev,
			      struct io_buffer *iobuf, int rc ) {

	/* Update statistics counter */
	if ( rc == 0 ) {
		netdev->stats.tx_ok++;
		DBGC ( netdev, "NETDEV %p transmission %p complete\n",
		       netdev, iobuf );
	} else {
		netdev->stats.tx_err++;
		DBGC ( netdev, "NETDEV %p transmission %p failed: %s\n",
		       netdev, iobuf, strerror ( rc ) );
	}

	/* Catch data corruption as early as possible */
	assert ( iobuf->list.next != NULL );
	assert ( iobuf->list.prev != NULL );

	/* Dequeue and free I/O buffer */
	list_del ( &iobuf->list );
	free_iob ( iobuf );
}

/**
 * Complete network transmission
 *
 * @v netdev		Network device
 * @v rc		Packet status code
 *
 * Completes the oldest outstanding packet in the TX queue.
 */
void netdev_tx_complete_next_err ( struct net_device *netdev, int rc ) {
	struct io_buffer *iobuf;

	list_for_each_entry ( iobuf, &netdev->tx_queue, list ) {
		netdev_tx_complete_err ( netdev, iobuf, rc );
		return;
	}
}

/**
 * Flush device's transmit queue
 *
 * @v netdev		Network device
 */
static void netdev_tx_flush ( struct net_device *netdev ) {

	/* Discard any packets in the TX queue */
	while ( ! list_empty ( &netdev->tx_queue ) ) {
		netdev_tx_complete_next_err ( netdev, -ECANCELED );
	}
}

/**
 * Add packet to receive queue
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer, or NULL
 *
 * The packet is added to the network device's RX queue.  This
 * function takes ownership of the I/O buffer.
 */
void netdev_rx ( struct net_device *netdev, struct io_buffer *iobuf ) {

	DBGC ( netdev, "NETDEV %p received %p (%p+%zx)\n",
	       netdev, iobuf, iobuf->data, iob_len ( iobuf ) );

	/* Enqueue packet */
	list_add_tail ( &iobuf->list, &netdev->rx_queue );

	/* Update statistics counter */
	netdev->stats.rx_ok++;
}

/**
 * Discard received packet
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer, or NULL
 * @v rc		Packet status code
 *
 * The packet is discarded and an RX error is recorded.  This function
 * takes ownership of the I/O buffer.  @c iobuf may be NULL if, for
 * example, the net device wishes to report an error due to being
 * unable to allocate an I/O buffer.
 */
void netdev_rx_err ( struct net_device *netdev,
		     struct io_buffer *iobuf, int rc ) {

	DBGC ( netdev, "NETDEV %p failed to receive %p: %s\n",
	       netdev, iobuf, strerror ( rc ) );

	/* Discard packet */
	free_iob ( iobuf );

	/* Update statistics counter */
	netdev->stats.rx_err++;
}

/**
 * Poll for completed and received packets on network device
 *
 * @v netdev		Network device
 *
 * Polls the network device for completed transmissions and received
 * packets.  Any received packets will be added to the RX packet queue
 * via netdev_rx().
 */
void netdev_poll ( struct net_device *netdev ) {

	if ( netdev->state & NETDEV_OPEN )
		netdev->op->poll ( netdev );
}

/**
 * Remove packet from device's receive queue
 *
 * @v netdev		Network device
 * @ret iobuf		I/O buffer, or NULL
 *
 * Removes the first packet from the device's RX queue and returns it.
 * Ownership of the packet is transferred to the caller.
 */
struct io_buffer * netdev_rx_dequeue ( struct net_device *netdev ) {
	struct io_buffer *iobuf;

	list_for_each_entry ( iobuf, &netdev->rx_queue, list ) {
		list_del ( &iobuf->list );
		return iobuf;
	}
	return NULL;
}

/**
 * Flush device's receive queue
 *
 * @v netdev		Network device
 */
static void netdev_rx_flush ( struct net_device *netdev ) {
	struct io_buffer *iobuf;

	/* Discard any packets in the RX queue */
	while ( ( iobuf = netdev_rx_dequeue ( netdev ) ) ) {
		netdev_rx_err ( netdev, iobuf, -ECANCELED );
	}
}

/**
 * Free network device
 *
 * @v refcnt		Network device reference counter
 */
static void free_netdev ( struct refcnt *refcnt ) {
	struct net_device *netdev =
		container_of ( refcnt, struct net_device, refcnt );
	
	netdev_tx_flush ( netdev );
	netdev_rx_flush ( netdev );
	free ( netdev );
}

/**
 * Allocate network device
 *
 * @v priv_size		Size of private data area (net_device::priv)
 * @ret netdev		Network device, or NULL
 *
 * Allocates space for a network device and its private data area.
 */
struct net_device * alloc_netdev ( size_t priv_size ) {
	struct net_device *netdev;
	size_t total_len;

	total_len = ( sizeof ( *netdev ) + priv_size );
	netdev = zalloc ( total_len );
	if ( netdev ) {
		netdev->refcnt.free = free_netdev;
		INIT_LIST_HEAD ( &netdev->tx_queue );
		INIT_LIST_HEAD ( &netdev->rx_queue );
		settings_init ( netdev_settings ( netdev ),
				&netdev_settings_operations, &netdev->refcnt,
				netdev->name );
		netdev->priv = ( ( ( void * ) netdev ) + sizeof ( *netdev ) );
	}
	return netdev;
}

/**
 * Register network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 *
 * Gives the network device a name and adds it to the list of network
 * devices.
 */
int register_netdev ( struct net_device *netdev ) {
	static unsigned int ifindex = 0;
	int rc;

	/* Create device name */
	snprintf ( netdev->name, sizeof ( netdev->name ), "net%d",
		   ifindex++ );

	/* Register per-netdev configuration settings */
	if ( ( rc = register_settings ( netdev_settings ( netdev ),
					NULL ) ) != 0 ) {
		DBGC ( netdev, "NETDEV %p could not register settings: %s\n",
		       netdev, strerror ( rc ) );
		return rc;
	}

	/* Add to device list */
	netdev_get ( netdev );
	list_add_tail ( &netdev->list, &net_devices );
	DBGC ( netdev, "NETDEV %p registered as %s (phys %s hwaddr %s)\n",
	       netdev, netdev->name, netdev->dev->name,
	       netdev_hwaddr ( netdev ) );

	return 0;
}

/**
 * Open network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
int netdev_open ( struct net_device *netdev ) {
	int rc;

	/* Do nothing if device is already open */
	if ( netdev->state & NETDEV_OPEN )
		return 0;

	DBGC ( netdev, "NETDEV %p opening\n", netdev );

	/* Open the device */
	if ( ( rc = netdev->op->open ( netdev ) ) != 0 )
		return rc;

	/* Mark as opened */
	netdev->state |= NETDEV_OPEN;
	return 0;
}

/**
 * Close network device
 *
 * @v netdev		Network device
 */
void netdev_close ( struct net_device *netdev ) {

	/* Do nothing if device is already closed */
	if ( ! ( netdev->state & NETDEV_OPEN ) )
		return;

	DBGC ( netdev, "NETDEV %p closing\n", netdev );

	/* Close the device */
	netdev->op->close ( netdev );

	/* Flush TX and RX queues */
	netdev_tx_flush ( netdev );
	netdev_rx_flush ( netdev );

	/* Mark as closed */
	netdev->state &= ~NETDEV_OPEN;
}

/**
 * Unregister network device
 *
 * @v netdev		Network device
 *
 * Removes the network device from the list of network devices.
 */
void unregister_netdev ( struct net_device *netdev ) {

	/* Ensure device is closed */
	netdev_close ( netdev );

	/* Unregister per-netdev configuration settings */
	unregister_settings ( netdev_settings ( netdev ) );

	/* Remove from device list */
	list_del ( &netdev->list );
	netdev_put ( netdev );
	DBGC ( netdev, "NETDEV %p unregistered\n", netdev );
}

/** Enable or disable interrupts
 *
 * @v netdev		Network device
 * @v enable		Interrupts should be enabled
 */
void netdev_irq ( struct net_device *netdev, int enable ) {
	netdev->op->irq ( netdev, enable );
}

/**
 * Get network device by name
 *
 * @v name		Network device name
 * @ret netdev		Network device, or NULL
 */
struct net_device * find_netdev ( const char *name ) {
	struct net_device *netdev;

	list_for_each_entry ( netdev, &net_devices, list ) {
		if ( strcmp ( netdev->name, name ) == 0 )
			return netdev;
	}

	return NULL;
}

/**
 * Get network device by PCI bus:dev.fn address
 *
 * @v bus_type		Bus type
 * @v location		Bus location
 * @ret netdev		Network device, or NULL
 */
struct net_device * find_netdev_by_location ( unsigned int bus_type,
					      unsigned int location ) {
	struct net_device *netdev;

	list_for_each_entry ( netdev, &net_devices, list ) {
		if ( ( netdev->dev->desc.bus_type == bus_type ) &&
		     ( netdev->dev->desc.location == location ) )
			return netdev;
	}

	return NULL;	
}

/**
 * Transmit network-layer packet
 *
 * @v iobuf		I/O buffer
 * @v netdev		Network device
 * @v net_protocol	Network-layer protocol
 * @v ll_dest		Destination link-layer address
 * @ret rc		Return status code
 *
 * Prepends link-layer headers to the I/O buffer and transmits the
 * packet via the specified network device.  This function takes
 * ownership of the I/O buffer.
 */
int net_tx ( struct io_buffer *iobuf, struct net_device *netdev,
	     struct net_protocol *net_protocol, const void *ll_dest ) {
	int rc;

	/* Force a poll on the netdevice to (potentially) clear any
	 * backed-up TX completions.  This is needed on some network
	 * devices to avoid excessive losses due to small TX ring
	 * sizes.
	 */
	netdev_poll ( netdev );

	/* Add link-layer header */
	if ( ( rc = netdev->ll_protocol->push ( iobuf, netdev, net_protocol,
						ll_dest ) ) != 0 ) {
		free_iob ( iobuf );
		return rc;
	}

	/* Transmit packet */
	return netdev_tx ( netdev, iobuf );
}

/**
 * Process received network-layer packet
 *
 * @v iobuf		I/O buffer
 * @v netdev		Network device
 * @v net_proto		Network-layer protocol, in network-byte order
 * @v ll_source		Source link-layer address
 * @ret rc		Return status code
 */
int net_rx ( struct io_buffer *iobuf, struct net_device *netdev,
	     uint16_t net_proto, const void *ll_source ) {
	struct net_protocol *net_protocol;

	/* Hand off to network-layer protocol, if any */
	for ( net_protocol = net_protocols ; net_protocol < net_protocols_end ;
	      net_protocol++ ) {
		if ( net_protocol->net_proto == net_proto ) {
			return net_protocol->rx ( iobuf, netdev, ll_source );
		}
	}
	free_iob ( iobuf );
	return 0;
}

/**
 * Single-step the network stack
 *
 * @v process		Network stack process
 *
 * This polls all interfaces for received packets, and processes
 * packets from the RX queue.
 */
static void net_step ( struct process *process __unused ) {
	struct net_device *netdev;
	struct io_buffer *iobuf;
	struct ll_protocol *ll_protocol;
	uint16_t net_proto;
	const void *ll_source;
	int rc;

	/* Poll and process each network device */
	list_for_each_entry ( netdev, &net_devices, list ) {

		/* Poll for new packets */
		netdev_poll ( netdev );

		/* Process at most one received packet.  Give priority
		 * to getting packets out of the NIC over processing
		 * the received packets, because we advertise a window
		 * that assumes that we can receive packets from the
		 * NIC faster than they arrive.
		 */
		if ( ( iobuf = netdev_rx_dequeue ( netdev ) ) ) {

			DBGC ( netdev, "NETDEV %p processing %p (%p+%zx)\n",
			       netdev, iobuf, iobuf->data,
			       iob_len ( iobuf ) );

			/* Remove link-layer header */
			ll_protocol = netdev->ll_protocol;
			if ( ( rc = ll_protocol->pull ( iobuf, netdev,
							&net_proto,
							&ll_source ) ) != 0 ) {
				free_iob ( iobuf );
				continue;
			}

			net_rx ( iobuf, netdev, net_proto, ll_source );
		}
	}
}

/** Networking stack process */
struct process net_process __permanent_process = {
	.step = net_step,
};
