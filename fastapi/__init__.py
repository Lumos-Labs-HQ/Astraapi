"""FastAPI framework, high performance, easy to learn, fast to code, ready for production"""

__version__ = "0.128.3"

# ── Lazy imports via __getattr__ ────────────────────────────────────────────
# Deferring heavy imports (pydantic, openapi models, dependency resolution)
# until the symbol is actually accessed.  This reduces
# `from fastapi import FastAPI` from ~4.7s to ~1-1.5s on Windows.

_LAZY_IMPORTS: dict[str, tuple[str, str]] = {
    # (module_path, attribute_name)
    "status": ("fastapi._status", None),  # module re-export
    "FastAPI": ("fastapi.applications", "FastAPI"),
    "BackgroundTasks": ("fastapi.background", "BackgroundTasks"),
    "UploadFile": ("fastapi.datastructures", "UploadFile"),
    "HTTPException": ("fastapi.exceptions", "HTTPException"),
    "WebSocketException": ("fastapi.exceptions", "WebSocketException"),
    "Body": ("fastapi.param_functions", "Body"),
    "Cookie": ("fastapi.param_functions", "Cookie"),
    "Depends": ("fastapi.param_functions", "Depends"),
    "File": ("fastapi.param_functions", "File"),
    "Form": ("fastapi.param_functions", "Form"),
    "Header": ("fastapi.param_functions", "Header"),
    "Path": ("fastapi.param_functions", "Path"),
    "Query": ("fastapi.param_functions", "Query"),
    "Security": ("fastapi.param_functions", "Security"),
    "Request": ("fastapi.requests", "Request"),
    "Response": ("fastapi.responses", "Response"),
    "APIRouter": ("fastapi.routing", "APIRouter"),
    "WebSocket": ("fastapi.websockets", "WebSocket"),
    "WebSocketDisconnect": ("fastapi.websockets", "WebSocketDisconnect"),
}


def __getattr__(name: str):
    if name in _LAZY_IMPORTS:
        module_path, attr = _LAZY_IMPORTS[name]
        import importlib
        mod = importlib.import_module(module_path)
        val = mod if attr is None else getattr(mod, attr)
        globals()[name] = val  # cache for subsequent access
        return val
    raise AttributeError(f"module 'fastapi' has no attribute {name!r}")


__all__ = list(_LAZY_IMPORTS.keys()) + ["__version__"]
