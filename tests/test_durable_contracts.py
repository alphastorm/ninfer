"""Host-only K0 canonical JSON/HMAC reference and committed known-answer fixtures.

This independent oracle does not claim to test the production store/native importer.
The C++ companion exercises the production nlohmann encoder against the same bytes.
"""

import hashlib
import hmac
import json
import pathlib
import unittest

FIXTURES = pathlib.Path(__file__).parent / "fixtures" / "durable"
ORIGIN_DOMAIN = b"ninfer-checkpoint-manifest-origin-v1"


def canonical(value):
    def check(item):
        if item is None or isinstance(item, bool):
            return
        if isinstance(item, int):
            if not -(1 << 63) <= item < (1 << 64):
                raise ValueError("integer outside wire range")
        elif isinstance(item, str):
            item.encode("utf-8", errors="strict")
        elif isinstance(item, list):
            for child in item:
                check(child)
        elif isinstance(item, dict):
            for key, child in item.items():
                if not isinstance(key, str) or not key.isascii():
                    raise ValueError("object keys must be ASCII strings")
                check(key)
                check(child)
        else:
            raise ValueError("unsupported canonical value")
    check(value)
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def decode_canonical(data):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate key")
            result[key] = value
        return result

    def reject_number(value):
        raise ValueError("non-integer number: " + value)

    value = json.loads(data.decode("utf-8", errors="strict"), object_pairs_hook=pairs,
                       parse_float=reject_number, parse_constant=reject_number)
    if canonical(value) != data:
        raise ValueError("noncanonical encoding")
    return value


class DurableContractTests(unittest.TestCase):
    def test_canonical_known_answer(self):
        value = {"z": [True, False, None, -9223372036854775808, 18446744073709551615],
                 "a": 'é/"\\\b\t\n\f\r\x00\x1f'}
        expected = ('{"a":"é/\\\"\\\\\\b\\t\\n\\f\\r\\u0000\\u001f",'
                    '"z":[true,false,null,-9223372036854775808,18446744073709551615]}').encode()
        self.assertEqual(canonical(value), expected)
        self.assertEqual(decode_canonical(expected), value)

    def test_rejects_ambiguous_encodings(self):
        for data in (b'{"a":1,"a":2}', b'{"z":1,"a":2}', b'{"a": 1}',
                     b'{"a":1}\n', b'{"a":-0}', b'{"a":1.0}', b'{"a":1e0}',
                     b'{"a":NaN}', b'{"a":Infinity}', b'{"a":18446744073709551616}',
                     b'{"a":-9223372036854775809}', b'{"a":"\\ud800"}',
                     b'{"a":"\\/"}', b'{"a":"\\u00e9"}', b'{"a":"\xff"}',
                     b'\xef\xbb\xbf{}', '{"é":1}'.encode()):
            with self.subTest(data=data), self.assertRaises((ValueError, UnicodeError)):
                decode_canonical(data)

    def test_rejects_noncanonical_value_types(self):
        for value in (1.0, float("nan"), 1 << 64, -(1 << 63) - 1, b"bytes", {1: "key"}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                canonical(value)

    def test_rfc4231_hmac_known_answers(self):
        vectors = json.loads((FIXTURES / "mac-vectors.json").read_text())
        for vector in vectors["rfc4231"]:
            with self.subTest(case=vector["case"]):
                self.assertEqual(hmac.digest(bytes.fromhex(vector["key_hex"]),
                                             bytes.fromhex(vector["message_hex"]), "sha256").hex(),
                                 vector["hmac_sha256"])

    def test_manifest_encoding_and_origin_mac(self):
        vectors = json.loads((FIXTURES / "mac-vectors.json").read_text())
        bearer = bytes.fromhex(vectors["fixture_bearer_hex"])
        origin_key = hmac.digest(bearer, ORIGIN_DOMAIN, "sha256")
        self.assertEqual(origin_key.hex(), vectors["origin_key_hex"])
        for vector in vectors["manifests"]:
            with self.subTest(manifest=vector["file"]):
                data = (FIXTURES / vector["file"]).read_bytes()
                value = decode_canonical(data)
                self.assertEqual(canonical(value), data)
                self.assertEqual(hashlib.sha256(data).hexdigest(), vector["sha256"])
                mac = hmac.digest(origin_key, data, "sha256").hex()
                recorded = (FIXTURES / vector["mac_file"]).read_text()
                self.assertEqual(recorded, vector["hmac_sha256"])
                self.assertTrue(hmac.compare_digest(mac, recorded))
                self.assertFalse(hmac.compare_digest(hmac.digest(origin_key, data + b"\n", "sha256").hex(), recorded))
                self.assertFalse(hmac.compare_digest(hmac.digest(b"wrong-key", data, "sha256").hex(), recorded))
                self.assertEqual(value["schema_version"], 3)
                self.assertEqual(value["runtime_fingerprint"]["schema_version"], 2)
                files = value["files"]
                self.assertEqual([row["path"] for row in files], sorted(row["path"] for row in files))
                self.assertEqual(len({row["path"] for row in files}), len(files))
                self.assertEqual(sum(row["bytes"] for row in files), value["payload_bytes"])
                self.assertEqual(sum(row["bytes"] for row in files if row["path"].startswith("engine/")),
                                 value["engine_payload_bytes"])
                for row in files:
                    self.assertEqual(len(bytes.fromhex(row["sha256"])), 32)
                    self.assertGreater(row["alignment"], 0)
                    self.assertEqual(row["alignment"] & (row["alignment"] - 1), 0)
                fp = value["runtime_fingerprint"]
                projection = {key: fp[key] for key in ("engine", "frontend", "model", "native_layout", "responses")}
                self.assertEqual(hashlib.sha256(canonical(projection)).hexdigest(), fp["deployment"]["effective_config_sha256"])

    def test_publication_ack_modes(self):
        for name, mode in (("ack-posix.json", "fsync"), ("ack-windows.json", "ntfs-write-through")):
            value = decode_canonical((FIXTURES / name).read_bytes())
            self.assertTrue(value["committed"])
            self.assertEqual(value["schema"], "ninfer-durable-operation/1")
            self.assertEqual(value["operation"], "save")
            self.assertEqual(value["state"], "exported")
            self.assertEqual(value["reason"], "none")
            self.assertEqual(value["dir_sync"], mode)


if __name__ == "__main__":
    unittest.main()
