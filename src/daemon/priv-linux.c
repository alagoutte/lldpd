/* -*- mode: c; c-file-style: "openbsd" -*- */
/*
 * Copyright (c) 2008 Vincent Bernat <bernat@luffy.cx>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "lldpd.h"

#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <regex.h>
#include <sys/ioctl.h>
#include <netpacket/packet.h> /* For sockaddr_ll */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"
#endif
#include <linux/filter.h>     /* For BPF filtering */
#include <linux/ethtool.h>
#include <linux/sockios.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/* Proxy for open */
int
priv_open(char *file)
{
	int len, rc;
	enum priv_cmd cmd = PRIV_OPEN;
	must_write(PRIV_UNPRIVILEGED, &cmd, sizeof(enum priv_cmd));
	len = strlen(file);
	must_write(PRIV_UNPRIVILEGED, &len, sizeof(int));
	must_write(PRIV_UNPRIVILEGED, file, len);
	priv_wait();
	must_read(PRIV_UNPRIVILEGED, &rc, sizeof(int));
	if (rc == -1)
		return rc;
	return receive_fd(PRIV_UNPRIVILEGED);
}

static int
asroot_ethtool_real(const char *ifname, struct ethtool_link_usettings *uset) {
	int rc, sock = -1;
	struct ifreq ifr = {};
	if ((rc = sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		return rc;
	}
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

	/* Try with ETHTOOL_GLINKSETTINGS first */
	struct {
		struct ethtool_link_settings req;
		uint32_t link_mode_data[3 * ETHTOOL_LINK_MODE_MASK_MAX_KERNEL_NU32];
	} ecmd;
	static int8_t nwords = 0;

	if (nwords == 0) {
		/* Do a handshake first. We assume that this is device-independant. */
		memset(&ecmd, 0, sizeof(ecmd));
		ecmd.req.cmd = ETHTOOL_GLINKSETTINGS;
		ifr.ifr_data = (caddr_t)&ecmd;
		rc = ioctl(sock, SIOCETHTOOL, &ifr);
		if (rc == 0) {
			nwords = -ecmd.req.link_mode_masks_nwords;
			log_debug("privsep", "glinksettings nwords is %" PRId8, nwords);
		}
	}
	if (nwords > 0) {
		memset(&ecmd, 0, sizeof(ecmd));
		ecmd.req.cmd = ETHTOOL_GLINKSETTINGS;
		ecmd.req.link_mode_masks_nwords = nwords;
		ifr.ifr_data = (caddr_t)&ecmd;
		rc = ioctl(sock, SIOCETHTOOL, &ifr);
		if (rc == 0) {
			log_debug("privsep", "got ethtool results for %s with GLINKSETTINGS",
			    ifname);
			memcpy(&uset->base, &ecmd.req, sizeof(uset->base));
			unsigned int u32_offs = 0;
			memcpy(uset->link_modes.supported,
			    &ecmd.link_mode_data[u32_offs],
			    4 * ecmd.req.link_mode_masks_nwords);
			u32_offs += ecmd.req.link_mode_masks_nwords;
			memcpy(uset->link_modes.advertising,
			    &ecmd.link_mode_data[u32_offs],
			    4 * ecmd.req.link_mode_masks_nwords);
			u32_offs += ecmd.req.link_mode_masks_nwords;
			memcpy(uset->link_modes.lp_advertising,
			    &ecmd.link_mode_data[u32_offs],
			    4 * ecmd.req.link_mode_masks_nwords);
			goto end;
		}
	}

	/* Try with ETHTOOL_GSET */
	struct ethtool_cmd ethc;
	memset(&ethc, 0, sizeof(ethc));
	ethc.cmd = ETHTOOL_GSET;
	ifr.ifr_data = (caddr_t)&ethc;
	rc = ioctl(sock, SIOCETHTOOL, &ifr);
	if (rc == 0) {
		/* Do a partial copy (only what we need) */
		log_debug("privsep", "got ethtool results for %s with GSET",
		    ifname);
		memset(uset, 0, sizeof(*uset));
		uset->base.cmd = ETHTOOL_GSET;
		uset->base.link_mode_masks_nwords = 1;
		uset->link_modes.supported[0] = ethc.supported;
		uset->link_modes.advertising[0] = ethc.advertising;
		uset->link_modes.lp_advertising[0] = ethc.lp_advertising;
		uset->base.speed = (ethc.speed_hi << 16) | ethc.speed;
		uset->base.duplex = ethc.duplex;
		uset->base.port = ethc.port;
		uset->base.autoneg = ethc.autoneg;
	}
end:
	close(sock);
	return rc;
}

/* Proxy for ethtool ioctl (GSET/GLINKSETTINGS only). Not needed since
 * 0fdc100bdc4b7ab61ed632962c76dfe539047296 (2.6.37). But needed until
 * 8006f6bf5e39f11c697f48df20382b81d2f2f8b8 (4.9). */
int
priv_ethtool(char *ifname, struct ethtool_link_usettings *uset)
{
	int rc;
	int len;
	enum priv_cmd cmd = PRIV_ETHTOOL;
	must_write(PRIV_UNPRIVILEGED, &cmd, sizeof(enum priv_cmd));
	len = strlen(ifname);
	must_write(PRIV_UNPRIVILEGED, &len, sizeof(int));
	must_write(PRIV_UNPRIVILEGED, ifname, len);
	priv_wait();
	must_read(PRIV_UNPRIVILEGED, &rc, sizeof(int));
	if (rc != 0)
		return rc;
	must_read(PRIV_UNPRIVILEGED, uset, sizeof(struct ethtool_link_usettings));
	return rc;
}

void
asroot_open()
{
	const char* authorized[] = {
		"/proc/sys/net/ipv4/ip_forward",
		"/proc/net/bonding/[^.][^/]*",
		"/proc/self/net/bonding/[^.][^/]*",
#ifdef ENABLE_OLDIES
		SYSFS_CLASS_NET "[^.][^/]*/brforward",
		SYSFS_CLASS_NET "[^.][^/]*/brport",
		SYSFS_CLASS_NET "[^.][^/]*/brif/[^.][^/]*/port_no",
#endif
		SYSFS_CLASS_DMI "product_version",
		SYSFS_CLASS_DMI "product_serial",
		SYSFS_CLASS_DMI "product_name",
		SYSFS_CLASS_DMI "bios_version",
		SYSFS_CLASS_DMI "sys_vendor",
		SYSFS_CLASS_DMI "chassis_asset_tag",
		NULL
	};
	const char **f;
	char *file;
	int fd, len, rc;
	regex_t preg;

	must_read(PRIV_PRIVILEGED, &len, sizeof(len));
	if ((file = (char *)malloc(len + 1)) == NULL)
		fatal("privsep", NULL);
	must_read(PRIV_PRIVILEGED, file, len);
	file[len] = '\0';

	for (f=authorized; *f != NULL; f++) {
		if (regcomp(&preg, *f, REG_NOSUB) != 0)
			/* Should not happen */
			fatal("privsep", "unable to compile a regex");
		if (regexec(&preg, file, 0, NULL, 0) == 0) {
			regfree(&preg);
			break;
		}
		regfree(&preg);
	}
	if (*f == NULL) {
		log_warnx("privsep", "not authorized to open %s", file);
		rc = -1;
		must_write(PRIV_PRIVILEGED, &rc, sizeof(int));
		free(file);
		return;
	}
	if ((fd = open(file, O_RDONLY)) == -1) {
		rc = -1;
		must_write(PRIV_PRIVILEGED, &rc, sizeof(int));
		free(file);
		return;
	}
	free(file);
	must_write(PRIV_PRIVILEGED, &fd, sizeof(int));
	send_fd(PRIV_PRIVILEGED, fd);
	close(fd);
}

void
asroot_ethtool()
{
	struct ethtool_link_usettings uset;
	int len, rc;
	char *ifname;

	must_read(PRIV_PRIVILEGED, &len, sizeof(int));
	if ((ifname = (char*)malloc(len + 1)) == NULL)
		fatal("privsep", NULL);
	must_read(PRIV_PRIVILEGED, ifname, len);
	ifname[len] = '\0';
	rc = asroot_ethtool_real(ifname, &uset);
	free(ifname);
	must_write(PRIV_PRIVILEGED, &rc, sizeof(int));
	if (rc == -1) return;
	must_write(PRIV_PRIVILEGED, &uset, sizeof(struct ethtool_link_usettings));
}

int
asroot_iface_init_os(int ifindex, char *name, int *fd)
{
	int rc;
	/* Open listening socket to receive/send frames */
	if ((*fd = socket(PF_PACKET, SOCK_RAW,
		    htons(ETH_P_ALL))) < 0) {
		rc = errno;
		return rc;
	}

	struct sockaddr_ll sa = {
		.sll_family = AF_PACKET,
		.sll_ifindex = ifindex
	};
	if (bind(*fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
		rc = errno;
		log_warn("privsep",
		    "unable to bind to raw socket for interface %s",
		    name);
		return rc;
	}

	/* Set filter */
	log_debug("privsep", "set BPF filter for %s", name);
	static struct sock_filter lldpd_filter_f[] = { LLDPD_FILTER_F };
	struct sock_fprog prog = {
		.filter = lldpd_filter_f,
		.len = sizeof(lldpd_filter_f) / sizeof(struct sock_filter)
	};
	if (setsockopt(*fd, SOL_SOCKET, SO_ATTACH_FILTER,
                &prog, sizeof(prog)) < 0) {
		rc = errno;
		log_warn("privsep", "unable to change filter for %s", name);
		return rc;
	}

#ifdef SO_LOCK_FILTER
	int enable = 1;
	if (setsockopt(*fd, SOL_SOCKET, SO_LOCK_FILTER,
		&enable, sizeof(enable)) < 0) {
		if (errno != ENOPROTOOPT) {
			rc = errno;
			log_warn("privsep", "unable to lock filter for %s", name);
			return rc;
		}
	}
#endif

	return 0;
}

int
asroot_iface_description_os(const char *name, const char *description)
{
	/* We could use netlink but this is a lot to do in a privileged
	 * process. Just write to /sys/class/net/XXXX/ifalias. */
	char *file;
#ifndef IFALIASZ
# define IFALIASZ 256
#endif
	char descr[IFALIASZ];
	FILE *fp;
	int rc;
	if (name[0] == '\0' || name[0] == '.') {
		log_warnx("privsep", "odd interface name %s", name);
		return -1;
	}
	if (asprintf(&file, SYSFS_CLASS_NET "%s/ifalias", name) == -1) {
		log_warn("privsep", "unable to allocate memory for setting description");
		return -1;
	}
	if ((fp = fopen(file, "r+")) == NULL) {
		rc = errno;
		free(file);
		log_debug("privsep", "cannot open interface description for %s",
		    name);
		return rc;
	}
	free(file);
	if (strlen(description) == 0 &&
	    fgets(descr, sizeof(descr), fp) != NULL) {
		if (strncmp(descr, "lldpd: ", 7) == 0) {
			if (strncmp(descr + 7, "was ", 4) == 0) {
				/* Already has an old neighbor */
				fclose(fp);
				return 0;
			} else {
				/* Append was */
				memmove(descr + 11, descr + 7,
				    sizeof(descr) - 11);
				memcpy(descr, "lldpd: was ", 11);
			}
		} else {
			/* No description, no neighbor */
			strlcpy(descr, "lldpd: no neighbor", sizeof(descr));
		}
	} else
		snprintf(descr, sizeof(descr), "lldpd: connected to %s", description);
	if (fputs(descr, fp) == EOF) {
		log_debug("privsep", "cannot set interface description for %s",
		    name);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

int
asroot_iface_promisc_os(const char *name)
{
	int s, rc;
	if ((s = socket(PF_PACKET, SOCK_RAW,
		    htons(ETH_P_ALL))) < 0) {
		rc = errno;
		log_warn("privsep", "unable to open raw socket");
		return rc;
	}

	struct ifreq ifr = {};
	strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));

	if (ioctl(s, SIOCGIFFLAGS, &ifr) == -1) {
		rc = errno;
		log_warn("privsep", "unable to get interface flags for %s",
		    name);
		close(s);
		return rc;
	}

	if (ifr.ifr_flags & IFF_PROMISC) {
		close(s);
		return 0;
	}
	ifr.ifr_flags |= IFF_PROMISC;
	if (ioctl(s, SIOCSIFFLAGS, &ifr) == -1) {
		rc = errno;
		log_warn("privsep", "unable to set promisc mode for %s",
		    name);
		close(s);
		return rc;
	}
	log_info("privsep", "promiscuous mode enabled for %s", name);
	close(s);
	return 0;
}
