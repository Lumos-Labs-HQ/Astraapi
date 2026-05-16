"""Regression tests for response model serialization edge cases.

Covers: AttributeError/TypeError during validate_python with from_attributes=True
when re-validating model instances that contain nested dicts (e.g. model_construct
bypass) or when computed fields access missing attributes.
"""
import pytest
from pydantic import BaseModel, computed_field
from typing import Optional

from astraapi import AstraAPI
from astraapi.testclient import TestClient
from astraapi.exceptions import ResponseValidationError


class Inner(BaseModel):
    name: str


class Outer(BaseModel):
    inner: Optional[Inner] = None

    @computed_field
    @property
    def display_name(self) -> str:
        # This will fail if inner is a dict instead of Inner
        return self.inner.name


class PlainOuter(BaseModel):
    inner: Optional[Inner] = None


def test_valid_model_instance_serializes_normally():
    """Normal model instances should still serialize correctly."""
    app = AstraAPI()

    @app.get("/items", response_model=list[PlainOuter])
    def get_items():
        return [PlainOuter(inner=Inner(name="test"))]

    client = TestClient(app)
    response = client.get("/items")
    assert response.status_code == 200
    assert response.json() == [{"inner": {"name": "test"}}]


def test_computed_field_attribute_error_serialization():
    """A computed field that accesses an attribute on a dict should raise
    ResponseValidationError, not raw AttributeError."""
    app = AstraAPI()

    @app.get("/items", response_model=list[Outer])
    def get_items():
        return [Outer.model_construct(inner={"name": "test"})]

    client = TestClient(app)
    with pytest.raises(ResponseValidationError):
        client.get("/items")


def test_computed_field_with_custom_exception_handler():
    """Even with a catch-all Exception handler, ResponseValidationError
    should be raised (TestClient re-raises it as a programming error)."""
    app = AstraAPI()

    @app.exception_handler(Exception)
    async def generic_handler(request, exc):
        return {"error": str(exc)}

    @app.get("/items", response_model=list[Outer])
    def get_items():
        return [Outer.model_construct(inner={"name": "test"})]

    client = TestClient(app)
    with pytest.raises(ResponseValidationError):
        client.get("/items")
