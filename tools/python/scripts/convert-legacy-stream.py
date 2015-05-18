#!/usr/bin/env python
# -*- coding: utf-8 -*-

import sys
import os, os.path
import syslog
import traceback

from struct import calcsize, unpack, pack

from xen.migration import libxc, libxl

__version__ = 1

fin = None             # Input file/fd
fout = None            # Output file/fd
twidth = 0             # Legacy toolstack bitness (32 or 64)
pv = None              # Boolean (pv or hvm)
qemu = True            # Boolean - process qemu record?
log_to_syslog = False  # Boolean - Log to syslog instead of stdout/err?
verbose = False        # Boolean - Summarise stream contents

def stream_read(_ = None):
    return fin.read(_)

def stream_write(_):
    return fout.write(_)

def info(msg):
    """Info message, routed to appropriate destination"""
    if verbose:
        if log_to_syslog:
            for line in msg.split("\n"):
                syslog.syslog(syslog.LOG_INFO, line)
        else:
            print msg

def err(msg):
    """Error message, routed to appropriate destination"""
    if log_to_syslog:
        for line in msg.split("\n"):
            syslog.syslog(syslog.LOG_ERR, line)
    print >> sys.stderr, msg

class StreamError(StandardError):
    pass

class VM(object):

    def __init__(self, fmt):
        # Common
        self.p2m_size = 0

        # PV
        self.max_vcpu_id = 0
        self.online_vcpu_map = []
        self.width = 0
        self.levels = 0
        self.basic_len = 0
        self.extd = False
        self.xsave_len = 0

        # libxl
        self.libxl = fmt == "libxl"
        self.xenstore = [] # Deferred "toolstack" records

def write_libxc_ihdr():
    stream_write(pack(libxc.IHDR_FORMAT,
                      libxc.IHDR_MARKER,  # Marker
                      libxc.IHDR_IDENT,   # Ident
                      libxc.IHDR_VERSION, # Version
                      libxc.IHDR_OPT_LE,  # Options
                      0, 0))              # Reserved

def write_libxc_dhdr():
    if pv:
        dtype = libxc.DHDR_TYPE_x86_pv
    else:
        dtype = libxc.DHDR_TYPE_x86_hvm

    stream_write(pack(libxc.DHDR_FORMAT,
                      dtype,        # Type
                      12,           # Page size
                      0,            # Reserved
                      0,            # Xen major (converted)
                      __version__)) # Xen minor (converted)

def write_libxl_hdr():
    stream_write(pack(libxl.HDR_FORMAT,
                      libxl.HDR_IDENT,     # Ident
                      libxl.HDR_VERSION,   # Version 2
                      libxl.HDR_OPT_LE |   # Options
                      libxl.HDR_OPT_LEGACY # Little Endian and Legacy
                      ))

def write_record(rt, *argl):
    alldata = ''.join(argl)
    length = len(alldata)

    record = pack(libxc.RH_FORMAT, rt, length) + alldata
    plen = (8 - (length & 7)) & 7
    record += '\x00' * plen

    stream_write(record)

def write_libxc_pv_info(vm):
    write_record(libxc.REC_TYPE_x86_pv_info,
                 pack(libxc.X86_PV_INFO_FORMAT,
                      vm.width, vm.levels, 0, 0))

def write_libxc_pv_p2m_frames(vm, pfns):
    write_record(libxc.REC_TYPE_x86_pv_p2m_frames,
                 pack(libxc.X86_PV_P2M_FRAMES_FORMAT,
                      0, vm.p2m_size - 1),
                 pack("Q" * len(pfns), *pfns))

def write_libxc_pv_vcpu_basic(vcpu_id, data):
    write_record(libxc.REC_TYPE_x86_pv_vcpu_basic,
                 pack(libxc.X86_PV_VCPU_HDR_FORMAT, vcpu_id, 0), data)

def write_libxc_pv_vcpu_extd(vcpu_id, data):
    write_record(libxc.REC_TYPE_x86_pv_vcpu_extended,
                 pack(libxc.X86_PV_VCPU_HDR_FORMAT, vcpu_id, 0), data)

def write_libxc_pv_vcpu_xsave(vcpu_id, data):
    write_record(libxc.REC_TYPE_x86_pv_vcpu_xsave,
                 pack(libxc.X86_PV_VCPU_HDR_FORMAT, vcpu_id, 0), data)

def write_page_data(pfns, pages):
    if fout is None: # Save copying 1M buffers around for no reason
        return

    new_pfns = [(((x & 0xf0000000) << 32) | (x & 0x0fffffff)) for x in pfns]

    # Optimise the needless buffer copying in write_record()
    stream_write(pack(libxc.RH_FORMAT,
                      libxc.REC_TYPE_page_data,
                      8 + (len(new_pfns) * 8) + len(pages)))
    stream_write(pack(libxc.PAGE_DATA_FORMAT, len(new_pfns), 0))
    stream_write(pack("Q" * len(new_pfns), *new_pfns))
    stream_write(pages)

def write_libxc_tsc_info(mode, khz, nsec, incarn):
    write_record(libxc.REC_TYPE_tsc_info,
                 pack(libxc.TSC_INFO_FORMAT,
                      mode, khz, nsec, incarn, 0))

def write_libxc_hvm_params(params):
    if pv:
        raise StreamError("HVM-only param in PV stream")
    elif len(params) % 2:
        raise RuntimeError("Expected even length list of hvm parameters")

    write_record(libxc.REC_TYPE_hvm_params,
                 pack(libxc.HVM_PARAMS_FORMAT, len(params) / 2, 0),
                 pack("Q" * len(params), *params))

def write_libxl_end():
    write_record(libxl.REC_TYPE_end, "")

def write_libxl_libxc_context():
    write_record(libxl.REC_TYPE_libxc_context, "")

def write_libxl_xenstore_data(data):
    write_record(libxl.REC_TYPE_xenstore_data, data)

def write_libxl_emulator_context(blob):
    write_record(libxl.REC_TYPE_emulator_context,
                 pack(libxl.EMULATOR_CONTEXT_FORMAT,
                      libxl.EMULATOR_ID_unknown, 0) + blob)

def rdexact(nr_bytes):
    """Read exactly nr_bytes from fin"""
    _ = stream_read(nr_bytes)
    if len(_) != nr_bytes:
        raise IOError("Stream truncated")
    return _

def unpack_exact(fmt):
    """Unpack a format from fin"""
    sz = calcsize(fmt)
    return unpack(fmt, rdexact(sz))

def unpack_ulongs(nr_ulongs):
    if twidth == 32:
        return unpack_exact("I" * nr_ulongs)
    else:
        return unpack_exact("Q" * nr_ulongs)

def read_pv_extended_info(vm):

    marker, = unpack_ulongs(1)

    if twidth == 32:
        expected = 0xffffffff
    else:
        expected = 0xffffffffffffffff

    if marker != expected:
        raise StreamError("Unexpected extended info marker 0x%x" % (marker, ))

    total_length, = unpack_exact("I")
    so_far = 0

    info("Extended Info: length 0x%x" % (total_length, ))

    while so_far < total_length:

        blkid, datasz = unpack_exact("=4sI")
        so_far += 8

        info("  Record type: %s, size 0x%x" % (blkid, datasz))

        data = rdexact(datasz)
        so_far += datasz

        # Eww, but this is how it is done :(
        if blkid == "vcpu":

            vm.basic_len = datasz

            if datasz == 0x1430:
                vm.width = 8
                vm.levels = 4
                info("    64bit domain, 4 levels")
            elif datasz == 0xaf0:
                vm.width = 4
                vm.levels = 3
                info("    32bit domain, 3 levels")
            else:
                raise StreamError("Unable to determine guest width/level")

            write_libxc_pv_info(vm)

        elif blkid == "extv":
            vm.extd = True

        elif blkid == "xcnt":
            vm.xsave_len, = unpack("I", data[:4])
            info("xcnt sz 0x%x" % (vm.xsave_len, ))

        else:
            raise StreamError("Unrecognised extended block")


    if so_far != total_length:
        raise StreamError("Overshot Extended Info size by %d bytes"
                          % (so_far - total_length,))

def read_pv_p2m_frames(vm):
    fpp = 4096 / vm.width
    p2m_frame_len = (vm.p2m_size - 1) / fpp + 1

    info("P2M frames: fpp %d, p2m_frame_len %d" % (fpp, p2m_frame_len))
    write_libxc_pv_p2m_frames(vm, unpack_ulongs(p2m_frame_len))

def read_pv_tail(vm):

    nr_unmapped_pfns, = unpack_exact("I")

    if nr_unmapped_pfns != 0:
        # "Unmapped" pfns are bogus
        _ = unpack_ulongs(nr_unmapped_pfns)
        info("discarding %d bogus 'unmapped pfns'" % (nr_unmapped_pfns, ))
        #raise StreamError("Found bogus 'unmapped pfns'")

    for vcpu_id in vm.online_vcpu_map:

        basic = rdexact(vm.basic_len)
        info("Got VCPU basic (size 0x%x)" % (vm.basic_len, ))
        write_libxc_pv_vcpu_basic(vcpu_id, basic)

        if vm.extd:
            extd = rdexact(128)
            info("Got VCPU extd (size 0x%x)" % (128, ))
            write_libxc_pv_vcpu_extd(vcpu_id, extd)

        if vm.xsave_len:
            mask, size = unpack_exact("QQ")
            assert vm.xsave_len - 16 == size

            xsave = rdexact(size)
            info("Got VCPU xsave (mask 0x%x, size 0x%x)" % (mask, size))
            write_libxc_pv_vcpu_xsave(vcpu_id, xsave)

    shinfo = rdexact(4096)
    info("Got shinfo")

    write_record(libxc.REC_TYPE_shared_info, shinfo)
    write_record(libxc.REC_TYPE_end, "")


def read_chunks(vm):

    hvm_params = []

    while True:

        marker, = unpack_exact("=i")
        if marker <= 0:
            info("Chunk: type 0x%x" % (marker, ))

        if marker == 0:
            info("  End")

            if hvm_params:
                write_libxc_hvm_params(hvm_params)

            return

        elif marker > 0:

            if marker > 1024:
                raise StreamError("Page batch (%d) exceeded MAX_BATCH"
                                  % (marker, ))
            pfns = unpack_ulongs(marker)

            # xc_domain_save() leaves many XEN_DOMCTL_PFINFO_XTAB records for
            # sequences of pfns it cant map.  Drop these.
            pfns = [ x for x in pfns if x != 0xf0000000 ]

            if len(set(pfns)) != len(pfns):
                raise StreamError("Duplicate pfns in batch")

                # print "0x[",
                # for pfn in pfns:
                #     print "%x" % (pfn, ),
                # print "]"

            nr_pages = len([x for x in pfns if (x & 0xf0000000) < 0xd0000000])

            #print "  Page Batch, %d PFNs, %d pages" % (marker, nr_pages)
            pages = rdexact(nr_pages * 4096)

            write_page_data(pfns, pages)

        elif marker == -1: # XC_SAVE_ID_ENABLE_VERIFY_MODE
            # Verify mode... Seemingly nothing to do...
            pass

        elif marker == -2: # XC_SAVE_ID_VCPU_INFO
            max_id, = unpack_exact("i")

            if max_id > 4095:
                raise StreamError("Vcpu max_id out of range: %d > 4095"
                                  % (max_id, ) )

            vm.max_vcpu_id = max_id
            bitmap = unpack_exact("Q" * ((max_id/64) + 1))

            for idx, word in enumerate(bitmap):
                bit_idx = 0

                while word > 0:
                    if word & 1:
                        vm.online_vcpu_map.append((idx * 64) + bit_idx)

                    bit_idx += 1
                    word >>= 1

            info("  Vcpu info: max_id %d, online map %s"
                 % (vm.max_vcpu_id, vm.online_vcpu_map))

        elif marker == -3: # XC_SAVE_ID_HVM_IDENT_PT
            _, ident_pt = unpack_exact("=IQ")
            info("  EPT Identity Pagetable: 0x%x" % (ident_pt, ))
            hvm_params.extend([12, # HVM_PARAM_IDENT_PT
                               ident_pt])

        elif marker == -4: # XC_SAVE_ID_HVM_VM86_TSS
            _, vm86_tss = unpack_exact("=IQ")
            info("  VM86 TSS: 0x%x" % (vm86_tss, ))
            hvm_params.extend([15, # HVM_PARAM_VM86_TSS
                               vm86_tss])

        elif marker == -5: # XC_SAVE_ID_TMEM
            raise RuntimeError("todo")

        elif marker == -6: # XC_SAVE_ID_TMEM_EXTRA
            raise RuntimeError("todo")

        elif marker == -7: # XC_SAVE_ID_TSC_INFO
            mode, nsec, khz, incarn = unpack_exact("=IQII")
            info("  TSC_INFO: mode %s, %d ns, %d khz, %d incarn"
                 % (mode, nsec, khz, incarn))
            write_libxc_tsc_info(mode, khz, nsec, incarn)

        elif marker == -8: # XC_SAVE_ID_HVM_CONSOLE_PFN
            _, console_pfn = unpack_exact("=IQ")
            info("  Console pfn: 0x%x" % (console_pfn, ))
            hvm_params.extend([17, # HVM_PARAM_CONSOLE_PFN
                               console_pfn])

        elif marker == -9: # XC_SAVE_ID_LAST_CHECKPOINT
            info("  Last Checkpoint")
            # Nothing to do

        elif marker == -10: # XC_SAVE_ID_HVM_ACPI_IOPORTS_LOCATION
            _, loc = unpack_exact("=IQ")
            info("  ACPI ioport location: 0x%x" % (loc, ))
            hvm_params.extend([19, # HVM_PARAM_ACPI_IOPORTS_LOCATION
                               loc])

        elif marker == -11: # XC_SAVE_ID_HVM_VIRIDIAN
            _, loc = unpack_exact("=IQ")
            info("  Viridian location: 0x%x" % (loc, ))
            hvm_params.extend([9, # HVM_PARAM_VIRIDIAN
                               loc])

        elif marker == -12: # XC_SAVE_ID_COMPRESSED_DATA
            sz, = unpack_exact("I")
            data = rdexact(sz)
            info("  Compressed Data: sz 0x%x" % (sz, ))
            raise RuntimeError("todo")

        elif marker == -13: # XC_SAVE_ID_ENABLE_COMPRESSION
            raise RuntimeError("todo")

        elif marker == -14: # XC_SAVE_ID_HVM_GENERATION_ID_ADDR
            _, genid_loc = unpack_exact("=IQ")
            info("  Generation ID Address: 0x%x" % (genid_loc, ))
            hvm_params.extend([34, # HVM_PARAM_VM_GENERATION_ID_ADDR
                               genid_loc])

        elif marker == -15: # XC_SAVE_ID_HVM_PAGING_RING_PFN
            _, paging_ring_pfn = unpack_exact("=IQ")
            info("  Paging ring pfn: 0x%x" % (paging_ring_pfn, ))
            hvm_params.extend([27, # HVM_PARAM_PAGING_RING_PFN
                               paging_ring_pfn])

        elif marker == -16: # XC_SAVE_ID_HVM_ACCESS_RING_PFN
            _, access_ring_pfn = unpack_exact("=IQ")
            info("  Access ring pfn: 0x%x" % (access_ring_pfn, ))
            hvm_params.extend([28, # HVM_PARAM_ACCESS_RING_PFN
                               access_ring_pfn])

        elif marker == -17: # XC_SAVE_ID_HVM_SHARING_RING_PFN
            _, sharing_ring_pfn = unpack_exact("=IQ")
            info("  Sharing ring pfn: 0x%x" % (sharing_ring_pfn, ))
            hvm_params.extend([29, # HVM_PARAM_SHARING_RING_PFN
                               sharing_ring_pfn])

        elif marker == -18:
            sz, = unpack_exact("I")

            if sz:
                data = rdexact(sz)
                info("  Toolstack Data: sz 0x%x" % (sz, ))

                if vm.libxl:
                    vm.xenstore.append(data)
                else:
                    info("    Discarding")

        elif marker == -19: # XC_SAVE_ID_HVM_IOREQ_SERVER_PFN
            _, ioreq_server_pfn = unpack_exact("=IQ")
            info("  IOREQ server pfn: 0x%x" % (ioreq_server_pfn, ))
            hvm_params.extend([32 , # HVM_PARAM_IOREQ_SERVER_PFN
                               ioreq_server_pfn])

        elif marker == -20: # XC_SAVE_ID_HVM_NR_IOREQ_SERVER_PAGES
            _, nr_pages = unpack_exact("=IQ")
            info("  IOREQ server pages: %d" % (nr_pages, ))
            hvm_params.extend([33 , # HVM_PARAM_NR_IOREQ_SERVER_PAGES
                               nr_pages])

        else:
            raise StreamError("Unrecognised chunk %d" % (marker,))

def read_hvm_tail(vm):

    io, bufio, store = unpack_exact("QQQ")
    info("Magic pfns: 0x%x 0x%x 0x%x" % (io, bufio, store))
    write_libxc_hvm_params([5, io,     # HVM_PARAM_IOREQ_PFN
                            6, bufio,  # HVM_PARAM_BUFIOREQ_PFN
                            1, store]) # HVM_PARAM_STORE_PFN

    blobsz, = unpack_exact("I")
    info("Got HVM Context (0x%x bytes)" % (blobsz, ))
    blob = rdexact(blobsz)

    write_record(libxc.REC_TYPE_hvm_context, blob)
    write_record(libxc.REC_TYPE_end, "")



def read_qemu(vm):

    rawsig = rdexact(21)
    sig, = unpack("21s", rawsig)
    info("Qemu signature: %s" % (sig, ))

    if sig == "DeviceModelRecord0002":
        rawsz = rdexact(4)
        sz, = unpack("I", rawsz)
        qdata = rdexact(sz)

        if vm.libxl:
            write_libxl_emulator_context(qdata)
        else:
            stream_write(rawsig)
            stream_write(rawsz)
            stream_write(qdata)

    else:
        raise RuntimeError("Unrecognised Qemu sig '%s'" % (sig, ))


def skip_xl_header(fmt):
    """Skip over an xl header in the stream"""

    hdr = rdexact(32)
    if hdr != "Xen saved domain, xl format\n \0 \r":
        raise StreamError("No xl header")

    end, mflags, oflags, optlen = unpack_exact("=IIII")

    if fmt == "libxl":
        mflags |= 2 # XL_MANDATORY_FLAG_STREAMv2

    opts = pack("=IIII", end, mflags, oflags, optlen)

    optdata = rdexact(optlen)

    info("Processed xl header")

    stream_write(hdr)
    stream_write(opts)
    stream_write(optdata)

def read_legacy_stream(vm):

    try:
        vm.p2m_size, = unpack_ulongs(1)
        info("P2M Size: 0x%x" % (vm.p2m_size,))

        if vm.libxl:
            write_libxl_hdr()
            write_libxl_libxc_context()

        write_libxc_ihdr()
        write_libxc_dhdr()

        if pv:
            read_pv_extended_info(vm)
            read_pv_p2m_frames(vm)

        read_chunks(vm)

        if pv:
            read_pv_tail(vm)
        else:
            read_hvm_tail(vm)

        if vm.libxl:
            for x in vm.xenstore:
                write_libxl_xenstore_data(x)

        if not pv and (vm.libxl or qemu):
            read_qemu(vm)

        if vm.libxl:
            write_libxl_end()

    except (IOError, StreamError):
        err("Stream Error:")
        err(traceback.format_exc())
        return 1

    except RuntimeError:
        err("Script Error:")
        err(traceback.format_exc())
        err("Please fix me")
        return 2
    return 0

def open_file_or_fd(val, mode):
    """
    If 'val' looks like a decimal integer, open it as an fd.  If not, try to
    open it as a regular file.
    """

    fd = -1
    try:
        # Does it look like an integer?
        try:
            fd = int(val, 10)
        except ValueError:
            pass

        # Try to open it...
        if fd != -1:
            return os.fdopen(fd, mode, 0)
        else:
            return open(val, mode, 0)

    except StandardError, e:
        if fd != -1:
            err("Unable to open fd %d: %s" % (fd, e))
        else:
            err("Unable to open file '%s': %s" % (val, e))

    raise SystemExit(1)


def main(argv):
    from optparse import OptionParser
    global fin, fout, twidth, pv, qemu, verbose

    # Change stdout to be line-buffered.
    sys.stdout = os.fdopen(sys.stdout.fileno(), 'w', 1)

    parser = OptionParser(version = __version__,
                          usage = ("%prog [options] -i INPUT -o OUTPUT"
                                   " -w WIDTH -g GUEST"),
                          description =
                          "Convert a legacy stream to a v2 stream")

    # Required options
    parser.add_option("-i", "--in", dest = "fin", metavar = "<FD or FILE>",
                      help = "Legacy input to convert")
    parser.add_option("-o", "--out", dest = "fout", metavar = "<FD or FILE>",
                      help = "v2 format output")
    parser.add_option("-w", "--width", dest = "twidth",
                      metavar = "<32/64>", choices = ["32", "64"],
                      help = "Legacy toolstack bitness")
    parser.add_option("-g", "--guest-type", dest = "gtype",
                      metavar = "<pv/hvm>", choices = ["pv", "hvm"],
                      help = "Type of guest in stream")

    # Optional options
    parser.add_option("-f", "--format", dest = "format",
                      metavar = "<libxc|libxl>", default = "libxc",
                      choices = ["libxc", "libxl"],
                      help = "Desired format of the outgoing stream (defaults to libxc)")
    parser.add_option("-v", "--verbose", action = "store_true", default = False,
                      help = "Summarise stream contents")
    parser.add_option("-x", "--xl", action = "store_true", default = False,
                      help = ("Is an `xl` header present in the stream?"
                              " (default no)"))
    parser.add_option("--skip-qemu", action = "store_true", default = False,
                      help = ("Skip processing of the qemu tail?"
                              " (default no)"))
    parser.add_option("--syslog", action = "store_true", default = False,
                      help = "Log to syslog instead of stdout")

    opts, _ = parser.parse_args()

    if (opts.fin is None or opts.fout is None or
        opts.twidth is None or opts.gtype is None):

        parser.print_help(sys.stderr)
        raise SystemExit(1)

    if opts.syslog:
        global log_to_syslog

        syslog.openlog("convert-legacy-stream", syslog.LOG_PID)
        log_to_syslog = True

    fin     = open_file_or_fd(opts.fin,  "rb")
    fout    = open_file_or_fd(opts.fout, "wb")
    twidth  = int(opts.twidth)
    pv      = opts.gtype == "pv"
    verbose = opts.verbose
    if opts.skip_qemu:
        qemu = False

    if opts.xl:
        skip_xl_header(opts.format)

    rc = read_legacy_stream(VM(opts.format))
    fout.close()

    return rc

if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except SystemExit, e:
        sys.exit(e.code)
    except KeyboardInterrupt:
        sys.exit(1)
