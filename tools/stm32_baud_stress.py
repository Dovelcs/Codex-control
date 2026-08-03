import argparse
import time

import serial


def read_quiet(port, quiet=0.12, limit=3.0):
    output = bytearray()
    deadline = time.monotonic() + limit
    last_data = time.monotonic()
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            output.extend(chunk)
            last_data = time.monotonic()
        elif output and time.monotonic() - last_data >= quiet:
            break
    return bytes(output)


def command(port, value, limit=3.0):
    port.write(value.encode("ascii") + b"\r\n")
    port.flush()
    return read_quiet(port, limit=limit)


def command_to_prompt(port, value, limit=3.0):
    port.write(value.encode("ascii") + b"\r\n")
    port.flush()
    output = bytearray()
    deadline = time.monotonic() + limit
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            output.extend(chunk)
            if output.endswith(b"cli> "):
                break
    return bytes(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baud", type=int)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--status-trials", type=int, default=200)
    parser.add_argument("--loop-trials", type=int, default=200)
    parser.add_argument("--loop-timeout", type=float, default=1.0)
    args = parser.parse_args()

    device = serial.Serial(args.port, args.baud, timeout=0.01)
    try:
        time.sleep(1.0)
        device.reset_input_buffer()
        device.write(b"\x1d\x1d")
        device.flush()
        entered = read_quiet(device)
        print(f"CLI_ENTRY ok={b'cli> ' in entered} length={len(entered)}", flush=True)
        if b"cli> " not in entered:
            return 2

        command(device, "cache clear", limit=10.0)
        status_bad = []
        status_sizes = []
        for trial in range(args.status_trials):
            output = command_to_prompt(device, "cache status")
            status_sizes.append(len(output))
            valid = b"cli> " in output
            try:
                output.decode("utf-8")
            except UnicodeDecodeError:
                valid = False
            if not valid:
                status_bad.append((trial, len(output)))
        print(
            f"CLI_STATUS bad={len(status_bad)}/{args.status_trials} "
            f"sizes={sorted(set(status_sizes))} details={status_bad[:20]}",
            flush=True,
        )

        command(device, "cache clear", limit=10.0)
        command(device, "exit")
        time.sleep(0.2)

        loop_bad = []
        missing = 0
        for trial in range(args.loop_trials):
            pattern = bytes(32 + ((trial * 37 + index) % 95) for index in range(512))
            device.reset_input_buffer()
            device.write(pattern)
            device.flush()
            output = bytearray()
            deadline = time.monotonic() + args.loop_timeout
            while len(output) < len(pattern) and time.monotonic() < deadline:
                chunk = device.read(len(pattern) - len(output))
                if chunk:
                    output.extend(chunk)
            if bytes(output) != pattern:
                first = next(
                    (
                        index
                        for index, (actual, expected) in enumerate(zip(output, pattern))
                        if actual != expected
                    ),
                    min(len(output), len(pattern)),
                )
                loop_bad.append((trial, len(output), first))
                missing += max(0, len(pattern) - len(output))
        total = args.loop_trials * 512
        print(
            f"LOOPBACK bad={len(loop_bad)}/{args.loop_trials} "
            f"missing={missing}/{total} details={loop_bad[:30]}",
            flush=True,
        )

        device.reset_input_buffer()
        device.write(b"\x1d\x1d")
        device.flush()
        read_quiet(device)
        status = command(device, "system status", limit=5.0).decode(
            "utf-8", errors="replace"
        )
        for line in status.splitlines():
            if any(
                label in line
                for label in (
                    "Debug RX",
                    "Debug TX",
                    "UART2 RX",
                    "UART2 TX",
                    "Debug\u9519\u8bef",
                    "UART2\u9519\u8bef",
                )
            ):
                print("COUNTER " + line.strip(), flush=True)
        command(device, "cache clear", limit=10.0)
        command(device, "exit")
        return 0 if not status_bad and not loop_bad else 1
    finally:
        device.close()


if __name__ == "__main__":
    raise SystemExit(main())
