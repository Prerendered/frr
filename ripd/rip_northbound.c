/*
 * Copyright (C) 1997, 1998, 1999 Kunihiro Ishiguro <kunihiro@zebra.org>
 * Copyright (C) 2018  NetDEF, Inc.
 *                     Renato Westphal
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include "if.h"
#include "vrf.h"
#include "log.h"
#include "prefix.h"
#include "table.h"
#include "command.h"
#include "routemap.h"
#include "northbound.h"
#include "libfrr.h"

#include "ripd/ripd.h"
#include "ripd/rip_cli.h"

/*
 * XPath: /frr-ripd:ripd/instance
 */
static int ripd_instance_create(enum nb_event event,
				const struct lyd_node *dnode,
				union nb_resource *resource)
{
	int socket;

	switch (event) {
	case NB_EV_VALIDATE:
		break;
	case NB_EV_PREPARE:
		socket = rip_create_socket();
		if (socket < 0)
			return NB_ERR_RESOURCE;
		resource->fd = socket;
		break;
	case NB_EV_ABORT:
		socket = resource->fd;
		close(socket);
		break;
	case NB_EV_APPLY:
		socket = resource->fd;
		rip_create(socket);
		break;
	}

	return NB_OK;
}

static int ripd_instance_delete(enum nb_event event,
				const struct lyd_node *dnode)
{
	if (event != NB_EV_APPLY)
		return NB_OK;

	rip_clean();

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/allow-ecmp
 */
static int ripd_instance_allow_ecmp_modify(enum nb_event event,
					   const struct lyd_node *dnode,
					   union nb_resource *resource)
{
	if (event != NB_EV_APPLY)
		return NB_OK;

	rip->ecmp = yang_dnode_get_bool(dnode, NULL);
	if (!rip->ecmp)
		rip_ecmp_disable();

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/default-information-originate
 */
static int
ripd_instance_default_information_originate_modify(enum nb_event event,
						   const struct lyd_node *dnode,
						   union nb_resource *resource)
{
	bool default_information;
	struct prefix_ipv4 p;

	if (event != NB_EV_APPLY)
		return NB_OK;

	default_information = yang_dnode_get_bool(dnode, NULL);

	memset(&p, 0, sizeof(struct prefix_ipv4));
	p.family = AF_INET;
	if (default_information) {
		struct nexthop nh;

		memset(&nh, 0, sizeof(nh));
		nh.type = NEXTHOP_TYPE_IPV4;
		rip_redistribute_add(ZEBRA_ROUTE_RIP, RIP_ROUTE_DEFAULT, &p,
				     &nh, 0, 0, 0);
	} else {
		rip_redistribute_delete(ZEBRA_ROUTE_RIP, RIP_ROUTE_DEFAULT, &p,
					0);
	}

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/default-metric
 */
static int ripd_instance_default_metric_modify(enum nb_event event,
					       const struct lyd_node *dnode,
					       union nb_resource *resource)
{
	if (event != NB_EV_APPLY)
		return NB_OK;

	rip->default_metric = yang_dnode_get_uint8(dnode, NULL);
	/* rip_update_default_metric (); */

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/distance/default
 */
static int ripd_instance_distance_default_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	if (event != NB_EV_APPLY)
		return NB_OK;

	rip->distance = yang_dnode_get_uint8(dnode, NULL);

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/distance/source
 */
static int ripd_instance_distance_source_create(enum nb_event event,
						const struct lyd_node *dnode,
						union nb_resource *resource)
{
	struct prefix_ipv4 prefix;
	struct route_node *rn;

	if (event != NB_EV_APPLY)
		return NB_OK;

	yang_dnode_get_ipv4p(&prefix, dnode, "./prefix");

	/* Get RIP distance node. */
	rn = route_node_get(rip_distance_table, (struct prefix *)&prefix);
	rn->info = rip_distance_new();
	yang_dnode_set_entry(dnode, rn);

	return NB_OK;
}

static int ripd_instance_distance_source_delete(enum nb_event event,
						const struct lyd_node *dnode)
{
	struct route_node *rn;
	struct rip_distance *rdistance;

	if (event != NB_EV_APPLY)
		return NB_OK;

	rn = yang_dnode_get_entry(dnode);
	rdistance = rn->info;
	if (rdistance->access_list)
		free(rdistance->access_list);
	rip_distance_free(rdistance);

	rn->info = NULL;
	route_unlock_node(rn);

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/distance/source/distance
 */
static int
ripd_instance_distance_source_distance_modify(enum nb_event event,
					      const struct lyd_node *dnode,
					      union nb_resource *resource)
{
	struct route_node *rn;
	uint8_t distance;
	struct rip_distance *rdistance;

	if (event != NB_EV_APPLY)
		return NB_OK;

	/* Set distance value. */
	rn = yang_dnode_get_entry(dnode);
	distance = yang_dnode_get_uint8(dnode, NULL);
	rdistance = rn->info;
	rdistance->distance = distance;

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/distance/source/access-list
 */
static int
ripd_instance_distance_source_access_list_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	const char *acl_name;
	struct route_node *rn;
	struct rip_distance *rdistance;

	if (event != NB_EV_APPLY)
		return NB_OK;

	acl_name = yang_dnode_get_string(dnode, NULL);

	/* Set access-list */
	rn = yang_dnode_get_entry(dnode);
	rdistance = rn->info;
	if (rdistance->access_list)
		free(rdistance->access_list);
	rdistance->access_list = strdup(acl_name);

	return NB_OK;
}

static int
ripd_instance_distance_source_access_list_delete(enum nb_event event,
						 const struct lyd_node *dnode)
{
	struct route_node *rn;
	struct rip_distance *rdistance;

	if (event != NB_EV_APPLY)
		return NB_OK;

	/* Reset access-list configuration. */
	rn = yang_dnode_get_entry(dnode);
	rdistance = rn->info;
	free(rdistance->access_list);
	rdistance->access_list = NULL;

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/explicit-neighbor
 */
static int ripd_instance_explicit_neighbor_create(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	struct prefix_ipv4 p;

	if (event != NB_EV_APPLY)
		return NB_OK;

	p.family = AF_INET;
	p.prefixlen = IPV4_MAX_BITLEN;
	yang_dnode_get_ipv4(&p.prefix, dnode, NULL);

	return rip_neighbor_add(&p);
}

static int ripd_instance_explicit_neighbor_delete(enum nb_event event,
						  const struct lyd_node *dnode)
{
	struct prefix_ipv4 p;

	if (event != NB_EV_APPLY)
		return NB_OK;

	p.family = AF_INET;
	p.prefixlen = IPV4_MAX_BITLEN;
	yang_dnode_get_ipv4(&p.prefix, dnode, NULL);

	return rip_neighbor_delete(&p);
}

/*
 * XPath: /frr-ripd:ripd/instance/network
 */
static int ripd_instance_network_create(enum nb_event event,
					const struct lyd_node *dnode,
					union nb_resource *resource)
{
	struct prefix p;

	if (event != NB_EV_APPLY)
		return NB_OK;

	yang_dnode_get_ipv4p(&p, dnode, NULL);

	return rip_enable_network_add(&p);
}

static int ripd_instance_network_delete(enum nb_event event,
					const struct lyd_node *dnode)
{
	struct prefix p;

	if (event != NB_EV_APPLY)
		return NB_OK;

	yang_dnode_get_ipv4p(&p, dnode, NULL);

	return rip_enable_network_delete(&p);
}

/*
 * XPath: /frr-ripd:ripd/instance/interface
 */
static int ripd_instance_interface_create(enum nb_event event,
					  const struct lyd_node *dnode,
					  union nb_resource *resource)
{
	const char *ifname;

	if (event != NB_EV_APPLY)
		return NB_OK;

	ifname = yang_dnode_get_string(dnode, NULL);

	return rip_enable_if_add(ifname);
}

static int ripd_instance_interface_delete(enum nb_event event,
					  const struct lyd_node *dnode)
{
	const char *ifname;

	if (event != NB_EV_APPLY)
		return NB_OK;

	ifname = yang_dnode_get_string(dnode, NULL);

	return rip_enable_if_delete(ifname);
}

/*
 * XPath: /frr-ripd:ripd/instance/offset-list
 */
static int ripd_instance_offset_list_create(enum nb_event event,
					    const struct lyd_node *dnode,
					    union nb_resource *resource)
{
	const char *ifname;
	struct rip_offset_list *offset;

	if (event != NB_EV_APPLY)
		return NB_OK;

	ifname = yang_dnode_get_string(dnode, "./interface");

	offset = rip_offset_list_new(ifname);
	yang_dnode_set_entry(dnode, offset);

	return NB_OK;
}

static int ripd_instance_offset_list_delete(enum nb_event event,
					    const struct lyd_node *dnode)
{
	int direct;
	struct rip_offset_list *offset;

	if (event != NB_EV_APPLY)
		return NB_OK;

	direct = yang_dnode_get_enum(dnode, "./direction");

	offset = yang_dnode_get_entry(dnode);
	if (offset->direct[direct].alist_name) {
		free(offset->direct[direct].alist_name);
		offset->direct[direct].alist_name = NULL;
	}
	if (offset->direct[RIP_OFFSET_LIST_IN].alist_name == NULL
	    && offset->direct[RIP_OFFSET_LIST_OUT].alist_name == NULL)
		offset_list_del(offset);

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/offset-list/access-list
 */
static int
ripd_instance_offset_list_access_list_modify(enum nb_event event,
					     const struct lyd_node *dnode,
					     union nb_resource *resource)
{
	int direct;
	struct rip_offset_list *offset;
	const char *alist_name;

	if (event != NB_EV_APPLY)
		return NB_OK;

	direct = yang_dnode_get_enum(dnode, "../direction");
	alist_name = yang_dnode_get_string(dnode, NULL);

	offset = yang_dnode_get_entry(dnode);
	if (offset->direct[direct].alist_name)
		free(offset->direct[direct].alist_name);
	offset->direct[direct].alist_name = strdup(alist_name);

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/offset-list/metric
 */
static int ripd_instance_offset_list_metric_modify(enum nb_event event,
						   const struct lyd_node *dnode,
						   union nb_resource *resource)
{
	int direct;
	uint8_t metric;
	struct rip_offset_list *offset;

	if (event != NB_EV_APPLY)
		return NB_OK;

	direct = yang_dnode_get_enum(dnode, "../direction");
	metric = yang_dnode_get_uint8(dnode, NULL);

	offset = yang_dnode_get_entry(dnode);
	offset->direct[direct].metric = metric;

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/passive-default
 */
static int ripd_instance_passive_default_modify(enum nb_event event,
						const struct lyd_node *dnode,
						union nb_resource *resource)
{
	if (event != NB_EV_APPLY)
		return NB_OK;

	rip->passive_default = yang_dnode_get_bool(dnode, NULL);
	rip_passive_nondefault_clean();

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/passive-interface
 */
static int ripd_instance_passive_interface_create(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	const char *ifname;

	if (event != NB_EV_APPLY)
		return NB_OK;

	ifname = yang_dnode_get_string(dnode, NULL);

	return rip_passive_nondefault_set(ifname);
}

static int ripd_instance_passive_interface_delete(enum nb_event event,
						  const struct lyd_node *dnode)
{
	const char *ifname;

	if (event != NB_EV_APPLY)
		return NB_OK;

	ifname = yang_dnode_get_string(dnode, NULL);

	return rip_passive_nondefault_unset(ifname);
}

/*
 * XPath: /frr-ripd:ripd/instance/non-passive-interface
 */
static int
ripd_instance_non_passive_interface_create(enum nb_event event,
					   const struct lyd_node *dnode,
					   union nb_resource *resource)
{
	const char *ifname;

	if (event != NB_EV_APPLY)
		return NB_OK;

	ifname = yang_dnode_get_string(dnode, NULL);

	return rip_passive_nondefault_unset(ifname);
}

static int
ripd_instance_non_passive_interface_delete(enum nb_event event,
					   const struct lyd_node *dnode)
{
	const char *ifname;

	if (event != NB_EV_APPLY)
		return NB_OK;

	ifname = yang_dnode_get_string(dnode, NULL);

	return rip_passive_nondefault_set(ifname);
}

/*
 * XPath: /frr-ripd:ripd/instance/redistribute
 */
static int ripd_instance_redistribute_create(enum nb_event event,
					     const struct lyd_node *dnode,
					     union nb_resource *resource)
{
	return NB_OK;
}

static int ripd_instance_redistribute_delete(enum nb_event event,
					     const struct lyd_node *dnode)
{
	int type;

	if (event != NB_EV_APPLY)
		return NB_OK;

	type = yang_dnode_get_enum(dnode, "./protocol");

	rip_redistribute_conf_delete(type);

	return NB_OK;
}

static void
ripd_instance_redistribute_apply_finish(const struct lyd_node *dnode)
{
	int type;

	type = yang_dnode_get_enum(dnode, "./protocol");
	rip_redistribute_conf_update(type);
}

/*
 * XPath: /frr-ripd:ripd/instance/redistribute/route-map
 */
static int
ripd_instance_redistribute_route_map_modify(enum nb_event event,
					    const struct lyd_node *dnode,
					    union nb_resource *resource)
{
	int type;
	const char *rmap_name;

	if (event != NB_EV_APPLY)
		return NB_OK;

	type = yang_dnode_get_enum(dnode, "../protocol");
	rmap_name = yang_dnode_get_string(dnode, NULL);

	if (rip->route_map[type].name)
		free(rip->route_map[type].name);
	rip->route_map[type].name = strdup(rmap_name);
	rip->route_map[type].map = route_map_lookup_by_name(rmap_name);

	return NB_OK;
}

static int
ripd_instance_redistribute_route_map_delete(enum nb_event event,
					    const struct lyd_node *dnode)
{
	int type;

	if (event != NB_EV_APPLY)
		return NB_OK;

	type = yang_dnode_get_enum(dnode, "../protocol");

	if (rip->route_map[type].name) {
		free(rip->route_map[type].name);
		rip->route_map[type].name = NULL;
	}

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/redistribute/metric
 */
static int
ripd_instance_redistribute_metric_modify(enum nb_event event,
					 const struct lyd_node *dnode,
					 union nb_resource *resource)
{
	int type;
	uint8_t metric;

	if (event != NB_EV_APPLY)
		return NB_OK;

	type = yang_dnode_get_enum(dnode, "../protocol");
	metric = yang_dnode_get_uint8(dnode, NULL);

	rip->route_map[type].metric_config = true;
	rip->route_map[type].metric = metric;

	return NB_OK;
}

static int
ripd_instance_redistribute_metric_delete(enum nb_event event,
					 const struct lyd_node *dnode)
{
	int type;

	if (event != NB_EV_APPLY)
		return NB_OK;

	type = yang_dnode_get_enum(dnode, "../protocol");

	rip->route_map[type].metric_config = false;
	rip->route_map[type].metric = 0;

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/static-route
 */
static int ripd_instance_static_route_create(enum nb_event event,
					     const struct lyd_node *dnode,
					     union nb_resource *resource)
{
	struct nexthop nh;
	struct prefix_ipv4 p;

	if (event != NB_EV_APPLY)
		return NB_OK;

	yang_dnode_get_ipv4p(&p, dnode, NULL);

	memset(&nh, 0, sizeof(nh));
	nh.type = NEXTHOP_TYPE_IPV4;
	rip_redistribute_add(ZEBRA_ROUTE_RIP, RIP_ROUTE_STATIC, &p, &nh, 0, 0,
			     0);

	return NB_OK;
}

static int ripd_instance_static_route_delete(enum nb_event event,
					     const struct lyd_node *dnode)
{
	struct prefix_ipv4 p;

	if (event != NB_EV_APPLY)
		return NB_OK;

	yang_dnode_get_ipv4p(&p, dnode, NULL);

	rip_redistribute_delete(ZEBRA_ROUTE_RIP, RIP_ROUTE_STATIC, &p, 0);

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/timers/
 */
static void ripd_instance_timers_apply_finish(const struct lyd_node *dnode)
{
	/* Reset update timer thread. */
	rip_event(RIP_UPDATE_EVENT, 0);
}

/*
 * XPath: /frr-ripd:ripd/instance/timers/flush-interval
 */
static int
ripd_instance_timers_flush_interval_modify(enum nb_event event,
					   const struct lyd_node *dnode,
					   union nb_resource *resource)
{
	if (event != NB_EV_APPLY)
		return NB_OK;

	rip->garbage_time = yang_dnode_get_uint32(dnode, NULL);

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/timers/holddown-interval
 */
static int
ripd_instance_timers_holddown_interval_modify(enum nb_event event,
					      const struct lyd_node *dnode,
					      union nb_resource *resource)
{
	if (event != NB_EV_APPLY)
		return NB_OK;

	rip->timeout_time = yang_dnode_get_uint32(dnode, NULL);

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/timers/update-interval
 */
static int
ripd_instance_timers_update_interval_modify(enum nb_event event,
					    const struct lyd_node *dnode,
					    union nb_resource *resource)
{
	if (event != NB_EV_APPLY)
		return NB_OK;

	rip->update_time = yang_dnode_get_uint32(dnode, NULL);

	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/version/receive
 */
static int ripd_instance_version_receive_modify(enum nb_event event,
						const struct lyd_node *dnode,
						union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/instance/version/send
 */
static int ripd_instance_version_send_modify(enum nb_event event,
					     const struct lyd_node *dnode,
					     union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-ripd:rip/split-horizon
 */
static int lib_interface_rip_split_horizon_modify(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-ripd:rip/v2-broadcast
 */
static int lib_interface_rip_v2_broadcast_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-ripd:rip/version-receive
 */
static int
lib_interface_rip_version_receive_modify(enum nb_event event,
					 const struct lyd_node *dnode,
					 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-ripd:rip/version-send
 */
static int lib_interface_rip_version_send_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-ripd:rip/authentication-scheme/mode
 */
static int lib_interface_rip_authentication_scheme_mode_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath:
 * /frr-interface:lib/interface/frr-ripd:rip/authentication-scheme/md5-auth-length
 */
static int lib_interface_rip_authentication_scheme_md5_auth_length_modify(
	enum nb_event event, const struct lyd_node *dnode,
	union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int lib_interface_rip_authentication_scheme_md5_auth_length_delete(
	enum nb_event event, const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-ripd:rip/authentication-password
 */
static int
lib_interface_rip_authentication_password_modify(enum nb_event event,
						 const struct lyd_node *dnode,
						 union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
lib_interface_rip_authentication_password_delete(enum nb_event event,
						 const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-ripd:rip/authentication-key-chain
 */
static int
lib_interface_rip_authentication_key_chain_modify(enum nb_event event,
						  const struct lyd_node *dnode,
						  union nb_resource *resource)
{
	/* TODO: implement me. */
	return NB_OK;
}

static int
lib_interface_rip_authentication_key_chain_delete(enum nb_event event,
						  const struct lyd_node *dnode)
{
	/* TODO: implement me. */
	return NB_OK;
}

/*
 * XPath: /frr-ripd:ripd/state/neighbors/neighbor
 */
static const void *
ripd_state_neighbors_neighbor_get_next(const char *xpath,
				       const void *list_entry)
{
	/* TODO: implement me. */
	return NULL;
}

static int ripd_state_neighbors_neighbor_get_keys(const void *list_entry,
						  struct yang_list_keys *keys)
{
	/* TODO: implement me. */
	return NB_OK;
}

static const void *
ripd_state_neighbors_neighbor_lookup_entry(const struct yang_list_keys *keys)
{
	/* TODO: implement me. */
	return NULL;
}

/*
 * XPath: /frr-ripd:ripd/state/neighbors/neighbor/address
 */
static struct yang_data *
ripd_state_neighbors_neighbor_address_get_elem(const char *xpath,
					       const void *list_entry)
{
	/* TODO: implement me. */
	return NULL;
}

/*
 * XPath: /frr-ripd:ripd/state/neighbors/neighbor/last-update
 */
static struct yang_data *
ripd_state_neighbors_neighbor_last_update_get_elem(const char *xpath,
						   const void *list_entry)
{
	/* TODO: implement me. */
	return NULL;
}

/*
 * XPath: /frr-ripd:ripd/state/neighbors/neighbor/bad-packets-rcvd
 */
static struct yang_data *
ripd_state_neighbors_neighbor_bad_packets_rcvd_get_elem(const char *xpath,
							const void *list_entry)
{
	/* TODO: implement me. */
	return NULL;
}

/*
 * XPath: /frr-ripd:ripd/state/neighbors/neighbor/bad-routes-rcvd
 */
static struct yang_data *
ripd_state_neighbors_neighbor_bad_routes_rcvd_get_elem(const char *xpath,
						       const void *list_entry)
{
	/* TODO: implement me. */
	return NULL;
}

/*
 * XPath: /frr-ripd:ripd/state/routes/route
 */
static const void *ripd_state_routes_route_get_next(const char *xpath,
						    const void *list_entry)
{
	/* TODO: implement me. */
	return NULL;
}

static int ripd_state_routes_route_get_keys(const void *list_entry,
					    struct yang_list_keys *keys)
{
	/* TODO: implement me. */
	return NB_OK;
}

static const void *
ripd_state_routes_route_lookup_entry(const struct yang_list_keys *keys)
{
	/* TODO: implement me. */
	return NULL;
}

/*
 * XPath: /frr-ripd:ripd/state/routes/route/prefix
 */
static struct yang_data *
ripd_state_routes_route_prefix_get_elem(const char *xpath,
					const void *list_entry)
{
	/* TODO: implement me. */
	return NULL;
}

/*
 * XPath: /frr-ripd:ripd/state/routes/route/next-hop
 */
static struct yang_data *
ripd_state_routes_route_next_hop_get_elem(const char *xpath,
					  const void *list_entry)
{
	/* TODO: implement me. */
	return NULL;
}

/*
 * XPath: /frr-ripd:ripd/state/routes/route/interface
 */
static struct yang_data *
ripd_state_routes_route_interface_get_elem(const char *xpath,
					   const void *list_entry)
{
	/* TODO: implement me. */
	return NULL;
}

/*
 * XPath: /frr-ripd:ripd/state/routes/route/metric
 */
static struct yang_data *
ripd_state_routes_route_metric_get_elem(const char *xpath,
					const void *list_entry)
{
	/* TODO: implement me. */
	return NULL;
}

/*
 * XPath: /frr-ripd:clear-rip-route
 */
static int clear_rip_route_rpc(const char *xpath, const struct list *input,
			       struct list *output)
{
	/* TODO: implement me. */
	return NB_OK;
}

/* clang-format off */
const struct frr_yang_module_info frr_ripd_info = {
	.name = "frr-ripd",
	.nodes = {
		{
			.xpath = "/frr-ripd:ripd/instance",
			.cbs.create = ripd_instance_create,
			.cbs.delete = ripd_instance_delete,
			.cbs.cli_show = cli_show_router_rip,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/allow-ecmp",
			.cbs.modify = ripd_instance_allow_ecmp_modify,
			.cbs.cli_show = cli_show_rip_allow_ecmp,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/default-information-originate",
			.cbs.modify = ripd_instance_default_information_originate_modify,
			.cbs.cli_show = cli_show_rip_default_information_originate,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/default-metric",
			.cbs.modify = ripd_instance_default_metric_modify,
			.cbs.cli_show = cli_show_rip_default_metric,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/distance/default",
			.cbs.modify = ripd_instance_distance_default_modify,
			.cbs.cli_show = cli_show_rip_distance,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/distance/source",
			.cbs.create = ripd_instance_distance_source_create,
			.cbs.delete = ripd_instance_distance_source_delete,
			.cbs.cli_show = cli_show_rip_distance_source,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/distance/source/distance",
			.cbs.modify = ripd_instance_distance_source_distance_modify,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/distance/source/access-list",
			.cbs.modify = ripd_instance_distance_source_access_list_modify,
			.cbs.delete = ripd_instance_distance_source_access_list_delete,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/explicit-neighbor",
			.cbs.create = ripd_instance_explicit_neighbor_create,
			.cbs.delete = ripd_instance_explicit_neighbor_delete,
			.cbs.cli_show = cli_show_rip_neighbor,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/network",
			.cbs.create = ripd_instance_network_create,
			.cbs.delete = ripd_instance_network_delete,
			.cbs.cli_show = cli_show_rip_network_prefix,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/interface",
			.cbs.create = ripd_instance_interface_create,
			.cbs.delete = ripd_instance_interface_delete,
			.cbs.cli_show = cli_show_rip_network_interface,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/offset-list",
			.cbs.create = ripd_instance_offset_list_create,
			.cbs.delete = ripd_instance_offset_list_delete,
			.cbs.cli_show = cli_show_rip_offset_list,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/offset-list/access-list",
			.cbs.modify = ripd_instance_offset_list_access_list_modify,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/offset-list/metric",
			.cbs.modify = ripd_instance_offset_list_metric_modify,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/passive-default",
			.cbs.modify = ripd_instance_passive_default_modify,
			.cbs.cli_show = cli_show_rip_passive_default,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/passive-interface",
			.cbs.create = ripd_instance_passive_interface_create,
			.cbs.delete = ripd_instance_passive_interface_delete,
			.cbs.cli_show = cli_show_rip_passive_interface,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/non-passive-interface",
			.cbs.create = ripd_instance_non_passive_interface_create,
			.cbs.delete = ripd_instance_non_passive_interface_delete,
			.cbs.cli_show = cli_show_rip_non_passive_interface,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/redistribute",
			.cbs.create = ripd_instance_redistribute_create,
			.cbs.delete = ripd_instance_redistribute_delete,
			.cbs.apply_finish = ripd_instance_redistribute_apply_finish,
			.cbs.cli_show = cli_show_rip_redistribute,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/redistribute/route-map",
			.cbs.modify = ripd_instance_redistribute_route_map_modify,
			.cbs.delete = ripd_instance_redistribute_route_map_delete,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/redistribute/metric",
			.cbs.modify = ripd_instance_redistribute_metric_modify,
			.cbs.delete = ripd_instance_redistribute_metric_delete,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/static-route",
			.cbs.create = ripd_instance_static_route_create,
			.cbs.delete = ripd_instance_static_route_delete,
			.cbs.cli_show = cli_show_rip_route,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/timers",
			.cbs.apply_finish = ripd_instance_timers_apply_finish,
			.cbs.cli_show = cli_show_rip_timers,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/timers/flush-interval",
			.cbs.modify = ripd_instance_timers_flush_interval_modify,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/timers/holddown-interval",
			.cbs.modify = ripd_instance_timers_holddown_interval_modify,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/timers/update-interval",
			.cbs.modify = ripd_instance_timers_update_interval_modify,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/version/receive",
			.cbs.modify = ripd_instance_version_receive_modify,
		},
		{
			.xpath = "/frr-ripd:ripd/instance/version/send",
			.cbs.modify = ripd_instance_version_send_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-ripd:rip/split-horizon",
			.cbs.modify = lib_interface_rip_split_horizon_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-ripd:rip/v2-broadcast",
			.cbs.modify = lib_interface_rip_v2_broadcast_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-ripd:rip/version-receive",
			.cbs.modify = lib_interface_rip_version_receive_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-ripd:rip/version-send",
			.cbs.modify = lib_interface_rip_version_send_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-ripd:rip/authentication-scheme/mode",
			.cbs.modify = lib_interface_rip_authentication_scheme_mode_modify,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-ripd:rip/authentication-scheme/md5-auth-length",
			.cbs.modify = lib_interface_rip_authentication_scheme_md5_auth_length_modify,
			.cbs.delete = lib_interface_rip_authentication_scheme_md5_auth_length_delete,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-ripd:rip/authentication-password",
			.cbs.modify = lib_interface_rip_authentication_password_modify,
			.cbs.delete = lib_interface_rip_authentication_password_delete,
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-ripd:rip/authentication-key-chain",
			.cbs.modify = lib_interface_rip_authentication_key_chain_modify,
			.cbs.delete = lib_interface_rip_authentication_key_chain_delete,
		},
		{
			.xpath = "/frr-ripd:ripd/state/neighbors/neighbor",
			.cbs.get_next = ripd_state_neighbors_neighbor_get_next,
			.cbs.get_keys = ripd_state_neighbors_neighbor_get_keys,
			.cbs.lookup_entry = ripd_state_neighbors_neighbor_lookup_entry,
		},
		{
			.xpath = "/frr-ripd:ripd/state/neighbors/neighbor/address",
			.cbs.get_elem = ripd_state_neighbors_neighbor_address_get_elem,
		},
		{
			.xpath = "/frr-ripd:ripd/state/neighbors/neighbor/last-update",
			.cbs.get_elem = ripd_state_neighbors_neighbor_last_update_get_elem,
		},
		{
			.xpath = "/frr-ripd:ripd/state/neighbors/neighbor/bad-packets-rcvd",
			.cbs.get_elem = ripd_state_neighbors_neighbor_bad_packets_rcvd_get_elem,
		},
		{
			.xpath = "/frr-ripd:ripd/state/neighbors/neighbor/bad-routes-rcvd",
			.cbs.get_elem = ripd_state_neighbors_neighbor_bad_routes_rcvd_get_elem,
		},
		{
			.xpath = "/frr-ripd:ripd/state/routes/route",
			.cbs.get_next = ripd_state_routes_route_get_next,
			.cbs.get_keys = ripd_state_routes_route_get_keys,
			.cbs.lookup_entry = ripd_state_routes_route_lookup_entry,
		},
		{
			.xpath = "/frr-ripd:ripd/state/routes/route/prefix",
			.cbs.get_elem = ripd_state_routes_route_prefix_get_elem,
		},
		{
			.xpath = "/frr-ripd:ripd/state/routes/route/next-hop",
			.cbs.get_elem = ripd_state_routes_route_next_hop_get_elem,
		},
		{
			.xpath = "/frr-ripd:ripd/state/routes/route/interface",
			.cbs.get_elem = ripd_state_routes_route_interface_get_elem,
		},
		{
			.xpath = "/frr-ripd:ripd/state/routes/route/metric",
			.cbs.get_elem = ripd_state_routes_route_metric_get_elem,
		},
		{
			.xpath = "/frr-ripd:clear-rip-route",
			.cbs.rpc = clear_rip_route_rpc,
		},
		{
			.xpath = NULL,
		},
	}
};
