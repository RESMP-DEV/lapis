"""Record bounded, content-free evidence from the installed Codex app-server."""

import argparse
import asyncio
import hashlib
import json
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path

ATTENTION_METHODS = {
    "item/commandExecution/requestApproval",
    "item/fileChange/requestApproval",
    "item/permissions/requestApproval",
    "item/tool/requestUserInput",
    "mcpServer/elicitation/request",
    "thread/status/changed",
    "turn/started",
    "turn/completed",
    "serverRequest/resolved",
    "error",
}


def schema_methods(value):
    """Read literal method discriminators, without assuming schema filenames."""
    found = set()
    if isinstance(value, dict):
        method = value.get("properties", {}).get("method", {})
        if isinstance(method, dict):
            if isinstance(method.get("const"), str):
                found.add(method["const"])
            found.update(
                item for item in method.get("enum", []) if isinstance(item, str)
            )
        for child in value.values():
            found.update(schema_methods(child))
    elif isinstance(value, list):
        for child in value:
            found.update(schema_methods(child))
    return found


async def probe_transport(binary):
    process = await asyncio.create_subprocess_exec(
        str(binary),
        "app-server",
        "--listen",
        "stdio://",
        "-c",
        "analytics.enabled=false",
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
        limit=2 * 1024 * 1024,
    )
    notifications = set()

    async def send(message):
        process.stdin.write((json.dumps(message) + "\n").encode())
        await process.stdin.drain()

    async def response(request_id):
        while True:
            line = await process.stdout.readline()
            if not line:
                raise RuntimeError(
                    "app-server closed stdout before the expected response"
                )
            message = json.loads(line)
            if "method" in message:
                if "id" in message:
                    raise RuntimeError(
                        "unexpected server request during read-only probe"
                    )
                notifications.add(message["method"])
                continue
            if message.get("id") == request_id:
                if "error" in message:
                    code = message["error"].get("code")
                    raise RuntimeError(f"app-server returned RPC error code {code}")
                return message["result"]

    try:
        async with asyncio.timeout(30):
            await send(
                {
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "clientInfo": {
                            "name": "lapis_probe",
                            "title": "lapis protocol probe",
                            "version": "0.1.0",
                        },
                        "capabilities": {"experimentalApi": True},
                    },
                }
            )
            initialized = await response(1)
            await send({"method": "initialized"})
            await send(
                {"id": 2, "method": "thread/loaded/list", "params": {"limit": 1}}
            )
            loaded = await response(2)
            if not isinstance(initialized, dict) or not isinstance(loaded, dict):
                raise TypeError(
                    "unexpected initialization or loaded-thread response shape"
                )
            if not isinstance(loaded.get("data"), list):
                raise TypeError("unexpected loaded-thread data shape")
            return {
                "transport": "newline-delimited JSON over private stdio",
                "experimental_api": True,
                "initialize": "passed",
                "initialize_result_keys": sorted(initialized),
                "platform_os": initialized.get("platformOs"),
                "thread_loaded_list": "passed",
                "loaded_thread_count_in_page": len(loaded["data"]),
                "notification_methods_observed": sorted(notifications),
                "turns_started": 0,
                "attention_events_exercised": False,
            }
    finally:
        process.stdin.close()
        try:
            await asyncio.wait_for(process.wait(), timeout=3)
        except TimeoutError:
            process.terminate()
            try:
                await asyncio.wait_for(process.wait(), timeout=3)
            except TimeoutError:
                process.kill()
                await asyncio.wait_for(process.wait(), timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--codex", default="codex", help="Codex executable path or command"
    )
    parser.add_argument("--output", type=Path, help="Write a sanitized JSON receipt")
    args = parser.parse_args()
    executable = shutil.which(args.codex)
    if executable is None:
        parser.error("Codex executable not found")
    binary = Path(executable).resolve()
    with binary.open("rb") as stream:
        binary_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    version = subprocess.run(
        [str(binary), "--version"],
        check=True,
        capture_output=True,
        text=True,
        timeout=10,
    ).stdout.strip()

    with tempfile.TemporaryDirectory(prefix="lapis-codex-schema-") as directory:
        subprocess.run(
            [
                str(binary),
                "app-server",
                "generate-json-schema",
                "--experimental",
                "--out",
                directory,
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=45,
        )
        methods = set()
        files = sorted(Path(directory).rglob("*.json"))
        if not files:
            raise RuntimeError("Codex produced no JSON schema files")
        for path in files:
            methods.update(schema_methods(json.loads(path.read_text())))

    receipt = {
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "binary_path": str(binary),
        "binary_sha256": binary_hash,
        "reported_version": version,
        "schema_includes_experimental": True,
        "schema_file_count": len(files),
        "attention_methods_advertised": sorted(ATTENTION_METHODS & methods),
        "attention_methods_missing": sorted(ATTENTION_METHODS - methods),
        "live_probe": asyncio.run(probe_transport(binary)),
        "scope": "Schema export and initialization/list only; no model turn or hook exercise.",
    }
    output = json.dumps(receipt, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end="")


if __name__ == "__main__":
    main()
