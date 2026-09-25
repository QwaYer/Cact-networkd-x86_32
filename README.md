# networkd — CactOS network manager

A userspace analogue of `systemd-networkd`: brings the network up after boot.
The CactOS kernel runs no DHCP client, so all address selection happens here.

## Behaviour

1. Wait for the NIC to appear (`/dev/net` `CACT_NETCTL_NETCFG_GET`, `link_up`).
2. Read `/etc/networkd.conf`.
3. `dhcp=yes` (the default) — keep `/sbin/dhcpd` alive
   (see the `Cact-dhcpd-x86_32` repository), restarting it when it exits.
4. `dhcp=no` — apply the static `address`/`gateway`/`dns`
   through `CACT_NETCTL_NETCFG` and watch the link.

## /etc/networkd.conf

```
# interface (CactOS has a single NIC)
interface=eth0
# dhcp=yes | no
dhcp=yes
# the following keys are only used when dhcp=no
address=10.0.2.15/24
gateway=10.0.2.2
dns=8.8.8.8
```

The file is optional: if it is missing, networkd creates it on first run with
the default values (DHCP on `eth0`), after which it can be edited.

## Building and installing

```
meson setup build-meson --cross-file cross/i686-cact-clang.ini -Dcactlib=../CactLibc-x86_32
ninja -C build-meson            # build-meson/networkd
ninja -C build-meson stage      # copy into ../LocalRepoCactOS-x86_32/lib/sbin (-Dlr_sbin)
```

## Starting from init

```
/sbin/networkd &
```

Needs root (applying the config through `CACT_NETCTL_NETCFG`).
