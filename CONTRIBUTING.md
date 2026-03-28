# Contributing to AstraAPI

Thanks for helping improve AstraAPI.

This project combines Python framework ergonomics with a C++ runtime core, so good contributions usually include both correctness and performance awareness.

## Ways to Contribute

- Report bugs and edge cases with a minimal reproduction.
- Improve docs, examples, and developer guides.
- Add tests for regressions and behavior coverage.
- Improve runtime performance in Python or C++ hot paths.
- Propose API improvements that keep compatibility and clarity.

## Development Setup

1. Fork and clone the repository.
2. Create a feature branch from `main`.
3. Install in editable mode:

```bash
pip install -e .
pip install -e .[standard]
```

4. If your change touches native runtime internals, configure and build the C++ core under `cpp_core/`.

## Architecture Areas

When opening PRs, mention which layer you changed:

- Python app/runtime layer:
	- `astraapi/applications.py`
	- `astraapi/routing.py`
	- `astraapi/dependencies/`
- Native bridge/runtime layer:
	- `astraapi/_cpp_server.py`
	- `astraapi/_core_bridge.py`
	- `astraapi/_multiworker.py`
- C++ core:
	- `cpp_core/src/`
	- `cpp_core/include/`

## Request Lifecycle Awareness

For request-path changes, validate impact across:

1. Parse and route matching behavior.
2. Middleware ordering and exception handling.
3. Dependency injection and validation behavior.
4. Response serialization and transport writes.
5. Keep-alive and protocol state transitions.

For WebSocket changes, include handshake, frame handling, and close path testing.

## Multi-Worker Awareness

For worker/supervisor changes, test both:

- Linux/Unix dispatch patterns (`SCM_RIGHTS`/fork or reuse-port mode).
- Windows `socket.share()` and subprocess worker behavior.

Document any platform-specific caveats in the PR description.

## Testing

Run relevant tests before submitting:

```bash
pytest
```

Targeted test runs for touched modules are encouraged when the full suite is expensive.

For performance-sensitive contributions, include before/after metrics and methodology.

## Pull Request Guidelines

- Keep PR scope focused.
- Include tests for new behavior and bug fixes.
- Update docs when APIs or behavior change.
- Explain compatibility impact.
- Include benchmark evidence when claiming speedups.

## Commit and Review Expectations

- Use clear commit messages describing intent.
- Respond to review comments with code changes or rationale.
- Keep history understandable; avoid unrelated refactors in the same PR.

## Release Lifecycle

Publishing is tag-driven through GitHub Actions (`.github/workflows/publish.yml`):

1. Tag push matching `v*`.
2. Wheel builds across Linux, macOS, and Windows.
3. Source distribution build.
4. PyPI publish.
5. GitHub release notes attachment.

If your contribution affects packaging or native builds, ensure it is compatible with the release matrix.

## Code of Conduct

Please be respectful, constructive, and collaborative in all interactions.

## License

By contributing, you agree that your contributions are licensed under the MIT License in `LICENSE`.

## Attribution

This project is inspired by FastAPI and maintained by Lumos-Labs-HQ with community contributors.
