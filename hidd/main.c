/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2003-2004  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation;
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
 *  CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES 
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF 
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS, 
 *  COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS 
 *  SOFTWARE IS DISCLAIMED.
 *
 *
 *  $Id$
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <malloc.h>
#include <syslog.h>
#include <signal.h>
#include <getopt.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/sdp.h>
#include <bluetooth/hidp.h>

#include "hidd.h"

static volatile sig_atomic_t __io_canceled = 0;

static void sig_hup(int sig)
{
}

static void sig_term(int sig)
{
	__io_canceled = 1;
}

static int l2cap_connect(bdaddr_t *src, bdaddr_t *dst, unsigned short psm)
{
	struct sockaddr_l2 addr;
	struct l2cap_options opts;
	int sk;

	if ((sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)) < 0)
		return -1;

	addr.l2_family  = AF_BLUETOOTH;
	bacpy(&addr.l2_bdaddr, src);
	addr.l2_psm = 0;

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(sk);
		return -1;
	}

	opts.imtu = HIDP_DEFAULT_MTU;
	opts.omtu = HIDP_DEFAULT_MTU;
	opts.flush_to = 0xffff;

	setsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts));

	addr.l2_family  = AF_BLUETOOTH;
	bacpy(&addr.l2_bdaddr, dst);
	addr.l2_psm = htobs(psm);

	if (connect(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(sk);
		return -1;
	}

	return sk;
}

static int l2cap_listen(const bdaddr_t *bdaddr, unsigned short psm, int lm, int backlog)
{
	struct sockaddr_l2 addr;
	struct l2cap_options opts;
	int sk;

	if ((sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)) < 0)
		return -1;

	addr.l2_family = AF_BLUETOOTH;
	bacpy(&addr.l2_bdaddr, bdaddr);
	addr.l2_psm = htobs(psm);

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(sk);
		return -1;
	}

	setsockopt(sk, SOL_L2CAP, L2CAP_LM, &lm, sizeof(lm));

	opts.imtu = HIDP_DEFAULT_MTU;
	opts.omtu = HIDP_DEFAULT_MTU;
	opts.flush_to = 0xffff;

	setsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts));

	if (listen(sk, backlog) < 0) {
		close(sk);
		return -1;
	}

	return sk;
}

static int l2cap_accept(int sk, bdaddr_t *bdaddr)
{
	struct sockaddr_l2 addr;
	socklen_t addrlen;
	int nsk;

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	if ((nsk = accept(sk, (struct sockaddr *) &addr, &addrlen)) < 0)
		return -1;

	if (bdaddr)
		bacpy(bdaddr, &addr.l2_bdaddr);

	return nsk;
}

static int request_authentication(bdaddr_t *src, bdaddr_t *dst)
{
	struct hci_conn_info_req *cr;
	char addr[18];
	int err, dd, dev_id;

	ba2str(src, addr);
	dev_id = hci_devid(addr);
	if (dev_id < 0)
		return dev_id;

	dd = hci_open_dev(dev_id);
	if (dd < 0)
		return dd;

	cr = malloc(sizeof(*cr) + sizeof(struct hci_conn_info));
	if (!cr)
		return -ENOMEM;

	bacpy(&cr->bdaddr, dst);
	cr->type = ACL_LINK;
	err = ioctl(dd, HCIGETCONNINFO, (unsigned long) cr);
	if (err < 0) {
		free(cr);
		hci_close_dev(dd);
		return err;
	}

	err = hci_authenticate_link(dd, htobs(cr->conn_info->handle), 25000);

	free(cr);
	hci_close_dev(dd);

	return err;
}

static int request_encryption(bdaddr_t *src, bdaddr_t *dst)
{
	struct hci_conn_info_req *cr;
	char addr[18];
	int err, dd, dev_id;

	ba2str(src, addr);
	dev_id = hci_devid(addr);
	if (dev_id < 0)
		return dev_id;

	dd = hci_open_dev(dev_id);
	if (dd < 0)
		return dd;

	cr = malloc(sizeof(*cr) + sizeof(struct hci_conn_info));
	if (!cr)
		return -ENOMEM;

	bacpy(&cr->bdaddr, dst);
	cr->type = ACL_LINK;
	err = ioctl(dd, HCIGETCONNINFO, (unsigned long) cr);
	if (err < 0) {
		free(cr);
		hci_close_dev(dd);
		return err;
	}

	err = hci_encrypt_link(dd, htobs(cr->conn_info->handle), 1, 25000);

	free(cr);
	hci_close_dev(dd);

	return err;
}

static int create_device(int ctl, int csk, int isk, uint8_t subclass, int nosdp, int encrypt, int timeout)
{
	struct hidp_connadd_req req;
	struct sockaddr_l2 addr;
	socklen_t addrlen;
	bdaddr_t src, dst;
	char bda[18];
	int err;

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	if (getsockname(csk, (struct sockaddr *) &addr, &addrlen) < 0)
		return -1;

	bacpy(&src, &addr.l2_bdaddr);

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	if (getpeername(csk, (struct sockaddr *) &addr, &addrlen) < 0)
		return -1;

	bacpy(&dst, &addr.l2_bdaddr);

	memset(&req, 0, sizeof(req));
	req.ctrl_sock = csk;
	req.intr_sock = isk;
	req.flags     = 0;
	req.idle_to   = timeout * 60;

	if (!nosdp) {
		err = get_hid_device_info(&src, &dst, &req);
		if (err < 0)
			goto error;
	} else
		req.subclass = 0xc0;

	if (subclass != 0x00)
		req.subclass = subclass;

	ba2str(&dst, bda);
	syslog(LOG_INFO, "New HID device %s (%s)", bda, req.name);

	if (encrypt && (req.subclass & 0x40)) {
		err = request_authentication(&src, &dst);
		if (err < 0) {
			syslog(LOG_ERR, "Authentication for %s failed", bda);
			goto error;
		}

		err = request_encryption(&src, &dst);
		if (err < 0)
			syslog(LOG_ERR, "Encryption for %s failed", bda);
	}

	err = ioctl(ctl, HIDPCONNADD, &req);

error:
	if (req.rd_data)
		free(req.rd_data);

	return err;
}

static void run_server(int ctl, int csk, int isk, uint8_t subclass, int nosdp, int encrypt, int timeout)
{
	struct pollfd p[2];
	short events;
	int err, ncsk, nisk;

	p[0].fd = csk;
	p[0].events = POLLIN | POLLERR | POLLHUP;

	p[1].fd = isk;
	p[1].events = POLLIN | POLLERR | POLLHUP;

	while (!__io_canceled) {
		p[0].revents = 0;
		p[1].revents = 0;

		err = poll(p, 2, 100);
		if (err <= 0)
			continue;

		events = p[0].revents | p[1].revents;

		if (events & POLLIN) {
			ncsk = l2cap_accept(csk, NULL);
			nisk = l2cap_accept(isk, NULL);

			err = create_device(ctl, ncsk, nisk, subclass, nosdp, encrypt, timeout);
			if (err < 0)
				syslog(LOG_ERR, "HID create error %d (%s)",
						errno, strerror(errno));

			close(nisk);
			sleep(1);
			close(ncsk);
		}
	}
}

static char *hidp_state[] = {
	"unknown",
	"connected",
	"open",
	"bound",
	"listening",
	"connecting",
	"connecting",
	"config",
	"disconnecting",
	"closed"
};

static char *hidp_flagstostr(uint32_t flags)
{
	static char str[100];
	str[0] = 0;

	strcat(str, "[");

	if (flags & (1 << HIDP_BOOT_PROTOCOL_MODE))
		strcat(str, "boot-protocol");

	strcat(str, "]");

	return str;
}

static void do_show(int ctl)
{
	struct hidp_connlist_req req;
	struct hidp_conninfo ci[16];
	char addr[18];
	int i;

	req.cnum = 16;
	req.ci   = ci;

	if (ioctl(ctl, HIDPGETCONNLIST, &req) < 0) {
		perror("Can't get connection list");
		close(ctl);
		exit(1);
	}

	for (i = 0; i < req.cnum; i++) {
		ba2str(&ci[i].bdaddr, addr);
		printf("%s %s [%04x:%04x] %s %s\n", addr, ci[i].name,
			ci[i].vendor, ci[i].product, hidp_state[ci[i].state],
			ci[i].flags ? hidp_flagstostr(ci[i].flags) : "");
	}
}

static void do_connect(int ctl, bdaddr_t *src, bdaddr_t *dst, uint8_t subclass, int nosdp, int encrypt, int timeout)
{
	int csk, isk, err;

	csk = l2cap_connect(src, dst, L2CAP_PSM_HIDP_CTRL);
	if (csk < 0) {
		perror("Can't create HID control channel");
		close(ctl);
		exit(1);
	}

	isk = l2cap_connect(src, dst, L2CAP_PSM_HIDP_INTR);
	if (isk < 0) {
		perror("Can't create HID interrupt channel");
		close(csk);
		close(ctl);
		exit(1);
	}

	err = create_device(ctl, csk, isk, subclass, nosdp, encrypt, timeout);
	if (err < 0) {
		fprintf(stderr, "HID create error %d (%s)\n",
						errno, strerror(errno));
		close(isk);
		sleep(1);
		close(csk);
		close(ctl);
		exit(1);
	}
}

static void do_search(int ctl, bdaddr_t *bdaddr, uint8_t subclass, int nosdp, int encrypt, int timeout)
{
	inquiry_info *info = NULL;
	bdaddr_t src, dst;
	int i, dev_id, num_rsp, length, flags;
	char addr[18];
	uint8_t class[3];

	ba2str(bdaddr, addr);
	dev_id = hci_devid(addr);
	if (dev_id < 0) {
		dev_id = hci_get_route(NULL);
		hci_devba(dev_id, &src);
	} else
		bacpy(&src, bdaddr);

	length  = 8;	/* ~10 seconds */
	num_rsp = 0;
	flags   = IREQ_CACHE_FLUSH;

	printf("Searching ...\n");

	num_rsp = hci_inquiry(dev_id, length, num_rsp, NULL, &info, flags);

	for (i = 0; i < num_rsp; i++) {
		memcpy(class, (info+i)->dev_class, 3);
		if (class[1] == 0x25 && (class[2] == 0x00 || class[2] == 0x01)) {
			bacpy(&dst, &(info+i)->bdaddr);
			ba2str(&dst, addr);

			printf("\tConnecting to device %s\n", addr);
			do_connect(ctl, &src, &dst, subclass, nosdp, encrypt, timeout);
		}
	}

	free(info);

	if (!num_rsp) {
		fprintf(stderr, "\tNo devices in range or visible\n");
		close(ctl);
		exit(1);
	}
}

static void do_kill(int ctl, bdaddr_t *bdaddr, uint32_t flags)
{
	struct hidp_conndel_req req;
	struct hidp_connlist_req cl;
	struct hidp_conninfo ci[16];
	int i;

	if (!bacmp(bdaddr, BDADDR_ALL)) {
		cl.cnum = 16;
		cl.ci   = ci;

		if (ioctl(ctl, HIDPGETCONNLIST, &cl) < 0) {
			perror("Can't get connection list");
			close(ctl);
			exit(1);
		}

		for (i = 0; i < cl.cnum; i++) {
			bacpy(&req.bdaddr, &ci[i].bdaddr);
			req.flags = flags;

			if (ioctl(ctl, HIDPCONNDEL, &req) < 0) {
				perror("Can't release connection");
				close(ctl);
				exit(1);
			}
		}

	} else {
		bacpy(&req.bdaddr, bdaddr);
		req.flags = flags;

		if (ioctl(ctl, HIDPCONNDEL, &req) < 0) {
			perror("Can't release connection");
			close(ctl);
			exit(1);
		}
	}
}

static void usage(void)
{
	printf("hidd - Bluetooth HID daemon version %s\n\n", VERSION);

	printf("Usage:\n"
		"\thidd [options] [commands]\n"
		"\n");

	printf("Options:\n"
		"\t-i <hciX|bdaddr>     Local HCI device or BD Address\n"
		"\t-t <timeout>         Set idle timeout (in minutes)\n"
		"\t-b <subclass>        Overwrite the boot mode subclass\n"
		"\t-n, --nodaemon       Don't fork daemon to background\n"
		"\t-h, --help           Display help\n"
		"\n");

	printf("Commands:\n"
		"\t--server             Start HID server\n"
		"\t--search             Search for HID devices\n"
		"\t--connect <bdaddr>   Connect remote HID device\n"
		"\t--unplug <bdaddr>    Unplug the HID connection\n"
		"\t--kill <bdaddr>      Terminate HID connection\n"
		"\t--killall            Terminate all connections\n"
		"\t--show               List current HID connections\n"
		"\n");
}

static struct option main_options[] = {
	{ "help",	0, 0, 'h' },
	{ "nodaemon",	0, 0, 'n' },
	{ "subclass",	1, 0, 'b' },
	{ "timeout",	1, 0, 't' },
	{ "device",	1, 0, 'i' },
	{ "master",	0, 0, 'M' },
	{ "encrypt",	0, 0, 'E' },
	{ "nosdp",	0, 0, 'D' },
	{ "show",	0, 0, 'l' },
	{ "list",	0, 0, 'l' },
	{ "server",	0, 0, 'd' },
	{ "listen",	0, 0, 'd' },
	{ "search",	0, 0, 's' },
	{ "create",	1, 0, 'c' },
	{ "connect",	1, 0, 'c' },
	{ "disconnect",	1, 0, 'k' },
	{ "terminate",	1, 0, 'k' },
	{ "release",	1, 0, 'k' },
	{ "kill",	1, 0, 'k' },
	{ "killall",	0, 0, 'K' },
	{ "unplug",	1, 0, 'u' },
	{ 0, 0, 0, 0 }
};

int main(int argc, char *argv[])
{
	struct sigaction sa;
	bdaddr_t bdaddr, dev;
	uint32_t flags = 0;
	uint8_t subclass = 0x00;
	char addr[18];
	int log_option = LOG_NDELAY | LOG_PID;
	int opt, fd, ctl, csk, isk;
	int mode = 0, daemon = 1, nosdp = 0, encrypt = 0, timeout = 30, lm = 0;

	bacpy(&bdaddr, BDADDR_ANY);

	while ((opt = getopt_long(argc, argv, "+i:nt:b:MEDldsc:k:Ku:h", main_options, NULL)) != -1) {
		switch(opt) {
		case 'i':
			if (!strncasecmp(optarg, "hci", 3))
				hci_devba(atoi(optarg + 3), &bdaddr);
			else
				str2ba(optarg, &bdaddr);
			break;
		case 'n':
			daemon = 0;
			break;
		case 't':
			timeout = atoi(optarg);
			break;
		case 'b':
			if (!strncasecmp(optarg, "0x", 2))
				subclass = (uint8_t) strtol(optarg, NULL, 16);
			else
				subclass = atoi(optarg);
			break;
		case 'M':
			lm |= L2CAP_LM_MASTER;
			break;
		case 'E':
			encrypt = 1;
			break;
		case 'D':
			nosdp = 1;
			break;
		case 'l':
			mode = 0;
			break;
		case 'd':
			mode = 1;
			break;
		case 's':
			mode = 2;
			break;
		case 'c':
			str2ba(optarg, &dev);
			mode = 3;
			break;
		case 'k':
			str2ba(optarg, &dev);
			mode = 4;
			break;
		case 'K':
			bacpy(&dev, BDADDR_ALL);
			mode = 4;
			break;
		case 'u':
			str2ba(optarg, &dev);
			flags = (1 << HIDP_VIRTUAL_CABLE_UNPLUG);
			mode = 4;
			break;
		case 'h':
			usage();
			exit(0);
		default:
			exit(0);
		}
	}

	ba2str(&bdaddr, addr);

	ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HIDP);
	if (ctl < 0) {
		perror("Can't open HIDP control socket");
		exit(1);
	}

	switch (mode) {
	case 1:
		csk = l2cap_listen(&bdaddr, L2CAP_PSM_HIDP_CTRL, lm, 10);
		if (csk < 0) {
			perror("Can't listen on HID control channel");
			close(ctl);
			exit(1);
		}

		isk = l2cap_listen(&bdaddr, L2CAP_PSM_HIDP_INTR, lm, 10);
		if (isk < 0) {
			perror("Can't listen on HID interrupt channel");
			close(ctl);
			close(csk);
			exit(1);
		}
		break;

	case 2:
		do_search(ctl, &bdaddr, subclass, nosdp, encrypt, timeout);
		close(ctl);
		exit(0);

	case 3:
		do_connect(ctl, &bdaddr, &dev, subclass, nosdp, encrypt, timeout);
		close(ctl);
		exit(0);

	case 4:
		do_kill(ctl, &dev, flags);
		close(ctl);
		exit(0);

	default:
		do_show(ctl);
		close(ctl);
		exit(0);
	}

	if (daemon) {
		if (fork())
			exit(0);

		fd = open("/dev/null", O_RDWR);
		dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
		close(fd);

		setsid();
		chdir("/");
	} else
		log_option |= LOG_PERROR;

	openlog("hidd", log_option, LOG_DAEMON);

	if (bacmp(&bdaddr, BDADDR_ANY))
		syslog(LOG_INFO, "Bluetooth HID daemon (%s)", addr);
	else
		syslog(LOG_INFO, "Bluetooth HID daemon");

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_NOCLDSTOP;

	sa.sa_handler = sig_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);
	sa.sa_handler = sig_hup;
	sigaction(SIGHUP, &sa, NULL);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);

	run_server(ctl, csk, isk, subclass, nosdp, encrypt, timeout);

	syslog(LOG_INFO, "Exit");

	close(csk);
	close(isk);
	close(ctl);

	return 0;
}
