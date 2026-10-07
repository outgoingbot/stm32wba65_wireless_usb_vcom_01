"""Rough one-way serial bridge latency, measured with one PC's monotonic clock.

Install: py -m pip install pyserial
Example: py tests/serial_latency.py COM13 COM16 --baud 115200 --poll-ms 100
The polling interval controls how often a timestamp probe is sent. A separate
reader waits continuously, so reception is not delayed until the next poll.
"""

import argparse
import secrets
import statistics
import threading
import time


def positive(value):
    number = float(value)
    if not 0 < number < float("inf"):
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tx_port", help="port to send timestamps into")
    parser.add_argument("rx_port", help="port to receive timestamps from")
    parser.add_argument("--baud", type=int, default=115200, help="baud rate (default: 115200)")
    parser.add_argument("--poll-ms", type=positive, default=100, help="probe interval in milliseconds (default: 100)")
    parser.add_argument("--timeout-ms", type=positive, default=3000, help="maximum probe latency before counting a timeout (default: 3000)")
    parser.add_argument("--count", type=int, default=0, help="number of probes; 0 runs until Ctrl+C (default: 0)")
    args = parser.parse_args()
    if args.tx_port.upper() == args.rx_port.upper():
        parser.error("send and receive ports must be different")
    if args.baud <= 0 or args.count < 0:
        parser.error("baud must be positive and count must be nonnegative")
    try:
        import serial
    except ImportError:
        parser.exit(1, "Missing pyserial. Install with: py -m pip install pyserial\n")

    run_id = secrets.token_hex(4)
    pending = {}
    latencies = []
    lock = threading.Lock()
    stop = threading.Event()
    errors = []
    sent = missed = 0
    timeout_ns = int(args.timeout_ms * 1_000_000)

    def receive(port):
        buffer = bytearray()
        try:
            while not stop.is_set():
                # read_until returns immediately on a newline, even with a timeout.
                chunk = port.read_until(b"\n", size=256)
                received_ns = time.perf_counter_ns()
                buffer.extend(chunk)
                while b"\n" in buffer:
                    line, _, remainder = buffer.partition(b"\n")
                    buffer[:] = remainder
                    fields = line.split(b",")
                    if len(fields) != 4 or fields[:2] != [b"LAT", run_id.encode()]:
                        continue
                    try:
                        sequence, stamp = int(fields[2]), int(fields[3])
                    except ValueError:
                        continue
                    with lock:
                        if pending.get(sequence) != stamp:
                            continue
                        elapsed = received_ns - stamp
                        # Leave expired probes to the main loop's timeout accounting.
                        if elapsed > timeout_ns:
                            continue
                        del pending[sequence]
                        latency = elapsed / 1_000_000
                        latencies.append(latency)
                    print(f"seq={sequence:6d}  latency={latency:9.3f} ms", flush=True)
                if len(buffer) > 256:
                    buffer.clear()
        except (serial.SerialException, OSError) as exc:
            errors.append(str(exc))
            stop.set()

    print(f"{args.tx_port} -> {args.rx_port}, {args.baud} baud, probe every {args.poll_ms:g} ms")
    print("Measures host write through host receipt, including USB/OS buffering. Ctrl+C stops.")
    try:
        with serial.Serial(args.tx_port, args.baud, timeout=0.1, write_timeout=1,
                           xonxoff=False, rtscts=False, dsrdtr=False) as tx, \
             serial.Serial(args.rx_port, args.baud, timeout=0.1, write_timeout=1,
                           xonxoff=False, rtscts=False, dsrdtr=False) as rx:
            rx.reset_input_buffer()
            reader = threading.Thread(target=receive, args=(rx,), daemon=True)
            reader.start()
            next_send = time.perf_counter()
            try:
                while not stop.is_set():
                    now_ns = time.perf_counter_ns()
                    with lock:
                        expired = [seq for seq, stamp in pending.items() if now_ns - stamp > timeout_ns]
                        for seq in expired:
                            del pending[seq]
                        remaining = len(pending)
                    missed += len(expired)
                    for seq in expired:
                        print(f"seq={seq:6d}  TIMEOUT (>{args.timeout_ms:g} ms)", flush=True)
                    if args.count and sent >= args.count:
                        if not remaining:
                            break
                    elif time.perf_counter() >= next_send:
                        sequence = sent + 1
                        stamp = time.perf_counter_ns()
                        packet = f"LAT,{run_id},{sequence},{stamp}\n".encode("ascii")
                        with lock:
                            pending[sequence] = stamp
                        sent += 1
                        if tx.write(packet) != len(packet):
                            raise serial.SerialException("Incomplete probe write")
                        # Avoid catch-up bursts when Windows schedules us late.
                        next_send = time.perf_counter() + args.poll_ms / 1000
                    stop.wait(0.001)
            except KeyboardInterrupt:
                print("\nStopped.")
            finally:
                stop.set()
                reader.join()
    except (serial.SerialException, OSError) as exc:
        errors.append(str(exc))
    finally:
        print(f"\nSent: {sent}, received: {len(latencies)}, timed out: {missed}, pending at stop: {len(pending)}")
        if latencies:
            ordered = sorted(latencies)
            p95 = ordered[max(0, (95 * len(ordered) + 99) // 100 - 1)]
            print(f"Latency ms: min={min(latencies):.3f}, median={statistics.median(latencies):.3f}, "
                  f"mean={statistics.mean(latencies):.3f}, p95={p95:.3f}, max={max(latencies):.3f}")
    if errors:
        parser.exit(1, "Error: " + "; ".join(errors) + "\nClose other applications using these ports and check connections.\n")
    if not latencies:
        parser.exit(1, "No valid probes received. Check port direction and the wireless link.\n")


if __name__ == "__main__":
    main()
