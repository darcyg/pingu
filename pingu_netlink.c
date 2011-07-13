/* pingu_netlink.c - Linux netlink glue
 *
 * Copyright (C) 2007-2009 Timo Teräs <timo.teras@iki.fi>
 * Copyright (C) 2011 Natanael Copa <ncopa@alpinelinux.org>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or later as
 * published by the Free Software Foundation.
 *
 * See http://www.gnu.org/ for details.
 */

#include <arpa/inet.h>
#include <asm/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/ip.h>
#include <linux/if.h>

#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include <ev.h>

#include "log.h"
#include "pingu_iface.h"
#include "pingu_netlink.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#endif 

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define NETLINK_KERNEL_BUFFER	(256 * 1024)
#define NETLINK_RECV_BUFFER	(8 * 1024)

#define NLMSG_TAIL(nmsg) \
	((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

#define NDA_RTA(r)  ((struct rtattr*)(((char*)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg))))
#define NDA_PAYLOAD(n) NLMSG_PAYLOAD(n,sizeof(struct ndmsg))

typedef void (*netlink_dispatch_f)(struct nlmsghdr *msg);

struct netlink_fd {
	int fd;
	__u32 seq;
	struct ev_io io;

	int dispatch_size;
	const netlink_dispatch_f *dispatch;
};

static const int netlink_groups[] = {
	0,
	RTMGRP_LINK,
	RTMGRP_IPV4_IFADDR,
	RTMGRP_IPV4_ROUTE,
};
static struct netlink_fd netlink_fds[ARRAY_SIZE(netlink_groups)];
#define talk_fd netlink_fds[0]

static void netlink_parse_rtattr(struct rtattr *tb[], int max, struct rtattr *rta, int len)
{
	memset(tb, 0, sizeof(struct rtattr *) * (max + 1));
	while (RTA_OK(rta, len)) {
		if (rta->rta_type <= max)
			tb[rta->rta_type] = rta;
		rta = RTA_NEXT(rta,len);
	}
}

static int netlink_receive(struct netlink_fd *fd, struct nlmsghdr *reply)
{
	struct sockaddr_nl nladdr;
	struct iovec iov;
	struct msghdr msg = {
		.msg_name = &nladdr,
		.msg_namelen = sizeof(nladdr),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	int got_reply = FALSE, len;
	char buf[NETLINK_RECV_BUFFER];

	iov.iov_base = buf;
	while (!got_reply) {
		int status;
		struct nlmsghdr *h;

		iov.iov_len = sizeof(buf);
		status = recvmsg(fd->fd, &msg, MSG_DONTWAIT);
		if (status < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				return reply == NULL;
			log_perror("Netlink overrun");
			continue;
		}

		if (status == 0) {
			log_error("Netlink returned EOF");
			return FALSE;
		}

		h = (struct nlmsghdr *) buf;
		while (NLMSG_OK(h, status)) {
			if (reply != NULL &&
			    h->nlmsg_seq == reply->nlmsg_seq) {
				len = h->nlmsg_len;
				if (len > reply->nlmsg_len) {
					log_error("Netlink message truncated");
					len = reply->nlmsg_len;
				}
				memcpy(reply, h, len);
				got_reply = TRUE;
			} else if (h->nlmsg_type <= fd->dispatch_size &&
				fd->dispatch[h->nlmsg_type] != NULL) {
				fd->dispatch[h->nlmsg_type](h);
			} else if (h->nlmsg_type != NLMSG_DONE) {
				log_info("Unknown NLmsg: 0x%08x, len %d",
					  h->nlmsg_type, h->nlmsg_len);
			}
			h = NLMSG_NEXT(h, status);
		}
	}

	return TRUE;
}

static int netlink_enumerate(struct netlink_fd *fd, int family, int type)
{
	struct {
		struct nlmsghdr nlh;
		struct rtgenmsg g;
	} req;
	struct sockaddr_nl addr;

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;

	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len = sizeof(req);
	req.nlh.nlmsg_type = type;
	req.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH | NLM_F_REQUEST;
	req.nlh.nlmsg_pid = 0;
	req.nlh.nlmsg_seq = ++fd->seq;
	req.g.rtgen_family = family;

	return sendto(fd->fd, (void *) &req, sizeof(req), 0,
		      (struct sockaddr *) &addr, sizeof(addr)) >= 0;
}

static void netlink_link_new(struct nlmsghdr *msg)
{
	struct pingu_iface *iface;
	struct ifinfomsg *ifi = NLMSG_DATA(msg);
	struct rtattr *rta[IFLA_MAX+1];
	const char *ifname;

	netlink_parse_rtattr(rta, IFLA_MAX, IFLA_RTA(ifi), IFLA_PAYLOAD(msg));
	if (rta[IFLA_IFNAME] == NULL)
		return;

	ifname = RTA_DATA(rta[IFLA_IFNAME]);
	iface = pingu_iface_find(ifname);
	if (iface == NULL)
		return;

	if (iface->index == 0 || (ifi->ifi_flags & ifi->ifi_change & IFF_UP)) {
		log_info("Interface %s: new or configured up",
			  ifname);
	} else {
		log_info("Interface %s: config change",
			  ifname);
	}

	iface->index = ifi->ifi_index;
	iface->has_link = 1;
	pingu_iface_bind_socket(iface, 1);
}

static void netlink_link_del(struct nlmsghdr *msg)
{
	struct pingu_iface *iface;
	struct ifinfomsg *ifi = NLMSG_DATA(msg);
	struct rtattr *rta[IFLA_MAX+1];
	const char *ifname;

	netlink_parse_rtattr(rta, IFLA_MAX, IFLA_RTA(ifi), IFLA_PAYLOAD(msg));
	if (rta[IFLA_IFNAME] == NULL)
		return;

	ifname = RTA_DATA(rta[IFLA_IFNAME]);
	iface = pingu_iface_find(ifname);
	if (iface == NULL)
		return;

	log_info("Interface '%s' deleted", ifname);
	iface->index = 0;
	iface->has_link = 0;
}

static const netlink_dispatch_f route_dispatch[RTM_MAX] = {
	[RTM_NEWLINK] = netlink_link_new,
	[RTM_DELLINK] = netlink_link_del,
/*	[RTM_NEWADDR] = netlink_addr_new,
	[RTM_DELADDR] = netlink_addr_del,
	[RTM_NEWROUTE] = netlink_route_new,
	[RTM_DELROUTE] = netlink_route_del,
*/
};

static void netlink_read_cb(struct ev_loop *loop, struct ev_io *w, int revents)
{
	struct netlink_fd *nfd = container_of(w, struct netlink_fd, io);

	if (revents & EV_READ)
		netlink_receive(nfd, NULL);
}

static void netlink_close(struct ev_loop *loop, struct netlink_fd *fd)
{
	if (fd->fd >= 0) {
		ev_io_stop(loop, &fd->io);
		close(fd->fd);
		fd->fd = 0;
	}
}

static int netlink_open(struct ev_loop *loop, struct netlink_fd *fd,
			int protocol, int groups)
{
	struct sockaddr_nl addr;
	int buf = NETLINK_KERNEL_BUFFER;

	fd->fd = socket(AF_NETLINK, SOCK_RAW, protocol);
	fd->seq = time(NULL);
	if (fd->fd < 0) {
		log_perror("Cannot open netlink socket");
		return FALSE;
	}

	fcntl(fd->fd, F_SETFD, FD_CLOEXEC);
	if (setsockopt(fd->fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf)) < 0) {
		log_perror("SO_SNDBUF");
		goto error;
	}

	if (setsockopt(fd->fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf)) < 0) {
		log_perror("SO_RCVBUF");
		goto error;
	}

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = groups;
	if (bind(fd->fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		log_perror("Cannot bind netlink socket");
		goto error;
	}

	ev_io_init(&fd->io, netlink_read_cb, fd->fd, EV_READ);
	ev_io_start(loop, &fd->io);

	return TRUE;

error:
	netlink_close(loop, fd);
	return FALSE;
}

int kernel_init(struct ev_loop *loop)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(netlink_groups); i++) {
		netlink_fds[i].dispatch_size = sizeof(route_dispatch) / sizeof(route_dispatch[0]);
		netlink_fds[i].dispatch = route_dispatch;
		if (!netlink_open(loop, &netlink_fds[i], NETLINK_ROUTE,
				  netlink_groups[i]))
			goto err_close_all;
	}

	netlink_enumerate(&talk_fd, PF_UNSPEC, RTM_GETLINK);
	netlink_read_cb(loop, &talk_fd.io, EV_READ);

/*
	netlink_enumerate(&talk_fd, PF_UNSPEC, RTM_GETADDR);
	netlink_read_cb(&talk_fd.io, EV_READ);

	netlink_enumerate(&talk_fd, PF_UNSPEC, RTM_GETROUTE);
	netlink_read_cb(&talk_fd.io, EV_READ);
*/
	return TRUE;

err_close_all:
	for (i = 0; i < ARRAY_SIZE(netlink_groups); i++)
		netlink_close(loop, &netlink_fds[i]);

	return FALSE;
}
