#include "icmp.h"
#include "eth.h"
#include "ip.h"
#include "chksum.h"

void icmp_output(pkt_t *out, iface_t *iface, int type, uint16_t id,
		 uint16_t seq, const buf_t *id_data)
{
	icmp_hdr_t *icmp_hdr;

	icmp_hdr = btod(out, icmp_hdr_t *);
	icmp_hdr->type = type;
	icmp_hdr->code = 0;
	icmp_hdr->id = id;
	icmp_hdr->seq = seq;
	icmp_hdr->cksum = 0;
	pkt_adj(out, (int)sizeof(icmp_hdr_t));
	buf_addbuf(&out->buf, id_data);
	pkt_adj(out, -(int)sizeof(icmp_hdr_t));
	icmp_hdr->cksum = cksum(icmp_hdr, sizeof(icmp_hdr_t) + id_data->len);
	pkt_adj(out, -(int)sizeof(ip_hdr_t));
	ip_output(out, iface);
}

void icmp_input(pkt_t *pkt, iface_t *iface)
{
	icmp_hdr_t *icmp_hdr;
	ip_hdr_t *ip = btod(pkt, ip_hdr_t *);
	ip_hdr_t *ip_out;
	buf_t id_data;
	pkt_t *out;

	/* XXX make sure pkt_adj is the same in all *_output() functions */
	pkt_adj(pkt, ip->hl * 4);

	icmp_hdr = btod(pkt, icmp_hdr_t *);
	pkt_adj(pkt, sizeof(icmp_hdr_t));
	buf_init(&id_data, icmp_hdr->id_data, pkt_len(pkt));

	switch (icmp_hdr->type) {
	case ICMP_ECHO:
		if ((out = pkt_alloc()) == NULL) {
			/* inc stats */
			pkt_free(pkt);
			return;
		}
		pkt_adj(out, (int)sizeof(eth_hdr_t));
		ip_out = btod(out, ip_hdr_t *);
		ip_out->dst = ip->src;
		ip_out->src = ip->dst;
		ip_out->p = IPPROTO_ICMP;
		/* XXX adj */
		pkt_adj(out, (int)sizeof(ip_hdr_t));
		icmp_output(out, iface, ICMP_ECHOREPLY, icmp_hdr->id,
			    icmp_hdr->seq, &id_data);
		break;
	case ICMP_ECHOREPLY:
		//icmp_();
		break;
	default:
		/* unsupported type */
		/* inc stats */
		break;
	}
	pkt_free(pkt);
}