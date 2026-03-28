"""Native StaticFiles — replaces starlette.staticfiles."""
from __future__ import annotations

import os
import mimetypes
from typing import Any, Optional

from astraapi._response import FileResponse, PlainTextResponse, Response
from astraapi._types import Receive, Scope, Send


class StaticFiles:
    """Serve static files from a directory.

    Parameters
    ----------
    directory : str | None
        Path to the directory to serve.
    packages : list | None
        List of packages to serve static files from.
    html : bool
        If True, serve index.html for directories and 404.html on not found.
    check_dir : bool
        If True, verify the directory exists on init.
    """

    def __init__(
        self,
        *,
        directory: Optional[str] = None,
        packages: Optional[list[Any]] = None,
        html: bool = False,
        check_dir: bool = True,
    ) -> None:
        self.directory = directory
        self.html = html
        self.all_directories: list[str] = []
        if directory is not None:
            if check_dir and not os.path.isdir(directory):
                raise RuntimeError(f"Directory '{directory}' does not exist")
            self.all_directories.append(directory)
        if packages:
            for pkg in packages:
                if isinstance(pkg, str):
                    pkg_name = pkg
                    pkg_dir = "statics"
                else:
                    pkg_name, pkg_dir = pkg
                try:
                    import importlib.resources as pkg_resources
                    pkg_path = str(pkg_resources.files(pkg_name) / pkg_dir)
                    self.all_directories.append(pkg_path)
                except (ImportError, TypeError):
                    pass

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        assert scope["type"] == "http"
        path = scope.get("path", "/")
        # Strip leading slash for joining
        rel_path = path.lstrip("/")

        response = self._lookup(rel_path, scope)
        await response(scope, receive, send)

    def _lookup(self, rel_path: str, scope: Scope) -> Response:
        for directory in self.all_directories:
            full_path = os.path.join(directory, rel_path)
            # Prevent directory traversal
            full_path = os.path.normpath(full_path)
            if not full_path.startswith(os.path.normpath(directory)):
                continue

            if os.path.isfile(full_path):
                return FileResponse(
                    full_path,
                    method=scope.get("method", "GET"),
                )

            if self.html and os.path.isdir(full_path):
                index = os.path.join(full_path, "index.html")
                if os.path.isfile(index):
                    return FileResponse(
                        index,
                        method=scope.get("method", "GET"),
                    )

        if self.html:
            # Try 404.html
            for directory in self.all_directories:
                not_found = os.path.join(directory, "404.html")
                if os.path.isfile(not_found):
                    return FileResponse(
                        not_found,
                        status_code=404,
                        method=scope.get("method", "GET"),
                    )

        return PlainTextResponse("Not Found", status_code=404)
