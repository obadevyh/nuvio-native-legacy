"""Prepare an NVP1 pack for an external seeder/TorBox. Never uploads or publishes.

Usage: python tools/thumbnail-pack-torrent.py PACK.nvp OUTPUT.torrent
The seeder's save directory must contain the named PACK.nvp file.
This experiment has no strong video identity; do not register it as a public pack.
Preparation is offline. Private flag + absent discovery metadata are precautions,
not peer authentication. Do not seed until a TorBox-only connection is verified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def bencode(value):
    if isinstance(value, int):
        return b"i" + str(value).encode() + b"e"
    if isinstance(value, bytes):
        return str(len(value)).encode() + b":" + value
    if isinstance(value, str):
        return bencode(value.encode())
    if isinstance(value, list):
        return b"l" + b"".join(map(bencode, value)) + b"e"
    if isinstance(value, dict):
        return b"d" + b"".join(bencode(k) + bencode(value[k]) for k in sorted(value)) + b"e"
    raise TypeError(type(value))


def prepare(pack, destination):
    size = pack.stat().st_size
    if not 60 <= size <= 64 * 1024 * 1024 + 44:
        raise ValueError("pack size")
    piece_size = 256 * 1024
    with pack.open("rb") as source:
        magic, length, expected = struct.unpack(">4sQ32s", source.read(44))
        if magic != b"NVP1" or length != size - 44:
            raise ValueError("pack envelope")
        digest = hashlib.sha256()
        while chunk := source.read(piece_size):
            digest.update(chunk)
        if digest.digest() != expected:
            raise ValueError("pack digest")
        source.seek(0)
        pieces = []
        while chunk := source.read(piece_size):
            pieces.append(hashlib.sha1(chunk).digest())
    info = {b"length": size, b"name": pack.name.encode(), b"private": 1,
            b"piece length": piece_size, b"pieces": b"".join(pieces)}
    torrent = {b"comment": b"Nuvio local thumbnail portability experiment; not a published catalogue entry", b"info": info}
    destination.write_bytes(bencode(torrent))
    return {"infoHash": hashlib.sha1(bencode(info)).hexdigest(), "file": pack.name,
            "bytes": size, "pieceCount": len(pieces), "pieceLength": piece_size,
            "payloadSha256": expected.hex(), "status": "prepared-not-seeded-not-uploaded",
            "videoIdentity": "not-established", "private": True,
            "publicTrackers": [], "webSeeds": [], "dhtNodes": [],
            "torboxOnlyTransferVerified": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pack", type=Path)
    parser.add_argument("torrent", type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.pack, args.torrent), indent=2))
