import random
import subprocess
import sys


MAX64 = (1 << 64) - 1
MAX128 = (1 << 128) - 1
rng = random.Random(0x128)
cases = []


def add(op, left, right):
    if op == "m":
        expected = (left & MAX64) * (right & MAX64)
    elif op == "a":
        expected = min(MAX128, left + right)
    elif op == "p":
        expected = min(MAX128, left * (right & MAX64))
    elif op == "s":
        expected = left >> right
    else:
        expected = divmod(left, right)
    cases.append((op, left, right, expected))


boundaries = [0, 1, 2, (1 << 32) - 1, 1 << 32, MAX64 - 1, MAX64,
              1 << 64, (1 << 127) - 1, 1 << 127, MAX128 - 1, MAX128]
for left in boundaries:
    for right in boundaries:
        for op in "map":
            add(op, left, right)
        if right:
            add("d", left, right)
    for shift in (1, 20, 32, 63):
        add("s", left, shift)
for _ in range(1024):
    left, right = rng.getrandbits(128), rng.getrandbits(128)
    for op in "mapd":
        add(op, left, right or 1)
    add("s", left, rng.randrange(1, 64))
# CLI fractional MiB inventory: the denominator can exceed 64 bits.
for value in (1, 524287, 524288, 1048575):
    numerator = value * (10 ** 20 // (1 << 20))
    add("d", numerator * (1 << 20), 10 ** 20)

payload = "".join(f"{op} {left >> 64:x} {left & MAX64:x} {right >> 64:x} {right & MAX64:x}\n"
                  for op, left, right, _ in cases)
result = subprocess.run([sys.argv[1]], input=payload, text=True, capture_output=True, check=True)
rows = result.stdout.splitlines()
if len(rows) != len(cases):
    raise AssertionError(f"expected {len(cases)} arithmetic rows, received {len(rows)}")
for case, row in zip(cases, rows):
    numbers = [int(word, 16) for word in row.split()]
    actual = tuple((numbers[i] << 64) | numbers[i + 1] for i in range(0, len(numbers), 2))
    expected = case[3] if isinstance(case[3], tuple) else (case[3],)
    if actual != expected:
        raise AssertionError(f"{case[:3]}: expected {expected}, received {actual}")
print(f"OK {len(cases)} exact arithmetic cases against Python integers")
