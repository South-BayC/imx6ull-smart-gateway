#!/usr/bin/env python3
"""
indaq_test.py — Python ctypes binding for the INDAQ driver

Provides a simple Python interface to the INDAQ kernel driver via
ctypes-based C library calls to libdaq.so.

Usage:
    python3 indaq_test.py                   # read 5 samples (default)
    python3 indaq_test.py -n 20             # read 20 samples
    python3 indaq_test.py -n 10 -r 50       # 10 samples at 50 Hz
"""

import ctypes
import os
import sys
import argparse
from ctypes import (
    c_uint8, c_uint16, c_uint32, c_uint64,
    c_int16, c_ssize_t, c_int, c_char_p,
    POINTER, Structure, byref, CDLL, create_string_buffer,
)

# ======== Struct Definitions ========

class IndaqSample(ctypes.Structure):
    """Matches kernel's struct indaq_sample (28 bytes)"""
    _pack_ = 1
    _fields_ = [
        ("ts_ns", c_uint64),    #  0: timestamp
        ("als",   c_uint16),    #  8: ambient light
        ("ps",    c_uint16),    # 10: proximity
        ("ir",    c_uint16),    # 12: infrared
        ("ax",    c_int16),     # 14: accel X
        ("ay",    c_int16),     # 16: accel Y
        ("az",    c_int16),     # 18: accel Z
        ("temp",  c_int16),     # 20: temperature
        ("gx",    c_int16),     # 22: gyro X
        ("gy",    c_int16),     # 24: gyro Y
        ("gz",    c_int16),     # 26: gyro Z
    ]

    def __str__(self):
        """Human-readable sample string."""
        if self.ax == 0 and self.ay == 0 and self.az == 0 and \
           self.gx == 0 and self.gy == 0 and self.gz == 0 and self.temp == 0:
            return f"[{self.ts_ns:>5}] ALS={self.als:5d} PS={self.ps:5d} IR={self.ir:5d}"
        return (
            f"[{self.ts_ns:>5}] "
            f"ax={self.ax:>6d} ay={self.ay:>6d} az={self.az:>6d} "
            f"gx={self.gx:>6d} gy={self.gy:>6d} gz={self.gz:>6d} "
            f"temp={self.temp:>6d}"
        )


class IndaqInfo(ctypes.Structure):
    """Matches kernel's struct indaq_info"""
    _fields_ = [
        ("version",       c_uint32),
        ("sampling_rate", c_uint32),
        ("total_samples", c_uint32),
        ("errors",        c_uint32),
    ]

    def __str__(self):
        return (
            f"INDAQ v{self.version >> 8}.{self.version & 0xFF}\n"
            f"  Sampling rate: {self.sampling_rate} Hz\n"
            f"  Total samples: {self.total_samples}\n"
            f"  Errors:        {self.errors}"
        )


# ======== IOCTL Constants ========

INDAQ_IOC_MAGIC = ord('I')
INDAQ_IOCTL_GET_INFO          = 0x80104900  # _IOR('I', 0, struct indaq_info) sizeof=16
INDAQ_IOCTL_START_CAPTURE     = 0x00004903  # _IO('I', 3)
INDAQ_IOCTL_STOP_CAPTURE      = 0x00004904  # _IO('I', 4)
INDAQ_IOCTL_SET_SAMPLING_RATE = 0x40044905  # _IOW('I', 5, uint32_t)


# ======== Python API ========

class INDAQ:
    """Python interface to INDAQ driver via ctypes."""

    def __init__(self, lib_path="libdaq.so", dev_path="/dev/indaq"):
        self._lib = None
        self._fd = None

        # Try to load libdaq shared library
        try:
            self._lib = CDLL(lib_path)
        except OSError:
            # Fall back to direct ioctl/read syscalls via Python
            self._lib = None

        self._dev_path = dev_path

    def _check_lib(self):
        if self._lib is None:
            return False
        return True

    def open(self, dev=None):
        """Open the INDAQ device."""
        path = dev or self._dev_path

        if self._check_lib():
            # Use libdaq C API
            self._lib.indaq_open.restype = c_int
            self._fd = self._lib.indaq_open(path.encode())
        else:
            # Direct open
            import fcntl
            self._fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)

        if self._fd < 0:
            raise RuntimeError(f"Failed to open {path}")
        return self._fd

    def close(self):
        """Close the device."""
        if self._check_lib() and self._fd is not None:
            self._lib.indaq_close(self._fd)
        elif self._fd is not None:
            os.close(self._fd)
        self._fd = None

    def start(self):
        """Start capture."""
        if self._check_lib():
            self._lib.indaq_start.restype = c_int
            ret = self._lib.indaq_start(self._fd)
        else:
            ret = fcntl.ioctl(self._fd, INDAQ_IOCTL_START_CAPTURE)
        if ret < 0:
            raise RuntimeError("Failed to start capture")
        return ret

    def stop(self):
        """Stop capture."""
        if self._check_lib():
            self._lib.indaq_stop.restype = c_int
            ret = self._lib.indaq_stop(self._fd)
        else:
            ret = fcntl.ioctl(self._fd, INDAQ_IOCTL_STOP_CAPTURE)
        if ret < 0:
            raise RuntimeError("Failed to stop capture")
        return ret

    def read(self, count=1):
        """Read samples from device (blocking)."""
        buf = (IndaqSample * count)()

        if self._check_lib():
            self._lib.indaq_read.restype = c_ssize_t
            n = self._lib.indaq_read(self._fd, buf, count)
        else:
            data = os.read(self._fd, count * ctypes.sizeof(IndaqSample))
            n = len(data) // ctypes.sizeof(IndaqSample)
            ctypes.memmove(buf, data, len(data))

        if n < 0:
            raise RuntimeError("Failed to read samples")
        return list(buf[:n])

    def read_one(self):
        """Read a single sample."""
        samples = self.read(1)
        return samples[0] if samples else None

    def get_info(self):
        """Get driver info."""
        info = IndaqInfo()
        if self._check_lib():
            self._lib.indaq_get_info.restype = c_int
            ret = self._lib.indaq_get_info(self._fd, byref(info))
        else:
            import fcntl
            ret = fcntl.ioctl(self._fd, INDAQ_IOCTL_GET_INFO, info)
        if ret < 0:
            raise RuntimeError("Failed to get info")
        return info

    def set_rate(self, hz):
        """Set sampling rate (1-100 Hz)."""
        if self._check_lib():
            self._lib.indaq_set_rate.restype = c_int
            ret = self._lib.indaq_set_rate(self._fd, hz)
        else:
            import fcntl
            arg = c_uint32(hz)
            ret = fcntl.ioctl(self._fd, INDAQ_IOCTL_SET_SAMPLING_RATE, arg)
        if ret < 0:
            raise RuntimeError(f"Failed to set rate to {hz} Hz")
        return ret

    def wait(self, timeout_ms=-1):
        """Wait for data to become available (poll-based)."""
        import select
        if timeout_ms < 0:
            timeout_sec = None
        else:
            timeout_sec = timeout_ms / 1000.0

        r, _, _ = select.select([self._fd], [], [], timeout_sec)
        return len(r) > 0


# ======== Command-Line Entry Point ========

def main():
    parser = argparse.ArgumentParser(description="INDAQ Python test program")
    parser.add_argument("-n", type=int, default=5,
                        help="Number of samples to read (default: 5)")
    parser.add_argument("-r", type=int, default=0,
                        help="Sampling rate in Hz (default: current)")
    parser.add_argument("--info", action="store_true",
                        help="Print driver info and exit")
    args = parser.parse_args()

    daq = INDAQ()

    try:
        daq.open()

        if args.info:
            info = daq.get_info()
            print(info)
            return 0

        if args.r > 0:
            daq.set_rate(args.r)
            print(f"Set sampling rate to {args.r} Hz")

        info = daq.get_info()
        print(f"Device info: {info.sampling_rate} Hz, "
              f"{info.total_samples} total samples")

        daq.start()
        print(f"Reading {args.n} samples...\n")

        samples = daq.read(args.n)
        for i, s in enumerate(samples):
            print(f"[{i:3d}] {s}")

        daq.stop()
        print(f"\nRead {len(samples)} samples successfully.")

    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    finally:
        daq.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
