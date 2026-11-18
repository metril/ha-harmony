# Hub Bluetooth: read-only survey for a BT HID design

Probed live on the rooted hub, 2026-11-18, read-only (no daemon touched). Tags as in the other hardware docs.

## Facts **[MEASURED]**
- Kernel `2.6.31` mips, no `/proc/config.gz`, no debugfs, no `/proc/net/l2cap`; BT is built in (`lsmod` shows none).
  `/proc/net/protocols` lists `L2CAP`, `HCI`, `SCO`, `RFCOMM` (no HIDP, no BNEP). `kallsyms` has `l2cap_sock_{bind,listen,accept,connect,...}`,
  `hci_sock_*`, `bt_sock_*`. `HCI_CHANNEL_USER` (kernel 3.13+) does not exist; only raw/control-less HCI sockets.
- Controller: CSR (CSR8811 class), HCI/LMP 4.0, rev 0x2031, UART (`hciattach ttyS0 bcsp`), BD address stable, `UP RUNNING PSCAN`,
  ACL MTU 310x10, features `ff ff 8f fe db ff 5b 87` (LE and SSP bits set in the controller; the 2.6.31 host stack has no LE).
  Class `0x000100` (computer).
- Running: `hciattach ttyS0 bcsp` (pid ~100), `dbus-daemon --system`, `bluetoothd` (BlueZ **4.101**, started by `/etc/init.d/rcS`
  after `hciconfig hci0 up`, `sspmode 1` and three vendor `hcitool cmd` writes). `krfcommd` kernel thread present. `usbhid -s` is started
  at boot too (stock USB HID helper).
- Tools present: `bluetoothd`, `hciconfig`, `hcitool`, `hciattach`, `dbus-daemon`, `dbus-send`, `libbluetooth.so.3.13.0`.
  Absent: `sdptool`, `bluetoothctl`, `btmgmt`, `hidd`, `rfcomm`, `hcidump`, `strings`, `sort`, `head`, `tail`, `wc`.
- Storage (BlueZ 4 layout, in the unionfs rw layer): `/var/lib/bluetooth/<adapter>/{linkkeys (0600),profiles,names,lastused,sdp,did,classes,config}`;
  `/etc/bluetooth/main.conf` (Class `0x000100`, `DiscoverableTimeout=0`, `InitiallyPowered`, `EnableGatt=true`), `rfcomm.conf`,
  and a FIFO `pin_exchange_fifo` (never read it: a reader blocks forever). `/etc/dbus-1/system.d/bluetooth.conf` exists.
- Upstream (`payload/source/hub_btstack/hci_transport_linux.c`) opens `socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)` bound with
  `HCI_CHANNEL_RAW` on this same kernel and runs BTstack itself above hci0 (bluetoothd not in the data path).

## Conclusion on "own code on kernel L2CAP (PSM 17/19) + SDP record via stock bluetoothd"
**Probably feasible for the sockets, unknown for the SDP half.**
- Yes: kernel L2CAP sockets exist (bind/listen/accept); root can bind PSM 0x11/0x13; BlueZ 4 handles pairing, link keys and
  the adapter, so our process only needs HID framing on two L2CAP sockets.
- Unknown: how to register the HID SDP record (0x1124) with bluetoothd 4.101. Its binary has no `AddRecord`/`org.bluez.Service` text
  and there is no `sdptool`; the dbus policy file would have to be read and `dbus-send` tried (write action, not done here).
  Fallback: a record written through the SDP socket (PSM 1) from our code, or `libbluetooth` `sdp_record_register` over `/var/run/sdp`.
- Risk: bluetoothd's input plugin (host role) may already own PSM 17/19; not provable without a listen attempt (a bind is a write-ish
  action, skipped).
- Alternative proven upstream: stop bluetoothd and run BTstack on raw HCI (licence: non-commercial, ask before reuse).
