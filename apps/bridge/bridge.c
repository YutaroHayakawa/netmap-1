/*
 * (C) 2011-2014 Luigi Rizzo, Matteo Landi
 *
 * BSD license
 *
 * A netmap client to bridge two network interfaces
 * (or one interface and the host stack).
 *
 * $FreeBSD: head/tools/tools/netmap/bridge.c 228975 2011-12-30 00:04:11Z uqs $
 */

#include <stdio.h>
#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>
#include <sys/poll.h>

int verbose = 0;

static int do_abort = 0;
static int zerocopy = 1; /* enable zerocopy if possible */

#ifdef PB
static struct netmap_ring *qring;
#endif

static void
sigint_h(int sig)
{
	(void)sig;	/* UNUSED */
	do_abort = 1;
	signal(SIGINT, SIG_DFL);
}

/* XXX cut and paste from pkt-gen.c because I'm not sure whether this
 * program may include nm_util.h
 */
void parse_nmr_config(const char* conf, struct nmreq *nmr)
{
	char *w, *tok;
	int i, v;

	nmr->nr_tx_rings = nmr->nr_rx_rings = 0;
	nmr->nr_tx_slots = nmr->nr_rx_slots = 0;
	if (conf == NULL || ! *conf)
		return;
	w = strdup(conf);
	for (i = 0, tok = strtok(w, ","); tok; i++, tok = strtok(NULL, ",")) {
		v = atoi(tok);
		switch (i) {
		case 0:
			nmr->nr_tx_slots = nmr->nr_rx_slots = v;
			break;
		case 1:
			nmr->nr_rx_slots = v;
			break;
		case 2:
			nmr->nr_tx_rings = nmr->nr_rx_rings = v;
			break;
		case 3:
			nmr->nr_rx_rings = v;
			break;
		default:
			D("ignored config: %s", tok);
			break;
		}
	}
	D("txr %d txd %d rxr %d rxd %d",
			nmr->nr_tx_rings, nmr->nr_tx_slots,
			nmr->nr_rx_rings, nmr->nr_rx_slots);
	free(w);
}

/*
 * how many packets on this set of queues ?
 */
int
pkt_queued(struct nm_desc *d, int tx)
{
        u_int i, tot = 0;

        if (tx) {
                for (i = d->first_tx_ring; i <= d->last_tx_ring; i++) {
                        tot += nm_ring_space(NETMAP_TXRING(d->nifp, i));
                }
        } else {
                for (i = d->first_rx_ring; i <= d->last_rx_ring; i++) {
                        tot += nm_ring_space(NETMAP_RXRING(d->nifp, i));
                }
        }
        return tot;
}

/*
 * move up to 'limit' pkts from rxring to txring swapping buffers.
 */
static int
process_rings(struct netmap_ring *rxring, struct netmap_ring *txring,
	      u_int limit, const char *msg)
{
	u_int j, k, m = 0;

	/* print a warning if any of the ring flags is set (e.g. NM_REINIT) */
	if (rxring->flags || txring->flags)
		D("%s rxflags %x txflags %x",
			msg, rxring->flags, txring->flags);
	j = rxring->cur; /* RX */
	k = txring->cur; /* TX */
	m = nm_ring_space(rxring);
	if (m < limit)
		limit = m;
	if (m < limit)
		limit = m;
	m = nm_ring_space(txring);
	m = limit;
	while (limit-- > 0) {
		struct netmap_slot *rs = &rxring->slot[j];
		struct netmap_slot *ts = &txring->slot[k];
#ifdef PB
		struct netmap_slot *qin, *qout = NULL;

		qin = &qring->slot[qring->cur];
		qring->head = qring->cur = nm_ring_next(qring, qring->cur);
		if (!nm_ring_space(qring)) {
			qout = &qring->slot[qring->tail];
			qring->tail = nm_ring_next(qring, qring->tail);
		}
#endif /* PB */

		/* swap packets */
		if (ts->buf_idx < 2 || rs->buf_idx < 2) {
			D("wrong index rx[%d] = %d  -> tx[%d] = %d",
				j, rs->buf_idx, k, ts->buf_idx);
			sleep(2);
		}
		/* copy the packet length. */
		if (rs->len > 2048) {
			D("wrong len %d rx[%d] -> tx[%d]", rs->len, j, k);
			rs->len = 0;
		} else if (verbose > 1) {
			D("%s send len %d rx[%d] -> tx[%d]", msg, rs->len, j, k);
		}
#ifdef PB
		{ /* XXX always zerocopy */
			struct netmap_slot tmp;

			/* enqueue */
		       	tmp = *rs;
			*rs = *qin;
			*qin = tmp;
			rs->flags |= NS_BUF_CHANGED;
			/* dequeue */
			if (qout) {
				tmp = *ts;
				*ts = *qout;
				*qout = tmp;
				ts->flags |= NS_BUF_CHANGED;
			} else {
				j = nm_ring_next(rxring, j);
				continue;
			}
		}
#else
		ts->len = rs->len;
		if (zerocopy) {
			uint32_t pkt = ts->buf_idx;
			ts->buf_idx = rs->buf_idx;
			rs->buf_idx = pkt;
			/* report the buffer change. */
			ts->flags |= NS_BUF_CHANGED;
			rs->flags |= NS_BUF_CHANGED;
		} else {
			char *rxbuf = NETMAP_BUF(rxring, rs->buf_idx);
			char *txbuf = NETMAP_BUF(txring, ts->buf_idx);
			nm_pkt_copy(rxbuf, txbuf, ts->len);
		}
#endif /* PB */
		j = nm_ring_next(rxring, j);
		k = nm_ring_next(txring, k);
	}
	rxring->head = rxring->cur = j;
	txring->head = txring->cur = k;
	if (verbose && m > 0)
		D("%s sent %d packets to %p", msg, m, txring);

	return (m);
}

/* move packts from src to destination */
static int
move(struct nm_desc *src, struct nm_desc *dst, u_int limit)
{
	struct netmap_ring *txring, *rxring;
	u_int m = 0, si = src->first_rx_ring, di = dst->first_tx_ring;
	const char *msg = (src->req.nr_flags == NR_REG_SW) ?
		"host->net" : "net->host";

	while (si <= src->last_rx_ring && di <= dst->last_tx_ring) {
		rxring = NETMAP_RXRING(src->nifp, si);
		txring = NETMAP_TXRING(dst->nifp, di);
		ND("txring %p rxring %p", txring, rxring);
		if (nm_ring_empty(rxring)) {
			si++;
			continue;
		}
		if (nm_ring_empty(txring)) {
			di++;
			continue;
		}
		m += process_rings(rxring, txring, limit, msg);
	}

	return (m);
}


static void
usage(void)
{
	fprintf(stderr,
		"netmap bridge program: forward packets between two "
			"network interfaces\n"
		"    usage(1): bridge [-v] [-i ifa] [-i ifb] [-b burst] "
			"[-w wait_time] [-L]\n"
		"    usage(2): bridge [-v] [-w wait_time] [-L] "
			"[ifa [ifb [burst]]]\n"
		"\n"
		"    ifa and ifb are specified using the nm_open() syntax.\n"
		"    When ifb is missing (or is equal to ifa), bridge will\n"
		"    forward between between ifa and the host stack if -L\n"
		"    is not specified, otherwise loopback traffic on ifa.\n"
		"\n"
		"    example: bridge -w 10 -i netmap:eth3 -i netmap:eth1\n"
		);
	exit(1);
}

/*
 * bridge [-v] if1 [if2]
 *
 * If only one name, or the two interfaces are the same,
 * bridges userland and the adapter. Otherwise bridge
 * two intefaces.
 */
int
main(int argc, char **argv)
{
	struct pollfd pollfd[2];
	int ch;
	u_int burst = 1024, wait_link = 4;
	struct nm_desc *pa = NULL, *pb = NULL;
	char *ifa = NULL, *ifb = NULL;
	char ifabuf[64] = { 0 };
	int loopback = 0;
	struct nmreq nmr;
	char *nmr_config;

	fprintf(stderr, "%s built %s %s\n\n", argv[0], __DATE__, __TIME__);

	bzero(&nmr, sizeof(nmr));

	while ((ch = getopt(argc, argv, "hb:ci:vw:LC:B:")) != -1) {
		switch (ch) {
		default:
			D("bad option %c %s", ch, optarg);
			/* fallthrough */
		case 'h':
			usage();
			break;
		case 'b':	/* burst */
			burst = atoi(optarg);
			break;
		case 'i':	/* interface */
			if (ifa == NULL)
				ifa = optarg;
			else if (ifb == NULL)
				ifb = optarg;
			else
				D("%s ignored, already have 2 interfaces",
					optarg);
			break;
		case 'c':
			zerocopy = 0; /* do not zerocopy */
			break;
		case 'v':
			verbose++;
			break;
		case 'w':
			wait_link = atoi(optarg);
			break;
		case 'L':
			loopback = 1;
			break;
		case 'C':
			nmr_config = strdup(optarg);
			parse_nmr_config(nmr_config, &nmr);
			break;
#ifdef PB
		case 'B':
			nmr.nr_arg3 = atoi(optarg);
			break;
#endif /* PB */
		}

	}

	argc -= optind;
	argv += optind;

	if (argc > 0)
		ifa = argv[0];
	if (argc > 1)
		ifb = argv[1];
	if (argc > 2)
		burst = atoi(argv[2]);
	if (!ifb)
		ifb = ifa;
	if (!ifa) {
		D("missing interface");
		usage();
	}
	if (burst < 1 || burst > 8192) {
		D("invalid burst %d, set to 1024", burst);
		burst = 1024;
	}
	if (wait_link > 100) {
		D("invalid wait_link %d, set to 4", wait_link);
		wait_link = 4;
	}
	if (!strcmp(ifa, ifb)) {
		if (!loopback) {
			D("same interface, endpoint 0 goes to host");
			snprintf(ifabuf, sizeof(ifabuf) - 1, "%s^", ifa);
			ifa = ifabuf;
		} else {
			D("same interface, loopbacking traffic");
		}
	} else {
		/* two different interfaces. Take all rings on if1 */
	}
	pa = nm_open(ifa, &nmr, 0, NULL);
	if (pa == NULL) {
		D("cannot open %s", ifa);
		return (1);
	}
#ifdef PB
	if (pa->nifp->ni_bufs_head) {
		uint32_t next = pa->nifp->ni_bufs_head;
		uint32_t i, n = pa->req.nr_arg3;
		struct netmap_ring *any_ring = pa->some_ring;

		ND("got %u extra bufs at %u", n, next);
		qring = calloc(1, sizeof(struct netmap_ring) +
				sizeof(struct netmap_slot) * n);
		if (!qring) {
			perror("calloc");
			nm_close(pa);
			return (1);
		}
		*(u_int *)(uintptr_t)&qring->num_slots = n;
		qring->cur = qring->head = 0;
	       	qring->tail = qring->num_slots - 1;
		
		for (i = 0; i < n && next; i++) {
			char *p;

			qring->slot[i].buf_idx = next;
			p = NETMAP_BUF(any_ring, next);
			next = *(uint32_t *)p;
		}
	}
#endif /* PB */
	/* try to reuse the mmap() of the first interface, if possible */
	pb = nm_open(ifb, &nmr, NM_OPEN_NO_MMAP, pa);
	if (pb == NULL) {
		D("cannot open %s", ifb);
		nm_close(pa);
		return (1);
	}
	zerocopy = zerocopy && (pa->mem == pb->mem);
	D("------- zerocopy %ssupported", zerocopy ? "" : "NOT ");

	/* setup poll(2) array */
	memset(pollfd, 0, sizeof(pollfd));
	pollfd[0].fd = pa->fd;
	pollfd[1].fd = pb->fd;

	D("Wait %d secs for link to come up...", wait_link);
	sleep(wait_link);
	D("Ready to go, %s 0x%x/%d <-> %s 0x%x/%d.",
		pa->req.nr_name, pa->first_rx_ring, pa->req.nr_rx_rings,
		pb->req.nr_name, pb->first_rx_ring, pb->req.nr_rx_rings);

	/* main loop */
	signal(SIGINT, sigint_h);
	while (!do_abort) {
		int n0, n1, ret;
		pollfd[0].events = pollfd[1].events = 0;
		pollfd[0].revents = pollfd[1].revents = 0;
		n0 = pkt_queued(pa, 0);
		n1 = pkt_queued(pb, 0);
#if defined(_WIN32) || defined(BUSYWAIT)
		if (n0) {
			ioctl(pollfd[1].fd, NIOCTXSYNC, NULL);
			pollfd[1].revents = POLLOUT;
		} else {
			ioctl(pollfd[0].fd, NIOCRXSYNC, NULL);
		}
		if (n1) {
			ioctl(pollfd[0].fd, NIOCTXSYNC, NULL);
			pollfd[0].revents = POLLOUT;
		} else {
			ioctl(pollfd[1].fd, NIOCRXSYNC, NULL);
		}
		ret = 1;
#else
		if (n0)
			pollfd[1].events |= POLLOUT;
		else
			pollfd[0].events |= POLLIN;
		if (n1)
			pollfd[0].events |= POLLOUT;
		else
			pollfd[1].events |= POLLIN;

		/* poll() also cause kernel to txsync/rxsync the NICs */
		ret = poll(pollfd, 2, 2500);
#endif /* defined(_WIN32) || defined(BUSYWAIT) */
		if (ret <= 0 || verbose)
		    ND("poll %s [0] ev %x %x rx %d@%d tx %d,"
			     " [1] ev %x %x rx %d@%d tx %d",
				ret <= 0 ? "timeout" : "ok",
				pollfd[0].events,
				pollfd[0].revents,
				pkt_queued(pa, 0),
				NETMAP_RXRING(pa->nifp, pa->cur_rx_ring)->cur,
				pkt_queued(pa, 1),
				pollfd[1].events,
				pollfd[1].revents,
				pkt_queued(pb, 0),
				NETMAP_RXRING(pb->nifp, pb->cur_rx_ring)->cur,
				pkt_queued(pb, 1)
			);
		if (ret < 0)
			continue;
		if (pollfd[0].revents & POLLERR) {
			struct netmap_ring *rx = NETMAP_RXRING(pa->nifp, pa->cur_rx_ring);
			D("error on fd0, rx [%d,%d,%d)",
				rx->head, rx->cur, rx->tail);
		}
		if (pollfd[1].revents & POLLERR) {
			struct netmap_ring *rx = NETMAP_RXRING(pb->nifp, pb->cur_rx_ring);
			D("error on fd1, rx [%d,%d,%d)",
				rx->head, rx->cur, rx->tail);
		}
		if (pollfd[0].revents & POLLOUT)
			move(pb, pa, burst);

		if (pollfd[1].revents & POLLOUT)
			move(pa, pb, burst);

		/* We don't need ioctl(NIOCTXSYNC) on the two file descriptors here,
		 * kernel will txsync on next poll(). */
	}
	nm_close(pb);
	nm_close(pa);
#ifdef PB
	if (qring)
		free(qring);
#endif
	return (0);
}
