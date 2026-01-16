import argparse
import datetime as _dt
import os
import queue
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial


def _now_local_str() -> str:
    # Local time, ms precision
    return _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def _sanitize_text(s: str) -> str:
    """
    Make logs safe/readable in a text file / browser:
    - keep printable chars and common whitespace
    - escape other control characters as \\xNN
    """
    out = []
    for ch in s:
        if ch.isprintable() or ch in ("\t",):
            out.append(ch)
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\n":
            out.append("\\n")
        else:
            out.append(f"\\x{ord(ch):02x}")
    return "".join(out)


class _Broadcaster:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._subs: list[queue.Queue[str]] = []

    def subscribe(self, max_queue: int) -> queue.Queue[str]:
        q: queue.Queue[str] = queue.Queue(maxsize=max_queue)
        with self._lock:
            self._subs.append(q)
        return q

    def unsubscribe(self, q: queue.Queue[str]) -> None:
        with self._lock:
            try:
                self._subs.remove(q)
            except ValueError:
                pass

    def publish(self, msg: str) -> None:
        with self._lock:
            subs = list(self._subs)
        for q in subs:
            try:
                q.put_nowait(msg)
            except queue.Full:
                # Drop if a client is too slow.
                pass

    def count(self) -> int:
        with self._lock:
            return len(self._subs)


def _default_html_path() -> str:
    return str(Path(__file__).resolve().with_name("serial_sse_logger.html"))


def _load_html_bytes(path: str) -> bytes:
    try:
        return Path(path).read_bytes()
    except Exception as e:
        msg = f"<pre>Failed to load HTML: {path}\n{e!r}</pre>"
        return msg.encode("utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Read serial logs and share them over LAN via Server-Sent Events (SSE)."
    )

    # Defaults are tuned to match the user's typical command:
    # python log_manger/serial_sse_logger.py --port COM6 --baud 1000000 --out .\\uart.log --reconnect --flush-interval 0
    ap.add_argument("--port", default="COM6", help="Serial port (default: COM6)")
    ap.add_argument("--baud", type=int, default=1000000, help="Baudrate (default: 1000000)")
    ap.add_argument("--timeout", type=float, default=0.5, help="Serial read timeout seconds (default: 0.5)")
    ap.add_argument("--encoding", default="utf-8", help="Decode encoding for serial bytes (default: utf-8)")
    ap.add_argument(
        "--errors",
        default="replace",
        help="Decode errors handling (default: replace). Use 'ignore' to drop invalid bytes.",
    )
    ap.add_argument("--no-sanitize", action="store_true", help="Disable escaping of control characters.")
    ap.add_argument("--raw-bytes", action="store_true", help="Do not decode; send hex for each line.")
    ap.add_argument("--print", dest="also_print", action="store_true", help="Also print lines to stdout.")

    ap.add_argument(
        "--out",
        default="uart.log",
        help="Write to log file path (default: ./uart.log). Use --no-out to disable file logging.",
    )
    ap.add_argument("--no-out", action="store_true", help="Disable writing to log file.")
    ap.add_argument(
        "--utf8-bom",
        action="store_true",
        help="Write UTF-8 BOM at the beginning of a new log file (helps some Windows tools detect UTF-8).",
    )
    ap.add_argument(
        "--flush-interval",
        type=float,
        default=0.0,
        help="Flush interval seconds for file; 0 means flush every line (default: 0)",
    )

    # Default reconnect ON; you can disable it with --no-reconnect
    ap.add_argument(
        "--reconnect",
        dest="reconnect",
        action="store_true",
        default=True,
        help="Auto-reconnect when serial is disconnected (default: enabled).",
    )
    ap.add_argument(
        "--no-reconnect",
        dest="reconnect",
        action="store_false",
        help="Disable auto-reconnect.",
    )
    ap.add_argument(
        "--reconnect-delay",
        type=float,
        default=1.0,
        help="Seconds to wait before reconnect attempts (default: 1.0)",
    )

    ap.add_argument("--http-host", default="0.0.0.0", help="HTTP bind host (default: 0.0.0.0)")
    ap.add_argument("--http-port", type=int, default=8000, help="HTTP bind port (default: 8000)")
    ap.add_argument(
        "--max-client-queue",
        type=int,
        default=2000,
        help="Max buffered messages per client (default: 2000). Slow clients will drop messages.",
    )
    ap.add_argument(
        "--cors",
        action="store_true",
        help="Add Access-Control-Allow-Origin: * header (useful if embedding elsewhere).",
    )
    ap.add_argument(
        "--html",
        default=_default_html_path(),
        help="Path to the HTML page served at / (default: log_manger/serial_sse_logger.html next to this script).",
    )
    ap.add_argument(
        "--reload-html",
        action="store_true",
        help="Reload HTML file on each request (handy while editing the page).",
    )
    args = ap.parse_args()

    broadcaster = _Broadcaster()
    stop_event = threading.Event()

    out_path = None if args.no_out else args.out
    f = None
    last_flush = time.time()
    flush_interval_s = max(0.0, float(args.flush_interval))
    if out_path:
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        write_enc = "utf-8"
        if args.utf8_bom:
            try:
                if not os.path.exists(out_path) or os.path.getsize(out_path) == 0:
                    write_enc = "utf-8-sig"
            except Exception:
                write_enc = "utf-8"
        f = open(out_path, "a", encoding=write_enc, newline="\n")

    # HTML page content (default: cached once; optionally reload for easier maintenance)
    html_path = str(args.html)
    html_cached = _load_html_bytes(html_path)

    def _open_serial() -> serial.Serial:
        return serial.Serial(
            port=args.port,
            baudrate=args.baud,
            timeout=args.timeout,
        )

    def serial_thread() -> None:
        nonlocal f, last_flush
        try:
            try:
                ser = _open_serial()
            except Exception as e:
                msg = f"[ERROR] Failed to open {args.port} @ {args.baud}: {e}"
                broadcaster.publish(msg)
                print(msg, file=sys.stderr)
                return

            while not stop_event.is_set():
                try:
                    b = ser.readline()
                except (serial.SerialException, OSError) as e:
                    if not args.reconnect:
                        msg = f"[ERROR] Serial read failed: {e}"
                        broadcaster.publish(msg)
                        print(msg, file=sys.stderr)
                        return
                    try:
                        ser.close()
                    except Exception:
                        pass
                    msg = f"[WARN] Serial read failed ({e}); reconnecting in {args.reconnect_delay}s..."
                    broadcaster.publish(msg)
                    print(msg, file=sys.stderr)
                    time.sleep(max(0.1, float(args.reconnect_delay)))
                    while not stop_event.is_set():
                        try:
                            ser = _open_serial()
                            msg2 = f"[INFO] Reconnected: {args.port} @ {args.baud}"
                            broadcaster.publish(msg2)
                            print(msg2)
                            break
                        except Exception as e2:
                            msg3 = f"[WARN] Reconnect failed ({e2}); retrying in {args.reconnect_delay}s..."
                            broadcaster.publish(msg3)
                            print(msg3, file=sys.stderr)
                            time.sleep(max(0.1, float(args.reconnect_delay)))
                    continue

                if not b:
                    continue

                ts = _now_local_str()
                if args.raw_bytes:
                    payload = b.hex()
                    line = f"[{ts}] {payload}"
                else:
                    s = b.decode(args.encoding, errors=args.errors).rstrip("\r\n")
                    if not args.no_sanitize:
                        s = _sanitize_text(s)
                    line = f"[{ts}] {s}"

                broadcaster.publish(line)
                if args.also_print:
                    print(line)
                if f is not None:
                    f.write(line + "\n")
                    now = time.time()
                    if flush_interval_s <= 0.0 or (now - last_flush >= flush_interval_s):
                        try:
                            f.flush()
                        except Exception:
                            pass
                        last_flush = now
        finally:
            if f is not None:
                try:
                    f.flush()
                except Exception:
                    pass
                try:
                    f.close()
                except Exception:
                    pass

    class Handler(BaseHTTPRequestHandler):
        server_version = "serial-sse/1.0"

        def _set_common_headers(self) -> None:
            if args.cors:
                self.send_header("Access-Control-Allow-Origin", "*")

        def do_GET(self) -> None:
            if self.path == "/" or self.path.startswith("/?"):
                body = _load_html_bytes(html_path) if args.reload_html else html_cached
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self._set_common_headers()
                self.end_headers()
                self.wfile.write(body)
                return

            if self.path.startswith("/health"):
                body = f"ok clients={broadcaster.count()}\n".encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self._set_common_headers()
                self.end_headers()
                self.wfile.write(body)
                return

            if self.path.startswith("/events"):
                q = broadcaster.subscribe(max_queue=args.max_client_queue)
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream; charset=utf-8")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self._set_common_headers()
                self.end_headers()
                try:
                    # Initial comment to open stream
                    self.wfile.write(b": connected\n\n")
                    self.wfile.flush()
                    last_keepalive = time.time()
                    while not stop_event.is_set():
                        try:
                            msg = q.get(timeout=1.0)
                            data = msg.replace("\r", "\\r").replace("\n", "\\n")
                            self.wfile.write(("data: " + data + "\n\n").encode("utf-8"))
                            self.wfile.flush()
                        except queue.Empty:
                            # keepalive comment every ~15s to avoid proxies timing out
                            if time.time() - last_keepalive > 15.0:
                                self.wfile.write(b": keepalive\n\n")
                                self.wfile.flush()
                                last_keepalive = time.time()
                except (BrokenPipeError, ConnectionResetError, socket.error):
                    pass
                finally:
                    broadcaster.unsubscribe(q)
                return

            self.send_response(404)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self._set_common_headers()
            self.end_headers()
            self.wfile.write(b"not found\n")

        def log_message(self, format: str, *args2) -> None:
            # Keep stdout clean unless the user wants it.
            return

    class _QuietThreadingHTTPServer(ThreadingHTTPServer):
        """
        The stdlib server prints scary tracebacks for common client disconnects
        (browser refresh/close, network hiccups). Those are normal; silence them.
        """

        def handle_error(self, request, client_address):  # type: ignore[override]
            exc = sys.exc_info()[1]
            if isinstance(
                exc,
                (
                    ConnectionResetError,
                    ConnectionAbortedError,
                    BrokenPipeError,
                    TimeoutError,
                    OSError,
                ),
            ):
                # Common on Windows: WinError 10053/10054 when client disconnects.
                return
            return super().handle_error(request, client_address)

    t = threading.Thread(target=serial_thread, daemon=True)
    t.start()

    host = args.http_host
    port = args.http_port
    httpd = _QuietThreadingHTTPServer((host, port), Handler)
    addr = httpd.server_address
    print(f"[INFO] Serial: {args.port} @ {args.baud}")
    if out_path:
        print(f"[INFO] File log: {out_path}")
    else:
        print("[INFO] File log: disabled (use --out <path> to enable)")
    print(f"[INFO] SSE server listening on http://{addr[0]}:{addr[1]}/  (LAN: use your PC IP)")
    print("[INFO] Endpoints: / (web UI), /events (SSE), /health")
    print("[INFO] Press Ctrl+C to stop.")

    try:
        httpd.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        print("\n[INFO] Stopped by user.")
        return 0
    finally:
        stop_event.set()
        try:
            httpd.shutdown()
        except Exception:
            pass
        try:
            httpd.server_close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

