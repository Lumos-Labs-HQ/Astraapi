from collections.abc import AsyncGenerator
from contextlib import AbstractContextManager
from contextlib import asynccontextmanager as asynccontextmanager
from typing import TypeVar

from fastapi._concurrency import iterate_in_threadpool as iterate_in_threadpool  # noqa
from fastapi._concurrency import run_in_threadpool as run_in_threadpool  # noqa
from fastapi._concurrency import (  # noqa
    run_until_first_complete as run_until_first_complete,
)

_T = TypeVar("_T")


@asynccontextmanager
async def contextmanager_in_threadpool(
    cm: AbstractContextManager[_T],
) -> AsyncGenerator[_T, None]:
    # blocking __exit__ from running waiting on a free thread
    # can create race conditions/deadlocks if the context manager itself
    # has its own internal pool (e.g. a database connection pool)
    # to avoid this we let __exit__ run without a capacity limit
    try:
        yield await run_in_threadpool(cm.__enter__)
    except Exception as e:
        ok = bool(
            await run_in_threadpool(cm.__exit__, type(e), e, e.__traceback__)
        )
        if not ok:
            raise e
    else:
        await run_in_threadpool(cm.__exit__, None, None, None)
