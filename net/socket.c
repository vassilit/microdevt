#include "socket.h"
#include "../sys/errno.h"
#include "eth.h"
#include "ip.h"
#ifdef CONFIG_UDP
#include "udp.h"
#endif
#include "../sys/list.h"
#include "../sys/hash-tables.h"

#ifndef CONFIG_HT_STORAGE
#undef CONFIG_MAX_SOCK_HT_SIZE
#endif

#if defined(CONFIG_HT_STORAGE) && defined(CONFIG_BSD_COMPAT)
hash_table_t *fd_to_sock;
#else
struct list_head sock_list = LIST_HEAD_INIT(sock_list);
#endif

#ifdef CONFIG_BSD_COMPAT
uint8_t cur_fd = 3;
uint8_t max_fds = 100;
#endif

#ifdef CONFIG_HT_STORAGE
#ifdef CONFIG_UDP
hash_table_t *udp_binds;
#endif
#ifdef CONFIG_TCP
hash_table_t *tcp_binds;
extern hash_table_t *tcp_conns;
#endif
#else  /* CONFIG_HT_STORAGE */
#ifdef CONFIG_TCP
extern struct list_head tcp_conns;
#endif
#endif

#ifdef CONFIG_EVENT

void ev_cb(sock_info_t *sock_info, uint8_t event)
{
	uint8_t events = EV_NONE;

	if ((sock_info->events & EV_READ) && (event & EV_READ))
		events = EV_READ;
	if ((sock_info->events & EV_WRITE) && (event & EV_WRITE))
		events |= EV_WRITE;
	if (events != EV_NONE) {
		events = sock_info->events;
		sock_info->events = EV_NONE;
		sock_info->ev_cb(sock_info, events);
		sock_info->events = events;
	}
}

#endif

#ifdef CONFIG_HT_STORAGE
#ifdef CONFIG_BSD_COMPAT
static sock_info_t *fd2sockinfo(int fd)
{
	sbuf_t key = FD2SBUF(fd);
	sbuf_t *val;

	if (htable_lookup(fd_to_sock, &key, &val) < 0) {
		errno = EINVAL;
		return NULL;
	}
	return *(sock_info_t **)val->data;
}
#endif

static hash_table_t *get_hash_table(int type)
{
	switch (type) {
#ifdef CONFIG_UDP
	case SOCK_DGRAM:
		return udp_binds;
#endif
#ifdef CONFIG_TCP
	case SOCK_STREAM:
		return tcp_binds;
#endif
	default:
		errno = EINVAL;
		return NULL;
	}
}

static sock_info_t *port2sockinfo(int type, uint16_t port)
{
	sbuf_t key, *val;
	hash_table_t *ht = get_hash_table(type);

	if (ht == NULL)
		return NULL;
	sbuf_init(&key, &port, sizeof(port));
	if (htable_lookup(ht, &key, &val) < 0)
		return NULL;
	return *(sock_info_t **)val->data;
}

#ifdef CONFIG_BSD_COMPAT
static int sock_info_add(int fd, sock_info_t *sock_info)
{
	sbuf_t key = FD2SBUF(fd);
	sbuf_t val = SOCKINFO2SBUF(sock_info);

	sock_info->fd = fd;
	sock_info->listen = NULL;
	if (htable_add(fd_to_sock, &key, &val) < 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}
#endif

static int unbind_port(sock_info_t *sock_info)
{
	sbuf_t key;
	hash_table_t *ht = get_hash_table(sock_info->type);

	if (ht == NULL)
		return -1;

#ifdef CONFIG_TCP
	if (sock_info->type == SOCK_STREAM && sock_info->trq.tcp_conn) {
		tcp_conn_delete(sock_info->trq.tcp_conn);
		sock_info->trq.tcp_conn = NULL;
	}
#endif

	sbuf_init(&key, &sock_info->port, sizeof(sock_info->port));
	htable_del(ht, &key);
#ifdef CONFIG_BSD_COMPAT
	sbuf_init(&key, &sock_info->fd, sizeof(sock_info->fd));
	if (htable_del(fd_to_sock, &key) < 0)
		return -1;
#endif
	return 0;
}

static int bind_on_port(uint16_t port, sock_info_t *sock_info)
{
	hash_table_t *ht = get_hash_table(sock_info->type);
	sbuf_t key, val;

	if (ht == NULL) {
#ifdef CONFIG_BSD_COMPAT
		errno = EINVAL;
#endif
		return -1;
	}
	sbuf_init(&key, &port, sizeof(port));
	val = SOCKINFO2SBUF(sock_info);

	if (htable_add(ht, &key, &val) < 0) {
		unbind_port(sock_info);
#ifdef CONFIG_BSD_COMPAT
		errno = EINVAL;
#endif
		return -1;
	}
	sock_info->port = port;
	return 0;
}

#else  /* CONFIG_HT_STORAGE */
#ifdef CONFIG_BSD_COMPAT
static sock_info_t *fd2sockinfo(int fd)
{
	sock_info_t *sock_info;

	list_for_each_entry(sock_info, &sock_list, list) {
		if (sock_info->fd == fd)
			return sock_info;
	}
	errno = EINVAL;
	return NULL;
}
#endif

static sock_info_t *port2sockinfo(int type, uint16_t port)
{
	sock_info_t *sock_info;

	list_for_each_entry(sock_info, &sock_list, list) {
		if (sock_info->port == port && sock_info->type == type)
			return sock_info;
	}
	return NULL;
}

#ifdef CONFIG_BSD_COMPAT
static int sock_info_add(int fd, sock_info_t *sock_info)
{
	if (fd2sockinfo(fd))
		return -1;

	sock_info->fd = fd;
#ifdef CONFIG_TCP
	sock_info->listen = NULL;
#endif
	list_add_tail(&sock_info->list, &sock_list);
	return 0;
}
#endif

static int unbind_port(sock_info_t *sock_info)
{
#ifndef CONFIG_BSD_COMPAT
	if (port2sockinfo(sock_info->type, sock_info->port) == NULL)
		return -1;
#else
	if (fd2sockinfo(sock_info->fd) == NULL)
		return -1;
#endif

	sock_info->port = 0;
#ifdef CONFIG_TCP
	if (sock_info->type == SOCK_STREAM && sock_info->trq.tcp_conn) {
		tcp_conn_delete(sock_info->trq.tcp_conn);
		sock_info->trq.tcp_conn = NULL;
	}
#endif
	list_del_init(&sock_info->list);
	return 0;
}

static int bind_on_port(uint16_t port, sock_info_t *sock_info)
{
	if (port2sockinfo(sock_info->type, port)) {
#ifdef CONFIG_BSD_COMPAT
		errno = EINVAL;
#endif
		unbind_port(sock_info);
		return -1;
	}
	sock_info->port = port;
	return 0;
}

#ifdef CONFIG_TCP
tcp_conn_t *socket_tcp_conn_lookup(const tcp_uid_t *uid)
{
	sock_info_t *sock_info;

	list_for_each_entry(sock_info, &sock_list, list) {
		listen_t *listen = sock_info->listen;
		tcp_conn_t *tcp_conn;

		if (listen == NULL)
			continue;
		list_for_each_entry(tcp_conn, &listen->tcp_conn_list_head,
				    list) {
			if (memcmp(uid, &tcp_conn->syn.tuid, sizeof(tcp_uid_t)) == 0)
				return tcp_conn;
		}
	}
	return NULL;
}
#endif
#endif	/* CONFIG_HT_STORAGE not set */

#ifdef CONFIG_TCP
sock_status_t sock_info_state(const sock_info_t *sock_info)
{
	if (sock_info->type != SOCK_STREAM)
		return SOCK_CLOSED;

	if (sock_info->trq.tcp_conn == NULL)
		return SOCK_CLOSED;
	return sock_info->trq.tcp_conn->syn.status;
}

sock_info_t *tcpport2sockinfo(uint16_t port)
{
	return port2sockinfo(SOCK_STREAM, port);
}
#endif

#ifdef CONFIG_UDP
sock_info_t *udpport2sockinfo(uint16_t port)
{
	return port2sockinfo(SOCK_DGRAM, port);
}
#endif

int sock_info_init(sock_info_t *sock_info, int sock_type)
{
#ifdef CONFIG_BSD_COMPAT
	int fd;
	int retries = 0;
#endif
	switch (sock_type) {
#ifdef CONFIG_UDP
	case SOCK_DGRAM:
		INIT_LIST_HEAD(&sock_info->trq.pkt_list);
		break;
#endif
#ifdef CONFIG_TCP
	case SOCK_STREAM:
		sock_info->trq.tcp_conn = NULL;
		sock_info->listen = NULL;
		break;
#endif
	default:
		return -1;
	}
	sock_info->type = sock_type;
	sock_info->port = 0;
#ifndef CONFIG_HT_STORAGE
	INIT_LIST_HEAD(&sock_info->list);
#endif
#ifdef CONFIG_BSD_COMPAT
 again:
	fd = cur_fd;
	if (sock_info_add(fd, sock_info) < 0) {
		if (retries > max_fds) {
			free(sock_info);
			return -1;
		}
		retries++;
		cur_fd++;
		if (cur_fd > max_fds)
			cur_fd = 3;
		goto again;
	}

	cur_fd++;
	return fd;
#else
#ifndef CONFIG_HT_STORAGE
	list_add_tail(&sock_info->list, &sock_list);
#endif
	return 0;
#endif
}

#ifdef CONFIG_TCP
static void socket_listen_free(listen_t *listen)
{
	tcp_conn_t *tcp_conn, *tcp_conn_tmp;

	if (listen == NULL)
		return;
	if (list_empty(&listen->tcp_conn_list_head)) {
		free(listen);
		return;
	}
	list_for_each_entry_safe(tcp_conn, tcp_conn_tmp,
				 &listen->tcp_conn_list_head, list) {
		tcp_conn_delete(tcp_conn);
	}
	free(listen);
}

int sock_info_listen(sock_info_t *sock_info, int backlog)
{
	listen_t *listen;

	if ((listen = malloc(sizeof(listen_t))) == NULL)
		return -1;

	INIT_LIST_HEAD(&listen->tcp_conn_list_head);
	sock_info->listen = listen;
	listen->backlog = 0;
	listen->backlog_max = backlog;
	return 0;
}
#endif

#ifdef CONFIG_BSD_COMPAT
int socket(int family, int type, int protocol)
{
	sock_info_t *sock_info;
	uint8_t fd;

	(void)protocol;
	if (family != AF_INET || family >= SOCK_LAST)
		return -1;

	if ((sock_info = malloc(sizeof(sock_info_t))) == NULL)
		return -1;

	if ((fd = sock_info_init(sock_info, type)) < 0) {
		free(sock_info);
		return -1;
	}
	return fd;
}

#ifdef CONFIG_TCP
int listen(int fd, int backlog)
{
	sock_info_t *sock_info = fd2sockinfo(fd);

	if (sock_info == NULL)
		return -1;

	return sock_info_listen(sock_info, backlog);
}
#endif
#endif

int max_retries = CONFIG_EPHEMERAL_PORT_END - CONFIG_EPHEMERAL_PORT_START;

/* return val of 0 => no ports available  */
static uint16_t socket_get_ephemeral_port(uint8_t type)
{
	static unsigned ephemeral_port = CONFIG_EPHEMERAL_PORT_START;
	uint16_t port;
	int retries = 0;

	STATIC_ASSERT(CONFIG_EPHEMERAL_PORT_END > CONFIG_EPHEMERAL_PORT_START);
	do {
		port = htons(ephemeral_port);
		ephemeral_port++;
		if (ephemeral_port >= EPHEMERAL_PORT_END)
			ephemeral_port = EPHEMERAL_PORT_START;
		if (port2sockinfo(type, port) == NULL)
			return port;
	} while (retries < max_retries);
	return 0;
}

int sock_info_bind(sock_info_t *sock_info, uint16_t port)
{
	if (port == 0) { /* client */
		port = socket_get_ephemeral_port(sock_info->type);
		if (port == 0) {
#ifdef CONFIG_BSD_COMPAT
			errno = EADDRINUSE;
#endif
			return -1; /* no more available ports */
		}
	}
#ifdef CONFIG_EVENT
	if (sock_info->type == SOCK_DGRAM) {
		if (bind_on_port(port, sock_info) >= 0) {
			ev_cb(sock_info, EV_WRITE);
			return 0;
		} else
			return -1;
	} else
		return bind_on_port(port, sock_info);
#else
	return bind_on_port(port, sock_info);
#endif
}

int sock_info_close(sock_info_t *sock_info)
{
#ifdef CONFIG_TCP
	socket_listen_free(sock_info->listen);
#endif
	return unbind_port(sock_info);
}

#ifdef CONFIG_TCP
int socket_add_backlog(listen_t *listen, tcp_conn_t *tcp_conn)
{
	if (listen->backlog >= listen->backlog_max)
		return -1;
	list_add_tail(&tcp_conn->list, &listen->tcp_conn_list_head);
	listen->backlog++;
	return 0;
}

#ifdef CONFIG_TCP_CLIENT
int sock_info_connect(sock_info_t *sock_info, uint32_t addr, uint16_t port)
{
	if (sock_info_bind(sock_info, 0) < 0)
		return -1;

	if (tcp_connect(addr, port, sock_info) < 0) {
#ifdef CONFIG_BSD_COMPAT
		errno = EAGAIN;
#endif
		return -1;
	}
	return 0;
}
#endif
#endif

#ifdef CONFIG_BSD_COMPAT
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	sock_info_t *sock_info;
	struct sockaddr_in *sockaddr = (struct sockaddr_in *)addr;

	if (sockaddr->sin_family != AF_INET)
		return -1;

	if (addrlen != sizeof(struct sockaddr_in))
		return -1;

	if ((sock_info = fd2sockinfo(sockfd)) == NULL)
		return -1;

#if 0 /* only one ip address is allowed on an interface */
	sock_info->addr.ip4_addr = sockaddr->sin_addr.s_addr;
#endif

	/* TODO check sin_addr for ip addresses on available interfaces */
	return bind_on_port(sockaddr->sin_port, sock_info);
}

#ifdef CONFIG_TCP
#ifdef CONFIG_TCP_CLIENT
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	sock_info_t *sock_info;
	struct sockaddr_in *sockaddr = (struct sockaddr_in *)addr;

	if ((sock_info = fd2sockinfo(sockfd)) == NULL) {
#ifdef CONFIG_BSD_COMPAT
		errno = EBADF;
#endif
		return -1;
	}

	return sock_info_connect(sock_info, sockaddr->sin_addr.s_addr,
				 sockaddr->sin_port);
}
#endif

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	sock_info_t *sock_info, *sock_info_child;
	tcp_conn_t *tcp_conn;
	struct sockaddr_in *sockaddr = (struct sockaddr_in *)addr;
	int fd;

	if ((sock_info = fd2sockinfo(sockfd)) == NULL) {
#ifdef CONFIG_BSD_COMPAT
		errno = EBADF;
#endif
		return -1;
	}

	if (sock_info->type != SOCK_STREAM || sock_info->listen == NULL) {
#ifdef CONFIG_BSD_COMPAT
		errno = EBADF;
#endif
		return -1;
	}
	if (list_empty(&sock_info->listen->tcp_conn_list_head)) {
#ifdef CONFIG_BSD_COMPAT
		errno = EAGAIN;
#endif
		return -1;
	}
	tcp_conn = list_first_entry(&sock_info->listen->tcp_conn_list_head,
				    tcp_conn_t, list);
	list_del(&tcp_conn->list);
	sock_info->listen->backlog--;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
#ifdef CONFIG_BSD_COMPAT
		errno = EBADF;
#endif
		tcp_conn_delete(tcp_conn);
		return -1;
	}
#ifndef CONFIG_HT_STORAGE
	list_add_tail(&tcp_conn->list, &tcp_conns);
#endif
	sockaddr->sin_family = AF_INET;
	sockaddr->sin_addr.s_addr = tcp_conn->syn.tuid.src_addr;
	sockaddr->sin_port = tcp_conn->syn.tuid.src_port;
	*addrlen = sizeof(struct sockaddr_in);
	sock_info_child = fd2sockinfo(fd);
	sock_info_child->trq.tcp_conn = tcp_conn;
	sock_info_child->port = sock_info->port;
	tcp_conn->sock_info = sock_info_child;

	return fd;
}
#endif	/* TCP */
#else	/* CONFIG_BSD_COMPAT */
#ifdef CONFIG_TCP
int
sock_info_accept(sock_info_t *sock_info_server, sock_info_t *sock_info_client,
		 uint32_t *src_addr, uint16_t *src_port)
{
	tcp_conn_t *tcp_conn;

	if (sock_info_server->type != SOCK_STREAM)
		return -1;

	if (sock_info_server->listen == NULL ||
	    list_empty(&sock_info_server->listen->tcp_conn_list_head))
		return -1;
	tcp_conn = list_first_entry(&sock_info_server->listen->tcp_conn_list_head,
				    tcp_conn_t, list);
	list_del(&tcp_conn->list);
	sock_info_server->listen->backlog--;

	if (sock_info_init(sock_info_client, SOCK_STREAM) < 0)
		return -1;

#ifndef CONFIG_HT_STORAGE
	list_add_tail(&tcp_conn->list, &tcp_conns);
#endif
	*src_addr = tcp_conn->syn.tuid.src_addr;
	*src_port = tcp_conn->syn.tuid.src_port;
	sock_info_client->trq.tcp_conn = tcp_conn;
	tcp_conn->sock_info = sock_info_client;
	return 0;
}
#endif
#endif	/* BSD_COMPAT */

static pkt_t *socket_alloc_pkt(int hdrlen, const sbuf_t *sbuf)
{
	pkt_t *pkt;

	if ((pkt = pkt_alloc()) == NULL && (pkt = pkt_alloc_emergency()) == NULL) {
#ifdef CONFIG_BSD_COMPAT
		errno = ENOBUFS;
#endif
		return NULL;
	}

	pkt_adj(pkt, (int)sizeof(eth_hdr_t));
	pkt_adj(pkt, (int)sizeof(ip_hdr_t));
	pkt_adj(pkt, hdrlen);
	if (buf_addsbuf(&pkt->buf, sbuf) < 0) {
#ifdef CONFIG_BSD_COMPAT
		errno = ENOBUFS;
#endif
		goto error;
	}

	pkt_adj(pkt, -hdrlen);
	return pkt;
 error:
	pkt_free(pkt);
	return NULL;
}

#ifdef CONFIG_TCP
static tcp_conn_t *socket_get_tcp_conn(const sock_info_t *sock_info)
{
	tcp_conn_t *tcp_conn = sock_info->trq.tcp_conn;

	if (tcp_conn == NULL) {
#ifdef CONFIG_BSD_COMPAT
		errno = EAGAIN;
#endif
		return NULL;
	}
	if (list_empty(&tcp_conn->pkt_list_head)
	    && tcp_conn->syn.status == SOCK_CLOSED) {
		tcp_conn_delete(tcp_conn);
#ifdef CONFIG_BSD_COMPAT
		errno = EBADF;
#endif
		return NULL;
	}
	return tcp_conn;
}
#endif

int __socket_put_sbuf(sock_info_t *sock_info, const sbuf_t *sbuf,
		      uint32_t dst_addr, uint16_t dst_port)
{
	pkt_t *pkt;
#ifdef CONFIG_TCP
	tcp_conn_t *tcp_conn;
#endif
	if (sbuf->len == 0)
		return 0;

	switch (sock_info->type) {
#ifdef CONFIG_UDP
	case SOCK_TYPE_UDP:
		if (sock_info->port == 0 && sock_info_bind(sock_info, 0) < 0)
			return -1;

		if ((pkt = socket_alloc_pkt((int)sizeof(udp_hdr_t), sbuf)) == NULL)
			return -1;

		udp_output(pkt, dst_addr, sock_info->port, dst_port);
		return 0;
#endif
#ifdef CONFIG_TCP
	case SOCK_TYPE_TCP:
		if ((tcp_conn = socket_get_tcp_conn(sock_info)) == NULL)
			return -1;

		if (tcp_conn->syn.status != SOCK_CONNECTED) {
#ifdef CONFIG_BSD_COMPAT
			errno = EBADF;
#endif
			return -1;
		}

		if ((pkt = socket_alloc_pkt((int)sizeof(tcp_hdr_t), sbuf)) == NULL)
			return -1;

		tcp_output(pkt, tcp_conn, TH_PUSH | TH_ACK);
		tcp_conn->syn.seqid = htonl(ntohl(tcp_conn->syn.seqid) + sbuf->len);
		return 0;
#endif
	default:
#ifdef CONFIG_BSD_COMPAT
		errno = EBADF;
#endif
		return -1;
	}

	return 0;
}

#ifdef CONFIG_BSD_COMPAT
int
socket_put_sbuf(int fd, const sbuf_t *sbuf, const struct sockaddr_in *addr_in)
{
	sock_info_t *sock_info = fd2sockinfo(fd);

	if (sock_info == NULL) {
#ifdef CONFIG_BSD_COMPAT
		errno = EBADF;
#endif
		return -1;
	}
	return __socket_put_sbuf(sock_info, sbuf, addr_in->sin_addr.s_addr,
				 addr_in->sin_port);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
	       const struct sockaddr *dest_addr, socklen_t addrlen)
{
	sbuf_t sb;

	(void)flags;
	if (addrlen != sizeof(struct sockaddr_in))
		return -1;
	sbuf_init(&sb, buf, len);
	return socket_put_sbuf(sockfd, &sb, (const struct sockaddr_in *)dest_addr);
}
#endif

int __socket_get_pkt(const sock_info_t *sock_info, pkt_t **pktp,
		     uint32_t *src_addr, uint16_t *src_port)
{
	int transport_hdr_len = -1;
	pkt_t *pkt;
	ip_hdr_t *ip_hdr;
#ifdef CONFIG_UDP
	udp_hdr_t *udp_hdr;
#endif
#ifdef CONFIG_TCP
	tcp_hdr_t *tcp_hdr;
	tcp_conn_t *tcp_conn;
#endif
	switch (sock_info->type) {
#ifdef CONFIG_UDP
	case SOCK_DGRAM:
		if (list_empty(&sock_info->trq.pkt_list)) {
#ifdef CONFIG_BSD_COMPAT
			errno = EAGAIN;
#endif
			return -1;
		}

		pkt = list_first_entry(&sock_info->trq.pkt_list, pkt_t, list);
		list_del(&pkt->list);
		pkt_adj(pkt, -(int)sizeof(udp_hdr_t));
		udp_hdr = btod(pkt, udp_hdr_t *);
		*src_port = udp_hdr->src_port;
		transport_hdr_len = (int)sizeof(udp_hdr_t);
		break;
#endif
#ifdef CONFIG_TCP
	case SOCK_STREAM:
		if ((tcp_conn = socket_get_tcp_conn(sock_info)) == NULL)
			return -1;

		if (list_empty(&tcp_conn->pkt_list_head)) {
#ifdef CONFIG_BSD_COMPAT
			errno = EAGAIN;
#endif
			return -1;
		}

		pkt = list_first_entry(&tcp_conn->pkt_list_head, pkt_t, list);
		list_del(&pkt->list);
		pkt_adj(pkt, -(int)sizeof(tcp_hdr_t));
		tcp_hdr = btod(pkt, tcp_hdr_t *);
		transport_hdr_len = tcp_hdr->hdr_len * 4;
		break;
#endif
	default:
#ifdef CONFIG_BSD_COMPAT
		errno = EBADF;
#endif
		return -1;
	}
	pkt_adj(pkt, -(int)sizeof(ip_hdr_t));
	ip_hdr = btod(pkt, ip_hdr_t *);
	if (src_addr)
		*src_addr = ip_hdr->src;
	pkt_adj(pkt, sizeof(ip_hdr_t));
	pkt_adj(pkt, transport_hdr_len);

	*pktp = pkt;
	return 0;
}

#ifdef CONFIG_BSD_COMPAT
int socket_get_pkt(int fd, pkt_t **pktp, struct sockaddr_in *addr_in)
{
	sock_info_t *sock_info = fd2sockinfo(fd);
	int ret;

	if (sock_info == NULL) {
#ifdef CONFIG_BSD_COMPAT
		errno = EBADF;
#endif
		return -1;
	}
	ret = __socket_get_pkt(sock_info, pktp, &addr_in->sin_addr.s_addr,
			       &addr_in->sin_port);
	if (ret < 0)
		return -1;
	addr_in->sin_family = AF_INET;

	return 0;
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
		 struct sockaddr *src_addr, socklen_t *addrlen)
{
	pkt_t *pkt;
	int __len;

	(void)flags;
	if (socket_get_pkt(sockfd, &pkt, (struct sockaddr_in *)src_addr) < 0)
		return -1;
	*addrlen = sizeof(struct sockaddr_in);
	__len = MIN((int)len, pkt->buf.len);
	memcpy(buf, buf_data(&pkt->buf), __len);
	pkt_free(pkt);
	return __len;
}

int close(int fd)
{
	sock_info_t *sock_info;

	if ((sock_info = fd2sockinfo(fd)) == NULL)
		return -1;

	if (sock_info_close(sock_info) >= 0 && fd == cur_fd - 1)
		cur_fd--;
	free(sock_info);
	return 0;
}

#endif	/* CONFIG_BSD_COMPAT */

void socket_append_pkt(struct list_head *list_head, pkt_t *pkt)
{
	list_add_tail(&pkt->list, list_head);
}

#ifdef CONFIG_HT_STORAGE
int socket_init(void)
{
#ifdef CONFIG_BSD_COMPAT
	if ((fd_to_sock = htable_create(CONFIG_MAX_SOCK_HT_SIZE)) == NULL)
		return -1;
#endif
#ifdef CONFIG_UDP
	if ((udp_binds = htable_create(CONFIG_MAX_SOCK_HT_SIZE)) == NULL)
		return -1;
#endif
#ifdef CONFIG_TCP
	if ((tcp_binds = htable_create(CONFIG_MAX_SOCK_HT_SIZE)) == NULL)
		return -1;
	if ((tcp_conns = htable_create(CONFIG_MAX_SOCK_HT_SIZE)) == NULL)
		return -1;
#endif
	return 0;
}
#endif

#if 0 /* this code prevents from finding leaks */
static void socket_free_cb(sbuf_t *key, sbuf_t *val)
{
	sock_info_t *sock_info = SBUF2SOCKINFO(val);

	(void)key;
#ifdef CONFIG_BSD_COMPAT
	socket_listen_free(sock_info->listen);
#endif
	free(sock_info);
}
#endif

void socket_shutdown(void)
{
#ifdef CONFIG_HT_STORAGE
#ifdef CONFIG_UDP
	htable_free(udp_binds);
#endif
#ifdef CONFIG_TCP
	htable_free(tcp_binds);
	htable_free(tcp_conns);
#endif
#if 0 /* this code prevents from finding leaks */
#ifdef CONFIG_BSD_COMPAT
	htable_for_each(fd_to_sock, socket_free_cb);
#endif
	htable_free(fd_to_sock);
#endif
#else
	sock_info_t *sock_info, *si_tmp;

	list_for_each_entry_safe(sock_info, si_tmp, &sock_list, list) {
		list_del(&sock_info->list);
#ifdef CONFIG_TCP
		socket_listen_free(sock_info->listen);
#endif
		free(sock_info);
	}
#endif
}
