"""Hardware-only acceptance test; needs pyserial and both boards on one PC.
Run: python tests/hardware_bridge_check.py COM5 COM6
Close terminal applications first. This does not flash or configure firmware.
"""
import argparse
import random
from concurrent.futures import ThreadPoolExecutor
import serial


def exchange(port, payload):
    port.write(payload)
    port.flush()
    return port.read(len(payload))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port_a")
    parser.add_argument("port_b")
    args = parser.parse_args()
    rng = random.Random(154)
    with serial.Serial(args.port_a, 115200, timeout=5, write_timeout=5,
                       xonxoff=False, rtscts=False) as a, \
         serial.Serial(args.port_b, 115200, timeout=5, write_timeout=5,
                       xonxoff=False, rtscts=False) as b, \
         ThreadPoolExecutor(max_workers=2) as pool:
        a.reset_input_buffer()
        b.reset_input_buffer()
        for size in (1, 63, 64, 65, 95, 96, 97, 511, 512, 513, 1024):
            # Exercise both directions concurrently, including USB/MAC boundaries.
            sent_a = bytes(rng.randrange(256) for _ in range(size))
            sent_b = bytes(rng.randrange(256) for _ in range(size))
            fa = pool.submit(exchange, a, sent_a)
            fb = pool.submit(exchange, b, sent_b)
            got_a, got_b = fa.result(), fb.result()
            if got_a != sent_b or got_b != sent_a:
                raise RuntimeError(f"Mismatch at {size} bytes: A received {len(got_a)}, B received {len(got_b)}")
            print(f"PASS: {size} bytes each direction")
    print("All hardware exchanges passed.")


if __name__ == "__main__":
    main()
