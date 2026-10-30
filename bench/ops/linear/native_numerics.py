#!/usr/bin/env python3
"""GPU-free P15 arithmetic audit; not a CUDA emulator or a quality gate.

Run with Python 3.13+: python3 bench/ops/linear/native_numerics.py --output FILE
The sparse W4/W8 witness has exactly one nonzero product per group, so its
contraction is independent of undocumented tensor-core summation order. Random
rows use FP64 group dots rounded once to FP32 and are explicitly only a model.
No model weights, captured prompts, machine identities, or production settings
are read. The output records synthetic evidence, not native qualification.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
import math
from pathlib import Path
import random
import struct


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def f16(value):
    return struct.unpack("<e", struct.pack("<e", value))[0]


def bf16_bits(value):
    bits = struct.unpack("<I", struct.pack("<f", value))[0]
    return ((bits + 0x7FFF + ((bits >> 16) & 1)) >> 16) & 0xFFFF


def bf16_value(bits):
    return struct.unpack("<f", struct.pack("<I", bits << 16))[0]


# Use a true single-rounding FP32 FMA, not Python's separate multiply/add.
_libm = ctypes.CDLL(ctypes.util.find_library("m") or None)
_fmaf = _libm.fmaf
_fmaf.argtypes = (ctypes.c_float, ctypes.c_float, ctypes.c_float)
_fmaf.restype = ctypes.c_float


def group_reduce(dots, scales, warps, pairwise=True):
    partial = [0.0] * warps
    for g, dot in enumerate(dots):
        w = g % warps
        partial[w] = _fmaf(dot, scales[g], partial[w])
    if pairwise:
        partial = [f32(partial[i] + partial[i + 1]) for i in range(0, warps, 2)]
    total = partial[0]
    for value in partial[1:]:
        total = f32(total + value)
    return total


def split2(q, x, scales):
    """S1 Q5 T=1 scalar FMA and shuffle-down order, exact physical K."""
    partial = [[0.0] * 32 for _ in range(2)]
    for slab in range(len(x) // 1024):
        for part in range(2):
            for local in range(2):
                for lane in range(32):
                    begin = slab * 1024 + (part * 2 + local) * 256 + lane * 8
                    for k in range(begin, begin + 8):
                        weight = f32(q[k] * scales[k // 64])
                        partial[part][lane] = _fmaf(weight, x[k], partial[part][lane])
    for lanes in partial:
        for offset in (16, 8, 4, 2, 1):
            old = lanes[:]
            for lane in range(32 - offset):
                lanes[lane] = f32(old[lane] + old[lane + offset])
    return f32(partial[0][0] + partial[1][0])


def quantize_group(values):
    peak = max(abs(value) for value in values)
    scale = f32(peak / 127.0) if peak else 0.0
    inv = f32(127.0 / peak) if peak else 0.0
    codes = [max(-127, min(127, round(f32(value * inv)))) for value in values]
    return codes, scale


def a8(q, x, scales):
    total = 0.0
    zeros = 0
    for g, scale in enumerate(scales):
        values = x[g * 64:(g + 1) * 64]
        codes, activation_scale = quantize_group(values)
        zeros += sum(v != 0 and c == 0 for v, c in zip(values, codes))
        dot = sum(w * c for w, c in zip(q[g * 64:(g + 1) * 64], codes))
        total = _fmaf(float(dot), f32(scale * activation_scale), total)
    return total, zeros


def byte_perm(a, b, selector):
    source = a | (b << 32)
    return sum(((source >> (8 * ((selector >> (4 * i)) & 7))) & 255) << (8 * i)
               for i in range(4))


def decode_s1(word, high, q5):
    if not q5:
        word ^= 0x88888888
    lo, hi = word & 0x0F0F0F0F, (word >> 4) & 0x0F0F0F0F
    even, odd = byte_perm(lo, hi, 0x5140), byte_perm(lo, hi, 0x7362)
    if q5:
        inv = (~high) & 255
        even |= (((inv & 15) * 0x00204081) & 0x01010101) << 4
        odd |= ((((inv >> 4) & 15) * 0x00204081) & 0x01010101) << 4
    pairs = (byte_perm(even, 0x43004300, 0x7150), byte_perm(even, 0x43004300, 0x7372),
             byte_perm(odd, 0x43004300, 0x7150), byte_perm(odd, 0x43004300, 0x7372))
    return [bf16_value((pair >> shift) & 65535) - (144 if q5 else 136)
            for pair in pairs for shift in (0, 16)]


def decode_a8(pair, high, q5):
    lo, hi = pair & 255, (pair >> 8) & 255
    value = (lo & 15) | ((lo & 240) << 4) | ((hi & 15) << 16) | ((hi & 240) << 20)
    if q5:
        bits = high & 15
        value |= ((bits | (bits << 7) | (bits << 14) | (bits << 21)) & 0x01010101) << 4
    bias = 16 if q5 else 8
    return [(((value >> (8 * i)) & 255) ^ bias) - bias for i in range(4)]


def format_checks():
    checked = 0
    for q5 in (False, True):
        for position in range(8):
            for code in range(-16 if q5 else -8, 16 if q5 else 8):
                expected = [0] * 8
                expected[position] = code
                word = sum((v & 15) << (4 * i) for i, v in enumerate(expected))
                high = sum(((v & 31) >> 4) << i for i, v in enumerate(expected))
                assert decode_s1(word, high, q5) == expected
                assert decode_a8(word & 65535, high, q5) == expected[:4]
                assert decode_a8(word >> 16, high >> 4, q5) == expected[4:]
                checked += 1
    # Each S1 MMA k16 uses the same permutation for both operands; all 64 k's once.
    s1 = [16 * lid + 4 * step + j for step in range(4)
          for j in (0, 1, 2, 3) for lid in range(4)]
    integer = [32 * ks + 16 * hi + 4 * tig + j for ks in range(2)
               for hi in range(2) for tig in range(4) for j in range(4)]
    assert sorted(s1) == sorted(integer) == list(range(64))
    assert quantize_group([0.0] * 64) == ([0] * 64, 0.0)
    # peak 127 gives inv=1: ties use nearest-even, never truncation/away-from-zero.
    assert quantize_group([127.0, 0.5, 1.5, 2.5, -0.5, -1.5])[0] == [127, 0, 2, 2, 0, -2]
    return {"signed_decode_cases": checked, "k_permutation_cardinality": 64,
            "zero_and_nearest_even": "pass", "q5_int32_group_dot_bound": 64 * 16 * 127,
            "q4_int32_group_dot_bound": 64 * 8 * 127}


def witnesses():
    dots, scales = [0.0] * 80, [1.0] * 80
    dots[0], dots[4], dots[8] = 1.0, 2.0 ** -24, -1.0
    w4, w8 = group_reduce(dots, scales, 4), group_reduce(dots, scales, 8)
    assert w4 == 0.0 and w8 == 2.0 ** -24
    # All other rows can be zero. This is a legal Q4 row at K5120: q=1 at
    # positions 0,256,512; BF16 x=1,2^-24,-1; FP16 scales=1. Each dot is exact.
    x = [448.0] + [1.0] * 63
    q = [0] + [1] * 63
    exact = math.fsum(w * v for w, v in zip(q, x))
    result, zeros = a8(q, x, [1.0])
    assert exact == 63.0 and result == 0.0 and zeros == 63
    codes, scale = quantize_group(x)
    return {"s1_warp_count": {"k": 5120, "nonzero_k": [0, 256, 512],
                "q": [1, 1, 1], "x": [1.0, 2.0 ** -24, -1.0], "scale": 1.0,
                "fp64": 2.0 ** -24, "w4": w4, "w8": w8,
                "w4_bf16": bf16_bits(w4), "w8_bf16": bf16_bits(w8),
                "claim": "exact inter-group arithmetic witness, GPU confirmation pending"},
            "group64_outlier": {"x": "[448] + [1]*63", "q": "[0] + [1]*63",
                "scale": 1.0, "fp64": exact, "a8": result, "erased_nonzero": zeros,
                "peak_code": codes[0], "activation_scale": scale,
                "claim": "INT8 scale granularity, not E4M3 saturation"}}


def error_stats(actual, reference):
    errors = [abs(a - r) for a, r in zip(actual, reference)]
    relative = [e / max(abs(r), 1e-30) for e, r in zip(errors, reference)]
    return {"rows": len(actual), "max_abs": max(errors),
            "relative_l2": math.sqrt(math.fsum(e * e for e in errors) /
                                     max(math.fsum(r * r for r in reference), 1e-60)),
            "max_relative": max(relative), "mean_relative": math.fsum(relative) / len(relative),
            "bf16_different_from_reference": sum(bf16_bits(a) != bf16_bits(r)
                                                  for a, r in zip(actual, reference))}


def representative_rows():
    records = []
    for k in (5120, 6144, 17408):
        for q5 in (False, True):
            for outliers in (False, True):
                rng = random.Random(1500 + k + 13 * q5 + 101 * outliers)
                values = {"fp64": [], "w4": [], "w8": [], "linear_w8": [], "a8": [], "split2": []}
                erased = 0
                for _ in range(16):
                    x = [bf16_value(bf16_bits(rng.randint(-255, 255) / 128 * 2.0 ** rng.randrange(-8, 5)))
                         for _ in range(k)]
                    if outliers:
                        for base in range(0, k, 64):
                            x[base] = 448.0
                    q = [rng.randrange(-16 if q5 else -8, 16 if q5 else 8) for _ in range(k)]
                    scales = [f16(rng.randint(1, 1023) / 1024 * 2.0 ** rng.randrange(-8, -2))
                              for _ in range(k // 64)]
                    reference = math.fsum(w * v * scales[i // 64] for i, (w, v) in enumerate(zip(q, x)))
                    dots = [f32(math.fsum(q[i] * x[i] for i in range(base, base + 64)))
                            for base in range(0, k, 64)]
                    values["fp64"].append(reference)
                    values["w4"].append(group_reduce(dots, scales, 4))
                    values["w8"].append(group_reduce(dots, scales, 8))
                    values["linear_w8"].append(group_reduce(dots, scales, 8, pairwise=False))
                    quantized, zeros = a8(q, x, scales)
                    values["a8"].append(quantized)
                    erased += zeros
                    if q5:
                        values["split2"].append(split2(q, x, scales))
                ref = values.pop("fp64")
                records.append({"k": k, "format": "Q5_G64" if q5 else "Q4_G64",
                    "fixture": "one_448_outlier_per_group" if outliers else "synthetic_bf16",
                    "reference": "FP64 on stored BF16 activations and FP16 group scales",
                    "limitation": "W4/W8 models round FP64 group dots once; not tensor-core emulation",
                    "errors": {name: error_stats(v, ref) for name, v in values.items() if v},
                    "w4_w8_fp32_different": sum(a != b for a, b in zip(values["w4"], values["w8"])),
                    "w4_w8_bf16_different": sum(bf16_bits(a) != bf16_bits(b)
                                                  for a, b in zip(values["w4"], values["w8"])),
                    "a8_erased_nonzero_activations": erased})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = {"schema_version": 1, "qualification": False,
              "source_base": "752dd887be6a9cdb2da6217f3b664b3c03ed33ae",
              "s1_source": "e1c2f66fcbb29c77ed6f794adfcb721b04f099ce",
              "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "format_checks": format_checks(), "witnesses": witnesses(),
              "representative_rows": representative_rows(),
              "policy": "S1 remains unadmitted; native Group64 A8 remains off; no runtime change"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    print(json.dumps({"format_checks": result["format_checks"], "witnesses": result["witnesses"],
                      "row_sets": len(result["representative_rows"]), "output": str(args.output)}))


if __name__ == "__main__":
    main()
