# networkd — сетевой менеджер CactOS

Юзерспейсный аналог `systemd-networkd`: поднимает сеть после загрузки.
Ядро CactOS DHCP-клиент не выполняет, поэтому весь выбор адресации — здесь.

## Поведение

1. Ждёт появления NIC (`/dev/net` `CACT_NETCTL_NETCFG_GET`, `link_up`).
2. Читает `/etc/networkd.conf`.
3. `dhcp=yes` (по умолчанию) — держит живым `/sbin/dhcpd`
   (см. репозиторий `Cact-dhcpd-x86_32`), перезапуская его при завершении.
4. `dhcp=no` — применяет статический `address`/`gateway`/`dns`
   через `CACT_NETCTL_NETCFG` и следит за линком.

## /etc/networkd.conf

```
# интерфейс (в CactOS карта одна)
interface=eth0
# dhcp=yes | no
dhcp=yes
# следующие ключи используются только при dhcp=no
address=10.0.2.15/24
gateway=10.0.2.2
dns=8.8.8.8
```

Файл необязателен: без него networkd включает DHCP.

## Сборка и установка

```
make CACTLIB=../CactLibc-x86_32
make install LR_SBIN=../LocalRepoCactOS-x86_32/lib/sbin
```

## Запуск из init

```
/sbin/networkd &
```

Нужен root (применение конфига через `CACT_NETCTL_NETCFG`).
