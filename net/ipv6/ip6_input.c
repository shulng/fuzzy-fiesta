// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *	IPv6 input
 *	Linux INET6 implementation
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>
 *	Ian P. Morris		<I.P.Morris@soton.ac.uk>
 *
 *	Based in linux/net/ipv4/ip_input.c
 */
/* Changes
 *
 *	Mitsuru KANDA @USAGI and
 *	YOSHIFUJI Hideaki @USAGI: Remove ipv6_parse_exthdrs().
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/in6.h>
#include <linux/icmpv6.h>
#include <linux/mroute6.h>
#include <linux/slab.h>
#include <linux/indirect_call_wrapper.h>
#include <linux/skbuff.h>   /*ZTE_LC_IP_DEBUG */
#include <linux/netfilter.h>
#include <linux/netfilter_ipv6.h>

#include <net/sock.h>
#include <net/snmp.h>

#include <net/ipv6.h>
#include <net/protocol.h>
#include <net/transp_v6.h>
#include <net/rawv6.h>
#include <net/ndisc.h>
#include <net/ip6_route.h>
#include <net/addrconf.h>
#include <net/xfrm.h>
#include <net/net_log.h> /* ZTE_LC_IP_DEBUG */
#include <net/inet_ecn.h>
#include <net/dst_metadata.h>
extern int tcp_socket_debugfs; /*ZTE_LC_IP_DEBUG, 20170418 improved*/

INDIRECT_CALLABLE_DECLARE(void udp_v6_early_demux(struct sk_buff *));
INDIRECT_CALLABLE_DECLARE(void tcp_v6_early_demux(struct sk_buff *));
static void ip6_rcv_finish_core(struct net *net, struct sock *sk,
				struct sk_buff *skb)
{
	void (*edemux)(struct sk_buff *skb);

	if (net->ipv4.sysctl_ip_early_demux && !skb_dst(skb) && skb->sk == NULL) {
		const struct inet6_protocol *ipprot;

		ipprot = rcu_dereference(inet6_protos[ipv6_hdr(skb)->nexthdr]);
		if (ipprot && (edemux = READ_ONCE(ipprot->early_demux)))
			INDIRECT_CALL_2(edemux, tcp_v6_early_demux,
					udp_v6_early_demux, skb);
	}
	if (!skb_valid_dst(skb))
		ip6_route_input(skb);
}

int ip6_rcv_finish(struct net *net, struct sock *sk, struct sk_buff *skb)
{
	/* if ingress device is enslaved to an L3 master device pass the
	 * skb to its handler for processing
	 */
	skb = l3mdev_ip6_rcv(skb);
	if (!skb)
		return NET_RX_SUCCESS;
	ip6_rcv_finish_core(net, sk, skb);

	return dst_input(skb);
}

static void ip6_sublist_rcv_finish(struct list_head *head)
{
	struct sk_buff *skb, *next;

	list_for_each_entry_safe(skb, next, head, list) {
		skb_list_del_init(skb);
		dst_input(skb);
	}
}

static void ip6_list_rcv_finish(struct net *net, struct sock *sk,
				struct list_head *head)
{
	struct dst_entry *curr_dst = NULL;
	struct sk_buff *skb, *next;
	struct list_head sublist;

	INIT_LIST_HEAD(&sublist);
	list_for_each_entry_safe(skb, next, head, list) {
		struct dst_entry *dst;

		skb_list_del_init(skb);
		/* if ingress device is enslaved to an L3 master device pass the
		 * skb to its handler for processing
		 */
		skb = l3mdev_ip6_rcv(skb);
		if (!skb)
			continue;
		ip6_rcv_finish_core(net, sk, skb);
		dst = skb_dst(skb);
		if (curr_dst != dst) {
			/* dispatch old sublist */
			if (!list_empty(&sublist))
				ip6_sublist_rcv_finish(&sublist);
			/* start new sublist */
			INIT_LIST_HEAD(&sublist);
			curr_dst = dst;
		}
		list_add_tail(&skb->list, &sublist);
	}
	/* dispatch final sublist */
	ip6_sublist_rcv_finish(&sublist);
}

static struct sk_buff *ip6_rcv_core(struct sk_buff *skb, struct net_device *dev,
				    struct net *net)
{
	const struct ipv6hdr *hdr;
	u32 pkt_len;
	struct inet6_dev *idev;

	if (skb->pkt_type == PACKET_OTHERHOST) {
		kfree_skb(skb);
		return NULL;
	}

	rcu_read_lock();

	idev = __in6_dev_get(skb->dev);

	__IP6_UPD_PO_STATS(net, idev, IPSTATS_MIB_IN, skb->len);

	if ((skb = skb_share_check(skb, GFP_ATOMIC)) == NULL ||
	    !idev || unlikely(idev->cnf.disable_ipv6)) {
		__IP6_INC_STATS(net, idev, IPSTATS_MIB_INDISCARDS);
		goto drop;
	}

	memset(IP6CB(skb), 0, sizeof(struct inet6_skb_parm));

	/*
	 * Store incoming device index. When the packet will
	 * be queued, we cannot refer to skb->dev anymore.
	 *
	 * BTW, when we send a packet for our own local address on a
	 * non-loopback interface (e.g. ethX), it is being delivered
	 * via the loopback interface (lo) here; skb->dev = loopback_dev.
	 * It, however, should be considered as if it is being
	 * arrived via the sending interface (ethX), because of the
	 * nature of scoping architecture. --yoshfuji
	 */
	IP6CB(skb)->iif = skb_valid_dst(skb) ? ip6_dst_idev(skb_dst(skb))->dev->ifindex : dev->ifindex;

	if (unlikely(!pskb_may_pull(skb, sizeof(*hdr))))
		goto err;

	hdr = ipv6_hdr(skb);

	if (hdr->version != 6)
		goto err;

	__IP6_ADD_STATS(net, idev,
			IPSTATS_MIB_NOECTPKTS +
				(ipv6_get_dsfield(hdr) & INET_ECN_MASK),
			max_t(unsigned short, 1, skb_shinfo(skb)->gso_segs));
	/*
	 * RFC4291 2.5.3
	 * The loopback address must not be used as the source address in IPv6
	 * packets that are sent outside of a single node. [..]
	 * A packet received on an interface with a destination address
	 * of loopback must be dropped.
	 */
	if ((ipv6_addr_loopback(&hdr->saddr) ||
	     ipv6_addr_loopback(&hdr->daddr)) &&
	    !(dev->flags & IFF_LOOPBACK) &&
	    !netif_is_l3_master(dev))
		goto err;

	/* RFC4291 Errata ID: 3480
	 * Interface-Local scope spans only a single interface on a
	 * node and is useful only for loopback transmission of
	 * multicast.  Packets with interface-local scope received
	 * from another node must be discarded.
	 */
	if (!(skb->pkt_type == PACKET_LOOPBACK ||
	      dev->flags & IFF_LOOPBACK) &&
	    ipv6_addr_is_multicast(&hdr->daddr) &&
	    IPV6_ADDR_MC_SCOPE(&hdr->daddr) == 1)
		goto err;

	/* If enabled, drop unicast packets that were encapsulated in link-layer
	 * multicast or broadcast to protected against the so-called "hole-196"
	 * attack in 802.11 wireless.
	 */
	if (!ipv6_addr_is_multicast(&hdr->daddr) &&
	    (skb->pkt_type == PACKET_BROADCAST ||
	     skb->pkt_type == PACKET_MULTICAST) &&
	    idev->cnf.drop_unicast_in_l2_multicast)
		goto err;

	/* RFC4291 2.7
	 * Nodes must not originate a packet to a multicast address whose scope
	 * field contains the reserved value 0; if such a packet is received, it
	 * must be silently dropped.
	 */
	if (ipv6_addr_is_multicast(&hdr->daddr) &&
	    IPV6_ADDR_MC_SCOPE(&hdr->daddr) == 0)
		goto err;

	/*
	 * RFC4291 2.7
	 * Multicast addresses must not be used as source addresses in IPv6
	 * packets or appear in any Routing header.
	 */
	if (ipv6_addr_is_multicast(&hdr->saddr))
		goto err;

	/* While RFC4291 is not explicit about v4mapped addresses
	 * in IPv6 headers, it seems clear linux dual-stack
	 * model can not deal properly with these.
	 * Security models could be fooled by ::ffff:127.0.0.1 for example.
	 *
	 * https://tools.ietf.org/html/draft-itojun-v6ops-v4mapped-harmful-02
	 */
	if (ipv6_addr_v4mapped(&hdr->saddr))
		goto err;

	skb->transport_header = skb->network_header + sizeof(*hdr);
	IP6CB(skb)->nhoff = offsetof(struct ipv6hdr, nexthdr);

	pkt_len = ntohs(hdr->payload_len);

	/* pkt_len may be zero if Jumbo payload option is present */
	if (pkt_len || hdr->nexthdr != NEXTHDR_HOP) {
		if (pkt_len + sizeof(struct ipv6hdr) > skb->len) {
			__IP6_INC_STATS(net,
					idev, IPSTATS_MIB_INTRUNCATEDPKTS);
			goto drop;
		}
		if (pskb_trim_rcsum(skb, pkt_len + sizeof(struct ipv6hdr))) {
			__IP6_INC_STATS(net, idev, IPSTATS_MIB_INHDRERRORS);
			goto drop;
		}
		hdr = ipv6_hdr(skb);
	}

	if (hdr->nexthdr == NEXTHDR_HOP) {
		if (ipv6_parse_hopopts(skb) < 0) {
			__IP6_INC_STATS(net, idev, IPSTATS_MIB_INHDRERRORS);
			rcu_read_unlock();
			return NULL;
		}
	}

	rcu_read_unlock();

	/* Must drop socket now because of tproxy. */
	skb_orphan(skb);

	return skb;
err:
	__IP6_INC_STATS(net, idev, IPSTATS_MIB_INHDRERRORS);
drop:
	rcu_read_unlock();
	kfree_skb(skb);
	return NULL;
}

int ipv6_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt, struct net_device *orig_dev)
{
	struct net *net = dev_net(skb->dev);

	skb = ip6_rcv_core(skb, dev, net);
	if (skb == NULL)
		return NET_RX_DROP;
	return NF_HOOK(NFPROTO_IPV6, NF_INET_PRE_ROUTING,
		       net, NULL, skb, dev, NULL,
		       ip6_rcv_finish);
}

static void ip6_sublist_rcv(struct list_head *head, struct net_device *dev,
			    struct net *net)
{
	NF_HOOK_LIST(NFPROTO_IPV6, NF_INET_PRE_ROUTING, net, NULL,
		     head, dev, NULL, ip6_rcv_finish);
	ip6_list_rcv_finish(net, NULL, head);
}

/* Receive a list of IPv6 packets */
void ipv6_list_rcv(struct list_head *head, struct packet_type *pt,
		   struct net_device *orig_dev)
{
	struct net_device *curr_dev = NULL;
	struct net *curr_net = NULL;
	struct sk_buff *skb, *next;
	struct list_head sublist;

	INIT_LIST_HEAD(&sublist);
	list_for_each_entry_safe(skb, next, head, list) {
		struct net_device *dev = skb->dev;
		struct net *net = dev_net(dev);

		skb_list_del_init(skb);
		skb = ip6_rcv_core(skb, dev, net);
		if (skb == NULL)
			continue;

		if (curr_dev != dev || curr_net != net) {
			/* dispatch old sublist */
			if (!list_empty(&sublist))
				ip6_sublist_rcv(&sublist, curr_dev, curr_net);
			/* start new sublist */
			INIT_LIST_HEAD(&sublist);
			curr_dev = dev;
			curr_net = net;
		}
		list_add_tail(&skb->list, &sublist);
	}
	/* dispatch final sublist */
	ip6_sublist_rcv(&sublist, curr_dev, curr_net);
}

/*ZTE_LC_IP_DEBUG, 20170418 improved begin*/
static int
extract_icmp6_fields_lo(const struct sk_buff *skb,
		unsigned int outside_hdrlen,
		int *protocol,
		struct in6_addr **raddr,
		struct in6_addr **laddr,
		__be16 *rport,
		__be16 *lport)
{
	struct ipv6hdr *inside_iph, _inside_iph;
	struct icmp6hdr *icmph, _icmph;
	__be16 *ports, _ports[2];
	u8 inside_nexthdr;
	__be16 inside_fragoff;
	int inside_hdrlen;

	icmph = skb_header_pointer(skb, outside_hdrlen,
				   sizeof(_icmph), &_icmph);
	if (!icmph)
		return 1;

	if (icmph->icmp6_type & ICMPV6_INFOMSG_MASK)
		return 1;

	inside_iph = skb_header_pointer(skb, outside_hdrlen + sizeof(_icmph), sizeof(_inside_iph), &_inside_iph);
	if (!inside_iph)
		return 1;
	inside_nexthdr = inside_iph->nexthdr;

	inside_hdrlen = ipv6_skip_exthdr(skb, outside_hdrlen + sizeof(_icmph) + sizeof(_inside_iph),
					 &inside_nexthdr, &inside_fragoff);
	if (inside_hdrlen < 0)
		return 1; /* hjm: Packet has no/incomplete transport layer headers. */

	if (inside_nexthdr != IPPROTO_TCP &&
		inside_nexthdr != IPPROTO_UDP)
		return 1;

	ports = skb_header_pointer(skb, inside_hdrlen,
				   sizeof(_ports), &_ports);
	if (!ports)
		return 1;

	/* the inside IP packet is the one quoted from our side, thus
	 * its saddr is the local address */
	*protocol = inside_nexthdr;
	*laddr = &inside_iph->saddr;
	*lport = ports[0];
	*raddr = &inside_iph->daddr;
	*rport = ports[1];

	return 0;
}

void
xt_socket_get6_print(struct sk_buff *skb, int direction)
{
	struct ipv6hdr *iph = ipv6_hdr(skb);
	struct udphdr _hdr, *hp = NULL;
	struct sock *sk = skb->sk;
	struct in6_addr *daddr = NULL, *saddr = NULL;

	__be16 uninitialized_var(dport), uninitialized_var(sport);
	int thoff = 0, uninitialized_var(tproto);
	kuid_t uid = GLOBAL_ROOT_UID;
	char string[40], dirstr[12];
	int ulen = 0;

	if (!(tcp_socket_debugfs & TCP_IPV6_LOG_ENABLE))
		return;

	tproto = ipv6_find_hdr(skb, &thoff, -1, NULL, NULL);
	if (tproto < 0) {
		pr_debug("unable to find transport header in IPv6 packet, dropping\n");
		return;
	}

	if (tproto == IPPROTO_UDP || tproto == IPPROTO_TCP) {
		hp = skb_header_pointer(skb, thoff,
					sizeof(_hdr), &_hdr);
		if (!hp)
			return;

		saddr = &iph->saddr;
		sport = hp->source;
		daddr = &iph->daddr;
		dport = hp->dest;
		ulen = skb->len;

		if (tproto == IPPROTO_UDP)
			strlcpy(string, " UDP", sizeof(string));
		else
			strlcpy(string, " TCP", sizeof(string));
	} else if (tproto == IPPROTO_ICMPV6) {
		if (extract_icmp6_fields_lo(skb, thoff, &tproto,
					&saddr, &daddr, &sport, &dport))
			return;
		strlcpy(string, " ICMPV6", sizeof(string));
	} else
		return;

	if (sk) {
		uid = sk ? sk->sk_uid : GLOBAL_ROOT_UID;
		if (!uid_valid(uid))
			uid = GLOBAL_ROOT_UID;
	}

	if (direction == 1)  /*0 tx, 1 rx*/
		strlcpy(dirstr, " RCV  len=", sizeof(dirstr));
	else
		strlcpy(dirstr, " SEND len=", sizeof(dirstr));

	strlcat(string, dirstr, sizeof(string));

	pr_log_info("[IPv6]%s%d, uid=%d, Gpid:%d (%s), %pI6 :%hu -> %pI6 :%hu\n",
		string, ulen, uid.val, current->group_leader->pid, current->group_leader->comm,
		saddr, ntohs(sport),
		daddr, ntohs(dport));
}
EXPORT_SYMBOL(xt_socket_get6_print);
/*ZTE_LC_IP_DEBUG, 20170418 improved end*/
INDIRECT_CALLABLE_DECLARE(int udpv6_rcv(struct sk_buff *));
INDIRECT_CALLABLE_DECLARE(int tcp_v6_rcv(struct sk_buff *));

/*
 *	Deliver the packet to the host
 */
void ip6_protocol_deliver_rcu(struct net *net, struct sk_buff *skb, int nexthdr,
			      bool have_final)
{
	const struct inet6_protocol *ipprot;
	struct inet6_dev *idev;
	unsigned int nhoff;
	bool raw;

	/*
	 *	Parse extension headers
	 */

resubmit:
	idev = ip6_dst_idev(skb_dst(skb));
	nhoff = IP6CB(skb)->nhoff;
	if (!have_final) {
		if (!pskb_pull(skb, skb_transport_offset(skb)))
			goto discard;
		nexthdr = skb_network_header(skb)[nhoff];
	}

resubmit_final:
	raw = raw6_local_deliver(skb, nexthdr);
	ipprot = rcu_dereference(inet6_protos[nexthdr]);
	if (ipprot) {
		int ret;

		if (have_final) {
			if (!(ipprot->flags & INET6_PROTO_FINAL)) {
				/* Once we've seen a final protocol don't
				 * allow encapsulation on any non-final
				 * ones. This allows foo in UDP encapsulation
				 * to work.
				 */
				goto discard;
			}
		} else if (ipprot->flags & INET6_PROTO_FINAL) {
			const struct ipv6hdr *hdr;
			int sdif = inet6_sdif(skb);
			struct net_device *dev;

			/* Only do this once for first final protocol */
			have_final = true;

			/* Free reference early: we don't need it any more,
			   and it may hold ip_conntrack module loaded
			   indefinitely. */
			nf_reset_ct(skb);

			skb_postpull_rcsum(skb, skb_network_header(skb),
					   skb_network_header_len(skb));
			hdr = ipv6_hdr(skb);

			/* skb->dev passed may be master dev for vrfs. */
			if (sdif) {
				dev = dev_get_by_index_rcu(net, sdif);
				if (!dev)
					goto discard;
			} else {
				dev = skb->dev;
			}

			if (ipv6_addr_is_multicast(&hdr->daddr) &&
			    !ipv6_chk_mcast_addr(dev, &hdr->daddr,
						 &hdr->saddr) &&
			    !ipv6_is_mld(skb, nexthdr, skb_network_header_len(skb)))
				goto discard;
		}
		if (!(ipprot->flags & INET6_PROTO_NOPOLICY) &&
		    !xfrm6_policy_check(NULL, XFRM_POLICY_IN, skb))
			goto discard;

		ret = INDIRECT_CALL_2(ipprot->handler, tcp_v6_rcv, udpv6_rcv,
				      skb);
		if (ret > 0) {
			if (ipprot->flags & INET6_PROTO_FINAL) {
				/* Not an extension header, most likely UDP
				 * encapsulation. Use return value as nexthdr
				 * protocol not nhoff (which presumably is
				 * not set by handler).
				 */
				nexthdr = ret;
				goto resubmit_final;
			} else {
				goto resubmit;
			}
		} else if (ret == 0) {
			__IP6_INC_STATS(net, idev, IPSTATS_MIB_INDELIVERS);
		}
	} else {
		if (!raw) {
			if (xfrm6_policy_check(NULL, XFRM_POLICY_IN, skb)) {
				__IP6_INC_STATS(net, idev,
						IPSTATS_MIB_INUNKNOWNPROTOS);
				icmpv6_send(skb, ICMPV6_PARAMPROB,
					    ICMPV6_UNK_NEXTHDR, nhoff);
			}
			kfree_skb(skb);
		} else {
			__IP6_INC_STATS(net, idev, IPSTATS_MIB_INDELIVERS);
			consume_skb(skb);
		}
	}
	return;

discard:
	__IP6_INC_STATS(net, idev, IPSTATS_MIB_INDISCARDS);
	kfree_skb(skb);
}

static int ip6_input_finish(struct net *net, struct sock *sk, struct sk_buff *skb)
{
	rcu_read_lock();
	ip6_protocol_deliver_rcu(net, skb, 0, false);
	rcu_read_unlock();

	return 0;
}


int ip6_input(struct sk_buff *skb)
{
	return NF_HOOK(NFPROTO_IPV6, NF_INET_LOCAL_IN,
		       dev_net(skb->dev), NULL, skb, skb->dev, NULL,
		       ip6_input_finish);
}
EXPORT_SYMBOL_GPL(ip6_input);

int ip6_mc_input(struct sk_buff *skb)
{
	int sdif = inet6_sdif(skb);
	const struct ipv6hdr *hdr;
	struct net_device *dev;
	bool deliver;

	__IP6_UPD_PO_STATS(dev_net(skb_dst(skb)->dev),
			 __in6_dev_get_safely(skb->dev), IPSTATS_MIB_INMCAST,
			 skb->len);

	/* skb->dev passed may be master dev for vrfs. */
	if (sdif) {
		rcu_read_lock();
		dev = dev_get_by_index_rcu(dev_net(skb->dev), sdif);
		if (!dev) {
			rcu_read_unlock();
			kfree_skb(skb);
			return -ENODEV;
		}
	} else {
		dev = skb->dev;
	}

	hdr = ipv6_hdr(skb);
	deliver = ipv6_chk_mcast_addr(dev, &hdr->daddr, NULL);
	if (sdif)
		rcu_read_unlock();

#ifdef CONFIG_IPV6_MROUTE
	/*
	 *      IPv6 multicast router mode is now supported ;)
	 */
	if (dev_net(skb->dev)->ipv6.devconf_all->mc_forwarding &&
	    !(ipv6_addr_type(&hdr->daddr) &
	      (IPV6_ADDR_LOOPBACK|IPV6_ADDR_LINKLOCAL)) &&
	    likely(!(IP6CB(skb)->flags & IP6SKB_FORWARDED))) {
		/*
		 * Okay, we try to forward - split and duplicate
		 * packets.
		 */
		struct sk_buff *skb2;
		struct inet6_skb_parm *opt = IP6CB(skb);

		/* Check for MLD */
		if (unlikely(opt->flags & IP6SKB_ROUTERALERT)) {
			/* Check if this is a mld message */
			u8 nexthdr = hdr->nexthdr;
			__be16 frag_off;
			int offset;

			/* Check if the value of Router Alert
			 * is for MLD (0x0000).
			 */
			if (opt->ra == htons(IPV6_OPT_ROUTERALERT_MLD)) {
				deliver = false;

				if (!ipv6_ext_hdr(nexthdr)) {
					/* BUG */
					goto out;
				}
				offset = ipv6_skip_exthdr(skb, sizeof(*hdr),
							  &nexthdr, &frag_off);
				if (offset < 0)
					goto out;

				if (ipv6_is_mld(skb, nexthdr, offset))
					deliver = true;

				goto out;
			}
			/* unknown RA - process it normally */
		}

		if (deliver)
			skb2 = skb_clone(skb, GFP_ATOMIC);
		else {
			skb2 = skb;
			skb = NULL;
		}

		if (skb2) {
			ip6_mr_input(skb2);
		}
	}
out:
#endif
	if (likely(deliver))
		ip6_input(skb);
	else {
		/* discard */
		kfree_skb(skb);
	}

	return 0;
}
