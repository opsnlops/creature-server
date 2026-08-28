# Creature Network PTP Setup Guide

This guide documents the working Precision Time Protocol (PTP) configuration for the creature network.

The goal is:

- The **server** is the PTP grandmaster.
- Each **Raspberry Pi 5 or CM5 controller** is a PTP client.
- The server keeps normal NTP enabled.
- Pi controllers disable NTP and synchronize `CLOCK_REALTIME` from PTP.
- PTP runs directly over Ethernet Layer 2.
- The server uses a dedicated Intel I226 NIC for PTP.
- Normal creature/data traffic can continue using the existing network interfaces.

---

# Architecture

```text
Internet / NTP
      |
      v
server CLOCK_REALTIME
      |
      | phc2sys
      v
server Intel I226 PHC
      |
      | Layer-2 PTP
      v
Ethernet switch
      |
      v
Pi 5 / CM5 Ethernet PHC
      |
      | phc2sys
      v
Pi CLOCK_REALTIME
```

The server's PTP NIC does **not** need an IP address because PTP is transported directly over Ethernet.

---

# Packages

Install `linuxptp` and `ethtool` on the server and on every Pi controller:

```bash
sudo apt update
sudo apt install linuxptp ethtool
```

Useful tools installed by `linuxptp` include:

```text
ptp4l
phc2sys
phc_ctl
pmc
```

---

# Server Configuration

## Server hardware

The working server PTP NIC is:

```text
Interface: enp4s0
NIC:       Intel I226-V
Driver:    igc
PHC:       /dev/ptp0
```

The Intel X710/i40e 10 GbE NIC remains available for normal creature-server traffic.

The X710 was **not** used for PTP because its driver advertised hardware receive timestamping but did not actually attach receive timestamps to incoming PTP event packets.

Typical failure:

```text
received SYNC without timestamp
received DELAY_REQ without timestamp
```

Keep the X710 for data. Use the I226 for PTP.

---

## Bring the dedicated PTP NIC up at boot

The PTP interface needs to be administratively up, but it does not need an IPv4 address.

If networking is managed through `/etc/network/interfaces`, add:

```ini
auto enp4s0
iface enp4s0 inet manual
```

Then bring it up immediately:

```bash
sudo ifup enp4s0
```

Verify:

```bash
ip -br link show enp4s0
```

Expected:

```text
enp4s0    UP    ...
```

It is fine for the interface to have only an IPv6 link-local address such as:

```text
fe80::...
```

---

## Verify server hardware timestamping

Run:

```bash
sudo ethtool -T enp4s0
```

Expected capabilities include:

```text
hardware-transmit
hardware-receive
hardware-raw-clock
PTP Hardware Clock: 0
```

For the I226, the receive filter modes should include:

```text
none
all
```

---

## Configure ptp4l on the server

Edit:

```bash
sudoedit /etc/linuxptp/ptp4l.conf
```

Ensure the `[global]` section contains:

```ini
[global]
time_stamping     hardware
network_transport L2
serverOnly        1
hwts_filter       full
```

The important settings are:

- `network_transport L2` — PTP runs directly over Ethernet.
- `serverOnly 1` — this host never becomes a PTP client.
- `hwts_filter full` — request hardware RX timestamps for all received frames.

---

## Enable ptp4l on the server

Disable any old PTP service bound to another NIC:

```bash
sudo systemctl disable --now ptp4l@enp5s0f0np0.service 2>/dev/null || true
```

Enable the I226 instance:

```bash
sudo systemctl enable --now ptp4l@enp4s0.service
```

Verify:

```bash
systemctl status ptp4l@enp4s0.service
```

Healthy output should include:

```text
selected /dev/ptp0 as PTP clock
assuming the grand master role
```

---

## Synchronize the server PHC from CLOCK_REALTIME

The server's normal Linux clock remains disciplined by NTP.

The I226 hardware clock should follow `CLOCK_REALTIME`.

Create:

```bash
sudo tee /etc/systemd/system/phc2sys-server.service >/dev/null <<'EOF'
[Unit]
Description=Synchronize server PTP hardware clock from system clock
Documentation=man:phc2sys
Requires=ptp4l@enp4s0.service
After=ptp4l@enp4s0.service

[Service]
Type=simple
ExecStart=/usr/sbin/phc2sys -s CLOCK_REALTIME -c enp4s0 -w
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF
```

Enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now phc2sys-server.service
```

Verify:

```bash
systemctl status phc2sys-server.service
```

Healthy output looks like:

```text
enp4s0 sys offset       -42 s2 freq ...
enp4s0 sys offset       -48 s2 freq ...
```

Offsets are in nanoseconds.

---

## Verify server NTP

Run:

```bash
timedatectl
```

Expected:

```text
System clock synchronized: yes
NTP service: active
```

This is intentional.

The server's clock hierarchy is:

```text
NTP
 |
 v
CLOCK_REALTIME
 |
 | phc2sys
 v
I226 PHC
 |
 | ptp4l
 v
Layer-2 PTP network
```

---

# Raspberry Pi 5 Controller Configuration

This section is the **default procedure for a normal Raspberry Pi 5**.

Do this first on every Pi.

---

## Step 1: Verify PTP support

Run:

```bash
sudo ethtool -T eth0
sudo ethtool -i eth0
```

A working Pi 5 should show capabilities including:

```text
hardware-transmit
hardware-receive
hardware-raw-clock
```

A normal Pi 5 may show only one provider:

```text
Hardware timestamp provider index: 0
Hardware timestamp provider qualifier: Precise (IEEE 1588 quality)
```

and receive filters:

```text
none
all
```

If provider index `0` is the only provider, **do not create any provider-selection service**.

That is the normal Pi 5 path.

---

## Step 2: Test whether a second timestamp provider exists

Run:

```bash
sudo ethtool -T eth0 index 1 qualifier precise
```

There are two possible outcomes.

### Normal Pi 5

If you get:

```text
netlink error: No such device
```

then the Pi has only provider index `0`.

**Skip the entire CM5 timestamp-provider workaround section below.**

Continue directly to **Configure ptp4l on the Pi**.

### CM5 or other Pi exposing provider index 1

If index `1` exists and shows valid hardware timestamping capabilities, continue to the **CM5 timestamp-provider workaround** section before starting `ptp4l`.

---

# CM5 Timestamp-Provider Workaround

> **Only perform this section if `ethtool -T eth0 index 1 qualifier precise` succeeds.**
>
> Do not perform this on a normal Pi 5 that only exposes provider index 0.

Some CM5 systems expose two timestamp providers.

Provider `0` may be the PHY timestamp provider and may fail with errors such as:

```text
timed out while polling for tx timestamp
send delay request failed
```

The working workaround is to use provider `1`, the MAC timestamp provider.

---

## Select provider index 1

Test it manually:

```bash
sudo ethtool --set-hwtimestamp-cfg eth0 index 1 qualifier precise
```

Verify:

```bash
sudo ethtool --get-hwtimestamp-cfg eth0
```

Expected:

```text
Hardware timestamp provider index: 1
Hardware timestamp provider qualifier: Precise (IEEE 1588 quality)
Hardware Transmit Timestamp Mode:
    on
Hardware Receive Filter Mode:
    all
```

---

## Make provider selection persistent

Create:

```bash
sudo tee /etc/systemd/system/ptp-hwtstamp-pi.service >/dev/null <<'EOF'
[Unit]
Description=Select MAC hardware timestamp provider for PTP
After=network.target

[Service]
Type=oneshot
ExecStart=/usr/sbin/ethtool --set-hwtimestamp-cfg eth0 index 1 qualifier precise
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
```

Enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ptp-hwtstamp-pi.service
```

Verify:

```bash
sudo ethtool --get-hwtimestamp-cfg eth0
```

---

## Make ptp4l wait for provider selection

This step is **CM5-only** and is needed only when `ptp-hwtstamp-pi.service` exists.

Create an override:

```bash
sudo systemctl edit ptp4l@eth0.service
```

Insert:

```ini
[Unit]
Requires=ptp-hwtstamp-pi.service
After=ptp-hwtstamp-pi.service
```

Then:

```bash
sudo systemctl daemon-reload
```

---

# Configure ptp4l on the Pi

This applies to both normal Pi 5 and CM5.

Edit:

```bash
sudoedit /etc/linuxptp/ptp4l.conf
```

Ensure the `[global]` section contains:

```ini
[global]
time_stamping     hardware
network_transport L2
clientOnly        1
hwts_filter       full
```

Important:

- `clientOnly 1` is required.
- Without it, a Pi controller may decide that it should become grandmaster.
- `hwts_filter full` is required for the Pi MAC path because it timestamps all received frames.

---

# Test ptp4l Before Enabling It

Run:

```bash
sudo ptp4l -i eth0 -H -m -2 --hwts_filter full --clientOnly 1
```

For a normal Pi 5, expected startup includes:

```text
selected /dev/ptp0 as PTP clock
new foreign master 380525.fffe.33ef92-1
selected best master clock 380525.fffe.33ef92
LISTENING to UNCALIBRATED
UNCALIBRATED to SLAVE
```

For a CM5 using provider index `1`, the selected clock may instead be:

```text
selected /dev/ptp1 as PTP clock
```

Once synchronized, output should include:

```text
master offset        267 s2 freq ... path delay ...
master offset       -252 s2 freq ... path delay ...
```

Offsets are in nanoseconds.

Press `Ctrl-C` after the test succeeds.

---

# Add the Pi 5 ptp4l boot-delay override

On Raspberry Pi 5 systems, `ptp4l` can start so early in boot that the `macb`
Ethernet driver is not yet ready to accept the hardware timestamp configuration.

The failure typically looks like:

```text
ioctl SIOCSHWTSTAMP failed: Invalid argument
INITIALIZING to FAULTY
```

The same configuration usually works immediately when started manually a few
seconds later.

Make the following override part of the normal Pi 5 setup:

```bash
sudo mkdir -p /etc/systemd/system/ptp4l@eth0.service.d
sudo tee /etc/systemd/system/ptp4l@eth0.service.d/override.conf >/dev/null <<'EOF'
[Unit]
Wants=network-online.target
After=network-online.target

[Service]
ExecStartPre=/bin/sleep 3
EOF
```

Then reload systemd:

```bash
sudo systemctl daemon-reload
```

## CM5 note

If this CM5 also requires the provider-index-1 workaround, the same override file
must contain both the provider dependency and the startup delay:

```ini
[Unit]
Requires=ptp-hwtstamp-pi.service
Wants=network-online.target
After=ptp-hwtstamp-pi.service network-online.target

[Service]
ExecStartPre=/bin/sleep 3
```

# Enable ptp4l on the Pi

Enable the systemd service:

```bash
sudo systemctl enable --now ptp4l@eth0.service
```

Verify:

```bash
systemctl status ptp4l@eth0.service
```

Healthy output should include:

```text
selected best master clock 380525.fffe.33ef92
UNCALIBRATED to SLAVE
master offset ...
```

A controller should **not** say:

```text
assuming the grand master role
```

If it does, verify that `/etc/linuxptp/ptp4l.conf` contains:

```ini
clientOnly 1
```

---

# Synchronize Pi CLOCK_REALTIME from PTP

Create:

```bash
sudo tee /etc/systemd/system/phc2sys-pi.service >/dev/null <<'EOF'
[Unit]
Description=Synchronize Pi system clock from PTP hardware clock
Documentation=man:phc2sys
Requires=ptp4l@eth0.service
After=ptp4l@eth0.service

[Service]
Type=simple
ExecStart=/usr/sbin/phc2sys -s eth0 -c CLOCK_REALTIME -w
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF
```

Enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now phc2sys-pi.service
```

Verify:

```bash
systemctl status phc2sys-pi.service
```

Healthy output resembles:

```text
CLOCK_REALTIME phc offset      1538 s2 freq ...
CLOCK_REALTIME phc offset     -5055 s2 freq ...
```

Offsets are in nanoseconds.

---

# Disable NTP on the Pi

Do not allow NTP and `phc2sys` to discipline `CLOCK_REALTIME` at the same time.

If the Pi uses `systemd-timesyncd`:

```bash
sudo systemctl disable --now systemd-timesyncd
```

If the Pi uses `chrony` instead:

```bash
sudo systemctl disable --now chrony
```

Verify:

```bash
timedatectl
```

Expected:

```text
System clock synchronized: yes
NTP service: inactive
RTC in local TZ: no
```

---

# Reboot Test

Reboot the Pi:

```bash
sudo reboot
```

After it returns, run the following checks.

---

## Normal Pi 5

A normal Pi 5 with only provider index 0 does **not** need a provider-selection service.

Check:

```bash
sudo ethtool -T eth0
systemctl status ptp4l@eth0.service
systemctl status phc2sys-pi.service
timedatectl
```

Expected:

```text
Hardware timestamp provider index: 0
```

and:

```text
selected /dev/ptp0 as PTP clock
UNCALIBRATED to SLAVE
```

and:

```text
NTP service: inactive
```

---

## CM5 Using Provider Index 1

Check:

```bash
sudo ethtool --get-hwtimestamp-cfg eth0
systemctl status ptp-hwtstamp-pi.service
systemctl status ptp4l@eth0.service
systemctl status phc2sys-pi.service
timedatectl
```

Expected:

```text
Hardware timestamp provider index: 1
```

and:

```text
selected /dev/ptp1 as PTP clock
UNCALIBRATED to SLAVE
```

and:

```text
NTP service: inactive
```

---

# Quick Copy/Paste Recipe: Normal Pi 5

This is the shortest happy-path procedure for a normal Pi 5 that exposes only provider index 0.

Install tools:

```bash
sudo apt update
sudo apt install linuxptp ethtool
```

Verify that index 1 does not exist:

```bash
sudo ethtool -T eth0
sudo ethtool -T eth0 index 1 qualifier precise
```

If index 1 returns:

```text
netlink error: No such device
```

continue with the normal Pi path.

Configure `ptp4l`:

```bash
sudoedit /etc/linuxptp/ptp4l.conf
```

Add or update:

```ini
[global]
time_stamping     hardware
network_transport L2
clientOnly        1
hwts_filter       full
```

Test:

```bash
sudo ptp4l -i eth0 -H -m -2 --hwts_filter full --clientOnly 1
```

After it reaches `SLAVE`, press `Ctrl-C`.

Add the Pi 5 boot-delay override:

```bash
sudo mkdir -p /etc/systemd/system/ptp4l@eth0.service.d
sudo tee /etc/systemd/system/ptp4l@eth0.service.d/override.conf >/dev/null <<'EOF'
[Unit]
Wants=network-online.target
After=network-online.target

[Service]
ExecStartPre=/bin/sleep 3
EOF
```

Reload and enable PTP:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ptp4l@eth0.service
```

Create `phc2sys`:

```bash
sudo tee /etc/systemd/system/phc2sys-pi.service >/dev/null <<'EOF'
[Unit]
Description=Synchronize Pi system clock from PTP hardware clock
Documentation=man:phc2sys
Requires=ptp4l@eth0.service
After=ptp4l@eth0.service

[Service]
Type=simple
ExecStart=/usr/sbin/phc2sys -s eth0 -c CLOCK_REALTIME -w
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF
```

Enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now phc2sys-pi.service
```

Disable NTP:

```bash
sudo systemctl disable --now systemd-timesyncd 2>/dev/null || true
sudo systemctl disable --now chrony 2>/dev/null || true
```

Verify:

```bash
systemctl status ptp4l@eth0.service
systemctl status phc2sys-pi.service
timedatectl
```

Reboot:

```bash
sudo reboot
```

Then verify again.

---

# Quick Copy/Paste Recipe: CM5 With Provider Index 1

Only use this procedure if:

```bash
sudo ethtool -T eth0 index 1 qualifier precise
```

succeeds.

Install tools:

```bash
sudo apt update
sudo apt install linuxptp ethtool
```

Create the timestamp provider service:

```bash
sudo tee /etc/systemd/system/ptp-hwtstamp-pi.service >/dev/null <<'EOF'
[Unit]
Description=Select MAC hardware timestamp provider for PTP
After=network.target

[Service]
Type=oneshot
ExecStart=/usr/sbin/ethtool --set-hwtimestamp-cfg eth0 index 1 qualifier precise
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
```

Enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ptp-hwtstamp-pi.service
```

Verify:

```bash
sudo ethtool --get-hwtimestamp-cfg eth0
```

Configure `ptp4l`:

```bash
sudoedit /etc/linuxptp/ptp4l.conf
```

Add or update:

```ini
[global]
time_stamping     hardware
network_transport L2
clientOnly        1
hwts_filter       full
```

Make `ptp4l` wait for provider selection and for the Pi Ethernet driver to settle:

```bash
sudo mkdir -p /etc/systemd/system/ptp4l@eth0.service.d
sudo tee /etc/systemd/system/ptp4l@eth0.service.d/override.conf >/dev/null <<'EOF'
[Unit]
Requires=ptp-hwtstamp-pi.service
Wants=network-online.target
After=ptp-hwtstamp-pi.service network-online.target

[Service]
ExecStartPre=/bin/sleep 3
EOF
```

Reload:

```bash
sudo systemctl daemon-reload
```

Test manually:

```bash
sudo ptp4l -i eth0 -H -m -2 --hwts_filter full --clientOnly 1
```

After it reaches `SLAVE`, press `Ctrl-C`.

Enable PTP:

```bash
sudo systemctl enable --now ptp4l@eth0.service
```

Create `phc2sys`:

```bash
sudo tee /etc/systemd/system/phc2sys-pi.service >/dev/null <<'EOF'
[Unit]
Description=Synchronize Pi system clock from PTP hardware clock
Documentation=man:phc2sys
Requires=ptp4l@eth0.service
After=ptp4l@eth0.service

[Service]
Type=simple
ExecStart=/usr/sbin/phc2sys -s eth0 -c CLOCK_REALTIME -w
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF
```

Enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now phc2sys-pi.service
```

Disable NTP:

```bash
sudo systemctl disable --now systemd-timesyncd 2>/dev/null || true
sudo systemctl disable --now chrony 2>/dev/null || true
```

Verify:

```bash
sudo ethtool --get-hwtimestamp-cfg eth0
systemctl status ptp-hwtstamp-pi.service
systemctl status ptp4l@eth0.service
systemctl status phc2sys-pi.service
timedatectl
```

Reboot:

```bash
sudo reboot
```

Verify again after reboot.

---


# Raspberry Pi 4 Software PTP Fallback

Older Raspberry Pi 4 systems using the built-in `bcmgenet` Ethernet interface do
not expose a PTP Hardware Clock (PHC).

Typical output is:

```text
Capabilities:
    software-transmit
    software-receive
    software-system-clock
PTP Hardware Clock: none
Hardware Transmit Timestamp Modes: none
Hardware Receive Filter Modes: none
```

That means hardware PTP is not available on the built-in Pi 4 NIC.

Software PTP still works well enough for temporary creature-controller duty,
and can typically keep the system clock within tens of microseconds of the
server on a quiet LAN.

The important architectural difference is that there is no PHC and therefore
no `phc2sys` layer:

```text
server CLOCK_REALTIME
        |
server I226 PHC
        |
    Layer-2 PTP
        |
        v
Pi 4 Ethernet
        |
 software timestamps
        |
        v
Pi 4 CLOCK_REALTIME
```

## Install linuxptp

```bash
sudo apt update
sudo apt install linuxptp ethtool
```

## Verify that hardware PTP is unavailable

On the Pi 4, run:

```bash
sudo ethtool -T end0
sudo ethtool -i end0
```

If the NIC shows:

```text
PTP Hardware Clock: none
```

continue with software PTP.

## Disable NTP

Do not allow NTP and `ptp4l` to discipline `CLOCK_REALTIME` at the same time.

For `systemd-timesyncd`:

```bash
sudo systemctl disable --now systemd-timesyncd
```

For `chrony`:

```bash
sudo systemctl disable --now chrony
```

Verify:

```bash
timedatectl
```

Expected:

```text
NTP service: inactive
```

## Test software PTP manually

Run:

```bash
sudo ptp4l -i end0 -S -m -2 --clientOnly 1
```

The important difference from the Pi 5 hardware-PTP command is:

```text
-S
```

which selects software timestamping.

A healthy startup should look similar to:

```text
new foreign master 380525.fffe.33ef92-1
selected best master clock 380525.fffe.33ef92
LISTENING to UNCALIBRATED
UNCALIBRATED to SLAVE
master offset ...
```

There will be no line such as:

```text
selected /dev/ptp0 as PTP clock
```

because the Pi 4 NIC has no PHC.

Once the servo reaches `s2`, offsets in the low tens of microseconds are normal
for software PTP and are still far tighter than the creature animation frame
interval.

Press `Ctrl-C` after the test succeeds.

## Configure ptp4l permanently

Edit:

```bash
sudoedit /etc/linuxptp/ptp4l.conf
```

Use:

```ini
[global]
time_stamping     software
network_transport L2
clientOnly        1
```

Do not add:

```ini
hwts_filter full
```

because there is no hardware receive timestamp filter.

## Enable ptp4l at boot

For a Pi 4 whose Ethernet interface is named `end0`:

```bash
sudo systemctl enable --now ptp4l@end0.service
```

Verify:

```bash
systemctl status ptp4l@end0.service
```

Healthy output should include:

```text
selected best master clock 380525.fffe.33ef92
LISTENING to UNCALIBRATED
UNCALIBRATED to SLAVE
master offset ...
```

## Do not create a phc2sys service

A Pi 4 software-PTP client has no PHC, so do **not** create or enable:

```text
phc2sys-pi.service
```

`ptp4l` disciplines `CLOCK_REALTIME` directly in software-timestamp mode.

## Verify synchronization

Watch the PTP servo:

```bash
journalctl -u ptp4l@end0.service -f
```

A synchronized Pi 4 should eventually show `s2` entries such as:

```text
master offset      -9052 s2 freq ... path delay ...
master offset      -2455 s2 freq ... path delay ...
master offset     -10533 s2 freq ... path delay ...
```

Then verify:

```bash
timedatectl
```

Expected:

```text
System clock synchronized: yes
NTP service: inactive
```

## Reboot test

```bash
sudo reboot
```

After reboot:

```bash
systemctl status ptp4l@end0.service
timedatectl
```

The Pi should return to `SLAVE` state automatically and report:

```text
System clock synchronized: yes
NTP service: inactive
```

This software-PTP path is intended as a compatibility fallback for older Pi 4
controllers. When the controller is upgraded to a Pi 5, use the hardware-PTP
procedure instead.

---

# Troubleshooting

## ptp4l is FAULTY immediately after boot

Symptom:

```text
ioctl SIOCSHWTSTAMP failed: Invalid argument
INITIALIZING to FAULTY
```

If the same Pi works when `ptp4l` is started manually after boot, this is the
Pi 5 `macb` startup race.

Verify the override exists:

```bash
systemctl cat ptp4l@eth0.service
```

It should include:

```ini
[Unit]
Wants=network-online.target
After=network-online.target

[Service]
ExecStartPre=/bin/sleep 3
```

For a CM5 using provider index 1, it should additionally include:

```ini
Requires=ptp-hwtstamp-pi.service
After=ptp-hwtstamp-pi.service network-online.target
```

Then:

```bash
sudo systemctl daemon-reload
sudo systemctl restart ptp4l@eth0.service
```

## Pi becomes grandmaster

Symptom:

```text
assuming the grand master role
LISTENING to GRAND_MASTER
```

Fix:

```ini
clientOnly 1
```

must be present in:

```text
/etc/linuxptp/ptp4l.conf
```

For manual testing, also include:

```bash
--clientOnly 1
```

---

## received SYNC without timestamp

On Pi MAC hardware, use:

```ini
hwts_filter full
```

or during manual testing:

```bash
--hwts_filter full
```

---

## timed out while polling for tx timestamp on CM5

Symptom:

```text
timed out while polling for tx timestamp
send delay request failed
```

Check whether provider index 1 exists:

```bash
sudo ethtool -T eth0 index 1 qualifier precise
```

If it exists, switch to provider 1:

```bash
sudo ethtool --set-hwtimestamp-cfg eth0 index 1 qualifier precise
```

Then verify:

```bash
sudo ethtool --get-hwtimestamp-cfg eth0
```

---

## ptp-hwtstamp-pi.service fails on a normal Pi 5

If:

```bash
sudo ethtool -T eth0 index 1 qualifier precise
```

returns:

```text
netlink error: No such device
```

then **do not use `ptp-hwtstamp-pi.service`**.

If it was accidentally created, remove it:

```bash
sudo systemctl disable --now ptp-hwtstamp-pi.service 2>/dev/null || true
sudo rm -f /etc/systemd/system/ptp-hwtstamp-pi.service
sudo systemctl daemon-reload
```

If a `ptp4l` override was created that depends on that service, remove it:

```bash
sudo rm -rf /etc/systemd/system/ptp4l@eth0.service.d
sudo systemctl daemon-reload
```

Then continue with the normal Pi 5 procedure.

---

# Healthy Final State

## Server

```bash
systemctl status ptp4l@enp4s0.service
systemctl status phc2sys-server.service
timedatectl
```

Expected:

```text
ptp4l: active
grand master role
phc2sys: active
NTP service: active
```

---

## Pi controller

```bash
systemctl status ptp4l@eth0.service
systemctl status phc2sys-pi.service
timedatectl
```

Expected:

```text
ptp4l: active
state: SLAVE
phc2sys: active
NTP service: inactive
System clock synchronized: yes
```

Typical synchronized offsets should be in the microsecond range or better, which is vastly tighter than the creature animation frame interval.

---

# Final Clock Hierarchy

```text
                    Internet NTP
                         |
                         v
                server CLOCK_REALTIME
                         |
                      phc2sys
                         |
                         v
                 server I226 PHC
                         |
                    Layer-2 PTP
                         |
              +----------+----------+
              |                     |
              v                     v
         Pi 5 / CM5 PHC        Pi 5 / CM5 PHC
              |                     |
           phc2sys                phc2sys
              |                     |
              v                     v
       Pi CLOCK_REALTIME      Pi CLOCK_REALTIME
```

The application can therefore use ordinary synchronized Linux wall-clock timestamps across the creature network.
