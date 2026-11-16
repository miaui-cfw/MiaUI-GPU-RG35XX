#!/usr/bin/env python3
"""usblink_host: a user-space RNDIS host, for the console-to-console USB link.

The console's kernel has no rndis_host driver, so a console in host mode sees
the other console's "Network only" gadget (RNDIS) but can't use it. This does
the driver's job from user space: it drives the gadget through usbfs
(/dev/bus/usb) and passes its Ethernet frames to a TAP interface
(/dev/net/tun), so the link becomes an ordinary network interface:

    Console A (host, this program)          Console B (Network only)
    usblink0  192.168.137.1   <== USB ==>    rndis0  192.168.137.100

RetroArch netplay then runs over plain IP: B hosts, A connects to .100.
B's own DHCP server only leases .1, so A just takes it (no DHCP client).

    usblink_host.py [--ip 192.168.137.1/24] [--ifname usblink0] [--vid 10d6]

It waits for the gadget, survives unplugs and the bounces of plugging in (the
interface stays, with its address; only the USB side starts again), and ends
on SIGINT/SIGTERM. Needs root (usbfs, TAP and the interface set-up).

On a PC that has rndis_host (for testing against a console in ADB + network
mode) it detaches the kernel driver from the RNDIS interfaces only, never
the ADB one, and gives them back when it ends.
"""
import argparse
import ctypes
import errno
import fcntl
import os
import select
import signal
import socket
import struct
import sys
import threading
import time

VERSION = "1"

# ------------------------------------------------------------------ ioctls

libc = ctypes.CDLL(None, use_errno=True)  # CDLL: the GIL is let go during each call
libc.ioctl.argtypes = (ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p)
libc.ioctl.restype = ctypes.c_int


class CtrlTransfer(ctypes.Structure):
    _fields_ = [("bRequestType", ctypes.c_uint8), ("bRequest", ctypes.c_uint8),
                ("wValue", ctypes.c_uint16), ("wIndex", ctypes.c_uint16),
                ("wLength", ctypes.c_uint16), ("timeout", ctypes.c_uint32),
                ("data", ctypes.c_void_p)]


class BulkTransfer(ctypes.Structure):
    _fields_ = [("ep", ctypes.c_uint), ("len", ctypes.c_uint),
                ("timeout", ctypes.c_uint), ("data", ctypes.c_void_p)]


class UsbIoctl(ctypes.Structure):
    _fields_ = [("ifno", ctypes.c_int), ("ioctl_code", ctypes.c_int), ("data", ctypes.c_void_p)]


def _ioc(direction, kind, nr, size):
    return (direction << 30) | (size << 16) | (ord(kind) << 8) | nr


USBDEVFS_CONTROL = _ioc(3, "U", 0, ctypes.sizeof(CtrlTransfer))
USBDEVFS_BULK = _ioc(3, "U", 2, ctypes.sizeof(BulkTransfer))
USBDEVFS_CLAIMINTERFACE = _ioc(2, "U", 15, 4)
USBDEVFS_RELEASEINTERFACE = _ioc(2, "U", 16, 4)
USBDEVFS_IOCTL = _ioc(3, "U", 18, ctypes.sizeof(UsbIoctl))
USBDEVFS_CLEAR_HALT = _ioc(2, "U", 21, 4)
USBDEVFS_DISCONNECT = _ioc(0, "U", 22, 0)
USBDEVFS_CONNECT = _ioc(0, "U", 23, 0)
TUNSETIFF = _ioc(1, "T", 202, 4)
IFF_TAP, IFF_NO_PI = 0x0002, 0x1000
SIOCGIFFLAGS, SIOCSIFFLAGS = 0x8913, 0x8914
SIOCSIFADDR, SIOCSIFNETMASK, SIOCSIFHWADDR = 0x8916, 0x891C, 0x8924
IFF_UP, ARPHRD_ETHER = 0x1, 1


def ioctl(fd, req, arg):
    r = libc.ioctl(fd, req, arg)
    if r < 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e))
    return r


# ------------------------------------------------------------------ RNDIS

MSG_PACKET, MSG_INIT, MSG_HALT, MSG_QUERY, MSG_SET = 1, 2, 3, 4, 5
MSG_INDICATE, MSG_KEEPALIVE = 7, 8
COMPLETION = 0x80000000
OID_802_3_PERMANENT_ADDRESS = 0x01010101
OID_GEN_MAXIMUM_FRAME_SIZE = 0x00010106
OID_GEN_CURRENT_PACKET_FILTER = 0x0001010E
# directed, all-multicast, broadcast, promiscuous: what Linux's rndis_host asks for
PACKET_FILTER = 0x01 | 0x04 | 0x08 | 0x20
PACKET_HDR = 44          # REMOTE_NDIS_PACKET_MSG header; the frame follows it
RX_SIZE = 16384          # what we tell the gadget we take in one transfer
CTRL_TIMEOUT = 5000      # ms


class Gone(Exception):
    """The gadget went away (unplugged, or it stopped answering)."""


# ------------------------------------------------------------------ finding it

SYS_USB = "/sys/bus/usb/devices"


def rd(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def find_rndis(vid=None, pid=None):
    """The first RNDIS gadget on the bus: (sysfs name, control ifnum, data ifnum,
    {"int": ep, "in": ep, "out": ep, "maxp": n}) or None."""
    try:
        names = sorted(os.listdir(SYS_USB))
    except OSError:
        return None
    for dev in names:
        if ":" in dev or dev.startswith("usb"):
            continue
        d = os.path.join(SYS_USB, dev)
        if vid and rd(d + "/idVendor") != vid:
            continue
        if pid and rd(d + "/idProduct") != pid:
            continue
        ifs = {}
        cfg = rd(d + "/bConfigurationValue") or "1"
        for n in names:
            if n.startswith(dev + ":" + cfg + "."):
                i = os.path.join(SYS_USB, n)
                cls = "/".join(rd(i + "/" + a) or "?" for a in
                               ("bInterfaceClass", "bInterfaceSubClass", "bInterfaceProtocol"))
                ifs[int(rd(i + "/bInterfaceNumber") or "0", 16)] = (n, cls)
        for num, (name, cls) in sorted(ifs.items()):
            # RNDIS control: the Windows class (wceis) or the CDC one, then its data interface
            if cls not in ("e0/01/03", "02/02/ff", "ef/04/01") or num + 1 not in ifs:
                continue
            data_name, data_cls = ifs[num + 1]
            if not data_cls.startswith("0a/"):
                continue
            eps = {}
            for iname, kinds in ((name, ("Interrupt",)), (data_name, ("Bulk",))):
                base = os.path.join(SYS_USB, iname)
                for e in sorted(os.listdir(base)):
                    if not e.startswith("ep_"):
                        continue
                    addr = int(rd(base + "/" + e + "/bEndpointAddress"), 16)
                    kind = rd(base + "/" + e + "/type")
                    if kind not in kinds:
                        continue
                    if kind == "Interrupt":
                        eps["int"] = addr
                    elif addr & 0x80:
                        eps["in"] = addr
                    else:
                        eps["out"] = addr
                        eps["maxp"] = int(rd(base + "/" + e + "/wMaxPacketSize") or "0200", 16)
            if {"in", "out"} <= set(eps):
                return dev, num, num + 1, eps
    return None


def usbfs_node(dev):
    d = os.path.join(SYS_USB, dev)
    return "/dev/bus/usb/%03d/%03d" % (int(rd(d + "/busnum")), int(rd(d + "/devnum")))


def driver_of(dev, ifnum):
    cfg = rd(os.path.join(SYS_USB, dev, "bConfigurationValue")) or "1"
    link = os.path.join(SYS_USB, "%s:%s.%d" % (dev, cfg, ifnum), "driver")
    return os.path.basename(os.readlink(link)) if os.path.islink(link) else None


# ------------------------------------------------------------------ the link

class Log:
    def __init__(self, quiet=False):
        self.quiet = quiet
        self.t0 = time.time()

    def __call__(self, msg, always=False):
        if always or not self.quiet:
            sys.stdout.write("%8.2f  %s\n" % (time.time() - self.t0, msg))
            sys.stdout.flush()


class Tap:
    """The network side: a TAP interface that stays for the whole run."""

    def __init__(self, name, ip, log):
        self.name, self.ip, self.log = name, ip, log
        self.fd = os.open("/dev/net/tun", os.O_RDWR)
        ifr = struct.pack("16sH", name.encode(), IFF_TAP | IFF_NO_PI)
        buf = ctypes.create_string_buffer(ifr, 40)
        ioctl(self.fd, TUNSETIFF, buf)
        self.mac = None
        log("TAP %s created" % name)

    # the interface is set up with the kernel's own calls: no ip/ifconfig needed
    def ifreq(self, req, payload=b""):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            buf = struct.pack("16s", self.name.encode()) + payload
            return fcntl.ioctl(sock.fileno(), req, buf.ljust(40, b"\0"))
        finally:
            sock.close()

    def flags(self, up):
        cur = struct.unpack_from("H", self.ifreq(SIOCGIFFLAGS), 16)[0]
        self.ifreq(SIOCSIFFLAGS, struct.pack("H", (cur | IFF_UP) if up else (cur & ~IFF_UP)))

    def addr(self, req, dotted):
        self.ifreq(req, struct.pack("<H", socket.AF_INET) + b"\0\0" + socket.inet_aton(dotted))

    def up(self, mac):
        """Up with the gadget's idea of our MAC (as rndis_host does) and the address."""
        if mac != self.mac:
            self.flags(False)
            self.ifreq(SIOCSIFHWADDR, struct.pack("<H", ARPHRD_ETHER) + bytes.fromhex(mac.replace(":", "")))
            self.mac = mac
        if self.ip:
            ip, bits = (self.ip.split("/") + ["24"])[:2]
            mask = socket.inet_ntoa(struct.pack(">I", (0xffffffff << (32 - int(bits))) & 0xffffffff))
            self.addr(SIOCSIFADDR, ip)
            self.addr(SIOCSIFNETMASK, mask)
        self.flags(True)
        self.log("%s up: %s %s" % (self.name, mac, self.ip or "(no address)"), always=True)

    def close(self):
        os.close(self.fd)


class Session:
    """One plug-in: claims the gadget, sets RNDIS up, and moves frames until it goes."""

    def __init__(self, found, tap, log):
        self.dev, self.ctrl_if, self.data_if, self.eps = found
        self.tap, self.log = tap, log
        self.fd = None
        self.claimed = []
        self.detached = []
        self.req_id = 0
        self.stop = threading.Event()
        self.error = None
        self.stats = {"rx": 0, "tx": 0, "rx_bytes": 0, "tx_bytes": 0, "rx_bad": 0}
        self.ctrl_lock = threading.Lock()

    # --- usbfs
    def open(self):
        node = usbfs_node(self.dev)
        self.fd = os.open(node, os.O_RDWR)
        for ifnum in (self.ctrl_if, self.data_if):
            drv = driver_of(self.dev, ifnum)
            if drv and drv != "usbfs":
                arg = UsbIoctl(ifnum, USBDEVFS_DISCONNECT, None)
                ioctl(self.fd, USBDEVFS_IOCTL, ctypes.byref(arg))
                self.detached.append(ifnum)
                self.log("interface %d: kernel driver %s detached" % (ifnum, drv))
            n = ctypes.c_uint(ifnum)
            ioctl(self.fd, USBDEVFS_CLAIMINTERFACE, ctypes.byref(n))
            self.claimed.append(ifnum)
        self.log("%s (%s) claimed: control if %d, data if %d, eps %s" % (
            self.dev, node, self.ctrl_if, self.data_if,
            ", ".join("%s %s" % (k, hex(v) if k != "maxp" else v) for k, v in sorted(self.eps.items()))))

    def close(self):
        if self.fd is None:
            return
        for ifnum in self.claimed:
            try:
                ioctl(self.fd, USBDEVFS_RELEASEINTERFACE, ctypes.byref(ctypes.c_uint(ifnum)))
            except OSError:
                pass
        # give the kernel driver back (on a PC: its rndis_host); nothing to give on the console
        for ifnum in self.detached:
            try:
                arg = UsbIoctl(ifnum, USBDEVFS_CONNECT, None)
                ioctl(self.fd, USBDEVFS_IOCTL, ctypes.byref(arg))
                self.log("interface %d: kernel driver given back" % ifnum)
            except OSError:
                pass
        os.close(self.fd)
        self.fd = None

    def control(self, request_type, request, value, index, data_or_len, timeout=CTRL_TIMEOUT):
        if isinstance(data_or_len, int):
            buf = ctypes.create_string_buffer(data_or_len)
            length = data_or_len
        else:
            buf = ctypes.create_string_buffer(bytes(data_or_len), len(data_or_len))
            length = len(data_or_len)
        t = CtrlTransfer(request_type, request, value, index, length, timeout,
                         ctypes.cast(buf, ctypes.c_void_p))
        n = ioctl(self.fd, USBDEVFS_CONTROL, ctypes.byref(t))
        return buf.raw[:n]

    def bulk(self, ep, data_or_len, timeout):
        if isinstance(data_or_len, int):
            buf = ctypes.create_string_buffer(data_or_len)
            length = data_or_len
        else:
            buf = ctypes.create_string_buffer(bytes(data_or_len), len(data_or_len))
            length = len(data_or_len)
        t = BulkTransfer(ep, length, timeout, ctypes.cast(buf, ctypes.c_void_p))
        n = ioctl(self.fd, USBDEVFS_BULK, ctypes.byref(t))
        return buf.raw[:n]

    # --- RNDIS control messages
    def command(self, msg_type, body):
        """Send a control message and wait for its completion (polled, as Linux does)."""
        with self.ctrl_lock:
            self.req_id += 1
            rid = self.req_id
            msg = struct.pack("<III", msg_type, 12 + len(body), rid) + body
            # SEND_ENCAPSULATED_COMMAND
            self.control(0x21, 0x00, 0, self.ctrl_if, msg)
            for _ in range(50):
                # GET_ENCAPSULATED_RESPONSE
                r = self.control(0xA1, 0x01, 0, self.ctrl_if, 1025)
                if len(r) >= 8:
                    t, ln = struct.unpack_from("<II", r)
                    if t == msg_type | COMPLETION and len(r) >= 16 and struct.unpack_from("<I", r, 8)[0] == rid:
                        status = struct.unpack_from("<I", r, 12)[0]
                        if status:
                            raise OSError(errno.EIO, "RNDIS 0x%x failed: status 0x%08x" % (msg_type, status))
                        return r
                    if t == MSG_INDICATE:
                        self.log("gadget status: 0x%08x" % struct.unpack_from("<I", r, 8)[0])
                time.sleep(0.02)
            raise OSError(errno.ETIMEDOUT, "no RNDIS answer to message 0x%x" % msg_type)

    def query(self, oid):
        r = self.command(MSG_QUERY, struct.pack("<IIII", oid, 0, 0, 0))
        length, offset = struct.unpack_from("<II", r, 16)
        return r[8 + offset:8 + offset + length]

    def set(self, oid, value):
        self.command(MSG_SET, struct.pack("<IIII", oid, len(value), 20, 0) + value)

    def init(self):
        r = self.command(MSG_INIT, struct.pack("<III", 1, 0, RX_SIZE))
        (major, minor, flags, medium, max_pkts, max_xfer, align) = struct.unpack_from("<7I", r, 16)
        mac = self.query(OID_802_3_PERMANENT_ADDRESS)
        mac = ":".join("%02x" % b for b in mac[:6])
        frame = struct.unpack("<I", self.query(OID_GEN_MAXIMUM_FRAME_SIZE)[:4])[0]
        self.set(OID_GEN_CURRENT_PACKET_FILTER, struct.pack("<I", PACKET_FILTER))
        self.log("RNDIS %d.%d up: medium %d, up to %d packet(s) / %d bytes per transfer, "
                 "alignment 2^%d, frame %d, our MAC %s" % (major, minor, medium, max_pkts, max_xfer,
                                                          align, frame, mac), always=True)
        return mac

    # --- frames
    def rx_loop(self):
        """Gadget -> TAP. A transfer can hold several RNDIS packets."""
        try:
            while not self.stop.is_set():
                try:
                    data = self.bulk(self.eps["in"], RX_SIZE, 1000)
                except OSError as e:
                    if e.errno == errno.ETIMEDOUT:
                        continue
                    raise
                off = 0
                while off + 8 <= len(data):
                    t, ln = struct.unpack_from("<II", data, off)
                    if t != MSG_PACKET or ln < PACKET_HDR or off + ln > len(data):
                        if t != 0:
                            self.stats["rx_bad"] += 1
                        break
                    doff, dlen = struct.unpack_from("<II", data, off + 8)
                    start = off + 8 + doff
                    frame = data[start:start + dlen]
                    if len(frame) == dlen and dlen:
                        os.write(self.tap.fd, frame)
                        self.stats["rx"] += 1
                        self.stats["rx_bytes"] += dlen
                    off += ln
        except Exception as e:  # noqa: BLE001 - the session ends on any USB error
            self.fail(e)

    def tx_loop(self):
        """TAP -> gadget, one RNDIS packet per transfer."""
        maxp = self.eps.get("maxp", 512)
        try:
            while not self.stop.is_set():
                r, _, _ = select.select([self.tap.fd], [], [], 0.5)
                if not r:
                    continue
                frame = os.read(self.tap.fd, 2048)
                total = PACKET_HDR + len(frame)
                # a transfer that's a whole number of USB packets would need a
                # zero-length packet to end it: one byte of padding inside the
                # message instead (allowed by RNDIS; the gadget steps by MessageLength)
                pad = 1 if total % maxp == 0 else 0
                msg = struct.pack("<11I", MSG_PACKET, total + pad, 36, len(frame),
                                  0, 0, 0, 0, 0, 0, 0) + frame + b"\0" * pad
                self.bulk(self.eps["out"], msg, 2000)
                self.stats["tx"] += 1
                self.stats["tx_bytes"] += len(frame)
        except Exception as e:  # noqa: BLE001
            self.fail(e)

    def fail(self, e):
        if not self.stop.is_set():
            self.error = e
            self.stop.set()

    def run(self, keepalive=5.0, stats_every=30.0):
        self.open()
        mac = self.init()
        self.tap.up(mac)
        threads = [threading.Thread(target=self.rx_loop, daemon=True),
                   threading.Thread(target=self.tx_loop, daemon=True)]
        for t in threads:
            t.start()
        last_ka = last_stats = time.time()
        while not self.stop.wait(0.5):
            now = time.time()
            # the control channel's keepalive: also how an unplug is noticed when idle
            if now - last_ka >= keepalive:
                last_ka = now
                try:
                    self.command(MSG_KEEPALIVE, b"")
                except OSError as e:
                    self.fail(e)
            if stats_every and now - last_stats >= stats_every:
                last_stats = now
                self.log("frames in %(rx)d (%(rx_bytes)d B), out %(tx)d (%(tx_bytes)d B), bad %(rx_bad)d"
                         % self.stats)
        for t in threads:
            t.join(3)
        return self.error


def main():
    ap = argparse.ArgumentParser(description="User-space RNDIS host: a USB gadget as a TAP interface")
    ap.add_argument("--ifname", default="usblink0")
    ap.add_argument("--ip", default="192.168.137.1/24", help="the interface's address ('' for none)")
    ap.add_argument("--vid", default="10d6", help="only this vendor id ('' for any)")
    ap.add_argument("--pid", default="", help="only this product id")
    ap.add_argument("--settle", type=float, default=1.0,
                    help="seconds the gadget must stay plugged before it's used (plug-in bounces)")
    ap.add_argument("--once", action="store_true", help="end when the gadget goes, instead of waiting again")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    log = Log(args.quiet)
    log("usblink_host v%s, pid %d" % (VERSION, os.getpid()), always=True)
    stopping = threading.Event()
    current = {"s": None}

    def on_signal(sig, frame):
        stopping.set()
        if current["s"]:
            current["s"].stop.set()
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    tap = Tap(args.ifname, args.ip, log)
    waiting_said = False
    rc = 0
    try:
        while not stopping.is_set():
            found = find_rndis(args.vid or None, args.pid or None)
            if not found:
                if not waiting_said:
                    log("waiting for an RNDIS gadget%s..." % (" (vid %s)" % args.vid if args.vid else ""),
                        always=True)
                    waiting_said = True
                stopping.wait(0.5)
                continue
            # it must stay put for a moment: plugging in connects, drops and reconnects
            node = usbfs_node(found[0])
            t_end = time.time() + args.settle
            while time.time() < t_end and not stopping.is_set():
                stopping.wait(0.1)
                again = find_rndis(args.vid or None, args.pid or None)
                if not again or usbfs_node(again[0]) != node:
                    found = None
                    break
            if not found or stopping.is_set():
                continue
            waiting_said = False
            log("gadget %s %s:%s at %s" % (found[0], rd(os.path.join(SYS_USB, found[0], "idVendor")),
                                           rd(os.path.join(SYS_USB, found[0], "idProduct")), node), always=True)
            s = Session(found, tap, log)
            current["s"] = s
            try:
                err = s.run()
            except OSError as e:
                err = e
            finally:
                s.close()
                current["s"] = None
            log("link down: %s (frames in %d, out %d)" % (
                err or "stopped", s.stats["rx"], s.stats["tx"]), always=True)
            if args.once:
                rc = 0 if err is None else 1
                break
            stopping.wait(1.0)
    finally:
        tap.close()
        log("bye", always=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
