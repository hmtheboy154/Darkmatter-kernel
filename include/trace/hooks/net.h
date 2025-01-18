/* SPDX-License-Identifier: GPL-2.0 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM net
#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_NET_VH_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_NET_VH_H
#include <trace/hooks/vendor_hooks.h>

struct packet_type;
struct list_head;
DECLARE_HOOK(android_vh_ptype_head,
	TP_PROTO(const struct packet_type *pt, struct list_head *vendor_pt),
	TP_ARGS(pt, vendor_pt));

struct sock;
DECLARE_HOOK(android_vh_tcp_write_timeout_estab_retrans,
	TP_PROTO(struct sock *sk), TP_ARGS(sk));
DECLARE_HOOK(android_vh_tcp_connect,
	TP_PROTO(struct sk_buff *skb), TP_ARGS(skb));
struct request_sock;
DECLARE_HOOK(android_vh_inet_csk_clone_lock,
	TP_PROTO(struct sock *newsk, const struct request_sock *req), TP_ARGS(newsk, req));
DECLARE_HOOK(android_vh_tcp_clean_rtx_queue,
	TP_PROTO(struct sock *sk, int flag, long seq_rtt_us),
	TP_ARGS(sk, flag, seq_rtt_us));
struct inet_connection_sock;
DECLARE_HOOK(android_vh_tcp_rcv_synack,
	TP_PROTO(struct inet_connection_sock *icsk), TP_ARGS(icsk));
DECLARE_HOOK(android_vh_udp_unicast_rcv_skb,
	TP_PROTO(struct sk_buff *skb, struct sock *sk),
	TP_ARGS(skb, sk));
DECLARE_HOOK(android_vh_udp6_unicast_rcv_skb,
	TP_PROTO(struct sk_buff *skb, struct sock *sk),
	TP_ARGS(skb, sk));
/* macro versions of hooks are no longer required */

#endif /* _TRACE_HOOK_NET_VH_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
