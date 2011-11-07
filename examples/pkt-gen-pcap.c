/*
 * Copyright (C) 2011 Matteo Landi, Luigi Rizzo. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the
 *      distribution.
 *
 *   3. Neither the name of the authors nor the names of their contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY MATTEO LANDI AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTEO LANDI OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * $Id$
 *
 * Example program to show how to build a multithreaded packet
 * source/sink using the netmap device.
 *
 * In this example we create a programmable number of threads
 * to take care of all the queues of the interface used to
 * send or receive traffic.
 *
 */

const char *default_payload="netmap pkt-gen Luigi Rizzo and Matteo Landi\n"
	"http://info.iet.unipi.it/~luigi/netmap/ ";

#include <errno.h>
#include <pthread.h>	/* pthread_* */
#include <pthread_np.h>	/* pthread w/ affinity */
#include <signal.h>	/* signal */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>	/* strcmp */
#include <fcntl.h>	/* open */
#include <unistd.h>	/* close */
#include <ifaddrs.h>	/* getifaddrs */

#include <sys/ioctl.h>	/* ioctl */
#include <sys/poll.h>
#include <sys/socket.h>	/* sockaddr.. */
#include <arpa/inet.h>	/* ntohs */
#include <sys/param.h>
#include <sys/cpuset.h>	/* cpu_set */
#include <sys/sysctl.h>	/* sysctl */
#include <sys/time.h>	/* timersub */

#include <net/ethernet.h>
#include <net/if.h>	/* ifreq */
#include <net/if_dl.h>	/* LLADDR */

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <pcap/pcap.h>


static inline int min(int a, int b) { return a < b ? a : b; }

/* debug support */
#define D(format, ...)				\
	fprintf(stderr, "%s [%d] " format "\n", 	\
	__FUNCTION__, __LINE__, ##__VA_ARGS__)

int verbose = 0;
#define MAX_QUEUES 64	/* no need to limit */

#define MAX_DESCS 2048

#define SKIP_PAYLOAD 1 /* do not check payload. */

#define	NM_BUF_SIZE	2048


struct pkt {
	struct ether_header eh;
	struct ip ip;
	struct udphdr udp;
	uint8_t body[NM_BUF_SIZE];
} __attribute__((__packed__));

/*
 * global arguments for all threads
 */
struct glob_arg {
	const char *src_ip;
	const char *dst_ip;
	const char *src_mac;
	const char *dst_mac;
	int pkt_size;
	int burst;
	int npackets;
	int nthreads;
	int cpus;
	int force_txsync;
};
/*
 * Arguments for a new thread. The same structure is used by
 * the source and the sink
 */
struct targ {
	struct glob_arg *g;
	int used;
	int completed;
	int fd;
	pcap_t *p;
	uint64_t count;
	struct timeval tic, toc;
	int me;
	pthread_t thread;
	int affinity;


	uint8_t	dst_mac[6];
	uint8_t	src_mac[6];
	u_int dst_mac_range;
	u_int src_mac_range;
	uint32_t dst_ip;
	uint32_t src_ip;
	u_int dst_ip_range;
	u_int src_ip_range;
	u_long ctrs[MAX_QUEUES];
	u_long batches[MAX_DESCS + 1];

	struct pkt pkt;
};


static struct targ targs[1 + MAX_QUEUES];
static int global_nthreads;


/* control-C handler */
static void
sigint_h(__unused int sig)
{
	for (int i = 0; i < global_nthreads; i++) {
		/* cancel active threads. */
		if (targs[i].used == 0)
			continue;

		D("Cancelling thread #%d\n", i);
		pthread_cancel(targs[i].thread);
	}

	signal(SIGINT, SIG_DFL);
}


/* sysctl wrapper to return the number of active CPUs */
static int
system_ncpus(void)
{
	int mib[2], ncpus;
	size_t len;

	mib[0] = CTL_HW;
	mib[1] = HW_NCPU;
	len = sizeof(mib);
	sysctl(mib, 2, &ncpus, &len, NULL, 0);

	return (ncpus);
}

/*
 * locate the src mac address for our interface, put it
 * into the user-supplied buffer. return 0 if ok, -1 on error.
 */
static int
source_hwaddr(const char *ifname, char *buf)
{
	struct ifaddrs *ifaphead, *ifap;
	int l = sizeof(ifap->ifa_name);

	if (getifaddrs(&ifaphead) != 0) {
		D("getifaddrs %s failed", ifname);
		return (-1);
	}

	for (ifap = ifaphead; ifap; ifap = ifap->ifa_next) {
		struct sockaddr_dl *sdl =
			(struct sockaddr_dl *)ifap->ifa_addr;
		uint8_t *mac;

		if (!sdl || sdl->sdl_family != AF_LINK)
			continue;
		if (strncmp(ifap->ifa_name, ifname, l) != 0)
			continue;
		mac = (uint8_t *)LLADDR(sdl);
		sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x",
			mac[0], mac[1], mac[2],
			mac[3], mac[4], mac[5]);
		if (verbose)
			D("source hwaddr %s", buf);
		break;
	}
	freeifaddrs(ifaphead);
	return ifap ? 0 : 1;
}


/* set the thread affinity. */
static int
setaffinity(pthread_t me, int i)
{
	cpuset_t cpumask;

	if (i == -1)
		return 0;

	/* Set thread affinity affinity.*/
	CPU_ZERO(&cpumask);
	CPU_SET(i, &cpumask);

	if (pthread_setaffinity_np(me, sizeof(cpuset_t), &cpumask) != 0) {
		D("Unable to set affinity");
		return 1;
	}
	return 0;
}

/* Compute the checksum of the given ip header. */
static uint16_t
checksum(const void *data, uint16_t len)
{
        const uint8_t *addr = data;
        uint32_t sum = 0;

        while (len > 1) {
                sum += addr[0] * 256 + addr[1];
                addr += 2;
                len -= 2;
        }

        if (len == 1)
                sum += *addr * 256;

        sum = (sum >> 16) + (sum & 0xffff);
        sum += (sum >> 16);

        sum = htons(sum);

        return ~sum;
}

/*
 * Fill a packet with some payload.
 */
static void
initialize_packet(struct targ *targ)
{
	struct pkt *pkt = &targ->pkt;
	struct ether_header *eh;
	struct ip *ip;
	struct udphdr *udp;
	uint16_t paylen = targ->g->pkt_size - sizeof(*eh) - sizeof(*ip);
	int i, l, l0 = strlen(default_payload);
	char *p;

	for (i = 0; i < paylen;) {
		l = min(l0, paylen - i);
		bcopy(default_payload, pkt->body + i, l);
		i += l;
	}
	pkt->body[i-1] = '\0';

	udp = &pkt->udp;
	udp->uh_sport = htons(1234);
        udp->uh_dport = htons(4321);
	udp->uh_ulen = htons(paylen);
	udp->uh_sum = 0; // checksum(udp, sizeof(*udp));

	ip = &pkt->ip;
        ip->ip_v = IPVERSION;
        ip->ip_hl = 5;
        ip->ip_id = 0;
        ip->ip_tos = IPTOS_LOWDELAY;
	ip->ip_len = ntohs(targ->g->pkt_size - sizeof(*eh));
        ip->ip_id = 0;
        ip->ip_off = htons(IP_DF); /* Don't fragment */
        ip->ip_ttl = IPDEFTTL;
	ip->ip_p = IPPROTO_UDP;
	inet_aton(targ->g->src_ip, (struct in_addr *)&ip->ip_src);
	inet_aton(targ->g->dst_ip, (struct in_addr *)&ip->ip_dst);
	targ->dst_ip = ip->ip_dst.s_addr;
	targ->src_ip = ip->ip_src.s_addr;
	p = index(targ->g->src_ip, '-');
	if (p) {
		targ->dst_ip_range = atoi(p+1);
		D("dst-ip sweep %d addresses", targ->dst_ip_range);
	}
	ip->ip_sum = checksum(ip, sizeof(*ip));

	eh = &pkt->eh;
	bcopy(ether_aton(targ->g->src_mac), targ->src_mac, 6);
	bcopy(targ->src_mac, eh->ether_shost, 6);
	p = index(targ->g->src_mac, '-');
	if (p)
		targ->src_mac_range = atoi(p+1);

	bcopy(ether_aton(targ->g->dst_mac), targ->dst_mac, 6);
	bcopy(targ->dst_mac, eh->ether_dhost, 6);
	p = index(targ->g->dst_mac, '-');
	if (p)
		targ->dst_mac_range = atoi(p+1);
	eh->ether_type = htons(ETHERTYPE_IP);
}

/* Check the payload of the packet for errors (use it for debug).
 * Look for consecutive ascii representations of the size of the packet.
 */
static void
check_payload(char *p, int psize)
{
	char temp[64];
	int n_read, size, sizelen;

	/* get the length in ASCII of the length of the packet. */
	sizelen = sprintf(temp, "%d", psize) + 1; // include a whitespace

	/* dummy payload. */
	p += 14; /* skip packet header. */
	n_read = 14;
	while (psize - n_read >= sizelen) {
		sscanf(p, "%d", &size);
		if (size != psize) {
			D("Read %d instead of %d", size, psize);
			break;
		}

		p += sizelen;
		n_read += sizelen;
	}
}


static int
send_packets(pcap_t *p, struct pkt *pkt, int size, u_int count)
{
	u_int sent;
	for (sent = 0; sent < count; sent++) {
		if (pcap_inject(p, pkt, size) == -1)
			break;
	}
	return (sent);
}

static void *
sender_body(void *data)
{
	struct targ *targ = (struct targ *) data;

	struct pollfd fds[1];
	int n = targ->g->npackets / targ->g->nthreads, sent = 0;
	int fill_all = 1;


	if (setaffinity(targ->thread, targ->affinity))
		goto quit;
	/* setup poll(2) machanism. */
	memset(fds, 0, sizeof(fds));
	fds[0].fd = targ->fd;
	fds[0].events = (POLLOUT);

	/* main loop.*/
	gettimeofday(&targ->tic, NULL);
	while (sent < n) {
		/*
		 * wait for available room in the send queue(s)
		 */
		if (poll(fds, 1, 2000) <= 0) {
			D("poll error/timeout on queue %d\n", targ->me);
			goto quit;
		}
		/*
		 * scan our queues and send on those with room
		 */
		if (sent > 100000)
			fill_all = 0;
		int m, limit = MIN(n - sent, targ->g->burst);
		m = send_packets(targ->p, &targ->pkt, targ->g->pkt_size,
				 limit);
		targ->ctrs[0] += m;
		targ->batches[m] += 1;
		sent += m;
		targ->count = sent;
	}

	gettimeofday(&targ->toc, NULL);
	targ->completed = 1;
	targ->count = sent;

quit:
	/* reset the ``used`` flag. */
	targ->used = 0;

	return (NULL);
}


void
receive_packets(__unused u_char *user, const struct pcap_pkthdr *h, const u_char *buf)
{
	if (!SKIP_PAYLOAD)
		check_payload((char *)buf, h->caplen);
}

static void *
receiver_body(void *data)
{
	struct targ *targ = (struct targ *) data;
	struct pollfd fds[1];
	int i, received = 0;

	if (setaffinity(targ->thread, targ->affinity))
		goto quit;

	/* setup poll(2) machanism. */
	memset(fds, 0, sizeof(fds));
	fds[0].fd = targ->fd;
	fds[0].events = (POLLIN);

	/* unbounded wait for the first packet. */
	for (;;) {
		i = poll(fds, 1, 1000);
		if (i > 0 && !(fds[0].revents & POLLERR))
			break;
		D("waiting for initial packets, poll returns %d %d", i, fds[0].revents);
	}

	/* main loop, exit after 1s silence */
	gettimeofday(&targ->tic, NULL);
	while (1) {

		/* Once we started to receive packets, wait at most 1 seconds
		   before quitting. */
		if (poll(fds, 1, 1 * 1000) <= 0) {
			gettimeofday(&targ->toc, NULL);
			targ->toc.tv_sec -= 1; /* Substract timeout time. */
			break;
		}

		pcap_dispatch(targ->p, targ->g->burst, receive_packets, NULL);
	}

	targ->completed = 1;
	targ->count = received;

quit:
	/* reset the ``used`` flag. */
	targ->used = 0;

	return (NULL);
}

static void
tx_output(uint64_t sent, int size, double delta)
{
	double amount = 8.0 * (1.0 * size * sent) / delta;
	double pps = sent / delta;
	char units[4] = { '\0', 'K', 'M', 'G' };
	int aunit = 0, punit = 0;

	while (amount >= 1000) {
		amount /= 1000;
		aunit += 1;
	}
	while (pps >= 1000) {
		pps /= 1000;
		punit += 1;
	}

	printf("Sent %llu packets, %d bytes each, in %.2f seconds.\n",
	       sent, size, delta);
	printf("Speed: %.2f%cpps. Bandwidth: %.2f%cbps.\n",
	       pps, units[punit], amount, units[aunit]);
}


static void
rx_output(uint64_t received, double delta)
{

	double pps = received / delta;
	char units[4] = { '\0', 'K', 'M', 'G' };
	int punit = 0;

	while (pps >= 1000) {
		pps /= 1000;
		punit += 1;
	}

	printf("Received %llu packets, in %.2f seconds.\n", received, delta);
	printf("Speed: %.2f%cpps.\n", pps, units[punit]);
}

static void
usage(void)
{
	const char *cmd = "pkt-gen";
	fprintf(stderr,
		"Usage:\n"
		"%s arguments\n"
		"\t-i interface		interface name\n"
		"\t-t pkts_to_send	also forces send mode\n"
		"\t-r pkts_to_receive	also forces receive mode\n"
		"\t-l pkts_size		in bytes excluding CRC\n"
		"\t-d dst-ip		end with %%n to sweep n addresses\n"
		"\t-s src-ip		end with %%n to sweep n addresses\n"
		"\t-D dst-mac		end with %%n to sweep n addresses\n"
		"\t-S src-mac		end with %%n to sweep n addresses\n"
		"\t-b burst size		testing, mostly\n"
		"\t-c cores		cores to use\n"
		"\t-p threads		processes/threads to use\n"
		"\t-T report_ms		milliseconds between reports\n"
		"",
		cmd);

	exit(0);
}


int
main(int arc, char **argv)
{
	int i, j, n;

	struct glob_arg g;

	void *(*td_body)(void *) = receiver_body;
	u_long *ctrs, *batches;
	int ch;
	int report_interval = 1000;	/* report interval */
	char *ifname = NULL;

	bzero(&g, sizeof(g));

	g.src_ip = "10.0.0.1";
	g.dst_ip = "10.1.0.1";
	g.dst_mac = "ff:ff:ff:ff:ff:ff";
	g.src_mac = NULL;
	g.pkt_size = 60;
	g.burst = 512;		// default
	g.nthreads = 1;
	g.cpus = 1;

	ctrs = calloc(1, (MAX_QUEUES) * sizeof(u_long));
	if (ctrs == NULL)
		return (1);

	batches = calloc(1, (MAX_DESCS + 1) * sizeof(u_long));
	if (batches == NULL)
		return (1);

	while ( (ch = getopt(arc, argv,
			"i:t:r:l:d:s:D:S:b:c:p:T:vf")) != -1) {
		switch(ch) {
		default:
			D("bad option %c %s", ch, optarg);
			usage();
			break;
		case 'i':	/* interface */
			ifname = optarg;
			break;
		case 't':	/* send */
			td_body = sender_body;
			g.npackets = atoi(optarg);
			break;
		case 'r':	/* receive */
			td_body = receiver_body;
			g.npackets = atoi(optarg);
			break;
		case 'l':	/* pkt_size */
			g.pkt_size = atoi(optarg);
			break;
		case 'd':
			g.dst_ip = optarg;
			break;
		case 's':
			g.src_ip = optarg;
			break;
		case 'T':	/* report interval */
			report_interval = atoi(optarg);
			break;
		case 'b':	/* burst */
			g.burst = atoi(optarg);
			break;
		case 'c':
			g.cpus = atoi(optarg);
			break;
		case 'p':
			g.nthreads = atoi(optarg);
			break;
		case 'f':
			g.force_txsync = 1;
			D("forcing tx sync");
			break;
		case 'D': /* destination mac */
			g.dst_mac = optarg;
	{
		struct ether_addr *mac = ether_aton(g.dst_mac);
		D("ether_aton(%s) gives %p", g.dst_mac, mac);
	}
			break;
		case 'S': /* source mac */
			g.src_mac = optarg;
			break;
		case 'v':
			verbose++;
		}
	}

	if (ifname == NULL) {
		D("missing ifname");
		usage();
	}

	n = system_ncpus();
	if (g.cpus <= 0 || g.cpus > 1) {
		D("%d cpus is too high, have only %d cpus", g.cpus, n);
		usage();
	}
	if (g.cpus == 0)
		g.cpus = n;

	if (g.nthreads <= 0 || g.nthreads > 1) {
		D("Invalid nthreads: %d", g.nthreads);
		usage();
	}
	if (g.nthreads == 0)
		g.nthreads = g.cpus;

	if (g.pkt_size < 16 || g.pkt_size > 1536) {
		D("bad pktsize %d\n", g.pkt_size);
		usage();
	}

	if (td_body == sender_body && g.src_mac == NULL) {
		static char mybuf[20] = "ff:ff:ff:ff:ff:ff";
		/* retrieve source mac address. */
		if (source_hwaddr(ifname, mybuf) == -1) {
			D("Unable to retrieve source mac");
			// continue, fail later
		}
		g.src_mac = mybuf;
	}

	/* Print some debug information. */
	fprintf(stdout,
		"%s %s: %d threads and %d cpus.\n",
		(td_body == sender_body) ? "Sending on" : "Receiving from",
		ifname,
		g.nthreads,
		g.cpus);
	if (td_body == sender_body) {
		fprintf(stdout, "%s -> %s (%s -> %s)\n",
			g.src_ip, g.dst_ip,
			g.src_mac, g.dst_mac);
	}
			
	/* Install ^C handler. */
	global_nthreads = g.nthreads;
	signal(SIGINT, sigint_h);

	/*
	 * Now create the desired number of threads, each one
	 * using a single descriptor.
 	 */
	for (i = 0; i < g.nthreads; i++) {
		pcap_t *p;

		p = pcap_open_live(ifname, 0, 1, 100, NULL);

		/* start threads. */
		bzero(&targs[i], sizeof(targs[i]));
		targs[i].g = &g;
		targs[i].used = 1;
		targs[i].completed = 0;
		targs[i].fd = pcap_fileno(p);
		targs[i].p = p;
		targs[i].me = i;
		targs[i].affinity = g.cpus ? i % g.cpus : -1;
		if (td_body == sender_body) {
			/* initialize the packet to send. */
			initialize_packet(&targs[i]);
		}

		if (pthread_create(&targs[i].thread, NULL, td_body,
				   &targs[i]) == -1) {
			D("Unable to create thread %d", i);
			targs[i].used = 0;
		}
	}

    {
	uint64_t my_count = 0, prev = 0;
	uint64_t count = 0;
	double delta_t;
	struct timeval tic, toc;

	gettimeofday(&toc, NULL);
	for (;;) {
		struct timeval now, delta;
		uint64_t pps;
		int done = 0;

		delta.tv_sec = report_interval/1000;
		delta.tv_usec = (report_interval%1000)*1000;
		select(0, NULL, NULL, NULL, &delta);
		gettimeofday(&now, NULL);
		timersub(&now, &toc, &toc);
		my_count = 0;
		for (i = 0; i < g.nthreads; i++) {
			my_count += targs[i].count;
			if (targs[i].used == 0)
				done++;
		}
		pps = toc.tv_sec* 1000000 + toc.tv_usec;
		if (pps < 10000)
			continue;
		pps = (my_count - prev)*1000000 / pps;
		D("%llu pps", pps);
		prev = my_count;
		toc = now;
		if (done == g.nthreads)
			break;
	}

	timerclear(&tic);
	timerclear(&toc);
	for (i = 0; i < g.nthreads; i++) {
		/*
		 * Join active threads, unregister interfaces and close
		 * file descriptors.
		 */
		pthread_join(targs[i].thread, NULL);
		pcap_close(targs[i].p);

		if (targs[i].completed == 0)
			continue;

		/*
		 * Collect threads o1utput and extract information about
		 * how log it took to send all the packets.
		 */
		count += targs[i].count;
		if (!timerisset(&tic) || timercmp(&targs[i].tic, &tic, <))
			tic = targs[i].tic;
		if (!timerisset(&toc) || timercmp(&targs[i].toc, &toc, >))
			toc = targs[i].toc;

		for (j = 0; j < MAX_QUEUES; j++)
			ctrs[j] += targs[i].ctrs[j];

		for (j = 0; j < MAX_DESCS + 1; j++)
			batches[j] += targs[i].batches[j];
	}

	/* print output. */
	timersub(&toc, &tic, &toc);
	delta_t = toc.tv_sec + 1e-6* toc.tv_usec;
	if (td_body == sender_body)
		tx_output(count, g.pkt_size, delta_t);
	else
		rx_output(count, delta_t);
    }

	return (0);
}