"""Register a Spark-exported branch with a local Codex installation."""

import argparse
import json
from pathlib import Path
from queue import Empty, Queue
import subprocess
from threading import Thread
from uuid import UUID


def read_replies(stream, replies):
    try:
        for line in stream:
            message = json.loads(line)
            if "id" in message and "method" not in message:
                replies.put(message)
    finally:
        replies.put(None)


def register(codex, path, thread_id):
    with subprocess.Popen(
        [codex, "app-server", "--stdio", "-c", "mcp_servers={}"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        encoding="utf-8",
    ) as server:
        replies = Queue()
        Thread(target=read_replies, args=(server.stdout, replies), daemon=True).start()

        def send(method, params, number=None):
            message = {"method": method, "params": params}
            if number is not None:
                message["id"] = number
            server.stdin.write(json.dumps(message) + "\n")
            server.stdin.flush()
            if number is None:
                return None
            response = replies.get(timeout=60)
            if response is None or response.get("id") != number:
                raise ValueError("Codex app-server exited or returned an unexpected response")
            if "error" in response:
                raise ValueError(response["error"]["message"])
            return response["result"]

        try:
            send("initialize", {
                "clientInfo": {"name": "spark-branch", "version": "1"},
                "capabilities": {"experimentalApi": True},
            }, 1)
            send("initialized", {})
            result = send("thread/resume", {
                "threadId": thread_id,
                "path": str(path),
                "excludeTurns": True,
            }, 2)
            if result["thread"]["id"] != thread_id:
                raise ValueError("Codex restored a different thread")
        finally:
            server.stdin.close()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rollout", type=Path)
    parser.add_argument("--codex", default="codex", help="local Codex CLI executable")
    args = parser.parse_args()
    try:
        path = args.rollout.resolve(strict=True)
        with path.open(encoding="utf-8") as file:
            metadata = json.loads(file.readline())
        if metadata.get("type") != "session_meta" or metadata["payload"].get("originator") != "spark_agent":
            raise ValueError("expected a Spark-exported Codex rollout")
        thread_id = str(UUID(metadata["payload"]["id"]))
        if not path.name.endswith(f"-{thread_id}.jsonl"):
            raise ValueError("filename and thread ID disagree; re-export the branch in Spark")
        register(args.codex, path, thread_id)
    except Empty:
        parser.exit(1, "Codex app-server did not respond within 60 seconds.\n")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Could not register Codex branch: {error}\n")
    print(f"Registered Codex thread: {thread_id}")


if __name__ == "__main__":
    main()
