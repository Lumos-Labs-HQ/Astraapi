#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/cpp_core/build"
PKG_DIR="${ROOT_DIR}/astraapi"

cmake -S "${ROOT_DIR}/cpp_core" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j"$(nproc)"

export ASTRAAPI_ROOT_DIR="${ROOT_DIR}"
python - <<'PY'
import os
import shutil
import sysconfig
from pathlib import Path

root = Path(os.environ["ASTRAAPI_ROOT_DIR"])
build_dir = root / "cpp_core" / "build"
pkg_dir = root / "astraapi"

ext_suffix = sysconfig.get_config_var("EXT_SUFFIX") or ".so"
candidates = sorted(build_dir.glob(f"_astraapi_core*{ext_suffix}"))
if not candidates:
    candidates = sorted(build_dir.glob("_astraapi_core*.so"))
if not candidates:
    raise SystemExit("Could not find built extension in cpp_core/build")

src = candidates[0]

# Copy to project root (editable install source)
dst = pkg_dir / src.name
shutil.copy2(src, dst)
print(f"Copied {src} -> {dst}")

# Also copy to .venv if editable install redirects there
venv_pkg = root / ".venv" / "lib" / f"python{sysconfig.get_python_version()}" / "site-packages" / "astraapi" / src.name
if venv_pkg.parent.exists():
    shutil.copy2(src, venv_pkg)
    print(f"Copied {src} -> {venv_pkg}")
PY

echo "Build complete. You can verify with: python -c 'import astraapi._astraapi_core; print(astraapi._astraapi_core.__file__)'"
