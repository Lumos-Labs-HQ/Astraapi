"""Native Jinja2Templates — replaces starlette.templating."""
from __future__ import annotations

from typing import Any

from fastapi._response import HTMLResponse


class Jinja2Templates:
    """Jinja2 template rendering, returning HTMLResponse."""

    def __init__(self, directory: str | list[str], **env_options: Any) -> None:
        try:
            import jinja2
        except ImportError:
            raise RuntimeError(
                "jinja2 must be installed to use Jinja2Templates. "
                "Install it with: pip install jinja2"
            )
        if isinstance(directory, str):
            directory = [directory]
        loader = jinja2.FileSystemLoader(directory)
        self.env = jinja2.Environment(loader=loader, autoescape=True, **env_options)

    def get_template(self, name: str) -> Any:
        return self.env.get_template(name)

    def TemplateResponse(
        self,
        request: Any,
        name: str,
        context: dict[str, Any] | None = None,
        status_code: int = 200,
        headers: dict[str, str] | None = None,
        media_type: str | None = None,
        background: Any = None,
    ) -> HTMLResponse:
        ctx = dict(context) if context else {}
        ctx.setdefault("request", request)
        template = self.env.get_template(name)
        content = template.render(ctx)
        return HTMLResponse(
            content=content,
            status_code=status_code,
            headers=headers,
            media_type=media_type or "text/html",
            background=background,
        )
