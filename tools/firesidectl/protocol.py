"""SysEx framing for the FLEX-5000 opcode protocol.

Request (37 bytes):  F0 00 00 41 <32 ASCII-hex chars> F7
Payload (16 bytes, little-endian, before hex-encoding):
    u16 msg_id | u16 protocol_id(=0) | u32 opcode | u32 data1 | u32 data2
Reply: same framing; decoded payload is  u16 msg_id | u32/f32 result  (6+ bytes).
Set requests get no reply; get requests are matched to replies by msg_id.
"""
import struct

SYSEX_START, SYSEX_END = 0xF0, 0xF7
MFR = bytes([0x00, 0x00, 0x41])
HEX = b"0123456789ABCDEF"


def hex_encode(raw: bytes) -> bytes:
    return raw.hex().upper().encode("ascii")


def hex_decode(txt: bytes) -> bytes:
    if len(txt) % 2:
        raise ValueError("odd hex length")
    if any(c not in HEX for c in txt):
        raise ValueError("non-hex character in SysEx body")
    return bytes.fromhex(txt.decode("ascii"))


def pack_arg(value, kind: str) -> int:
    """Return the raw 32-bit pattern for an argument of the given type."""
    if kind == "u32":
        v = int(value)
        if not 0 <= v <= 0xFFFFFFFF:
            raise ValueError(f"u32 out of range: {v}")
        return v
    if kind == "i32":
        v = int(value)
        if not -0x80000000 <= v <= 0x7FFFFFFF:
            raise ValueError(f"i32 out of range: {v}")
        return v & 0xFFFFFFFF
    if kind == "f32":
        return struct.unpack("<I", struct.pack("<f", float(value)))[0]
    raise ValueError(f"unknown arg type {kind}")


def encode_request(msg_id: int, opcode: int, data1: int = 0, data2: int = 0) -> bytes:
    payload = struct.pack("<HHIII", msg_id & 0xFFFF, 0, opcode, data1, data2)
    return bytes([SYSEX_START]) + MFR + hex_encode(payload) + bytes([SYSEX_END])


def decode_request(frame: bytes):
    """Inverse of encode_request -> (msg_id, protocol_id, opcode, data1, data2)."""
    body = _strip(frame)
    raw = hex_decode(body)
    if len(raw) != 16:
        raise ValueError(f"request payload is {len(raw)} bytes, expected 16")
    return struct.unpack("<HHIII", raw)


def encode_reply(msg_id: int, result_raw: int) -> bytes:
    payload = struct.pack("<HI", msg_id & 0xFFFF, result_raw & 0xFFFFFFFF)
    return bytes([SYSEX_START]) + MFR + hex_encode(payload) + bytes([SYSEX_END])


def decode_reply(frame: bytes):
    """-> (msg_id, result_raw_u32). Extra trailing payload bytes are ignored."""
    raw = hex_decode(_strip(frame))
    if len(raw) < 6:
        raise ValueError(f"reply payload too short: {len(raw)} bytes")
    return struct.unpack_from("<HI", raw)


def as_f32(raw_u32: int) -> float:
    return struct.unpack("<f", struct.pack("<I", raw_u32))[0]


def as_i32(raw_u32: int) -> int:
    return struct.unpack("<i", struct.pack("<I", raw_u32))[0]


def _strip(frame: bytes) -> bytes:
    if len(frame) < 6 or frame[0] != SYSEX_START or frame[-1] != SYSEX_END:
        raise ValueError("not a SysEx frame")
    if frame[1:4] != MFR:
        raise ValueError(f"unexpected manufacturer id {frame[1:4].hex()}")
    return frame[4:-1]
