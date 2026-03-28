"""AstraAPI framework, high performance, easy to learn, fast to code, ready for production"""

__version__ = "0.128.3"

# ── Lazy imports via __getattr__ ────────────────────────────────────────────
# Deferring heavy imports (pydantic, openapi models, dependency resolution)
# until the symbol is actually accessed.  This reduces
# `from astraapi import AstraAPI` from ~4.7s to ~1-1.5s on Windows.

_LAZY_IMPORTS: dict[str, tuple[str, str]] = {
    # (module_path, attribute_name)
    "status": ("astraapi._status", None),  # module re-export
    "AstraAPI": ("astraapi.applications", "AstraAPI"),
    "BackgroundTasks": ("astraapi.background", "BackgroundTasks"),
    "UploadFile": ("astraapi.datastructures", "UploadFile"),
    "HTTPException": ("astraapi.exceptions", "HTTPException"),
    "WebSocketException": ("astraapi.exceptions", "WebSocketException"),
    "Body": ("astraapi.param_functions", "Body"),
    "Cookie": ("astraapi.param_functions", "Cookie"),
    "Depends": ("astraapi.param_functions", "Depends"),
    "File": ("astraapi.param_functions", "File"),
    "Form": ("astraapi.param_functions", "Form"),
    "Header": ("astraapi.param_functions", "Header"),
    "Path": ("astraapi.param_functions", "Path"),
    "Query": ("astraapi.param_functions", "Query"),
    "Security": ("astraapi.param_functions", "Security"),
    "Request": ("astraapi.requests", "Request"),
    "Response": ("astraapi.responses", "Response"),
    "APIRouter": ("astraapi.routing", "APIRouter"),
    "WebSocket": ("astraapi.websockets", "WebSocket"),
    "WebSocketDisconnect": ("astraapi.websockets", "WebSocketDisconnect"),
}


def __getattr__(name: str):
    if name in _LAZY_IMPORTS:
        module_path, attr = _LAZY_IMPORTS[name]
        import importlib
        mod = importlib.import_module(module_path)
        val = mod if attr is None else getattr(mod, attr)
        globals()[name] = val  # cache for subsequent access
        return val
    raise AttributeError(f"module 'astraapi' has no attribute {name!r}")


__all__ = list(_LAZY_IMPORTS.keys()) + ["__version__"]
