#define _GNU_SOURCE
#include <config.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <arpa/inet.h>

#include "qemu/slirp/slirp.h"
#include "libslirp.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>

struct libslirp_data {
	int tapfd;
};

void slirp_output(void *opaque, const uint8_t * pkt, int pkt_len)
{
	struct libslirp_data *data = (struct libslirp_data *)opaque;
	int rc;
	if ((rc = write(data->tapfd, pkt, pkt_len)) < 0) {
		perror("slirp_output: write");
	}
	assert(rc == pkt_len);
}

#ifdef SYSTEM_QEMU_PATH

int do_slirp(int tapfd, int exitfd, unsigned int mtu, bool ip6_enabled)
{
	int pid = fork();
	if (pid < 0) {
		perror("cannot fork");
		return -1;
	}

	if (pid) {
		if (exitfd < 0) {
			int ret, status;
			do
				ret = waitpid(pid, &status, 0);
			while (ret < 0 && errno == EINTR);
		} else {
			char c;
			errno = 0;
			do
				read(exitfd, &c, 1);
			while (errno == EINTR);
		}
	} else {
		char tap[32];

		prctl(PR_SET_PDEATHSIG, SIGKILL);
		sprintf(tap, "tap,fd=%d", tapfd);
		execlp(SYSTEM_QEMU_PATH, SYSTEM_QEMU_PATH, "-M", "none", "-nographic", "-net", tap, "-net", "user",
		       NULL);
		exit(EXIT_FAILURE);
	}
	return 0;
}

#else

Slirp *create_slirp(void *opaque, unsigned int mtu, bool ip6_enabled)
{
	Slirp *slirp = NULL;
	struct in_addr vnetwork, vnetmask, vhost, vdhcp_start, vnameserver;
	struct in6_addr vhost6, vprefix_addr6, vnameserver6;
	int vprefix_len = 64;
	inet_pton(AF_INET, "10.0.2.0", &vnetwork);
	inet_pton(AF_INET, "255.255.255.0", &vnetmask);
	inet_pton(AF_INET, "10.0.2.2", &vhost);
	inet_pton(AF_INET, "10.0.2.3", &vnameserver);
	inet_pton(AF_INET, "10.0.2.15", &vdhcp_start);
	inet_pton(AF_INET6, "fd00::2", &vhost6);
	inet_pton(AF_INET6, "fd00::", &vprefix_addr6);
	inet_pton(AF_INET6, "fd00::3", &vnameserver6);
	slirp = slirp_init(0 /* restricted */ , 1 /* is_enabled */ ,
			   vnetwork, vnetmask, vhost, (int)ip6_enabled, vprefix_addr6, vprefix_len, vhost6,
			   NULL /* vhostname */ , NULL /* bootfile */ , vdhcp_start,
			   vnameserver, vnameserver6, NULL /* vdnssearch */ , NULL /* vdomainname */ ,
			   mtu /* if_mtu */ , mtu /* if_mru */ ,
			   opaque);
	if (slirp == NULL) {
		fprintf(stderr, "slirp_init failed\n");
	}
	return slirp;
}

#define ETH_BUF_SIZE (65536)

int do_slirp(int tapfd, int exitfd, unsigned int mtu, bool ip6_enabled)
{
	int ret = -1;
	Slirp *slirp = NULL;
	uint8_t *buf = NULL;
	struct libslirp_data opaque = {.tapfd = tapfd };
	GArray pollfds = { 0 };
	size_t n_fds = 1;
	struct pollfd tap_pollfd = { tapfd, POLLIN | POLLHUP, 0 };
	struct pollfd exit_pollfd = { exitfd, POLLHUP, 0 };

	slirp = create_slirp((void *)&opaque, mtu, ip6_enabled);
	if (slirp == NULL) {
		fprintf(stderr, "create_slirp failed\n");
		goto err;
	}
	buf = malloc(ETH_BUF_SIZE);
	if (buf == NULL) {
		goto err;
	}
	g_array_append_val(&pollfds, tap_pollfd);
	if (exitfd >= 0) {
		n_fds++;
		g_array_append_val(&pollfds, exit_pollfd);
	}
	signal(SIGPIPE, SIG_IGN);
	while (1) {
		int pollout;
		uint32_t timeout = -1;
		pollfds.len = n_fds;
		slirp_pollfds_fill(&pollfds, &timeout);
		update_ra_timeout(&timeout);
		do
			pollout = poll(pollfds.pfd, pollfds.len, timeout);
		while (pollout < 0 && errno == EINTR);
		if (pollout < 0) {
			goto err;
		}

		if (pollfds.pfd[0].revents) {
			ssize_t rc = read(tapfd, buf, ETH_BUF_SIZE);
			if (rc < 0) {
				perror("do_slirp: read");
				goto after_slirp_input;
			}
			slirp_input(slirp, buf, (int)rc);
 after_slirp_input:
			pollout = -1;
		}

		/* The exitfd is closed.  */
		if (exitfd >= 0 && pollfds.pfd[1].revents) {
			fprintf(stderr, "exitfd event\n");
			goto success;
		}

		slirp_pollfds_poll(&pollfds, (pollout <= 0));
		check_ra_timeout();
	}
 success:
	ret = 0;
 err:
	fprintf(stderr, "do_slirp is exiting\n");
	if (buf != NULL) {
		free(buf);
	}
	return ret;
}
#endif
