#!/usr/bin/env python3
"""Score the planner prompt against a running OpenAI-compatible server.

Uses core/src/system_prompt.inc and plan_schema.inc (the files the core
compiles in) and builds the context the way Tools::context does. Cases are in
cases.json: what the user said, the Basecamp state, and the expected steps.

  eval.py http://127.0.0.1:18431 [catalog index.json]
"""
import json, os, re, sys, time, urllib.request, pathlib

here = pathlib.Path(__file__).resolve().parent
src = here.parent / "src"
def raw(path):
    s = path.read_text()
    return re.search(r'R"(\w*)\((.*)\)\1"', s, re.S).group(2)
SYSTEM = raw(src / "system_prompt.inc")
SCHEMA = json.loads(raw(src / "plan_schema.inc"))
CONTEXT_FMT = (src / "context_format.txt").read_text() if (src / "context_format.txt").exists() else None

def norm(s): return re.sub(r"[^a-z0-9]", "", s.lower())

def catalog_from(index):
    out = {}
    for p in index.get("packages", index):
        v = p["versions"][0]; m = v.get("manifest", {})
        deps = [d if isinstance(d, str) else d["name"] for d in m.get("dependencies", [])]
        out[p["name"]] = {"type": m.get("type", ""), "description": m.get("description", "")[:100], "deps": deps}
    return out

SYSTEM_MODULES = {"capability_module", "package_manager", "package_downloader", "modules_state", "basecamp_voice_core"}
# Words that say what to do, not to what: they would match half the catalog.
STOPWORDS = {"the", "and", "app", "apps", "application", "open", "start", "run", "launch", "install", "please",
             "can", "could", "you", "what", "which", "show", "list", "module", "modules", "for", "with", "this",
             "that", "there", "are", "have", "like", "want", "need", "get", "set", "thing", "about", "into",
             "from", "all", "any", "some", "now", "right", "uh", "um"}
MAX_MATCHES = 8
DESC_CHARS = 70

def load_recipes():
    """The recipes the core compiles in (core/recipes/*.inc): app, words, actions."""
    out = []
    for f in sorted((here.parent / "recipes").glob("*.inc")):
        if f.name == "all.inc": continue
        r = {"app": "", "words": [], "actions": [], "notes": []}
        for line in raw(f).splitlines():
            t = line.strip()
            if not t or t.startswith("#"): continue
            if line[0].isspace():
                if t.startswith("require ") and r["actions"]: r["actions"][-1]["requires"].append(t[8:t.index(":")].strip())
                continue
            k, _, v = t.partition(":")
            k, v = k.strip(), v.strip()
            if k == "app": r["app"] = v
            elif k == "words": r["words"] = [w.strip() for w in v.split(",") if w.strip()]
            elif k == "note": r["notes"].append(v)
            elif k.startswith("action "): r["actions"].append({"id": k[7:].strip(), "title": v, "requires": []})
        out.append(r)
    return out
RECIPES = load_recipes()
# Same narrowing as ModelPlanner::schema(): only recipes that exist.
for v in SCHEMA["properties"]["steps"]["items"]["anyOf"]:
    if v["properties"]["tool"].get("const") == "recipe":
        v["properties"]["args"] = {"anyOf": [{"type": "object", "properties": {"app": {"const": r["app"]},
            "action": {"enum": [a["id"] for a in r["actions"]]}}, "required": ["app", "action"], "additionalProperties": False}
            for r in RECIPES]}

def context(text, catalog, installed, running, methods, facts=None):
    """Mirror of Tools::context in core/src/tools.cpp: keep the two identical."""
    words = [w for w in re.findall(r"[a-z0-9]+", text.lower()) if len(w) >= 3 and w not in STOPWORDS]
    lines = []
    inst = [n for n in installed if n not in SYSTEM_MODULES]
    apps = [n for n in inst if catalog.get(n, {}).get("type") == "ui_qml"]
    mods = [n for n in inst if catalog.get(n, {}).get("type") != "ui_qml"]
    fmt = lambda n: n + (" (needs " + ", ".join(catalog[n]["deps"]) + ")" if catalog.get(n, {}).get("deps") else "")
    run = sorted(m for m in running if m not in SYSTEM_MODULES)
    lines.append("Installed apps: " + (", ".join(fmt(n) for n in sorted(apps)) or "none"))
    lines.append("Installed modules: " + (", ".join(sorted(mods)) or "none"))
    lines.append("Running modules: " + (", ".join(run) or "none"))
    all_apps = sorted(n for n, p in catalog.items() if p["type"] == "ui_qml" and n not in SYSTEM_MODULES)
    lines.append("Apps in the catalog: " + ", ".join(all_apps))
    matches = []
    for n, p in sorted(catalog.items()):
        if n in SYSTEM_MODULES: continue
        hay = norm(n + " " + p["description"])
        if any(w in hay for w in words):
            matches.append(n)
    matches.sort(key=lambda n: (catalog[n]["type"] != "ui_qml", n))
    if matches:
        lines.append("Catalog matches:")
        for n in matches[:MAX_MATCHES]:
            p = catalog[n]
            lines.append(f"- {n} ({'app' if p['type']=='ui_qml' else 'module'}){' needs ' + ', '.join(p['deps']) if p['deps'] and p['type']=='ui_qml' else ''}: {p['description'][:DESC_CHARS]}"
                         + (" [installed]" if n in installed else ""))
    rec_lines = []
    for r in RECIPES:
        inst = r["app"] in installed
        mentioned = any(norm(rw).find(w) >= 0 or w.find(norm(rw)) >= 0 for w in words for rw in r["words"])
        if not inst and not mentioned: continue
        have = (facts or {}).get(r["app"], [])
        acts = []
        for a in r["actions"]:
            ready = "ready" if all(f in have for f in a["requires"]) else "not set up in the app yet"
            acts.append(f'{a["id"]} ({a["title"]}; {ready})')
        rec_lines.append(f'- {r["app"]}{"" if inst else " (not installed)"}: ' + ", ".join(acts))
        rec_lines += [f"  Note: {n}" for n in r["notes"]]
    if rec_lines:
        lines.append("Recipes (they do what the app's own buttons do; use them when they fit):")
        lines += rec_lines
    for m in run:
        if m in methods:
            lines.append(f"Methods of {m}: " + ", ".join(methods[m]))
    return "Context:\n" + "\n".join(lines) + f'\n\nUser: "{text}"'

def history_text(history):
    """Mirror of ModelPlanner::historyText."""
    if not history: return ""
    out = 'Conversation so far (oldest first; the user may refer to it, e.g. "the third one", "that app"):\n'
    for h in history:
        out += f'- User: "{h["said"]}"\n  Result: {h["result"]}\n'
        if h.get("lists"): out += f'  Listed: {h["lists"]}\n'
    return out + "\n"

def ask(base, user):
    body = {"messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": user}],
            "temperature": 0, "max_tokens": 400,
            "chat_template_kwargs": {"enable_thinking": False},
            "response_format": {"type": "json_schema", "json_schema": {"name": "plan", "schema": SCHEMA}}}
    req = urllib.request.Request(base + "/v1/chat/completions", json.dumps(body).encode(), {"Content-Type": "application/json"})
    t = time.time()
    r = json.load(urllib.request.urlopen(req, timeout=300))
    msg = r["choices"][0]["message"]
    try:
        plan = json.loads(msg.get("content") or "")
    except ValueError:
        plan = {"reply": "UNPARSEABLE: " + repr(msg)[:300], "steps": [{"tool": "?"}]}
    return plan, time.time() - t, r.get("usage", {})

def engine_filter(steps):
    """Mirror of Engine::planJob: drop list/status steps when the plan also acts."""
    acts = any(s.get("tool") in ("install", "open_app", "recipe", "call", "add_repository") for s in steps)
    # a repository listing right after adding it is the point, not a check
    repo_listing = lambda s: s.get("tool") == "list_available" and s.get("args", {}).get("repository")
    return [s for s in steps if not (acts and s.get("tool") in ("list_installed", "list_available", "status") and not repo_listing(s))]

def matches(got, want):
    got = engine_filter(got)
    if len(got) != len(want): return False
    for g, w in zip(got, want):
        if g["tool"] != w["tool"]: return False
        for k, v in w.get("args", {}).items():
            if g["args"].get(k) != v: return False
    return True

if __name__ == "__main__":
    base = sys.argv[1]
    catalog = catalog_from(json.load(open(sys.argv[2] if len(sys.argv) > 2 else "/tmp/bcv/index.json")))
    for core in ("capability_module", "package_manager", "package_downloader", "modules_state"):
        catalog.setdefault(core, {"type": "core", "description": "", "deps": []})
    cases = json.load(open(here / "cases.json"))
    only = os.environ.get("ONLY")   # a substring of the cases to run
    if only: cases = [c for c in cases if only in json.dumps(c, ensure_ascii=False)]
    passed = 0
    times = []
    for c in cases:
        user = history_text(c.get("history")) + context(c["say"], catalog, c.get("installed", []), c.get("running", []), c.get("methods", {}), c.get("facts"))
        if c.get("done"):
            # A later round: the same text ModelPlanner::planNext adds.
            user += "\n\nDone so far for this request:\n" + "".join(f"- {d['step']}: {d['result'][:300]}\n" for d in c["done"])
            user += ("\nPlan only what is left of the request. If nothing is left, or you need something from the user "
                     "(say what), return no steps and say so in reply.")
        plan, secs, usage = ask(base, user)
        wants = c.get("want_any", [c.get("want", [])])
        ok = any(matches(plan["steps"], w) for w in wants)
        times.append(secs)
        passed += ok
        print(f"{'PASS' if ok else 'FAIL'} {secs:5.1f}s {usage.get('prompt_tokens','?'):>5}t  {c['say']!r}")
        if not ok or "-v" in sys.argv:
            print("      want:", json.dumps(wants))
            print("      got: ", json.dumps(plan))
    times.sort()
    print(f"{passed}/{len(cases)} passed; median {times[len(times)//2]:.1f}s, max {times[-1]:.1f}s")
