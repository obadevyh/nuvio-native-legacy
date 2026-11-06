import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("pack_torrent", Path(__file__).with_name("thumbnail-pack-torrent.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def decode(data):
    pos = 0
    def read():
        nonlocal pos
        ch = data[pos:pos + 1]
        if ch == b"d":
            pos += 1
            result = {}
            while data[pos:pos + 1] != b"e":
                key = read(); result[key] = read()
            pos += 1
            return result
        if ch == b"i":
            end = data.index(b"e", pos)
            result = int(data[pos + 1:end]); pos = end + 1
            return result
        end = data.index(b":", pos)
        size = int(data[pos:end]); pos = end + 1
        result = data[pos:pos + size]; pos += size
        return result
    result = read()
    assert pos == len(data)
    return result


class PrivatePackTorrentTest(unittest.TestCase):
    def test_private_no_discovery_and_exact_pieces(self):
        with tempfile.TemporaryDirectory() as d:
            pack, torrent = Path(d) / "test.nvp", Path(d) / "test.torrent"
            payload = b"local fixture" * 25000
            pack.write_bytes(struct.pack(">4sQ32s", b"NVP1", len(payload), hashlib.sha256(payload).digest()) + payload)
            receipt = module.prepare(pack, torrent)
            meta = decode(torrent.read_bytes())
            self.assertEqual(set(meta), {b"info", b"comment"})
            info = meta[b"info"]
            self.assertEqual(info[b"private"], 1)
            self.assertEqual(set(info), {b"private", b"length", b"name", b"piece length", b"pieces"})
            body = pack.read_bytes(); size = info[b"piece length"]
            self.assertEqual(info[b"pieces"], b"".join(hashlib.sha1(body[i:i+size]).digest() for i in range(0, len(body), size)))
            self.assertEqual(receipt["infoHash"], hashlib.sha1(module.bencode(info)).hexdigest())
            self.assertFalse(receipt["torboxOnlyTransferVerified"])

    def test_corrupt_pack_creates_no_torrent(self):
        with tempfile.TemporaryDirectory() as d:
            pack, torrent = Path(d) / "test.nvp", Path(d) / "test.torrent"
            pack.write_bytes(struct.pack(">4sQ32s", b"NVP1", 32, bytes(32)) + b"x" * 32)
            with self.assertRaises(ValueError):
                module.prepare(pack, torrent)
            self.assertFalse(torrent.exists())


if __name__ == "__main__":
    unittest.main()
