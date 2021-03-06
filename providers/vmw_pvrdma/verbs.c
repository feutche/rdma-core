/*
 * Copyright (c) 2012-2016 VMware, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of EITHER the GNU General Public License
 * version 2 as published by the Free Software Foundation or the BSD
 * 2-Clause License. This program is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; WITHOUT EVEN THE IMPLIED
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License version 2 for more details at
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program available in the file COPYING in the main
 * directory of this source tree.
 *
 * The BSD 2-Clause License
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "pvrdma.h"

int pvrdma_query_device(struct ibv_context *context,
			struct ibv_device_attr *attr)
{
	struct ibv_query_device cmd;
	uint64_t raw_fw_ver;
	unsigned major, minor, sub_minor;
	int ret;

	ret = ibv_cmd_query_device(context, attr, &raw_fw_ver,
				   &cmd, sizeof(cmd));
	if (ret)
		return ret;

	major = (raw_fw_ver >> 32) & 0xffff;
	minor = (raw_fw_ver >> 16) & 0xffff;
	sub_minor = raw_fw_ver & 0xffff;

	snprintf(attr->fw_ver, sizeof(attr->fw_ver),
		 "%d.%d.%03d", major, minor, sub_minor);

	return 0;
}

int pvrdma_query_port(struct ibv_context *context, uint8_t port,
		      struct ibv_port_attr *attr)
{
	struct ibv_query_port cmd;

	return ibv_cmd_query_port(context, port, attr, &cmd, sizeof(cmd));
}

struct ibv_pd *pvrdma_alloc_pd(struct ibv_context *context)
{
	struct ibv_alloc_pd cmd;
	struct user_pvrdma_alloc_pd_resp resp;
	struct pvrdma_pd *pd;

	pd = malloc(sizeof(*pd));
	if (!pd)
		return NULL;

	if (ibv_cmd_alloc_pd(context, &pd->ibv_pd, &cmd, sizeof(cmd),
			     &resp.ibv_resp, sizeof(resp))) {
		free(pd);
		return NULL;
	}

	pd->pdn = resp.udata.pdn;

	return &pd->ibv_pd;
}

int pvrdma_free_pd(struct ibv_pd *pd)
{
	int ret;

	ret = ibv_cmd_dealloc_pd(pd);
	if (ret)
		return ret;

	free(to_vpd(pd));

	return 0;
}

struct ibv_mr *pvrdma_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
			     int access)
{
	struct ibv_mr *mr;
	struct ibv_reg_mr cmd;
	struct ibv_reg_mr_resp resp;
	int ret;

	mr = malloc(sizeof(*mr));
	if (!mr)
		return NULL;

	ret = ibv_cmd_reg_mr(pd, addr, length, (uintptr_t) addr,
			     access, mr, &cmd, sizeof(cmd),
			     &resp, sizeof(resp));
	if (ret) {
		free(mr);
		return NULL;
	}

	return mr;
}

int pvrdma_dereg_mr(struct ibv_mr *mr)
{
	int ret;

	ret = ibv_cmd_dereg_mr(mr);
	if (ret)
		return ret;

	free(mr);

	return 0;
}

static int is_multicast_gid(const union ibv_gid *gid)
{
	return gid->raw[0] == 0xff;
}

static int is_link_local_gid(const union ibv_gid *gid)
{
	uint32_t *hi = (uint32_t *)(gid->raw);
	uint32_t *lo = (uint32_t *)(gid->raw + 4);
	if (hi[0] == htonl(0xfe800000) && lo[0] == 0)
		return 1;

	return 0;
}

static int is_ipv6_addr_v4mapped(const struct in6_addr *a)
{
	return IN6_IS_ADDR_V4MAPPED(&a->s6_addr32) ||
		/* IPv4 encoded multicast addresses */
		(a->s6_addr32[0] == htonl(0xff0e0000) &&
		((a->s6_addr32[1] |
		 (a->s6_addr32[2] ^ htonl(0x0000ffff))) == 0UL));
}

static void set_mac_from_gid(const union ibv_gid *gid,
			     __u8 mac[6])
{
	if (is_link_local_gid(gid)) {
		/*
		 * The MAC is embedded in GID[8-10,13-15] with the
		 * 7th most significant bit inverted.
		 */
		memcpy(mac, gid->raw + 8, 3);
		memcpy(mac + 3, gid->raw + 13, 3);
		mac[0] ^= 2;
	}
}

struct ibv_ah *pvrdma_create_ah(struct ibv_pd *pd,
				struct ibv_ah_attr *attr)
{
	struct pvrdma_ah *ah;
	struct pvrdma_av *av;
	struct ibv_port_attr port_attr;

	if (!attr->is_global)
		return NULL;

	if (ibv_query_port(pd->context, attr->port_num, &port_attr))
		return NULL;

	if (port_attr.link_layer == IBV_LINK_LAYER_UNSPECIFIED ||
	    port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND)
		return NULL;

	if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET &&
	    (!is_link_local_gid(&attr->grh.dgid) &&
	     !is_multicast_gid(&attr->grh.dgid)  &&
	     !is_ipv6_addr_v4mapped((struct in6_addr *)attr->grh.dgid.raw)))
		return NULL;

	ah = calloc(1, sizeof(*ah));
	if (!ah)
		return NULL;

	av = &ah->av;
	av->port_pd = to_vpd(pd)->pdn | (attr->port_num << 24);
	av->src_path_bits = attr->src_path_bits;
	av->src_path_bits |= 0x80;
	av->gid_index = attr->grh.sgid_index;
	av->hop_limit = attr->grh.hop_limit;
	av->sl_tclass_flowlabel = (attr->grh.traffic_class << 20) |
				   attr->grh.flow_label;
	memcpy(av->dgid, attr->grh.dgid.raw, 16);
	set_mac_from_gid(&attr->grh.dgid, av->dmac);

	return &ah->ibv_ah;
}

int pvrdma_destroy_ah(struct ibv_ah *ah)
{
	free(to_vah(ah));

	return 0;
}
