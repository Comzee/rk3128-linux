"""Minimal Rockusb client (loader mode) for the RK3128 reference boot-partition rescue.

  rkusb.py info                       USB descriptor, mode guess, test unit ready, flash id
  rkusb.py read  <lba> <sectors> <out>  READ_LBA into a file
  rkusb.py write <lba> <in>             WRITE_LBA from a file (whole file, 512-byte multiple)
  rkusb.py reset                        DEVICE_RESET (reboot)

LBA is the loader's LBA (parameter offsets: boot = 0x14000). Commands follow rkdeveloptool's
RKComm: 31-byte CBW 'USBC', CB = opcode, 0, LBA (BE32), 0, sectors (BE16); 13-byte CSW 'USBS'.
"""
import os
import random
import struct
import sys

import libusb_package
import usb.core
import usb.util

VID, PID = 0x2207, 0x310C
CHUNK = 128  # sectors per transfer (64 KiB)


def dev():
    d = libusb_package.find(idVendor=VID, idProduct=PID)
    if d is None:
        sys.exit("no 2207:310c device")
    d.set_configuration()
    intf = d.get_active_configuration()[(0, 0)]
    ep_in = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK)
    ep_out = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK)
    return d, ep_in, ep_out


def cbw(op, length, flags, lba=0, sectors=0, cblen=6):
    tag = random.getrandbits(32)
    cb = bytes([op, 0]) + struct.pack(">I", lba) + b"\0" + struct.pack(">H", sectors)
    cb = cb.ljust(16, b"\0")
    return tag, struct.pack("<IIIBBB", 0x43425355, tag, length, flags, 0, cblen) + cb


def csw(ep_in, tag):
    r = bytes(ep_in.read(13, timeout=20000))
    sig, t, resid, status = struct.unpack("<IIIB", r)
    if sig != 0x53425355 or t != tag:
        sys.exit("bad CSW %r" % r)
    return status


def cmd_in(eps, op, n, lba=0, sectors=0, cblen=6):
    _, ep_in, ep_out = eps
    tag, c = cbw(op, n, 0x80, lba, sectors, cblen)
    ep_out.write(c, timeout=5000)
    data = bytes(ep_in.read(n, timeout=20000)) if n else b""
    st = csw(ep_in, tag)
    return data, st


def main(argv):
    if argv[0] == "info":
        d = libusb_package.find(idVendor=VID, idProduct=PID)
        if d is None:
            sys.exit("no 2207:310c device")
        print("bcdUSB 0x%04x bcdDevice 0x%04x -> %s" % (d.bcdUSB, d.bcdDevice, "loader" if d.bcdUSB & 1 else "maskrom"))
        eps = dev()
        _, st = cmd_in(eps, 0x00, 0)
        print("test unit ready status", st)
        data, st = cmd_in(eps, 0x01, 5)
        print("flash id", data, "status", st)
    elif argv[0] == "read":
        lba, n, out = int(argv[1], 0), int(argv[2], 0), argv[3]
        eps = dev()
        with open(out, "wb") as f:
            done = 0
            while done < n:
                k = min(CHUNK, n - done)
                data, st = cmd_in(eps, 0x14, k * 512, lba + done, k, 0x0A)
                if st != 0 or len(data) != k * 512:
                    sys.exit("read failed at lba 0x%x status %d len %d" % (lba + done, st, len(data)))
                f.write(data)
                done += k
        print("read %d sectors from 0x%x" % (n, lba))
    elif argv[0] == "write":
        lba, path = int(argv[1], 0), argv[2]
        blob = open(path, "rb").read()
        if len(blob) % 512:
            sys.exit("size not a 512 multiple")
        n = len(blob) // 512
        _, ep_in, ep_out = dev()
        done = 0
        while done < n:
            k = min(CHUNK, n - done)
            tag, c = cbw(0x15, k * 512, 0x00, lba + done, k, 0x0A)
            ep_out.write(c, timeout=5000)
            ep_out.write(blob[done * 512:(done + k) * 512], timeout=20000)
            st = csw(ep_in, tag)
            if st != 0:
                sys.exit("write failed at lba 0x%x status %d" % (lba + done, st))
            done += k
        print("wrote %d sectors at 0x%x" % (n, lba))
    elif argv[0] == "reset":
        eps = dev()
        _, ep_in, ep_out = eps
        tag, c = cbw(0xFF, 0, 0x00)
        ep_out.write(c, timeout=5000)
        try:
            print("reset status", csw(ep_in, tag))
        except Exception as e:
            print("reset sent (%s)" % e)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
