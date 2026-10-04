# Which local model — measurements, 2026-10-03

## What was measured

- **The prompt and schema the core ships:** `core/src/system_prompt.inc` and
  `plan_schema.inc`.
- **The context the core builds:** `core/eval/eval.py` mirrors
  `Tools::context`, using the real official catalog.
- **28 cases** (`core/eval/cases.json`):
  - English, Czech, German and Spanish;
  - ambiguous names ("open the wallet": ask which);
  - requests the tools cannot serve ("stop the node", "order a pizza");
  - calls with arguments;
  - two-step plans.
- **How it was run:**
  - pinned `llama-server` b11379, **Vulkan build on the Intel Iris Xe** (the
    path users with an iGPU get), `-ngl 99`, `--reasoning off`;
  - temperature 0, one warm-up request first.
- **Machine:** i7-13700H, 62 GB RAM.

Reproduce:

```sh
LLAMA=<llama-b11379 dir> NGL=99 core/eval/bench.sh model.gguf ...
```

## Results

| Model (Q4_K_M) | Size | Passed | Median | Max | Notes |
|---|---|---|---|---|---|
| Qwen3-4B-Instruct-2507 | 2.5 GB | **26/28** | 6.3 s | 12.2 s | non-thinking; misses "what wallets are there?" and "install monero" (should ask which) |
| Qwen3.5-2B | 1.3 GB | 25/28 | **3.2 s** | 6.4 s | non-thinking by default; misses "install monero", "otevři peněženku" (picked one wallet) and "stop the blockchain node" (should refuse) |
| Qwen3.5-4B | 2.7 GB | 25/28 | 7.4 s | 16.7 s | thinks by default (switched off) |
| Gemma 4 E4B-it | 5.0 GB | 25/28 | 14.3 s | 27.6 s | reasons even with `--reasoning off`; needs `--reasoning-budget 0` |
| Granite 4.1 3B | 2.1 GB | 22/28 | 6.0 s | 12.7 s | misses most non-English cases |
| Granite 4.2 3B | 2.2 GB | – | – | – | 9 failures before an invalid reply; not competitive |

On the **CPU** instead of the iGPU, Qwen3-4B reads prompts at about 40 tokens/s
against about 150 (median 16.4 s per plan). Hence the Vulkan build whenever the
machine has Vulkan (docs/adr/0007).

## Reading

- **28 cases are few.** One case is 3.6 points, so Qwen3-4B (26), Qwen3.5-2B
  (25), Qwen3.5-4B (25) and Gemma 4 E4B (25) are within noise of each other on
  accuracy.
- **Speed and size separate them.** Qwen3.5-2B is twice as fast at half the
  download.
- **Wrong plans are still caught.** Every miss is either a question it should
  have asked or a refusal it should have made. All of them are plans the user
  sees before anything changes, and the core's own checks (names resolved
  against the catalog, methods checked) still apply.
- **The models aren't too old for this job.** Qwen3-4B-2507 (Aug 2025) is not
  beaten by the 2026 releases in this size class on this task. The newer
  models' gains are in reasoning modes, which are too slow here.

## Open

- **Decision for the owner:** Qwen3-4B (best measured, 2.5 GB) as the default,
  or Qwen3.5-2B (half the latency and size, one case fewer). Or offer both,
  e.g. "fast" and "careful", as a setting.
- **Grow the case set** with real transcripts once people use it; re-run before
  changing the pin.
- **Czech speech** was only tested with espeak's synthetic voice, which Parakeet
  mis-hears. A real recording is needed.
