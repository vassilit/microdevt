/*
 * microdevt - Microcontroller Development Toolkit
 *
 * Copyright (c) 2017, Krzysztof Witek
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "LICENSE".
 *
*/

#include <sys/utils.h>
#include <sys/chksum.h>
#include "tr-chksum.h"
#include "ip.h"
#include "icmp.h"
#include "arp.h"
#include "eth.h"
#include "route.h"
#include "udp.h"
#include "tcp.h"

int ip_output(pkt_t *out, iface_t *iface, uint16_t flags)
{
	ip_hdr_t *ip = btod(out);
	uint32_t ip_dst;
	uint32_t *mask;
	uint32_t *ip_addr;
	uint16_t payload_len = pkt_len(out);

	if (iface == NULL)
		iface = dft_route.iface;

	if (iface == NULL) {
		/* no interface to send the pkt to */
		pkt_free(out);
		return -1;
	}
	mask = (uint32_t *)iface->ip4_mask;
	ip_addr = (uint32_t *)iface->ip4_addr;

	/* XXX check for buf_adj coherency with other layers */
	if (ip->dst == 0) {
		/* no dest ip address set. Drop the packet */
		pkt_free(out);
		return -1;
	}

	ip->src = *ip_addr;
	ip->v = 4;
	ip->hl = sizeof(ip_hdr_t) / 4;
	ip->tos = 0;
	ip->len = htons(payload_len);
	ip->id = 0;
	ip->off = flags;
	ip->ttl = CONFIG_IP_TTL;
	assert(ip->p); /* must be set by upper layer */
	ip->chksum = 0;
	ip->chksum = cksum(ip, sizeof(ip_hdr_t));

	if ((ip->dst & *mask) != (*ip_addr & *mask))
		ip_dst = dft_route.ip;
	else
		ip_dst = ip->dst;

	pkt_adj(out, (int)sizeof(ip_hdr_t));
	if (ip->p == IPPROTO_UDP) {
		udp_hdr_t *udp_hdr = btod(out);
		set_transport_cksum(ip, udp_hdr, udp_hdr->length);
	} else if (ip->p == IPPROTO_TCP) {
		tcp_hdr_t *tcp_hdr = btod(out);
		set_transport_cksum(ip, tcp_hdr,
				    htons(payload_len - sizeof(ip_hdr_t)));
	}

	pkt_adj(out, -(int)sizeof(ip_hdr_t));
	return iface->if_output(out, iface, L3_PROTO_IP, &ip_dst);
}

void ip_input(pkt_t *pkt, iface_t *iface)
{
	ip_hdr_t *ip;
	uint32_t *ip_addr = (uint32_t *)iface->ip4_addr;
	int ip_len;

	ip = btod(pkt);

	if (ip->v != 4 || ip->dst != *ip_addr || ip->ttl == 0)
		goto error;

	if (ip->off & IP_MF) {
		/* ip fragmentation is unsupported */
		goto error;
	}
	if (ip->hl > IP_MAX_HDR_LEN || ip->hl < IP_MIN_HDR_LEN)
		goto error;

	ip_len = ip->hl * 4;
	if (cksum(ip, ip_len) != 0)
		goto error;

#ifdef CONFIG_IP_CHKSUM
#endif

	switch (ip->p) {
#ifdef CONFIG_ICMP
	case IPPROTO_ICMP:
		icmp_input(pkt, iface);
		return;

#ifdef CONFIG_IPV6
	case IPPROTO_ICMPV6:
		icmp6_input(pkt, iface);
		return;
#endif
#endif
#ifdef CONFIG_TCP
	case IPPROTO_TCP:
		tcp_input(pkt);
		return;
#endif
#ifdef CONFIG_UDP
	case IPPROTO_UDP:
		udp_input(pkt, iface);
		return;
#endif
	default:
		/* unsupported protocols */
		break;
	}

 error:
	pkt_free(pkt);
	/* inc stats */
}
