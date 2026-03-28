"""
Background task utilities -- replaces starlette.background.

Provides BackgroundTask and BackgroundTasks for running work after a
response has been sent, with zero starlette imports.
"""
from __future__ import annotations

from typing import Any, Callable, Optional, Sequence

from astraapi._concurrency import is_async_callable, run_in_threadpool


class BackgroundTask:
    """A single background task to run after the response is sent.

    Parameters
    ----------
    func : callable
        The function to execute (sync or async).
    *args, **kwargs :
        Positional and keyword arguments forwarded to *func*.
    """

    def __init__(self, func: Callable[..., Any], *args: Any, **kwargs: Any) -> None:
        self.func = func
        self.args = args
        self.kwargs = kwargs

    async def __call__(self) -> None:
        if is_async_callable(self.func):
            await self.func(*self.args, **self.kwargs)
        else:
            await run_in_threadpool(self.func, *self.args, **self.kwargs)


class BackgroundTasks(BackgroundTask):
    """A collection of background tasks to run sequentially after the
    response is sent.

    Parameters
    ----------
    tasks : sequence of BackgroundTask, optional
        Initial list of tasks.
    """

    def __init__(self, tasks: Optional[Sequence[BackgroundTask]] = None) -> None:
        self.tasks: list[BackgroundTask] = list(tasks) if tasks else []

    def add_task(self, func: Callable[..., Any], *args: Any, **kwargs: Any) -> None:
        """Append a new background task."""
        self.tasks.append(BackgroundTask(func, *args, **kwargs))

    async def __call__(self) -> None:
        for task in self.tasks:
            await task()
