"""
Lightweight zero-lock multi-worker process manager.

Architecture:
  - Each worker is a fully independent OS process with its own GIL,
    memory space, event loop, protocol pool, and connection tracking.
  - Zero shared state = zero locks = zero contention.
  - On Linux: Master-accept + fd dispatch (Node.js cluster pattern).
    Master does all accept(), round-robins fds to workers via Unix domain
    socketpairs (SCM_RIGHTS). Workers use uvloop for full-speed I/O.
    Zero thundering herd, guaranteed even distribution.
  - On Windows: subprocess.Popen + socket.share()/fromshare() — parent
    creates one socket, shares with children.
"""

from __future__ import annotations

import array
import os
import signal
import socket as _socket
import struct
import subprocess
import sys
import threading
import time
from typing import Any

_WORKER_ENV = "_ASTRAAPI_WORKER"
_WORKER_ID_ENV = "_ASTRAAPI_WORKER_ID"

_HAS_FORK = hasattr(os, "fork")
_HAS_REUSEPORT = hasattr(_socket, "SO_REUSEPORT")


def is_worker() -> bool:
    """Return True if running inside a worker process."""
    return os.environ.get(_WORKER_ENV) == "1"


def _try_tune_sysctl() -> None:
    """Tune TCP kernel parameters (Linux/WSL). Run once in parent."""
    try:
        for _cmd in (
            "sysctl -w net.core.somaxconn=65535",
            "sysctl -w net.ipv4.tcp_max_syn_backlog=65535",
            "sysctl -w net.core.netdev_max_backlog=65535",
            "sysctl -w net.ipv4.tcp_tw_reuse=1",
            "sysctl -w net.ipv4.tcp_fin_timeout=10",
        ):
            subprocess.run(_cmd.split(), capture_output=True, timeout=2)
    except Exception:
        pass


def _create_listen_socket(host: str, port: int) -> _socket.socket:
    """Create, bind, and listen on a single TCP socket."""
    sock = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    sock.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    sock.listen(65535)
    sock.setblocking(False)
    try:
        sock.setsockopt(_socket.IPPROTO_TCP, _socket.TCP_NODELAY, 1)
    except (OSError, AttributeError):
        pass
    return sock


def _run_worker_fork(app: Any, host: str, port: int, unix_sock: Any) -> None:
    """Worker entry point after os.fork() — receives connections via unix socket."""
    import asyncio

    try:
        import uvloop
        asyncio.set_event_loop_policy(uvloop.EventLoopPolicy())
    except ImportError:
        import winloop
        asyncio.set_event_loop_policy(winloop.EventLoopPolicy())

    from astraapi._cpp_server import run_server
    asyncio.run(run_server(app, host, port, unix_sock=unix_sock))


def _run_worker_reuseport(app: Any, host: str, port: int) -> None:
    """Worker entry point for SO_REUSEPORT mode — each worker binds its own socket.

    The kernel distributes incoming connections directly to worker sockets,
    eliminating the master accept thread and IPC overhead entirely.
    """
    import asyncio

    try:
        import uvloop
        asyncio.set_event_loop_policy(uvloop.EventLoopPolicy())
    except ImportError:
        try:
            import winloop
            asyncio.set_event_loop_policy(winloop.EventLoopPolicy())
        except ImportError:
            pass

    # Create a per-worker socket with SO_REUSEPORT
    sock = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    sock.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEPORT, 1)
    except (AttributeError, OSError):
        pass  # Fallback: will still work, just no kernel-level balancing
    sock.bind((host, port))
    sock.listen(65535)
    sock.setblocking(False)
    try:
        sock.setsockopt(_socket.IPPROTO_TCP, _socket.TCP_NODELAY, 1)
    except (OSError, AttributeError):
        pass
    try:
        sock.setsockopt(_socket.IPPROTO_TCP, 9, 1)  # TCP_DEFER_ACCEPT
    except (OSError, AttributeError):
        pass

    from astraapi._cpp_server import run_server
    asyncio.run(run_server(app, host, port, sock=sock))


def _master_accept_loop(
    listen_sock: _socket.socket,
    worker_socks: list[_socket.socket | None],
    worker_lock: threading.Lock,
) -> None:
    """Master accept thread — round-robin fd dispatch to workers.

    Accepts connections on the listen socket and dispatches fds to workers
    via SCM_RIGHTS over Unix domain socketpairs. Both accept() and sendmsg()
    release the GIL while blocking, so the supervisor thread is not stalled.
    """
    listen_sock.setblocking(True)
    idx = 0
    fds_data = array.array("i", [0])  # pre-allocate, reuse per connection
    _SCM = (_socket.SOL_SOCKET, _socket.SCM_RIGHTS)
    _msg = [b"\x00"]

    while True:
        try:
            conn, _ = listen_sock.accept()
        except OSError:
            break

        fds_data[0] = conn.fileno()
        target_sock = None

        with worker_lock:
            n = len(worker_socks)
            if n == 0:
                conn.close()
                continue
            for _ in range(n):
                target = idx % n
                idx += 1
                s = worker_socks[target]
                if s is not None:
                    target_sock = s
                    break

        if target_sock is not None:
            try:
                target_sock.sendmsg(_msg, [(*_SCM, fds_data)])
            except OSError:
                pass
        conn.close()


def _spawn_worker_reuseaddr(worker_id: int) -> subprocess.Popen:
    """Spawn a worker subprocess that binds its own socket (Windows).

    Each worker calls bind()+listen() with SO_REUSEADDR independently.
    Windows allows multiple processes to bind the same port this way.
    No socket sharing or dispatch needed.
    """
    env = {**os.environ, _WORKER_ENV: "1", _WORKER_ID_ENV: str(worker_id)}
    return subprocess.Popen(
        [sys.executable] + sys.argv, env=env,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )


def _signal_child(pid: int) -> None:
    """Send termination signal to a child process."""
    try:
        if sys.platform == "win32":
            os.kill(pid, signal.CTRL_BREAK_EVENT)
        else:
            os.kill(pid, signal.SIGTERM)
    except (OSError, ProcessLookupError):
        pass


def run_multiworker(app: Any, host: str, port: int, workers: int) -> None:
    """Supervisor: fork/spawn N workers and monitor them.

    Each worker is fully independent — separate GIL, separate memory,
    separate event loop.

    Linux: Master-accept + fd dispatch — master does all accept() and
    round-robins fds to workers via Unix domain socketpairs (SCM_RIGHTS).
    Workers keep uvloop for full-speed I/O. Zero thundering herd.

    Windows: Master-accept + socket.share() dispatch — master does all
    accept() and round-robins connections to workers via AF_INET
    socketpairs + socket.share(pid)/fromshare(). Zero thundering herd.
    """
    workers = max(1, workers)

    # Raise fd limit so master + workers can handle many connections
    try:
        import resource
        _soft, _hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        resource.setrlimit(resource.RLIMIT_NOFILE, (min(65535, _hard), _hard))
    except Exception:
        pass

    # Tune TCP kernel params ONCE in the parent
    _try_tune_sysctl()

    # Sync routes before forking so children inherit the frozen route table
    # via COW pages (read-only after freeze — never copied).
    app._sync_routes_to_core()
    core_app = getattr(app, "_core_app", None)
    if core_app and hasattr(core_app, "freeze_routes"):
        core_app.freeze_routes()

    print(f"Starting {workers} worker process{'es' if workers > 1 else ''}...")
    print(f"  Listening on http://{host}:{port}")

    listen_sock = _create_listen_socket(host, port)

    if _HAS_FORK and sys.platform != "win32":
        _run_fork_supervisor(app, host, port, workers, listen_sock)
    else:
        _run_subprocess_supervisor(host, port, workers, listen_sock)

    listen_sock.close()


def _run_fork_supervisor(
    app: Any, host: str, port: int, workers: int,
    listen_sock: _socket.socket,
) -> None:
    """Fork-based supervisor with master-accept (Linux/macOS).

    Master creates Unix domain socketpairs, forks workers, then runs a
    centralized accept thread that round-robins accepted fds to workers
    via SCM_RIGHTS. Workers never call accept() — zero thundering herd,
    guaranteed even distribution (Node.js cluster SCHED_RR pattern).
    """
    children: dict[int, int] = {}  # pid → worker_id
    _shutdown = False

    def _handle_signal(signum: int, frame: Any) -> None:
        nonlocal _shutdown
        _shutdown = True

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    # ── SO_REUSEPORT mode (Linux 3.9+): each worker binds its own socket ──
    # The kernel distributes connections directly — no master accept thread,
    # no IPC, no lock contention. Falls back to master-accept if unavailable.
    use_reuseport = _HAS_REUSEPORT
    if use_reuseport:
        # Verify SO_REUSEPORT actually works (some containers block it)
        try:
            _test = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
            _test.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEPORT, 1)
            _test.close()
        except (OSError, AttributeError):
            use_reuseport = False

    if use_reuseport:
        # Close the parent's listen socket BEFORE forking — it was created
        # without SO_REUSEPORT, which blocks worker bind() on the same port.
        # Each worker will create its own socket with SO_REUSEPORT.
        listen_sock.close()

    parent_socks: list[_socket.socket | None] = []
    child_socks: list[_socket.socket] = []
    worker_lock = threading.Lock()

    if not use_reuseport:
        # ── Create Unix domain socketpairs (one per worker) ──
        for _ in range(workers):
            p, c = _socket.socketpair(_socket.AF_UNIX, _socket.SOCK_STREAM)
            parent_socks.append(p)
            child_socks.append(c)

    # ── Fork workers ──
    for worker_id in range(workers):
        pid = os.fork()
        if pid == 0:
            signal.signal(signal.SIGINT, signal.SIG_DFL)
            signal.signal(signal.SIGTERM, signal.SIG_DFL)
            if not use_reuseport:
                listen_sock.close()
                # Close all socketpair ends except our own
                for i, s in enumerate(child_socks):
                    if i != worker_id:
                        s.close()
                for s in parent_socks:
                    if s is not None:
                        s.close()
            os.environ[_WORKER_ENV] = "1"
            os.environ[_WORKER_ID_ENV] = str(worker_id)
            # Pin worker to a dedicated core — eliminates cross-core cache
            # thrashing (L2/L3 invalidations, TLB flushes between migrations).
            if hasattr(os, "sched_setaffinity"):
                cpu_count = os.cpu_count() or 1
                try:
                    os.sched_setaffinity(0, {worker_id % cpu_count})
                except OSError:
                    pass
            try:
                if use_reuseport:
                    _run_worker_reuseport(app, host, port)
                else:
                    _run_worker_fork(app, host, port, child_socks[worker_id])
            except KeyboardInterrupt:
                pass
            except Exception:
                import traceback
                traceback.print_exc()
            finally:
                os._exit(0)
        else:
            children[pid] = worker_id
            mode = "reuseport" if use_reuseport else "master-accept"
            print(f"  Worker {worker_id} started (pid {pid}, {mode})")

    if not use_reuseport:
        # Master: close child ends (workers have their own copies via fork)
        for s in child_socks:
            s.close()

        # ── Start centralized accept thread ──
        accept_thread = threading.Thread(
            target=_master_accept_loop,
            args=(listen_sock, parent_socks, worker_lock),
            daemon=True,
        )
        accept_thread.start()

    # ── Supervisor loop: monitor children, restart on crash ──
    while not _shutdown and children:
        try:
            pid, status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            break
        except InterruptedError:
            continue

        if pid > 0 and pid in children:
            worker_id = children.pop(pid)
            exit_code = os.WEXITSTATUS(status) if os.WIFEXITED(status) else -1
            if _shutdown:
                break

            print(f"  Worker {worker_id} exited (code {exit_code}), restarting...")

            if use_reuseport:
                # SO_REUSEPORT: just fork a new worker — it creates its own socket
                new_pid = os.fork()
                if new_pid == 0:
                    signal.signal(signal.SIGINT, signal.SIG_DFL)
                    signal.signal(signal.SIGTERM, signal.SIG_DFL)
                    os.environ[_WORKER_ENV] = "1"
                    os.environ[_WORKER_ID_ENV] = str(worker_id)
                    if hasattr(os, "sched_setaffinity"):
                        cpu_count = os.cpu_count() or 1
                        try:
                            os.sched_setaffinity(0, {worker_id % cpu_count})
                        except OSError:
                            pass
                    try:
                        _run_worker_reuseport(app, host, port)
                    except KeyboardInterrupt:
                        pass
                    except Exception:
                        import traceback
                        traceback.print_exc()
                    finally:
                        os._exit(0)
                else:
                    children[new_pid] = worker_id
                    print(f"  Worker {worker_id} restarted (pid {new_pid}, reuseport)")
            else:
                # Master-accept: close old socketpair, create new one
                with worker_lock:
                    old_sock = parent_socks[worker_id]
                    if old_sock is not None:
                        old_sock.close()
                        parent_socks[worker_id] = None

                new_p, new_c = _socket.socketpair(
                    _socket.AF_UNIX, _socket.SOCK_STREAM)

                new_pid = os.fork()
                if new_pid == 0:
                    signal.signal(signal.SIGINT, signal.SIG_DFL)
                    signal.signal(signal.SIGTERM, signal.SIG_DFL)
                    listen_sock.close()
                    new_p.close()
                    os.environ[_WORKER_ENV] = "1"
                    os.environ[_WORKER_ID_ENV] = str(worker_id)
                    if hasattr(os, "sched_setaffinity"):
                        cpu_count = os.cpu_count() or 1
                        try:
                            os.sched_setaffinity(0, {worker_id % cpu_count})
                        except OSError:
                            pass
                    try:
                        _run_worker_fork(app, host, port, new_c)
                    except KeyboardInterrupt:
                        pass
                    finally:
                        os._exit(0)
                else:
                    new_c.close()
                    with worker_lock:
                        parent_socks[worker_id] = new_p
                    children[new_pid] = worker_id
                    print(f"  Worker {worker_id} restarted (pid {new_pid})")
        else:
            time.sleep(1.0)

    # ── Shutdown ──
    print("\nShutting down workers...")
    # Close listen socket to stop accept thread
    try:
        listen_sock.close()
    except OSError:
        pass

    for pid in list(children):
        _signal_child(pid)

    deadline = time.monotonic() + 5.0
    while children and time.monotonic() < deadline:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
            if pid > 0:
                children.pop(pid, None)
            else:
                time.sleep(0.05)
        except ChildProcessError:
            break

    for pid in children:
        try:
            os.kill(pid, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass

    # Clean up socketpairs
    with worker_lock:
        for s in parent_socks:
            if s is not None:
                try:
                    s.close()
                except OSError:
                    pass


def _run_subprocess_supervisor(
    host: str, port: int, workers: int,
    listen_sock: _socket.socket,
) -> None:
    """Subprocess-based supervisor for Windows.

    Each worker binds its own socket with SO_REUSEADDR — Windows allows
    multiple processes to bind the same port. No master-accept, no
    socket.share() overhead. Workers are fully independent.
    """
    # Close parent socket — each worker creates its own
    listen_sock.close()

    procs: dict[int, tuple[subprocess.Popen, int]] = {}
    _shutdown = False

    def _handle_signal(signum: int, frame: Any) -> None:
        nonlocal _shutdown
        _shutdown = True

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    print("  [MASTER] Using SO_REUSEADDR multi-bind mode (Windows)")
    for worker_id in range(workers):
        proc = _spawn_worker_reuseaddr(worker_id)
        procs[id(proc)] = (proc, worker_id)
        print(f"  Worker {worker_id} started (pid {proc.pid})")

    # Monitor and restart crashed workers
    while not _shutdown and procs:
        to_restart: list[int] = []
        for key, (proc, worker_id) in list(procs.items()):
            ret = proc.poll()
            if ret is not None:
                del procs[key]
                if not _shutdown:
                    print(f"  Worker {worker_id} exited (code {ret}), restarting...")
                    to_restart.append(worker_id)

        for worker_id in to_restart:
            proc = _spawn_worker_reuseaddr(worker_id)
            procs[id(proc)] = (proc, worker_id)
            print(f"  Worker {worker_id} restarted (pid {proc.pid})")

        if not _shutdown:
            time.sleep(1.0)

    # Shutdown
    print("\nShutting down workers...")
    for proc, _ in procs.values():
        _signal_child(proc.pid)

    deadline = time.monotonic() + 5.0
    for proc, _ in list(procs.values()):
        remaining = max(0.1, deadline - time.monotonic())
        try:
            proc.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            proc.kill()


