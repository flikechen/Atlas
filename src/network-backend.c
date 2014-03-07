/* $%BEGINLICENSE%$
 Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation; version 2 of the
 License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA

 $%ENDLICENSE%$ */

#include <stdlib.h> 
#include <string.h>

#include <glib.h>

#include "network-mysqld-packet.h"
#include "network-backend.h"
#include "chassis-plugin.h"
#include "glib-ext.h"

#define C(x) x, sizeof(x) - 1
#define S(x) x->str, x->len

network_backend_t *network_backend_new(guint event_thread_count) {
	network_backend_t *b;

	b = g_new0(network_backend_t, 1);

//	b->pool = network_connection_pool_new();
	b->pools = g_ptr_array_new();
	guint i;
	for (i = 0; i < event_thread_count; ++i) {
		network_connection_pool* pool = network_connection_pool_new();
		g_ptr_array_add(b->pools, pool);
	}

	b->uuid = g_string_new(NULL);
	b->addr = network_address_new();

	return b;
}

void network_backend_free(network_backend_t *b) {
	if (!b) return;

	guint i;
	for (i = 0; i < b->pools->len; ++i) {
		network_connection_pool* pool = g_ptr_array_index(b->pools, i);
		network_connection_pool_free(pool);
	}
	g_ptr_array_free(b->pools, TRUE);

	if (b->addr)     network_address_free(b->addr);
	if (b->uuid)     g_string_free(b->uuid, TRUE);

	g_free(b);
}

network_backends_t *network_backends_new(guint event_thread_count) {
	network_backends_t *bs;

	bs = g_new0(network_backends_t, 1);

	bs->backends = g_ptr_array_new();
	bs->backends_mutex = g_mutex_new();	/*remove lock*/
	bs->global_wrr = g_wrr_poll_new();
	bs->event_thread_count = event_thread_count;

	return bs;
}

g_wrr_poll *g_wrr_poll_new() {
    g_wrr_poll *global_wrr;

    global_wrr = g_new0(g_wrr_poll, 1);

    global_wrr->max_weight = 0;
    global_wrr->cur_weight = 0;
    global_wrr->next_ndx = 0;
    
    return global_wrr;
}

void g_wrr_poll_free(g_wrr_poll *global_wrr) {
    g_free(global_wrr);
}

void network_backends_free(network_backends_t *bs) {
	gsize i;

	if (!bs) return;

	g_mutex_lock(bs->backends_mutex);	/*remove lock*/
	for (i = 0; i < bs->backends->len; i++) {
		network_backend_t *backend = bs->backends->pdata[i];
		
		network_backend_free(backend);
	}
	g_mutex_unlock(bs->backends_mutex);	/*remove lock*/

	g_ptr_array_free(bs->backends, TRUE);
	g_mutex_free(bs->backends_mutex);	/*remove lock*/

	g_wrr_poll_free(bs->global_wrr);
	g_free(bs);
}

int network_backends_remove(network_backends_t *bs, guint index) {
	network_backend_t* b = bs->backends->pdata[index];
	if (b != NULL) {
		if (b->addr) network_address_free(b->addr);
		if (b->uuid) g_string_free(b->uuid, TRUE);
		g_mutex_lock(bs->backends_mutex);
		g_ptr_array_remove_index(bs->backends, index);
		g_mutex_unlock(bs->backends_mutex);
	}
	return 0;
}

/*
 * FIXME: 1) remove _set_address, make this function callable with result of same
 *        2) differentiate between reasons for "we didn't add" (now -1 in all cases)
 */
int network_backends_add(network_backends_t *bs, /* const */ gchar *address, backend_type_t type) {
	network_backend_t *new_backend;
	guint i;

	new_backend = network_backend_new(bs->event_thread_count);
	new_backend->type = type;

	if (type == BACKEND_TYPE_RO) {
		guint weight = 1;
		gchar *p = strrchr(address, '@');
		if (p != NULL) {
			*p = '\0';
			weight = atoi(p+1);
		}
		new_backend->weight = weight;
	}

	if (0 != network_address_set_address(new_backend->addr, address)) {
		network_backend_free(new_backend);
		return -1;
	}

	/* check if this backend is already known */
	g_mutex_lock(bs->backends_mutex);	/*remove lock*/
	gint first_slave = -1;
	for (i = 0; i < bs->backends->len; i++) {
		network_backend_t *old_backend = bs->backends->pdata[i];

		if (first_slave == -1 && old_backend->type == BACKEND_TYPE_RO) first_slave = i;

		if (strleq(S(old_backend->addr->name), S(new_backend->addr->name)) && type == old_backend->type) {
			network_backend_free(new_backend);

			g_mutex_unlock(bs->backends_mutex);	/*remove lock*/
			g_critical("backend %s is already known!", address);
			return -1;
		}
	}

	g_ptr_array_add(bs->backends, new_backend);
	if (first_slave != -1 && type == BACKEND_TYPE_RW) {
		network_backend_t *temp_backend = bs->backends->pdata[first_slave];
		bs->backends->pdata[first_slave] = bs->backends->pdata[bs->backends->len - 1];
		bs->backends->pdata[bs->backends->len - 1] = temp_backend;
	}
	g_mutex_unlock(bs->backends_mutex);	/*remove lock*/

	g_message("added %s backend: %s", (type == BACKEND_TYPE_RW) ?
			"read/write" : "read-only", address);

	return 0;
}

network_backend_t *network_backends_get(network_backends_t *bs, guint ndx) {
	if (ndx >= network_backends_count(bs)) return NULL;

	/* FIXME: shouldn't we copy the backend or add ref-counting ? */	
	return bs->backends->pdata[ndx];
}

guint network_backends_count(network_backends_t *bs) {
	guint len;

	g_mutex_lock(bs->backends_mutex);	/*remove lock*/
	len = bs->backends->len;
	g_mutex_unlock(bs->backends_mutex);	/*remove lock*/

	return len;
}

