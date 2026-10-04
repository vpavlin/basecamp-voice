import QtQuick
import ".."

// Runs the production Main.qml against a stand-in for basecamp_voice_core and
// the shell, the way Basecamp would host it: replies arrive asynchronously and
// JSON-encoded (sometimes twice), the open-app intent goes through
// bridge.request. Prints CHECK lines, saves a screenshot, exits non-zero on
// any failure.
Item {
    id: top
    width: 760; height: 680

    property string shot: Qt.application.arguments[Qt.application.arguments.length - 1]
    property var calls: []
    property var requests: []
    property int failures: 0
    property int blockingCalls: 0

    // What the stand-in core reports, advanced by the script below.
    property string phase: "idle"
    property string setupState: "missing"
    property string voiceState: "idle"
    function common(r) {
        var have = top.setupState === "ready"
        r.version = "0.2.0"
        r.setup = { state: top.setupState, error: "", missingBytes: have ? 0 : 2931000000,
                    items: [ { id: "runtime", title: "llama.cpp server", size: 17658949, have: have, received: 0 },
                             { id: "llm", title: "Language model", size: 2497281120, have: have, received: 0 },
                             { id: "stt", title: "Speech model", size: 415611879, have: have, received: 0 } ] }
        r.voice = { state: top.voiceState, error: "", transcript: "" }
        r.model = { mode: have ? "local" : "rules", server: have ? "ready" : "stopped", gpu: true, gpuAvailable: true }
        r.settings = { endpoint: "", model: "", apiKey: "" }
        return r
    }
    property bool failSubmit: false
    readonly property string planText: "Install eth_rpc_ui with eth_rpc_module (44 MB), then open it"

    function job(state, stepStatus, summary) {
        return { id: "j1", text: "open eth rpc <img src=\"http://example.invalid/x.png\">", state: state, summary: summary || "",
                 reply: "Installing eth_rpc_ui and opening it.",
                 timing: { planMs: 7200, prepareMs: 300, runMs: state === "done" ? 9100 : 0,
                           model: { promptTokens: 1240, promptMs: 8400, genTokens: 46, genMs: 4100, requests: 1 } },
                 steps: [ { description: top.planText, mutating: true, status: stepStatus, detail: stepStatus === "running" ? "Opening eth_rpc_ui" : "", items: [] },
                          { description: "List the packages in the repository", mutating: false, status: stepStatus === "done" ? "done" : "pending", detail: "",
                            items: [ { name: "kym", note: "app" }, { name: "kym_core", note: "module" } ] } ] }
    }

    function reply(method, args) {
        if (method === "snapshot") return top.common({ ok: true, seq: 0, jobs: [] })
        if (method === "startSetup") { top.setupState = "ready"; return { ok: true } }
        if (method === "recordStart") { top.voiceState = "recording"; return { ok: true } }
        if (method === "recordStop") { top.voiceState = "idle"; return { ok: true } }
        if (method === "submit") {
            if (top.failSubmit) return { ok: false, error: "The planner is not available." }
            top.phase = "planned"
            return { ok: true, job: "j1" }
        }
        if (method === "confirm") { top.phase = "running"; return { ok: true } }
        if (method === "cancel") return { ok: true }
        if (method === "viewActionDone") { top.phase = "done"; return { ok: true } }
        if (method === "events") {
            if (top.phase === "idle") return top.common({ ok: true, seq: 0, jobs: [] })
            if (top.phase === "planned") return top.common({ ok: true, seq: 3, jobs: [ job("awaiting_confirmation", "pending") ] })
            if (top.phase === "running") {
                top.phase = "waiting_view"
                return top.common({ ok: true, seq: 5, jobs: [ job("running", "running") ],
                         viewAction: { id: "a1", intent: "basecamp.apps.launch", params: { app: "eth_rpc_ui" } } })
            }
            if (top.phase === "waiting_view") return top.common({ ok: true, seq: 6, jobs: [ job("running", "running") ] })
            if (top.phase === "done")
                return top.common({ ok: true, seq: 9, jobs: [ job("done", "done", "Installed eth_rpc_module and eth_rpc_ui. Opened eth_rpc_ui. eth_rpc_module is running.") ] })
        }
        return { ok: false, error: "unexpected " + method }
    }

    Main {
        id: view
        anchors.fill: parent
        bridge: QtObject {
            function callModuleAsync(module, method, args, cb, timeoutMs) {
                top.calls.push(method + JSON.stringify(args))
                var r = top.reply(method, args)
                // Basecamp hands string results back JSON-quoted.
                var raw = method === "events" ? JSON.stringify(JSON.stringify(r)) : JSON.stringify(r)
                Qt.callLater(function () { cb(raw) })
            }
            function callModule(module, method, args) {
                top.blockingCalls++
                return ""
            }
            function request(intent, params, cb) {
                top.requests.push(intent + " " + JSON.stringify(params))
                Qt.callLater(function () { cb({ ok: true, data: null, error: "" }) })
            }
        }
    }

    function find(item, name) {
        if (!item) return null
        if (item.objectName === name) return item
        var kids = item.children || []
        for (var i = 0; i < kids.length; i++) {
            var f = find(kids[i], name)
            if (f) return f
        }
        if (item.contentItem && item.contentItem !== item) {
            var c = find(item.contentItem, name)
            if (c) return c
        }
        return null
    }

    function findAll(item, name, acc) {
        acc = acc || []
        if (!item) return acc
        if (item.objectName === name) acc.push(item)
        var kids = item.children || []
        for (var i = 0; i < kids.length; i++) findAll(kids[i], name, acc)
        if (item.contentItem && item.contentItem !== item) findAll(item.contentItem, name, acc)
        return acc
    }

    function check(name, ok, detail) {
        console.log("CHECK " + (ok ? "ok   " : "FAIL ") + name + (ok ? "" : "  -- " + detail))
        if (!ok) top.failures++
    }

    function called(prefix) {
        for (var i = 0; i < top.calls.length; i++) if (top.calls[i].indexOf(prefix) === 0) return true
        return false
    }

    property int step: 0
    Timer {
        interval: 700; running: true; repeat: true
        onTriggered: {
            top.step++
            var input = top.find(view, "commandInput")
            switch (top.step) {
            case 1:
                top.check("snapshot on load", top.called("snapshot"), JSON.stringify(top.calls))
                top.check("go disabled while empty", !top.find(view, "goButton").enabled, "enabled")
                top.check("setup card shows the size", top.find(view, "setupCard").visible
                          && top.find(view, "setupButton").text === "Download (2.9 GB)", top.find(view, "setupButton").text)
                top.check("mic off without the speech model", !top.find(view, "micButton").enabled, "enabled")
                top.find(view, "setupButton").clicked()
                // The failure path first: the error is shown, the text is kept.
                top.failSubmit = true
                input.text = "open eth rpc"
                top.find(view, "goButton").clicked()
                break
            case 2:
                top.check("submit error is shown", top.find(view, "notice").visible
                          && top.find(view, "notice").text === "The planner is not available.", top.find(view, "notice").text)
                top.check("text kept after a failed submit", input.text === "open eth rpc", input.text)
                top.check("setup started on click", top.called("startSetup"), JSON.stringify(top.calls))
                top.failSubmit = false
                input.text = "open eth rpc <img src=\"http://example.invalid/x.png\">"
                top.find(view, "goButton").clicked()
                break
            case 3:
                top.check("submit sends the text", top.called("submit[\"open eth rpc <img"), JSON.stringify(top.calls))
                top.check("input cleared", input.text === "", input.text)
                top.check("plan awaits confirmation", top.find(view, "jobState").text.indexOf("Go ahead?") >= 0, top.find(view, "jobState").text)
                top.check("confirm button shown", top.find(view, "confirmButton").visible, "hidden")
                top.check("nothing confirmed yet", !top.called("confirm"), JSON.stringify(top.calls))
                top.check("planner reply shown", top.find(view, "reply").visible
                          && top.find(view, "reply").text === "Installing eth_rpc_ui and opening it.", top.find(view, "reply").text)
                top.check("setup card gone once ready", !top.find(view, "setupCard").visible, "visible")
                top.check("mic on with the speech model", top.find(view, "micButton").enabled, "disabled")
                top.check("typing hidden once speech works", !top.find(view, "commandInput").visible, "visible")
                top.find(view, "typeToggle").clicked()
                top.check("type-instead shows the text box", top.find(view, "commandInput").visible, "hidden")
                top.find(view, "typeToggle").clicked()
                top.find(view, "micButton").clicked()
                top.find(view, "confirmButton").clicked()
                break
            case 4:
                top.check("recording started", top.called("recordStart"), JSON.stringify(top.calls))
                top.check("mic shows stop while recording", top.find(view, "micButton").text.indexOf("Stop") >= 0, top.find(view, "micButton").text)
                top.find(view, "micButton").clicked()
                break
            case 5:
                top.check("recording stopped", top.called("recordStop"), JSON.stringify(top.calls))
                break
            case 6:
                top.check("confirm sent for the job", top.called("confirm[\"j1\"]"), JSON.stringify(top.calls))
                top.check("view raised the launch intent", top.requests.length === 1
                          && top.requests[0] === "basecamp.apps.launch {\"app\":\"eth_rpc_ui\"}", JSON.stringify(top.requests))
                top.check("intent reply reported to the core",
                          top.called("viewActionDone[\"a1\",\"{\\\"ok\\\":true,\\\"data\\\":null,\\\"error\\\":\\\"\\\"}\"]"), JSON.stringify(top.calls))
                break
            case 7:
                top.check("summary shown", top.find(view, "summary").text.indexOf("Opened eth_rpc_ui.") >= 0, top.find(view, "summary").text)
                top.check("a conversation can be restarted", top.find(view, "newConversation").visible, "hidden")
                top.check("a listing is shown as a list", top.find(view, "stepItems") !== null && top.findAll(view, "stepItems").some(function (c) { return c.visible }), "no visible list")
                top.check("model speed shown", top.find(view, "timing").text.indexOf("model: read 1,240 tokens at 148 t/s, wrote 46 at 11.2 t/s on the GPU (12.5 s)") >= 0,
                          top.find(view, "timing").text)
                top.check("never used the blocking callModule", top.blockingCalls === 0, top.blockingCalls)
                top.check("diagnostics show versions", top.find(view, "diagnostics").text === "core 0.2.0 - view 0.2.0 - model on this computer (ready)", top.find(view, "diagnostics").text)
                view.grabToImage(function (result) {
                    result.saveToFile(top.shot)
                    console.log("SHOT " + top.shot)
                    console.log("RESULT " + (top.failures === 0 ? "PASS" : "FAIL " + top.failures))
                    Qt.exit(top.failures === 0 ? 0 : 1)
                })
                running = false
                break
            }
        }
    }
}
