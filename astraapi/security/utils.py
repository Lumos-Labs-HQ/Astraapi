from typing import Optional


def get_authorization_scheme_param(
    authorization_header_value: Optional[str],
) -> tuple[str, str]:
    if not authorization_header_value:
        return "", ""
    # Normalize whitespace: strip leading/trailing, collapse multiple spaces
    normalized = " ".join(authorization_header_value.split())
    scheme, _, param = normalized.partition(" ")
    return scheme, param
