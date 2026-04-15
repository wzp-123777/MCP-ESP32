from __future__ import annotations

import argparse
import queue
import sys
import threading
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--agent-root", required=True)
    parser.add_argument("--prompt", required=True)
    parser.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args()

    agent_root = Path(args.agent_root).resolve()
    if str(agent_root) not in sys.path:
        sys.path.insert(0, str(agent_root))

    from agentmain import GeneraticAgent  # type: ignore

    agent = GeneraticAgent()
    agent.verbose = False
    threading.Thread(target=agent.run, daemon=True).start()
    display_queue: queue.Queue = agent.put_task(args.prompt, "napcat-root")

    while True:
        item = display_queue.get(timeout=args.timeout)
        if "done" in item:
            print(str(item["done"]))
            return 0


if __name__ == "__main__":
    raise SystemExit(main())
