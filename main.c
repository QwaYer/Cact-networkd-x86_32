/*
 * networkd — сетевой менеджер CactOS (упрощённый аналог systemd-networkd).
 *
 * Запускается при загрузке (обычно из init), ждёт появления сетевой карты и
 * приводит её в рабочее состояние:
 *
 *   - если в /etc/networkd.conf указано dhcp=no  — применяет статический
 *     адрес (address/gateway/dns) через /dev/net CACT_NETCTL_NETCFG;
 *   - если dhcp=yes (по умолчанию) — поддерживает живым /sbin/dhcpd
 *     (отдельный репозиторий Cact-dhcpd-x86_32), перезапуская его при сбое.
 *
 * Ядро DHCP-клиент не содержит: вся адресация приходит из юзерспейса.
 *
 * /etc/networkd.conf (все ключи необязательны):
 *   interface=eth0
 *   dhcp=yes|no
 *   address=10.0.2.15/24
 *   gateway=10.0.2.2
 *   dns=8.8.8.8
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <wait.h>
#include <ioctl_abi.h>

#define CONFIG_PATH "/etc/networkd.conf"
#define DHCPD_PATH  "/sbin/dhcpd"

#define IFACE_DEFAULT "eth0"

typedef struct {
    char     iface[32];
    int      dhcp;           /* 1 = DHCP (default), 0 = static */
    uint32_t address_host;   /* 0 = не задан */
    uint32_t netmask_host;   /* 0 = не задан */
    uint32_t gateway_host;
    uint32_t dns_host;
} netconf_t;

static void netconf_defaults(netconf_t *c) {
    memset(c, 0, sizeof(*c));
    strncpy(c->iface, IFACE_DEFAULT, sizeof(c->iface) - 1);
    c->dhcp = 1;
}

static int parse_ipv4(const char *s, uint32_t *out) {
    uint32_t r = 0;
    int dots = 0, val = 0, has_digit = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (val > 255) return -1;
            has_digit = 1;
        } else if (*s == '.') {
            if (!has_digit) return -1;
            r = (r << 8) | (uint32_t)val;
            val = 0; has_digit = 0;
            if (++dots > 3) return -1;
        } else {
            return -1;
        }
        s++;
    }
    if (!has_digit || dots != 3) return -1;
    r = (r << 8) | (uint32_t)val;
    *out = r;
    return 0;
}

/* "A.B.C.D" или "A.B.C.D/prefix" -> адрес и префикс (по умолчанию 24). */
static int parse_address(const char *s, uint32_t *ip, int *prefix) {
    char tmp[32];
    int n = 0;
    const char *slash = 0;
    for (const char *p = s; *p && n < (int)sizeof(tmp) - 1; p++) {
        if (*p == '/') slash = tmp + n;
        tmp[n++] = *p;
    }
    tmp[n] = '\0';
    if (parse_ipv4(tmp, ip) < 0) return -1;
    int pre = 24;
    if (slash) {
        pre = 0;
        for (const char *p = slash + 1; *p >= '0' && *p <= '9'; p++)
            pre = pre * 10 + (*p - '0');
        if (pre < 0 || pre > 32) return -1;
    }
    *prefix = pre;
    return 0;
}

static int prefix_to_mask(int prefix) {
    return (prefix == 0) ? 0 : (int)(0xFFFFFFFFu << (32 - prefix));
}

static int yesno(const char *v) {
    if (v[0] == 'y' || v[0] == 'Y' || v[0] == '1') return 1;
    return 0;
}

static void config_load(netconf_t *c) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        printf("networkd: no %s, using DHCP on %s\n", CONFIG_PATH, c->iface);
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char *eq = p;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        int vlen = (int)strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';

        if (strcmp(key, "interface") == 0) {
            strncpy(c->iface, val, sizeof(c->iface) - 1);
        } else if (strcmp(key, "dhcp") == 0) {
            c->dhcp = yesno(val);
        } else if (strcmp(key, "address") == 0) {
            int prefix = 24;
            uint32_t ip = 0;
            if (parse_address(val, &ip, &prefix) == 0) {
                c->address_host = ip;
                c->netmask_host = (uint32_t)prefix_to_mask(prefix);
            } else {
                printf("networkd: bad address `%s`\n", val);
            }
        } else if (strcmp(key, "netmask") == 0) {
            uint32_t m = 0;
            if (parse_ipv4(val, &m) == 0)
                c->netmask_host = m;
        } else if (strcmp(key, "gateway") == 0) {
            uint32_t g = 0;
            if (parse_ipv4(val, &g) == 0)
                c->gateway_host = g;
        } else if (strcmp(key, "dns") == 0) {
            uint32_t d = 0;
            if (parse_ipv4(val, &d) == 0)
                c->dns_host = d;
        }
    }
    fclose(f);
    printf("networkd: config: iface=%s dhcp=%s", c->iface,
           c->dhcp ? "yes" : "no");
    if (!c->dhcp) {
        printf(" addr=%u.%u.%u.%u/%d",
               (unsigned)((c->address_host >> 24) & 0xFF),
               (unsigned)((c->address_host >> 16) & 0xFF),
               (unsigned)((c->address_host >> 8) & 0xFF),
               (unsigned)(c->address_host & 0xFF),
               c->netmask_host ? 32 - __builtin_ctz(c->netmask_host) : 0);
        if (c->gateway_host) printf(" gw=%u.%u.%u.%u",
               (unsigned)((c->gateway_host >> 24) & 0xFF),
               (unsigned)((c->gateway_host >> 16) & 0xFF),
               (unsigned)((c->gateway_host >> 8) & 0xFF),
               (unsigned)(c->gateway_host & 0xFF));
        if (c->dns_host) printf(" dns=%u.%u.%u.%u",
               (unsigned)((c->dns_host >> 24) & 0xFF),
               (unsigned)((c->dns_host >> 16) & 0xFF),
               (unsigned)((c->dns_host >> 8) & 0xFF),
               (unsigned)(c->dns_host & 0xFF));
    }
    printf("\n");
}

static int net_get(cact_netcfg_get_t *g) {
    int fd = open("/dev/net", O_RDWR);
    if (fd < 0) return -1;
    memset(g, 0, sizeof(*g));
    int r = ioctl(fd, CACT_NETCTL_NETCFG_GET, g);
    close(fd);
    return r;
}

static int net_apply_static(const netconf_t *c) {
    cact_netcfg_arg_t a;
    memset(&a, 0, sizeof(a));
    a.ip_host      = c->address_host;
    a.netmask_host = c->netmask_host;
    a.gateway_host = c->gateway_host;
    a.dns_host     = c->dns_host;
    int fd = open("/dev/net", O_RDWR);
    if (fd < 0) return -1;
    int r = ioctl(fd, CACT_NETCTL_NETCFG, &a);
    close(fd);
    return r;
}

/* ждать, пока появится карта (link_up), с логом */
static void wait_link_up(const char *iface) {
    cact_netcfg_get_t g;
    for (;;) {
        if (net_get(&g) == 0 && g.link_up) {
            printf("networkd: link up on %s (mac %02x:%02x:%02x:%02x:%02x:%02x)\n",
                   iface,
                   g.mac[0], g.mac[1], g.mac[2], g.mac[3], g.mac[4], g.mac[5]);
            return;
        }
        printf("networkd: waiting for NIC...\n");
        sleep(1);
    }
}

/* DHCP-режим: держим /sbin/dhcpd живым */
static void run_dhcp_mode(void) {
    for (;;) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("networkd: fork failed\n");
            sleep(3);
            continue;
        }
        if (pid == 0) {
            /* ребёнок: сам dhcpd */
            char *args[] = { (char *)DHCPD_PATH, 0 };
            execvp(DHCPD_PATH, args);
            printf("networkd: cannot exec %s\n", DHCPD_PATH);
            _exit(1);
        }
        printf("networkd: dhcpd running (pid=%d)\n", (int)pid);
        int st = 0;
        waitpid(pid, &st, 0);
        printf("networkd: dhcpd exited (status=%d), restarting in 3s\n", st);
        sleep(3);
    }
}

/* Статический режим: применить и держать конфиг */
static void run_static_mode(const netconf_t *c) {
    if (net_apply_static(c) != 0) {
        printf("networkd: failed to apply static config (root needed?)\n");
    } else {
        printf("networkd: static config applied\n");
    }
    for (;;) {
        sleep(30);
        cact_netcfg_get_t g;
        if (net_get(&g) != 0 || !g.link_up) {
            printf("networkd: link lost, waiting for NIC...\n");
            wait_link_up(c->iface);
            if (net_apply_static(c) != 0)
                printf("networkd: failed to re-apply static config\n");
            else
                printf("networkd: static config re-applied\n");
        }
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    printf("networkd: starting\n");
    netconf_t cfg;
    netconf_defaults(&cfg);
    config_load(&cfg);

    printf("networkd: managing %s\n", cfg.iface);
    wait_link_up(cfg.iface);

    if (cfg.dhcp) {
        printf("networkd: using DHCP (dhcpd)\n");
        run_dhcp_mode();
    } else {
        if (cfg.address_host == 0 || cfg.netmask_host == 0) {
            printf("networkd: dhcp=no but no address — falling back to DHCP\n");
            run_dhcp_mode();
        }
        run_static_mode(&cfg);
    }
    return 0;
}
