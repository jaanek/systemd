/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Tom Gundersen <teg@jklm.no>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
 ***/

#include <resolv.h>

#include "path-util.h"
#include "networkd.h"
#include "libudev-private.h"
#include "udev-util.h"
#include "mkdir.h"

const char* const network_dirs[] = {
        "/etc/systemd/network",
        "/run/systemd/network",
        "/usr/lib/systemd/network",
#ifdef HAVE_SPLIT_USER
        "/lib/systemd/network",
#endif
        NULL};

int manager_new(Manager **ret) {
        _cleanup_manager_free_ Manager *m = NULL;
        int r;

        m = new0(Manager, 1);
        if (!m)
                return -ENOMEM;

        r = sd_event_default(&m->event);
        if (r < 0)
                return r;

        sd_event_set_watchdog(m->event, true);

        r = sd_rtnl_open(RTMGRP_LINK | RTMGRP_IPV4_IFADDR, &m->rtnl);
        if (r < 0)
                return r;

        r = sd_bus_default_system(&m->bus);
        if (r < 0 && r != -ENOENT) /* TODO: drop when we can rely on kdbus */
                return r;

        m->udev = udev_new();
        if (!m->udev)
                return -ENOMEM;

        m->udev_monitor = udev_monitor_new_from_netlink(m->udev, "udev");
        if (!m->udev_monitor)
                return -ENOMEM;

        m->links = hashmap_new(uint64_hash_func, uint64_compare_func);
        if (!m->links)
                return -ENOMEM;

        m->bridges = hashmap_new(string_hash_func, string_compare_func);
        if (!m->bridges)
                return -ENOMEM;

        LIST_HEAD_INIT(m->networks);

        *ret = m;
        m = NULL;

        return 0;
}

void manager_free(Manager *m) {
        Network *network;
        Bridge *bridge;
        Link *link;

        udev_monitor_unref(m->udev_monitor);
        udev_unref(m->udev);
        sd_bus_unref(m->bus);
        sd_event_source_unref(m->udev_event_source);
        sd_event_unref(m->event);

        while ((network = m->networks))
                network_free(network);

        while ((link = hashmap_first(m->links)))
                link_free(link);
        hashmap_free(m->links);

        while ((bridge = hashmap_first(m->bridges)))
                bridge_free(bridge);
        hashmap_free(m->bridges);

        sd_rtnl_unref(m->rtnl);

        free(m);
}

int manager_load_config(Manager *m) {
        int r;

        /* update timestamp */
        paths_check_timestamp(network_dirs, &m->network_dirs_ts_usec, true);

        r = bridge_load(m);
        if (r < 0)
                return r;

        r = network_load(m);
        if (r < 0)
                return r;

        return 0;
}

bool manager_should_reload(Manager *m) {
        return paths_check_timestamp(network_dirs, &m->network_dirs_ts_usec, false);
}

static int manager_process_link(Manager *m, struct udev_device *device) {
        Link *link;
        int r;

        if (streq_ptr(udev_device_get_action(device), "remove")) {
                uint64_t ifindex;

                log_debug("%s: link removed", udev_device_get_sysname(device));

                ifindex = udev_device_get_ifindex(device);
                link = hashmap_get(m->links, &ifindex);
                if (!link)
                        return 0;

                link_free(link);
        } else {
                r = link_add(m, device, &link);
                if (r < 0) {
                        if (r == -EEXIST)
                                log_debug("%s: link already exists, ignoring",
                                          link->ifname);
                        else
                                log_error("%s: could not handle link: %s",
                                          udev_device_get_sysname(device),
                                          strerror(-r));
                } else
                        log_debug("%s: link (with ifindex %" PRIu64") added",
                                  link->ifname, link->ifindex);
        }

        return 0;
}

int manager_udev_enumerate_links(Manager *m) {
        _cleanup_udev_enumerate_unref_ struct udev_enumerate *e = NULL;
        struct udev_list_entry *item = NULL, *first = NULL;
        int r;

        assert(m);

        e = udev_enumerate_new(m->udev);
        if (!e)
                return -ENOMEM;

        r = udev_enumerate_add_match_subsystem(e, "net");
        if (r < 0)
                return r;

        r = udev_enumerate_add_match_is_initialized(e);
        if (r < 0)
                return r;

        r = udev_enumerate_scan_devices(e);
        if (r < 0)
                return r;

        first = udev_enumerate_get_list_entry(e);
        udev_list_entry_foreach(item, first) {
                _cleanup_udev_device_unref_ struct udev_device *d = NULL;
                int k;

                d = udev_device_new_from_syspath(m->udev, udev_list_entry_get_name(item));
                if (!d)
                        return -ENOMEM;

                k = manager_process_link(m, d);
                if (k < 0)
                        r = k;
        }

        return r;
}

static int manager_dispatch_link_udev(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;
        struct udev_monitor *monitor = m->udev_monitor;
        _cleanup_udev_device_unref_ struct udev_device *device = NULL;

        device = udev_monitor_receive_device(monitor);
        if (!device)
                return -ENOMEM;

        manager_process_link(m, device);
        return 0;
}

int manager_udev_listen(Manager *m) {
        int r;

        r = udev_monitor_filter_add_match_subsystem_devtype(m->udev_monitor, "net", NULL);
        if (r < 0) {
                log_error("Could not add udev monitor filter: %s", strerror(-r));
                return r;
        }

        r = udev_monitor_enable_receiving(m->udev_monitor);
        if (r < 0) {
                log_error("Could not enable udev monitor");
                return r;
        }

        r = sd_event_add_io(m->event,
                        udev_monitor_get_fd(m->udev_monitor),
                        EPOLLIN, manager_dispatch_link_udev,
                        m, &m->udev_event_source);
        if (r < 0)
                return r;

        return 0;
}

static int manager_rtnl_process_link(sd_rtnl *rtnl, sd_rtnl_message *message, void *userdata) {
        Manager *m = userdata;
        Link *link;
        uint64_t ifindex_64;
        int r, ifindex;

        r = sd_rtnl_message_link_get_ifindex(message, &ifindex);
        if (r < 0) {
                log_debug("received RTM_NEWLINK message without valid ifindex");
                return 0;
        }

        ifindex_64 = ifindex;
        link = hashmap_get(m->links, &ifindex_64);
        if (!link) {
                log_debug("received RTM_NEWLINK message for untracked ifindex %d", ifindex);
                return 0;
        }

        /* only track the status of links we want to manage */
        if (link->network) {
                r = link_update(link, message);
                if (r < 0)
                        return 0;
        } else
                log_debug("%s: received RTM_NEWLINK message for unmanaged link", link->ifname);

        return 1;
}

int manager_rtnl_listen(Manager *m) {
        int r;

        r = sd_rtnl_attach_event(m->rtnl, m->event, 0);
        if (r < 0)
                return r;

        r = sd_rtnl_add_match(m->rtnl, RTM_NEWLINK, &manager_rtnl_process_link, m);
        if (r < 0)
                return r;

        return 0;
}

int manager_bus_listen(Manager *m) {
        int r;

        assert(m->event);

        if (!m->bus) /* TODO: drop when we can rely on kdbus */
                return 0;

        r = sd_bus_attach_event(m->bus, m->event, 0);
        if (r < 0)
                return r;

        return 0;
}

static void append_dns(FILE *f, struct in_addr *dns, unsigned char family, unsigned *count) {
        char buf[INET6_ADDRSTRLEN];
        const char *address;

        address = inet_ntop(family, dns, buf, INET6_ADDRSTRLEN);
        if (!address) {
                log_warning("Invalid DNS address. Ignoring.");
                return;
        }

        if (*count == MAXNS)
                fputs("# Too many DNS servers configured, the following entries "
                      "will be ignored\n", f);

        fprintf(f, "nameserver %s\n", address);

        (*count) ++;
}

int manager_update_resolv_conf(Manager *m) {
        _cleanup_free_ char *temp_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        Link *link;
        Iterator i;
        unsigned count = 0;
        const char *domainname = NULL;
        int r;

        assert(m);

        r = mkdir_safe_label("/run/systemd/network", 0755, 0, 0);
        if (r < 0)
                return r;

        r = fopen_temporary("/run/systemd/network/resolv.conf", &f, &temp_path);
        if (r < 0)
                return r;

        fchmod(fileno(f), 0644);

        fputs("# This file is managed by systemd-networkd(8). Do not edit.\n#\n"
              "# Third party programs must not access this file directly, but\n"
              "# only through the symlink at /etc/resolv.conf. To manage\n"
              "# resolv.conf(5) in a different way, replace the symlink by a\n"
              "# static file or a different symlink.\n\n", f);

        HASHMAP_FOREACH(link, m->links, i) {
                if (link->dhcp) {
                        struct in_addr *nameservers;
                        size_t nameservers_size;

                        if (link->network->dhcp_dns) {
                                r = sd_dhcp_client_get_dns(link->dhcp, &nameservers, &nameservers_size);
                                if (r >= 0) {
                                        unsigned j;

                                        for (j = 0; j < nameservers_size; j++)
                                                append_dns(f, &nameservers[j], AF_INET, &count);
                                }
                        }

                        if (link->network->dhcp_domainname && !domainname) {
                                r = sd_dhcp_client_get_domainname(link->dhcp, &domainname);
                                if (r >= 0)
                                       fprintf(f, "domain %s\n", domainname);
                        }
                }
        }

        HASHMAP_FOREACH(link, m->links, i)
                if (link->network && link->network->dns)
                        append_dns(f, &link->network->dns->in_addr.in,
                                   link->network->dns->family, &count);

        fflush(f);

        if (ferror(f) || rename(temp_path, "/run/systemd/network/resolv.conf") < 0) {
                r = -errno;
                unlink("/run/systemd/network/resolv.conf");
                unlink(temp_path);
                return r;
        }

        return 0;
}
