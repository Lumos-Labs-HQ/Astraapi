"""
Process-level hot reloader for app.run(reload=True).

Architecture:
  Supervisor process — watches files, manages worker lifecycle
  Worker process    — runs the actual C++ HTTP server (app.run() without reload)

On file change: signal old worker, immediately spawn new one (overlapped restart).
The new worker binds with SO_REUSEADDR while the old one drains.
When reload=False: this module is never imported (zero overhead).
"""
from __future__ import annotations

import os
import signal
import subprocess
import sys
import threading
from pathlib import Path
from typing import Optional, Sequence

# Environment variable to distinguish supervisor vs worker
_RELOAD_WORKER_ENV = "_FASTAPI_RELOAD_WORKER"

# Debounce for file watcher (ms) — batches rapid multi-file saves
_DEBOUNCE_MS = 100

# Max consecutive crash restarts before giving up
_MAX_CRASH_RETRIES = 3


def is_reload_worker() -> bool:
    """Check if the current process is a reload worker (not the supervisor)."""
    return os.environ.get(_RELOAD_WORKER_ENV) == "1"


def _get_watch_dirs(
    reload_dirs: Optional[Sequence[str]] = None,
) -> list[Path]:
    """Determine which directories to watch for changes."""
    if reload_dirs:
        return [Path(d).resolve() for d in reload_dirs]
    return [Path.cwd()]


def _spawn_worker(
    args: list[str],
    env: dict[str, str],
) -> subprocess.Popen:
    """Spawn the worker process using the same Python interpreter and script.

    The worker runs with _FASTAPI_RELOAD_WORKER=1 set, which causes
    app.run(reload=True) to fall through to the normal server startup.
    """
    kwargs: dict = dict(
        env=env,
        stdout=None,  # inherit
        stderr=None,  # inherit
    )
    if sys.platform == "win32":
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    return subprocess.Popen(args, **kwargs)


def _signal_worker(process: subprocess.Popen) -> None:
    """Send termination signal to worker (non-blocking).

    Does NOT wait for the process to exit — the old worker drains
    in the background while the new worker starts up.
    """
    if process.poll() is not None:
        return
    try:
        if sys.platform == "win32":
            os.kill(process.pid, signal.CTRL_BREAK_EVENT)
        else:
            process.terminate()
    except OSError:
        pass


def _reap_worker(process: subprocess.Popen, timeout: float = 5.0) -> None:
    """Wait for worker to exit, escalate to SIGKILL if needed.

    Called in a background thread so the supervisor isn't blocked.
    """
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        try:
            process.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            pass


def _terminate_worker(process: subprocess.Popen) -> None:
    """Signal worker and reap in background thread (non-blocking).

    The old worker drains connections while the new worker is already
    starting up — this overlaps shutdown and startup for faster reloads.
    """
    _signal_worker(process)
    threading.Thread(target=_reap_worker, args=(process,), daemon=True).start()


def _terminate_worker_sync(process: subprocess.Popen, timeout: float = 5.0) -> None:
    """Signal worker and wait for exit (blocking). Used for final shutdown."""
    _signal_worker(process)
    _reap_worker(process, timeout)


def run_with_reload(
    reload_dirs: Optional[Sequence[str]] = None,
    reload_includes: Optional[Sequence[str]] = None,
    reload_excludes: Optional[Sequence[str]] = None,
) -> None:
    """Supervisor process: watch for file changes and restart the worker.

    1. Spawns a worker subprocess (same script, with _FASTAPI_RELOAD_WORKER=1)
    2. Uses watchfiles to monitor .py files for changes
    3. On change: signal old worker + immediately spawn new one (overlapped)
    4. On Ctrl+C: terminate worker, exit supervisor
    """
    from watchfiles import PythonFilter, watch

    watch_dirs = _get_watch_dirs(reload_dirs)

    # Build filter with user-specified include/exclude extensions
    extra_extensions: tuple[str, ...] = ()
    if reload_includes:
        extra_extensions = tuple(
            ext.lstrip("*.") for ext in reload_includes if ext.startswith("*.")
        )
    ignore_paths: list[Path] = []
    if reload_excludes:
        ignore_paths = [Path(p) for p in reload_excludes]

    watch_filter = PythonFilter(
        extra_extensions=extra_extensions,
        ignore_paths=ignore_paths,
    )

    # Re-invoke the same script as a worker
    worker_args = [sys.executable] + sys.argv
    worker_env = {**os.environ, _RELOAD_WORKER_ENV: "1"}

    n = len(watch_dirs)
    print(f"  [reload] Watching {n} director{'ies' if n > 1 else 'y'} for changes...")
    for d in watch_dirs:
        print(f"  [reload]   {d}")
    print()

    worker: Optional[subprocess.Popen] = None
    crash_count = 0

    try:
        worker = _spawn_worker(worker_args, worker_env)

        for changes in watch(
            *watch_dirs,
            watch_filter=watch_filter,
            debounce=_DEBOUNCE_MS,
            rust_timeout=3000,
            yield_on_timeout=True,
        ):
            # yield_on_timeout yields empty set — check worker health
            if not changes:
                if worker.poll() is not None:
                    code = worker.returncode
                    crash_count += 1
                    if crash_count >= _MAX_CRASH_RETRIES:
                        print(
                            f"\n  [reload] Worker crashed {crash_count} times "
                            f"consecutively (last exit code {code}). Shutting down.\n"
                            f"  [reload] Fix the error and restart manually.\n",
                            file=sys.stderr,
                        )
                        return
                    print(
                        f"\n  [reload] Worker exited with code {code} "
                        f"({crash_count}/{_MAX_CRASH_RETRIES}), restarting...\n"
                    )
                    worker = _spawn_worker(worker_args, worker_env)
                continue

            # File change detected — reset crash counter (user made a fix)
            crash_count = 0

            changed = [str(c[1]) for c in changes]
            print(f"\n  [reload] Detected {len(changed)} change(s):")
            for f in changed[:5]:
                print(f"  [reload]   {f}")
            if len(changed) > 5:
                print(f"  [reload]   ... and {len(changed) - 5} more")
            print("  [reload] Restarting server...\n")

            # Overlapped restart: signal old worker, immediately spawn new one.
            # New worker binds with SO_REUSEADDR while old one drains.
            _terminate_worker(worker)
            worker = _spawn_worker(worker_args, worker_env)

    except KeyboardInterrupt:
        print("\n  [reload] Shutting down...")
    finally:
        if worker is not None:
            _terminate_worker_sync(worker)
        print("  [reload] Stopped.")
