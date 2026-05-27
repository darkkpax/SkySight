from __future__ import annotations

import hmac
import os
from typing import Optional

from fastapi import HTTPException, Request, WebSocket, status

API_TOKEN_ENV = "FIRE_UAV_API_TOKEN"


def _expected_token() -> Optional[str]:
    """Return the configured API token or None when auth is disabled."""
    token = os.getenv(API_TOKEN_ENV)
    return token.strip() if token else None


def _normalize(provided: Optional[str]) -> Optional[str]:
    if not provided:
        return None
    if provided.lower().startswith("bearer "):
        return provided[7:].strip()
    return provided.strip()


async def require_api_key(request: Request) -> None:
    """
    Optional API-key guard for FastAPI routes.

    - If FIRE_UAV_API_TOKEN is not set, auth is disabled.
    - Otherwise, expect the token in header X-API-Key or Authorization: Bearer <token>.
    """
    expected = _expected_token()
    if not expected:
        return
    provided = _normalize(request.headers.get("x-api-key") or request.headers.get("authorization"))
    if provided is None or not hmac.compare_digest(provided, expected):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid or missing API token",
        )


async def require_ws_api_key(ws: WebSocket) -> bool:
    """
    WebSocket variant of the API-key guard.

    Returns True when accepted, False when rejected (and closes the socket).
    """
    expected = _expected_token()
    if not expected:
        return True
    provided = _normalize(
        ws.headers.get("x-api-key")
        or ws.headers.get("authorization")
        or ws.query_params.get("token")
    )
    if provided is None or not hmac.compare_digest(provided, expected):
        await ws.close(code=status.WS_1008_POLICY_VIOLATION, reason="Invalid API token")
        return False
    return True


__all__ = ["require_api_key", "require_ws_api_key", "API_TOKEN_ENV"]
