#!/usr/bin/env bash
# Benchmark models with eval.py: for each GGUF, start the pinned llama-server
# (CPU unless GPU=vulkan with LLAMA=<vulkan build dir>), run the cases, stop.
#   LLAMA=/path/to/llama-b11379 core/eval/bench.sh model1.gguf [model2.gguf ...]
set -u
here=$(cd "$(dirname "$0")" && pwd)
LLAMA=${LLAMA:?set LLAMA to the unpacked llama-b11379 directory}
port=18432
for model in "$@"; do
    name=$(basename "$model" .gguf)
    log=/tmp/bench-$name.server.log
    env -u LD_LIBRARY_PATH "$LLAMA/llama-server" -m "$model" --host 127.0.0.1 --port $port -c 8192 \
        --no-webui --reasoning off ${NGL:+-ngl $NGL} >"$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 180); do curl -sf http://127.0.0.1:$port/health >/dev/null && break; sleep 1; done
    # One warm-up request so the first case is not charged for loading.
    python3 - "$port" <<'PY' >/dev/null 2>&1
import json, sys, urllib.request
b = {"messages": [{"role": "user", "content": "hi"}], "max_tokens": 4}
urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{sys.argv[1]}/v1/chat/completions", json.dumps(b).encode(), {"Content-Type": "application/json"}), timeout=300)
PY
    echo "=== $name"
    python3 "$here/eval.py" "http://127.0.0.1:$port" | tee "/tmp/bench-$name.txt" | grep -E "^FAIL|passed" 
    kill $pid; wait $pid 2>/dev/null
done
