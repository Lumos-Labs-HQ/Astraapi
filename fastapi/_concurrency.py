"""Concurrency utilities - replaces starlette.concurrency."""
import asyncio
import concurrent.futures
import functools
import inspect
from typing import Any, AsyncGenerator, Callable, TypeVar

T = TypeVar("T")

# Shared thread pool - avoids per-call executor creation overhead.
_threadpool = concurrent.futures.ThreadPoolExecutor()


async def run_in_threadpool(func: Callable[..., T], *args: Any, **kwargs: Any) -> T:
    """Run a sync function in the default thread pool.

    Submits directly to a ThreadPoolExecutor and wraps the future, bypassing
    loop.run_in_executor() which (via uvloop) calls the deprecated
    asyncio.iscoroutinefunction() on every call in Python 3.14+.
    """
    loop = asyncio.get_running_loop()
    if kwargs:
        func = functools.partial(func, **kwargs)
    return await asyncio.wrap_future(
        _threadpool.submit(func, *args), loop=loop
    )


async def iterate_in_threadpool(iterator: Any) -> AsyncGenerator:
    """Iterate a sync iterator in the threadpool."""

    class _StopIteration(Exception):
        pass

    def _next(it: Any) -> Any:
        try:
            return next(it)
        except StopIteration:
            raise _StopIteration

    while True:
        try:
            yield await run_in_threadpool(_next, iterator)
        except _StopIteration:
            break


def is_async_callable(obj: Any) -> bool:
    """Check if an object is an async callable."""
    while isinstance(obj, functools.partial):
        obj = obj.func
    return inspect.iscoroutinefunction(obj) or (
        callable(obj)
        and inspect.iscoroutinefunction(getattr(obj, "__call__", None))
    )


async def run_until_first_complete(*args: tuple[Callable, dict]) -> None:
    """Run multiple async functions, returning when the first one completes."""
    tasks = []
    for func, kwargs in args:
        tasks.append(asyncio.ensure_future(func(**kwargs)))
    if not tasks:
        return
    done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    for task in pending:
        task.cancel()
    for task in done:
        task.result()
