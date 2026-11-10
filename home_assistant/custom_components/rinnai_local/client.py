"""Bounded, read-only HTTP client for durable completed uses."""
from __future__ import annotations

import json
from urllib.parse import urlsplit, urlunsplit

import aiohttp

from .ledger import ArchiveError, validate_snapshot


def normalize_url(value: str) -> str:
    """Keep credentials, query strings and API paths out of a device address."""
    if not isinstance(value, str):
        raise ValueError("Invalid base URL")
    value = value.strip()
    parsed = urlsplit(value)
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError("Use an HTTP(S) device URL without credentials")
    if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        raise ValueError("Use the base URL without /api/v1 or query parameters")
    if parsed.port is not None and not 1 <= parsed.port <= 65535:
        raise ValueError("Invalid port")
    return urlunsplit((parsed.scheme, parsed.netloc, "", "", ""))


async def async_fetch_snapshot(session: aiohttp.ClientSession, base: str, expected_device: str | None = None) -> dict:
    async with session.get(base + "/api/v1/sessions", allow_redirects=False,
                           timeout=aiohttp.ClientTimeout(total=15)) as response:
        if response.status != 200:
            raise ArchiveError(f"Completed-use endpoint returned HTTP {response.status}; firmware 1.2.0 is required")
        body = bytearray()
        async for chunk in response.content.iter_chunked(4096):
            body.extend(chunk)
            if len(body) > 32768:
                raise ArchiveError("Completed-use response exceeds its bounded size")
    try:
        result = json.loads(body)
    except (ValueError, UnicodeDecodeError) as error:
        raise ArchiveError("Invalid completed-use JSON") from error
    validate_snapshot(result, expected_device)
    return result
