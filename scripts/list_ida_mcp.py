#!/usr/bin/env python3
"""Print the EXE/DLL/SYS loaded in each running IDA Pro MCP instance.

Python 3.10+; standard library only. Run from a terminal, not inside IDA.

    python list_ida_mcp.py
    python list_ida_mcp.py --ports 13337-13350
    python list_ida_mcp.py --config mcp.json
    python list_ida_mcp.py --server MyIDA=http://127.0.0.1:13337/mcp

Default: read IDA's local instance registry and probe 127.0.0.1:13337-13436.
Explicit --ports / --config / --server options replace default discovery.
The registry is never modified; configured stdio commands are never launched.
Printed server names (including warnings) omit the leading ida-pro-mcp_ prefix.
SYS files appear under DRIVER, below DLLS, for both x64 and x86.
The interactive console is cleared at startup; redirected output is unchanged.
Trailing .idb/.i64 suffixes are hidden before EXE/DLL/SYS classification.
If the root filename is missing/unrecognized, try the input and database paths.
Architecture comes from IDA metadata, never from the database extension.
Requires the server's py_eval tool. Only a metadata-reading expression is sent;
it does not assign variables, clear py_eval locals, or modify the IDB.
"""
from __future__ import annotations

import argparse
import http.client
import json
import math
import ntpath
import os
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit

DEFAULT_PORTS = "13337-13436"
PROTOCOL_VERSION = "2025-06-18"
MAX_RESPONSE_BYTES = 2 * 1024 * 1024
FILE_KINDS = {".exe": "EXES", ".dll": "DLLS", ".sys": "DRIVER"}
DATABASE_SUFFIXES = frozenset((".idb", ".i64"))

# A single expression: do not overwrite variables from anyone else's py_eval.
# These imports are evaluated inside IDA, not by this standalone script.
METADATA_QUERY = """__import__('json').dumps({
    'module': __import__('ida_nalt').get_root_filename(),
    'input_path': __import__('ida_nalt').get_input_file_path(),
    'database_path': __import__('ida_loader').get_path(__import__('ida_loader').PATH_TYPE_IDB),
    'processor': __import__('ida_ida').inf_get_procname(),
    'bits': (64 if __import__('ida_ida').inf_is_64bit() else
             32 if __import__('ida_ida').inf_is_32bit_exactly() else 16)
})"""


@dataclass(frozen=True)
class Endpoint:
    name: str
    url: str
    explicit: bool = False


@dataclass(frozen=True)
class Record:
    server: str
    filename: str
    architecture: str
    kind: str


class MCPError(Exception):
    pass


def clean_line(value: str) -> str:
    return "".join(c if c.isprintable() else "?" for c in value)


def display_server(name: str) -> str:
    """Hide the standard prefix without changing endpoint identity."""
    return name.removeprefix("ida-pro-mcp_")


def strip_database_suffixes(value: object) -> str:
    """Return the basename with only trailing .idb/.i64 suffixes removed."""
    if not isinstance(value, str):
        return ""
    filename = ntpath.basename(value.strip())
    while filename:
        stem, suffix = ntpath.splitext(filename)
        if suffix.casefold() not in DATABASE_SUFFIXES:
            break
        filename = stem
    return filename


def filename_from_metadata(data: dict) -> str:
    """Prefer a recognized binary name; fall back to the live database path."""
    first_nonempty = ""
    # A reopened database may have no root filename. Try every source before
    # rejecting it; an unrecognized but nonempty root must not block a fallback.
    # Paths are metadata only: the original binary need not exist on disk.
    for key in ("module", "input_path", "database_path"):
        filename = strip_database_suffixes(data.get(key))
        if not first_nonempty:
            first_nonempty = filename
        if ntpath.splitext(filename)[1].casefold() in FILE_KINDS:
            return filename
    return first_nonempty


def endpoint(value: str, name: str = "", explicit: bool = False) -> Endpoint:
    if "://" not in value:
        value = "http://" + value
    p = urlsplit(value)
    if p.scheme not in ("http", "https") or not p.hostname:
        raise ValueError(f"Invalid MCP URL: {value!r}")
    if p.username is not None or p.password is not None or p.fragment:
        raise ValueError("MCP URLs must not contain credentials or fragments")
    port = p.port if p.port is not None else (443 if p.scheme == "https" else 80)
    if not 1 <= port <= 65535:
        raise ValueError("Port must be between 1 and 65535")
    host = p.hostname.lower()
    host = {"localhost": "127.0.0.1", "0.0.0.0": "127.0.0.1", "::": "::1"}.get(host, host)
    authority = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
    path = p.path if p.path not in ("", "/") else "/mcp"
    url = urlunsplit((p.scheme, authority, path, p.query, ""))
    if not name:
        name = f"ida-pro-mcp_{port}" if host in ("127.0.0.1", "::1") else f"ida-pro-mcp_{authority}"
    return Endpoint(clean_line(name), url, explicit)


def port_numbers(text: str) -> list[int]:
    ports: set[int] = set()
    for part in text.split(","):
        limits = part.strip().split("-")
        if len(limits) not in (1, 2):
            raise ValueError(f"Invalid port range: {part!r}")
        first = int(limits[0])
        last = int(limits[-1])
        if not 1 <= first <= last <= 65535:
            raise ValueError(f"Invalid port range: {part!r}")
        ports.update(range(first, last + 1))
    return sorted(ports)


def registered_endpoints() -> list[Endpoint]:
    # Same default location as discovery.py in the supplied ida-pro-mcp source.
    if os.name == "nt":
        appdata = os.environ.get("APPDATA")
        if not appdata:
            return []
        root = Path(appdata) / "Hex-Rays" / "IDA Pro"
    else:
        root = Path.home() / ".idapro"
    items: list[Endpoint] = []
    for path in (root / "mcp" / "instances").glob("instance_*.json"):
        try:
            data = json.loads(path.read_text(encoding="utf-8-sig"))
            host, port = str(data["host"]), int(data["port"])
            authority = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
            items.append(endpoint(authority))
        except (OSError, ValueError, KeyError, TypeError):
            continue
    # Do not trust registry filenames/bitness: query each live endpoint instead.
    return items


def configured_endpoints(path: Path) -> list[Endpoint]:
    config = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(config, dict):
        raise ValueError("The MCP configuration must be a JSON object")
    mcp = config.get("mcp", {})
    servers = config.get("mcpServers")
    if servers is None and isinstance(mcp, dict):
        servers = mcp.get("servers")
    if servers is None:
        servers = config.get("servers")
    if not isinstance(servers, dict):
        raise ValueError("Expected mcpServers, mcp.servers, or servers in the JSON config")
    items: list[Endpoint] = []
    for name, entry in servers.items():
        if not isinstance(entry, dict) or entry.get("enabled") is False or entry.get("disabled") is True:
            continue
        url = entry.get("url")
        args = entry.get("args", [])
        if isinstance(args, list):
            for index, arg in enumerate(args):
                if arg == "--ida-rpc" and index + 1 < len(args):
                    url = args[index + 1]
                    break
                if isinstance(arg, str) and arg.startswith("--ida-rpc="):
                    url = arg.split("=", 1)[1]
                    break
        if isinstance(url, str) and url:
            items.append(endpoint(os.path.expandvars(url), str(name), explicit=True))
    if not items:
        raise ValueError("No enabled HTTP URLs or --ida-rpc endpoints found in the config")
    return items


class MCPClient:
    """Small JSON-over-HTTP client for the supplied IDA MCP implementation."""

    def __init__(self, url: str, timeout: float, connect_timeout: float):
        self.url = urlsplit(url)
        self.timeout = timeout
        self.connect_timeout = connect_timeout
        self.session: str | None = None
        self.protocol = PROTOCOL_VERSION
        self.request_id = 0

    def send(self, method: str, params: dict | None = None, *, notification: bool = False) -> dict:
        self.request_id += 1
        payload: dict = {"jsonrpc": "2.0", "method": method}
        if not notification:
            payload["id"] = self.request_id
        if params is not None:
            payload["params"] = params
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": self.protocol,
        }
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        connection_type = http.client.HTTPSConnection if self.url.scheme == "https" else http.client.HTTPConnection
        connection = connection_type(self.url.hostname, self.url.port, timeout=self.connect_timeout)
        path = self.url.path + ("?" + self.url.query if self.url.query else "")
        try:
            connection.connect()
            if connection.sock is not None:
                connection.sock.settimeout(self.timeout)
            connection.request("POST", path, json.dumps(payload).encode("utf-8"), headers)
            response = connection.getresponse()
            session = response.getheader("Mcp-Session-Id")
            if session:
                self.session = session
            # This package returns application/json, not an open-ended SSE stream.
            if "text/event-stream" in response.getheader("Content-Type", "").lower():
                raise MCPError("SSE response not supported; use this package's direct /mcp endpoint")
            body = response.read(MAX_RESPONSE_BYTES + 1)
            if len(body) > MAX_RESPONSE_BYTES:
                raise MCPError("Response exceeds the 2 MiB safety limit")
            if not 200 <= response.status < 300:
                raise MCPError(f"HTTP {response.status}: {body.decode('utf-8', errors='replace')[:250]}")
            if notification:
                return {}
            try:
                reply = json.loads(body)
            except (ValueError, UnicodeError) as exc:
                raise MCPError("The endpoint did not return a JSON-RPC response") from exc
            if not isinstance(reply, dict) or reply.get("id") != self.request_id:
                raise MCPError("Invalid or mismatched JSON-RPC response")
            if "error" in reply:
                error = reply["error"]
                raise MCPError(str(error.get("message", error) if isinstance(error, dict) else error))
            result = reply.get("result")
            if not isinstance(result, dict):
                raise MCPError("Missing JSON-RPC result object")
            return result
        finally:
            connection.close()

    def initialize(self) -> bool:
        result = self.send("initialize", {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "ida-mcp-inventory", "version": "1.0.0"},
        })
        self.protocol = result.get("protocolVersion", PROTOCOL_VERSION)
        self.send("notifications/initialized", notification=True)
        info = result.get("serverInfo", {})
        # Do not send Python to an unrelated service found in the port range.
        return isinstance(info, dict) and "ida" in str(info.get("name", "")).lower()

    def metadata(self) -> dict:
        result = self.send("tools/call", {"name": "py_eval", "arguments": {"code": METADATA_QUERY}})
        if result.get("isError"):
            text = "; ".join(str(c.get("text", "")) for c in result.get("content", []) if isinstance(c, dict))
            raise MCPError(f"py_eval unavailable/failed: {text or 'unknown error'}")
        data = result.get("structuredContent")
        if not isinstance(data, dict):
            for block in result.get("content", []):
                if not isinstance(block, dict) or block.get("type") != "text":
                    continue
                try:
                    candidate = json.loads(block.get("text", ""))
                    if isinstance(candidate, dict) and "result" in candidate:
                        data = candidate
                        break
                except (ValueError, TypeError):
                    continue
        if not isinstance(data, dict):
            raise MCPError("py_eval returned no usable result")
        if data.get("stderr"):
            raise MCPError(str(data["stderr"]).strip())
        try:
            metadata = json.loads(data["result"])
        except (KeyError, TypeError, ValueError) as exc:
            raise MCPError("Could not decode py_eval metadata") from exc
        if not isinstance(metadata, dict):
            raise MCPError("Invalid metadata object")
        return metadata


def query_server(item: Endpoint, timeout: float, connect_timeout: float) -> tuple[Record | None, str | None, bool]:
    connected = False
    name = display_server(item.name)
    try:
        client = MCPClient(item.url, timeout, connect_timeout)
        if not client.initialize():
            return None, f"{name}: not an IDA MCP endpoint", item.explicit
        connected = True
        data = client.metadata()
        filename = filename_from_metadata(data)
        processor = str(data.get("processor") or "").casefold()
        bits = data.get("bits")
        suffix = ntpath.splitext(filename)[1].casefold()
        if processor != "metapc" or bits not in (32, 64):
            return None, f"{name}: skipped {filename!r} ({processor}, {bits}-bit; not x86/x64)", True
        kind = FILE_KINDS.get(suffix)
        if kind is None:
            if not filename:
                return None, (f"{name}: skipped (no filename available from IDA's root filename, "
                              "input path, or database path)"), True
            return None, (f"{name}: skipped {clean_line(filename)!r} "
                          "(not an EXE/DLL/SYS filename after removing .idb/.i64 suffixes)"), True
        return Record(item.name, clean_line(filename), "x64" if bits == 64 else "x86", kind), None, False
    except (OSError, http.client.HTTPException, MCPError, ValueError, TypeError) as exc:
        message = clean_line(str(exc))
        return None, f"{name} ({item.url}): {message}", connected or item.explicit


def render(records: list[Record]) -> str:
    sections: list[str] = []
    for architecture in ("x64", "x86"):
        lines = [f"[{architecture}]"]
        for kind in ("EXES", "DLLS", "DRIVER"):
            group = sorted((r for r in records if r.architecture == architecture and r.kind == kind),
                           key=lambda r: (r.filename.casefold(), r.server.casefold()))
            # Match the requested example, but never hide x86 executables.
            if architecture == "x86" and kind == "EXES" and not group:
                continue
            if len(lines) > 1:
                lines.append("")
            lines.append(kind)
            lines.extend(f"{display_server(r.server)} // {r.filename}" for r in group)
        sections.append("\n".join(lines))
    return "\n\n".join(sections)


def clear_console() -> None:
    """Clear the interactive console without adding control codes to redirected output."""
    try:
        if sys.stdout is None or not sys.stdout.isatty():
            return
        sys.stdout.flush()
        if os.name == "nt":
            os.system("cls")
        else:
            sys.stdout.write("\033[3J\033[2J\033[H")
            sys.stdout.flush()
    except (OSError, ValueError):
        # A terminal-clearing failure should not prevent server discovery.
        pass


def main(argv: list[str] | None = None) -> int:
    clear_console()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ports", help="Ports/ranges, e.g. 13337-13350 or 13337,13340")
    parser.add_argument("--host", default="127.0.0.1", help="Host for --ports (default: 127.0.0.1)")
    parser.add_argument("--config", type=Path, help="MCP JSON config; use its server names and endpoints")
    parser.add_argument("--server", action="append", default=[], metavar="[NAME=]URL", help="Explicit HTTP endpoint; repeatable")
    parser.add_argument("--timeout", type=float, default=5.0, help="Response timeout in seconds (default: 5)")
    parser.add_argument("--connect-timeout", type=float, default=0.75, help="Connection timeout in seconds (default: 0.75)")
    parser.add_argument("--workers", type=int, default=24, help="Parallel server queries (default: 24)")
    parser.add_argument("--verbose", action="store_true", help="Also report closed ports and unrecognized endpoints on stderr")
    args = parser.parse_args(argv)
    if any(not math.isfinite(t) or t <= 0 for t in (args.timeout, args.connect_timeout)):
        parser.error("Timeouts must be positive finite numbers")
    if not 1 <= args.workers <= 128:
        parser.error("--workers must be between 1 and 128")
    items: list[Endpoint] = []
    try:
        explicit = bool(args.config or args.server or args.ports is not None)
        if not explicit:
            if args.host == "127.0.0.1":
                items.extend(registered_endpoints())
            args.ports = DEFAULT_PORTS
        if args.ports is not None:
            host = args.host.strip("[]")
            for port in port_numbers(args.ports):
                authority = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
                items.append(endpoint(authority))
        if args.config:
            items.extend(configured_endpoints(args.config))
        for value in args.server:
            if not value.startswith(("http://", "https://")) and "=" in value:
                name, value = value.split("=", 1)
            else:
                name = ""
            items.append(endpoint(value, name, explicit=True))
    except (OSError, ValueError, TypeError) as exc:
        parser.error(str(exc))
    # One query/row per physical endpoint; prefer an explicitly supplied name.
    unique: dict[str, Endpoint] = {}
    for item in items:
        previous = unique.get(item.url)
        if previous is None or (item.explicit and not previous.explicit):
            unique[item.url] = item
    records: list[Record] = []
    issues: list[str] = []
    with ThreadPoolExecutor(max_workers=min(args.workers, max(1, len(unique)))) as pool:
        futures = [pool.submit(query_server, e, args.timeout, args.connect_timeout) for e in unique.values()]
        for future in as_completed(futures):
            record, error, visible = future.result()
            if record is not None:
                records.append(record)
            elif error and (visible or args.verbose):
                issues.append(error)
    print(render(records))
    for issue in sorted(issues, key=str.casefold):
        print(f"Warning: {issue}", file=sys.stderr)
    if not records:
        print("No x64/x86 EXE, DLL, or SYS was retrieved. Start MCP in the relevant IDA windows; use --verbose for diagnostics.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
