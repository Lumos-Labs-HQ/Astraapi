"""Test configuration — sets up resource limits and test fixes."""
import os
import resource
import sys

# Disable pydantic plugins (e.g. logfire) before any import triggers pydantic.
# The logfire plugin tries to spawn threads during import, which fails in
# resource-constrained environments (WSL, CI with low thread limits).
os.environ.setdefault("PYDANTIC_DISABLE_PLUGINS", "1")


def _patch_starlette_routing():
    """Make starlette.routing.Route etc. point to the project's own classes.

    Tests that do `from starlette.routing import Route` get the project's Route,
    so isinstance() checks pass without any starlette dependency in production code.
    """
    import fastapi._routing_base as _rb
    import starlette.routing as _sr

    _sr.BaseRoute = _rb.BaseRoute
    _sr.Route = _rb.Route
    _sr.WebSocketRoute = _rb.WebSocketRoute
    _sr.Mount = _rb.Mount
    _sr.Router = _rb.Router
    _sr.Match = _rb.Match
    _sr.NoMatchFound = _rb.NoMatchFound


_patch_starlette_routing()


def pytest_configure(config):
    """Increase the open-file-descriptor limit before any test starts.

    Each TestClient spawns a real server (1 listening socket + uvloop event-loop FDs
    ≈ 3-5 FDs).  With 400+ test modules each creating their own TestClient, the
    default soft limit of 1024 is quickly exhausted, turning legitimate test
    failures into ``OSError: [Errno 24] Too many open files`` setup errors.
    """
    if sys.platform != "win32":
        try:
            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            # Raise to the hard limit (typically 1048576 on Linux), or at least 65535
            new_soft = min(max(hard, 65535), 1048576)
            if new_soft > soft:
                resource.setrlimit(resource.RLIMIT_NOFILE, (new_soft, hard))
        except (ValueError, resource.error):
            # If we can't change the limit, at least try to set 65535
            try:
                resource.setrlimit(resource.RLIMIT_NOFILE, (65535, 65535))
            except Exception:
                pass
