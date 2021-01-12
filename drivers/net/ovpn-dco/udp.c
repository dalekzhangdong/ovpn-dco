// SPDX-License-Identifier: GPL-2.0-only
/*  OpenVPN data channel accelerator
 *
 *  Copyright (C) 2019-2021 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 */

#include "main.h"
#include "bind.h"
#include "ovpn.h"
#include "ovpnstruct.h"
#include "peer.h"
#include "proto.h"
#include "udp.h"

#include <linux/inetdevice.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <net/addrconf.h>
#include <net/dst_cache.h>
#include <net/route.h>
#include <net/ipv6_stubs.h>
#include <net/udp_tunnel.h>

/**
 * Start processing a received UDP packet.
 * If the first byte of the payload is DATA_V2, the packet is further processed,
 * otherwise it is forwarded to the UDP stack for delivery to user space.
 *
 * @sk: the socket the packet was received on
 * @skb: the sk_buff containing the actual packet
 *
 * Return codes:
 *  0 : we consumed or dropped packet
 * >0 : skb should be passed up to userspace as UDP (packet not consumed)
 * <0 : skb should be resubmitted as proto -N (packet not consumed)
 */
int ovpn_udp_encap_recv(struct sock *sk, struct sk_buff *skb)
{
	struct ovpn_peer *peer = NULL;
	struct ovpn_struct *ovpn;
	u32 peer_id;
	int ret;

	ovpn = ovpn_from_udp_sock(sk);
	if (unlikely(!ovpn)) {
		pr_err_ratelimited("%s: cannot obtain ovpn object from UDP socket\n", __func__);
		goto drop;
	}

	/* Make sure the first 4 bytes of the skb data buffer after the UDP header are accessible.
	 * They are required to fetch the OP code, the key ID and the peer ID.
	 */
	if (unlikely(!pskb_may_pull(skb, sizeof(struct udphdr) + 4))) {
		pr_debug_ratelimited("%s: packet too small\n", __func__);
		goto drop;
	}

	if (likely(ovpn_opcode_from_skb(skb, sizeof(struct udphdr)) == OVPN_DATA_V2)) {
		peer_id = ovpn_peer_id_from_skb(skb, sizeof(struct udphdr));
		peer = ovpn_peer_lookup_id(ovpn, peer_id);
		if (!peer) {
			pr_err_ratelimited("%s: received data from unknown peer (id: %d)\n",
					   __func__, peer_id);
			goto drop;
		}
	} else {
		peer = ovpn_peer_lookup_transp_addr(ovpn, skb);
		if (!peer) {
			pr_debug("%s: control packet from unknown peer, sending to userspace",
				 __func__);
			return 1;
		}
	}

	/* pop off outer UDP header */
	__skb_pull(skb, sizeof(struct udphdr));

	ret = ovpn_recv(ovpn, peer, skb);
	if (unlikely(ret < 0)) {
		pr_err_ratelimited("%s: cannot handle incoming packet: %d\n", __func__, ret);
		goto drop;
	}

	/* should this be a non DATA_V2 packet, ret will be >0 and this will instruct the UDP
	 * stack to continue processing this packet as usual (i.e. deliver to user space)
	 */
	return ret;

drop:
	if (peer)
		ovpn_peer_put(peer);
	kfree_skb(skb);
	return 0;
}

static int ovpn_udp4_output(struct ovpn_struct *ovpn, struct ovpn_bind *bind,
			    struct dst_cache *cache, struct sock *sk,
			    struct sk_buff *skb)
{
	struct rtable *rt;
	struct flowi4 fl = {
		.daddr = bind->sa.in4.sin_addr.s_addr,
		.fl4_sport = inet_sk(sk)->inet_sport,
		.fl4_dport = bind->sa.in4.sin_port,
		.flowi4_proto = sk->sk_protocol,
		.flowi4_mark = sk->sk_mark,
		.flowi4_oif = sk->sk_bound_dev_if,
	};

	rt = dst_cache_get_ip4(cache, &fl.saddr);
	if (rt && likely(inet_confirm_addr(sock_net(sk), NULL, 0, fl.saddr, RT_SCOPE_HOST)))
		goto transmit;

	/* we may end up here when the cached address is not usable anymore.
	 * In this case we reset address/cache and perform a new look up
	 */
	fl.saddr = 0;
	dst_cache_reset(cache);

	rt = ip_route_output_flow(sock_net(sk), &fl, sk);
	if (IS_ERR(rt)) {
		net_dbg_ratelimited("%s: no route to host %pISpc\n", ovpn->dev->name,
				    &bind->sa.in4);
		return -EHOSTUNREACH;
	}
	dst_cache_set_ip4(cache, &rt->dst, fl.saddr);

transmit:
	udp_tunnel_xmit_skb(rt, sk, skb, fl.saddr, fl.daddr, 0,
			    ip4_dst_hoplimit(&rt->dst), 0, fl.fl4_sport,
			    fl.fl4_dport, false, sk->sk_no_check_tx);
	return 0;
}

#if IS_ENABLED(CONFIG_IPV6)
static int ovpn_udp6_output(struct ovpn_struct *ovpn, struct ovpn_bind *bind,
			    struct dst_cache *cache, struct sock *sk,
			    struct sk_buff *skb)
{
	struct dst_entry *dst;
	int ret;

	struct flowi6 fl = {
		.daddr = bind->sa.in6.sin6_addr,
		.fl6_sport = inet_sk(sk)->inet_sport,
		.fl6_dport = bind->sa.in6.sin6_port,
		.flowi6_proto = sk->sk_protocol,
		.flowi6_mark = sk->sk_mark,
		.flowi6_oif = bind->sa.in6.sin6_scope_id,
	};

	dst = dst_cache_get_ip6(cache, &fl.saddr);
	if (dst && likely(ipv6_chk_addr(sock_net(sk), &fl.saddr, NULL, 0)))
		goto transmit;

	/* we may end up here when the cached address is not usable anymore.
	 * In this case we reset address/cache and perform a new look up
	 */
	fl.saddr = in6addr_any;
	dst_cache_reset(cache);

	dst = ipv6_stub->ipv6_dst_lookup_flow(sock_net(sk), sk, &fl, NULL);
	if (IS_ERR(dst)) {
		ret = PTR_ERR(dst);
		return ret;
	}
	dst_cache_set_ip6(cache, dst, &fl.saddr);

transmit:
	udp_tunnel6_xmit_skb(dst, sk, skb, skb->dev, &fl.saddr, &fl.daddr, 0,
			     ip6_dst_hoplimit(dst), 0, fl.fl6_sport,
			     fl.fl6_dport, udp_get_no_check6_tx(sk));
	return 0;
}
#endif

/* Transmit skb utilizing kernel-provided UDP tunneling framework.
 *
 * rcu_read_lock should be held on entry.
 * On return, the skb is consumed.
 */
static int ovpn_udp_output(struct ovpn_struct *ovpn, struct ovpn_bind *bind,
			   struct dst_cache *cache, struct sock *sk,
			   struct sk_buff *skb)
{
	int ret;

	ovpn_rcu_lockdep_assert_held();

	/* set sk to null if skb is already orphaned */
	if (!skb->destructor)
		skb->sk = NULL;

	switch (bind->sa.in4.sin_family) {
	case AF_INET:
		ret = ovpn_udp4_output(ovpn, bind, cache, sk, skb);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		ret = ovpn_udp6_output(ovpn, bind, cache, sk, skb);
		break;
#endif
	default:
		ret = -EAFNOSUPPORT;
		break;
	}

	return ret;
}

/* Called after encrypt to write IP packet to UDP port.
 * This method is expected to manage/free skb.
 */
void ovpn_udp_send_skb(struct ovpn_struct *ovpn, struct ovpn_peer *peer,
		       struct sk_buff *skb)
{
	struct ovpn_bind *bind;
	struct socket *sock;
	int ret = -1;

	skb->dev = ovpn->dev;
	/* no checksum performed at this layer */
	skb->ip_summed = CHECKSUM_NONE;

	/* get socket info */
	sock = peer->sock->sock;
	if (unlikely(!sock)) {
		pr_debug_ratelimited("%s: no sock for remote peer\n", __func__);
		goto out;
	}

	rcu_read_lock();
	/* get binding */
	bind = rcu_dereference(peer->bind);
	if (unlikely(!bind)) {
		pr_debug_ratelimited("%s: no bind for remote peer\n", __func__);
		goto out_unlock;
	}

	/* note event of authenticated packet xmit for keepalive */
	ovpn_peer_keepalive_xmit_reset(peer);

	/* crypto layer -> transport (UDP) */
	ret = ovpn_udp_output(ovpn, bind, &peer->dst_cache, sock->sk, skb);

out_unlock:
	rcu_read_unlock();
out:
	if (ret < 0)
		kfree_skb(skb);
}

/* Set UDP encapsulation callbacks */
int ovpn_udp_socket_attach(struct socket *sock, struct ovpn_struct *ovpn)
{
	struct udp_tunnel_sock_cfg cfg = {
		.sk_user_data = ovpn,
		.encap_type = UDP_ENCAP_OVPNINUDP,
		.encap_rcv = ovpn_udp_encap_recv,
	};
	struct ovpn_socket *old_data;

	/* sanity check */
	if (sock->sk->sk_protocol != IPPROTO_UDP) {
		pr_err("%s: expected UDP socket\n", __func__);
		return -EINVAL;
	}

	/* make sure no pre-existing encapsulation handler exists */
	rcu_read_lock();
	old_data = rcu_dereference_sk_user_data(sock->sk);
	rcu_read_unlock();
	if (old_data) {
		if (old_data->ovpn == ovpn) {
			pr_debug("%s: provided socket already owned by this interface\n", __func__);
			return -EALREADY;
		}

		pr_err("%s: provided socket already taken by other user\n", __func__);
		return -EBUSY;
	}

	setup_udp_tunnel_sock(sock_net(sock->sk), sock, &cfg);

	return 0;
}

/* Detach socket from encapsulation handler and/or other callbacks */
void ovpn_udp_socket_detach(struct socket *sock)
{
	struct udp_tunnel_sock_cfg cfg = { };

	setup_udp_tunnel_sock(sock_net(sock->sk), sock, &cfg);
	sockfd_put(sock);
}
