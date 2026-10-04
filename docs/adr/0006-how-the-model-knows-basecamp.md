# 0006 — How the model knows Basecamp

- Status: **Proposed** (owner's question, 2026-10-03; to settle before step 2)
- Date: 2026-10-03

## Context

A small local model (3–4B) has to turn what the user said into steps with
the tools of ADR 0004. It knows nothing about Basecamp. The catalog and the
installed set change daily, and module APIs differ by version. The core already
refuses anything outside the tools, and asks before anything changes.

## Proposal

No fine-tuning to start. Four layers:

1. **Constrained output.**
   - llama-server is asked for a JSON-schema / grammar-constrained reply, so
     the model can only produce a plan made of the seven tools with
     well-formed arguments.
   - The planner interface (`core/src/planner.h`) already takes this shape.
2. **A short, fixed system prompt.** It covers:
   - what Basecamp is: apps vs modules; "open X" means open its app;
     packages come from a catalog;
   - each tool, in one line;
   - 10–20 worked examples: spoken command → exact plan, including "I don't
     know" and "which one?" cases.
3. **Facts supplied with each request, not memorised.** The core adds to the
   prompt:
   - the installed apps;
   - the catalog entries relevant to what was said (names and display names
     matched as in `Tools::resolve`);
   - what's running.

   When the model needs a module's methods, it calls `module_methods` and plans
   in a second turn. Methods come from `getPluginMethods`, which includes
   doc-comment descriptions.
4. **An eval set before any tuning.**
   - Spoken commands paired with their expected plans. The planner tests and
     e2e scenario are the seed; real transcripts join once voice lands.
   - Each model × prompt combination is scored on the set.
   - Fine-tune (LoRA) only if a model that size keeps failing cases a prompt
     change can't fix.

## Rejected

- **Letting the model call anything by name with free-form arguments.** The
  tool set and its checks are the safety boundary.
- **Fine-tuning first.** It needs the eval set anyway. It would bake in a
  catalog that changes daily, and has to be redone for every model swap.

## Consequences

- **The prompt stays small,** because only relevant catalog entries are
  included. That matters for CPU-only machines.
- **Two-turn planning adds latency.** A method lookup costs one more model
  turn; acceptable while plans are short.

## Update 2026-10-04: a conversation, not single commands

**What changed:** each request now carries the conversation so far, so a user
can say "add this repository and show me its apps", look at the list, then say
"install the second one" or "open it".
- The conversation is the last 6 finished requests since **New conversation**
  (`Engine::history`), each with its result and its listings.
- **Lists are numbered the same way** in the window and in what the model
  reads, so "the third one" means the same thing to both.

**Measured:** Qwen3-4B resolved "the second one", "it" and the Czech "tu
třetí" correctly (4/4, cases tagged `convo` in `core/eval/cases.json`).
