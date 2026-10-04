# Trying Basecamp Voice (for 2026-10-04)

On a Mac, see `docs/macos.md` ("Trying it on a Mac").

Everything below runs on this laptop.

## Quickest path: an isolated Basecamp (your real data untouched)

```sh
cd ~/devel/github.com/vpavlin/basecamp-voice
export PATH=~/.nix-profile/bin:$PATH
(cd core && nix build .#lgx-portable -o result-lgx)
(cd view && nix build .#lgx-portable -o result-lgx)

scripts/install-local.sh ~/devel/tmp/bv-try
~/Downloads/LogosBasecamp-Desktop-v0.3.1-aeb819-x86_64.AppImage --user-dir ~/devel/tmp/bv-try
```

The build commands are already done and the outputs are current. Re-run them
only after changing code.

1. In Basecamp, open **Basecamp Voice**. The setup card shows what it needs:
   the llama.cpp server (Vulkan build on this machine), the language model and
   the speech model, about 2.9 GB.
2. **Optional: skip the 2.9 GB download.** With Basecamp Voice open once, run
   in another terminal:

   ```sh
   scripts/seed-models.sh ~/devel/tmp/bv-try
   ```

   It links the models already downloaded and checked here. Then press
   **Download**: only the ~30 MB runtime is fetched, and the models are
   verified in place.
3. **Pick a model:** **Careful** is Qwen3-4B, the default; **Fast** is
   Qwen3.5-2B. You can switch later under *Model settings*.

To use your normal Basecamp instead: quit it and run
`scripts/install-local.sh` with no argument, which installs to
`~/.local/share/Logos/LogosBasecamp`.

## What to try

| Try | Expect |
|---|---|
| Press **Speak**, say "open the token list app", press **Stop** | "Listening…", then "Working out what you said…". Then the plan "Install token_list_ui with token_list_module (42 MB), then open it" and **Do it** / **Cancel** |
| **Do it** | Live steps (downloading with MB, installing, opening, waiting for the module), then the app opens and a summary appears |
| Say it in Czech: "otevři peněženku" | It asks which wallet: eth_wallet_ui, lez_wallet_ui or monero_wallet_ui |
| "install and run the blockchain app" | 161 MB plan; watch the download progress |
| "what's running?" | Runs without asking (read-only tool) and lists modules |
| "call eth_rpc_module.list_chains" (eth rpc open) | Asks first: every module call is confirmed |
| "order me a pizza" | Says it can't |
| Model settings → point at Ollama (`http://localhost:11434`, model e.g. `qwen3:4b`) | Plans come from there; speech stays local |

## What to look for

- **Speed.** Each job shows "planned in X s (+Y s checking), ran in Z s".
  Measured here:
  - Qwen3-4B: about 4–6 s of model time per plan on the Iris Xe.
  - Fast (Qwen3.5-2B): about 2.5 s.
  - The very first command after opening also starts and warms the server.
- **The microphone path has not been tried with a real voice:**
  - which recorder was used (`pw-record` here);
  - whether the transcript is right, in English and in Czech;
  - whether Stop finishes cleanly.

  The transcript shows under the input row, and the job title is what was
  heard.
- **What the window looks like.** It was only seen offscreen, in the test
  harness.
- **When the app goes to the background.** Opening an app takes focus away
  from Basecamp Voice. Does progress still update when you come back?

## Where things are

- **Logs:** `<user-dir>/module_data/basecamp_voice_core/<id>/llama-server.log`
  (the model server), and Basecamp's own log in `<user-dir>/logs/`.
- **Downloaded runtime and models:**
  `<user-dir>/module_data/basecamp_voice_core/<id>/assets/`.
- **Settings:** `…/settings.json`, owner-only because it may hold an API key.
- **Automated checks:**
  - `(cd core && nix build .#unit-tests)`: 69 tests;
  - `view/test/check.sh`: the window, offscreen;
  - `tests/e2e/run.sh`: real Basecamp; see the README.
