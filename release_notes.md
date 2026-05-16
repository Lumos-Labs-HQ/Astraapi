# AstraAPI 0.2.11 Release Notes

**PyPI:** https://pypi.org/project/astraapi/0.2.11  
**Python:** 3.14+ | **C++:** 20

---

## Overview

Patch release that fixes a runtime compatibility issue where endpoints returning `list[PydanticModel]` could crash with `'dict' object has no attribute 'name'` during response validation/serialization.

---

## Bug Fixes

### Response Model Validation/Serialization Robustness

- **`astraapi/_compat/v2.py`** — `ModelField.validate()` now catches `TypeError` and `AttributeError` in addition to `ValidationError`. Pydantic v2's `validate_python(..., from_attributes=True)` can raise these exceptions when re-validating model instances that contain nested dicts (e.g., `model_construct` bypass) or when computed fields access missing attributes.
- **`astraapi/applications.py`** — Both `_make_response_model_shim` and `_make_response_class_shim` now wrap `response_field.serialize()` in try-except to catch `TypeError` and `AttributeError` and convert them to `ResponseValidationError` instead of letting raw exceptions propagate and crash the request.

---

## Installation

```bash
pip install astraapi
```

Requires **Python 3.14+** and a C++20-capable compiler (GCC 10+, Clang 12+, MSVC 2019+). Pre-built wheels are provided for Linux x86_64/aarch64, macOS arm64, and Windows x64.
