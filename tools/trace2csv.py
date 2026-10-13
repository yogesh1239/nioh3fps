#!/usr/bin/env python3
"""Convert a nioh3fps flight-recorder file (nioh3fps_trace.bin / .prev.bin) to CSV.

Same layout and columns as the DLL's own converter (src/trace.c, src/common.h):
4096-byte header ("N3FPSTR1"), then rec_count records of 128 bytes. A record is valid
when seq != 0, (seq - 1) % rec_count == slot and CRC32(bytes 12..127) matches; torn or
partial records are dropped and counted. Valid records are written ordered by seq.

Usage: python trace2csv.py nioh3fps_trace.bin [out.csv]   (default: <input>.csv)
"""
import struct
import sys
import zlib

HDR_SIZE = 4096
REC_SIZE = 128
HDR = struct.Struct("<8sIIIIqqQIIQ64s32s")
REC = struct.Struct("<QIHHqQIiiffffIddddqIIQII")
TYPES = {1: "H1", 2: "H1_OVERLAP", 3: "H2", 4: "H2_OVERLAP", 5: "CAM", 6: "CAM2",
         7: "MARKER", 8: "TOGGLE", 9: "XELL", 10: "HB", 11: "GATE_BEGIN", 12: "GATE_END", 13: "XELL_EXIT", 14: "BYPASS"}
COLUMNS = ("seq,type,point,t_s,qpc,frame,tid,fps_a,fps_b,speed_req,speed_eff,r_would,r_applied,flags,"
           "dt_ms,debt_ms,time_lost_ms,time_gained_ms,xell_qpc,ovl,enabled,aux,aux2,marker,hb_seq,hb_packet,hb_mailbox,hb_repeat,hb_other,gate_seq,gate_wait_us,gate_timeout")
assert REC.size == REC_SIZE


def cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def convert(src, dst):
    data = open(src, "rb").read()
    if len(data) < HDR_SIZE:
        raise SystemExit("%s: too short for a trace header" % src)
    (magic, version, header_size, rec_size, rec_count, qpf, start_qpc, game_base, pid, _r0,
     _ft, build_id, mod_version) = HDR.unpack_from(data, 0)
    (write_index,) = struct.unpack_from("<q", data, 256)
    if (magic != b"N3FPSTR1" or rec_size != REC_SIZE or header_size != HDR_SIZE
            or rec_count == 0 or rec_count & (rec_count - 1) or rec_count > 524288):
        raise SystemExit("%s: not a v1 nioh3fps trace" % src)
    if len(data) < HDR_SIZE + rec_count * REC_SIZE:
        raise SystemExit("%s: truncated (%d bytes)" % (src, len(data)))

    valid, bad = [], 0
    for slot in range(rec_count):
        off = HDR_SIZE + slot * REC_SIZE
        rec = REC.unpack_from(data, off)
        seq, crc = rec[0], rec[1]
        if seq == 0:
            continue
        if (seq - 1) % rec_count != slot or zlib.crc32(data[off + 12:off + REC_SIZE]) & 0xFFFFFFFF != crc:
            bad += 1
            continue
        valid.append(rec)
    valid.sort(key=lambda r: r[0])

    with open(dst, "w", newline="\r\n") as f:   # CRLF, like the DLL's text-mode fopen
        f.write("# nioh3fps trace build=%s version=%s pid=%u qpf=%d write_index=%d valid=%u dropped_torn=%u\n"
                % (cstr(build_id), cstr(mod_version), pid, qpf, write_index, len(valid), bad))
        f.write(COLUMNS + "\n")
        for (seq, _crc, typ, point, qpc, frame, tid, fps_a, fps_b, speed_req, speed_eff, r_would,
             r_applied, flags, dt, debt, lost, gained, xell_qpc, ovl, enabled, aux, aux2, marker) in valid:
            t_s = (qpc - start_qpc) / qpf if qpf else 0.0
            f.write("%d,%s,%u,%.6f,%d,%d,%u,%d,%d,%.6g,%.6g,%.7g,%.7g,0x%04X,%.4f,%.4f,%.4f,%.4f,%d,%u,%u,0x%X,%u,%u,%u,0x%X,0x%X,%u,%u,%u,%.3f,%u\n"
                    % (seq, TYPES.get(typ, "?"), point, t_s, qpc, frame, tid, fps_a, fps_b, speed_req,
                       speed_eff, r_would, r_applied, flags, dt * 1e3, debt * 1e3, lost * 1e3,
                       gained * 1e3, xell_qpc, ovl, enabled, aux, aux2, marker,
                       frame if typ == 10 else 0, aux if typ == 10 else 0,
                       (xell_qpc & 0xffffffffffffffff) if typ == 10 else 0,
                       int(bool(flags & 2)) if typ == 10 else 0,
                       int(bool(flags & 1)) if typ == 10 else 0,
                       aux if typ in (11, 12) else 0, dt * 1e6 if typ == 12 else 0,
                       int(bool(flags & 1)) if typ == 12 else 0))
    print("%s -> %s: %d valid records, %d torn/partial dropped" % (src, dst, len(valid), bad))


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 2
    src = argv[1]
    dst = argv[2] if len(argv) > 2 else (src[:-4] if src.lower().endswith(".bin") else src) + ".csv"
    convert(src, dst)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
