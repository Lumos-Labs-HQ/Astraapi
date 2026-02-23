from __future__ import annotations

import types
from enum import Enum
from typing import TYPE_CHECKING, Any, Callable, Optional, TypeVar, Union

if TYPE_CHECKING:
    from pydantic import BaseModel
    from pydantic.main import IncEx as IncEx

    ModelNameMap = dict[Union[type[BaseModel], type[Enum]], str]
else:
    IncEx = Any
    ModelNameMap = dict[Any, str]

DecoratedCallable = TypeVar("DecoratedCallable", bound=Callable[..., Any])
UnionType = getattr(types, "UnionType", Union)
DependencyCacheKey = tuple[Optional[Callable[..., Any]], tuple[str, ...], str]
