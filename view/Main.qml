import QtQuick
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

// Basecamp Voice. A thin view over basecamp_voice_core: it sends what the user
// asked for, shows the plan, asks before anything changes, follows the steps
// live and shows the summary. All the work happens in the core.
//
// Rules (logos-skills): every call goes through callVia (logos.callModuleAsync,
// never the blocking callModule), polls are single-flight, every text item is
// PlainText, every action ends in a visible outcome.
//
// The one thing only the view can do is open apps: shell intents are a QML API.
// The core asks through the poll reply (viewAction) and gets the answer back
// through viewActionDone.
Item {
    id: root
    implicitWidth: 720
    implicitHeight: 640

    // `logos` inside Basecamp; a stand-in in test/VoiceHarness.qml.
    property var bridge: typeof logos !== "undefined" ? logos : null

    readonly property string viewVersion: "0.2.0"
    readonly property string coreModule: "basecamp_voice_core"
    property string coreVersion: ""

    property var jobs: []
    property int lastSeq: 0
    property int conversationStart: 0
    readonly property var current: convoJobs.length > 0 ? convoJobs[convoJobs.length - 1] : null
    readonly property bool jobActive: current !== null
        && (current.state === "planning" || current.state === "running" || current.state === "awaiting_confirmation")
    // Poll fast while anything is moving.
    readonly property bool active: jobActive || setup.state === "downloading" || voice.state !== "idle"

    // From the core's poll reply: downloads, microphone, model.
    property var setup: ({ state: "missing", items: [], missingBytes: 0, error: "" })
    property var voice: ({ state: "idle", error: "", transcript: "" })
    property var model: ({ mode: "rules", server: "stopped" })
    property var settings: ({ endpoint: "", model: "", apiKey: "", localModel: "" })
    property var models: []
    property var intents: ({})   // the core's last look for app intents: {count, apps, problem}
    property bool setupDismissed: false
    property bool showSettings: false
    property bool voiceBusy: false
    property bool typing: false
    // When each in-flight flag was raised: a callback the host never delivers
    // must not leave a button disabled for good (cleared after 45 s).
    property double submittingSince: 0
    property double actingSince: 0
    property double voiceBusySince: 0
    readonly property bool speechReady: root.haveAsset("stt")

    property bool pollBusy: false
    property bool pollAgain: false
    property double pollStartedAt: 0
    property int pollMisses: 0

    property bool submitting: false
    property string actingOn: ""        // job id while confirm/cancel is in flight
    property string notice: ""
    property bool noticeIsError: false

    property int callTimeoutMs: 20000

    // ---- talking to the core ---------------------------------------------------

    // The ONLY way this view talks to a module.
    function callVia(mod, method, args, cb) {
        var a = args || []
        var done = function (raw) {
            if (cb) {
                try { cb(raw === undefined || raw === null ? "" : raw) } catch (e) { console.warn("basecamp_voice:", e) }
            }
        }
        if (!root.bridge) { Qt.callLater(function () { done("") }); return }
        if (typeof root.bridge.callModuleAsync === "function") {
            try { root.bridge.callModuleAsync(mod, method, a, done, root.callTimeoutMs) }
            catch (e) { Qt.callLater(function () { done("") }) }
            return
        }
        // A host without the async call: at least let the frame paint first.
        Qt.callLater(function () {
            var r = ""
            try { r = root.bridge.callModule(mod, method, a) } catch (e) {}
            done(r)
        })
    }

    function core(method, args, cb) { root.callVia(root.coreModule, method, args, cb) }

    // Replies may arrive JSON-quoted, sometimes twice.
    function parse(raw) {
        var v = raw
        for (var i = 0; i < 3 && typeof v === "string"; i++) {
            try { v = JSON.parse(v) } catch (e) { return null }
        }
        return (v !== null && typeof v === "object") ? v : null
    }

    function showNotice(text, isError) {
        root.notice = text
        root.noticeIsError = isError
    }

    function clearStuckFlags() {
        var now = Date.now()
        if (root.submitting && now - root.submittingSince > 45000) root.submitting = false
        if (root.actingOn !== "" && now - root.actingSince > 45000) root.actingOn = ""
        if (root.voiceBusy && now - root.voiceBusySince > 45000) root.voiceBusy = false
    }

    function poll() {
        root.clearStuckFlags()
        if (root.pollBusy) {
            // One lost reply must not stop the poll forever.
            if (Date.now() - root.pollStartedAt < 45000) { root.pollAgain = true; return }
        }
        root.pollBusy = true
        root.pollStartedAt = Date.now()
        root.core("events", [String(root.lastSeq)], function (raw) {
            root.pollBusy = false
            var r = root.parse(raw)
            if (!r || r.ok === false) {
                root.pollMisses++
            } else {
                root.pollMisses = 0
                root.lastSeq = r.seq || root.lastSeq
                root.applyState(r)
                // "Going ahead." is stale once the job has finished.
                if (!root.active && !root.noticeIsError && root.actingOn === "") root.notice = ""
                if (r.viewAction) root.performViewAction(r.viewAction)
            }
            if (root.pollAgain) { root.pollAgain = false; root.poll() }
        })
    }

    function applyState(r) {
        root.jobs = r.jobs || []
        if (r.conversationStart !== undefined) root.conversationStart = r.conversationStart
        if (r.version) root.coreVersion = r.version
        if (r.setup) root.setup = r.setup
        if (r.voice) root.voice = r.voice
        if (r.model) root.model = r.model
        if (r.settings) root.settings = r.settings
        if (r.models) root.models = r.models
        if (r.intents) root.intents = r.intents
    }

    function haveAsset(id) {
        var items = root.setup.items || []
        for (var i = 0; i < items.length; i++) if (items[i].id === id) return items[i].have
        return false
    }

    function sizeText(bytes) {
        if (!bytes || bytes <= 0) return ""
        if (bytes >= 1e9) return (bytes / 1e9).toFixed(1) + " GB"
        return Math.round(bytes / 1e6) + " MB"
    }

    function simple(method, args, okText, after) {
        root.core(method, args || [], function (raw) {
            var r = root.parse(raw)
            if (r && r.ok) { if (okText) root.showNotice(okText, false) }
            else root.showNotice(r && r.error ? r.error : "No answer from basecamp_voice_core.", true)
            if (after) after(r)
            root.poll()
        })
    }

    function toggleRecording() {
        if (root.voiceBusy) return
        root.voiceBusy = true
        root.voiceBusySince = Date.now()
        var method = root.voice.state === "recording" ? "recordStop" : "recordStart"
        root.simple(method, [], "", function () { root.voiceBusy = false })
    }

    function saveSettings() {
        var s = { endpoint: endpointField.text.trim(), model: modelField.text.trim() }
        if (apiKeyField.text.length > 0) s.apiKey = apiKeyField.text
        root.simple("configure", [JSON.stringify(s)], "Saved.", function () { apiKeyField.text = "" })
    }

    function performViewAction(action) {
        var report = function (res) {
            root.core("viewActionDone", [action.id, JSON.stringify(res)], function () { root.poll() })
        }
        if (!root.bridge || typeof root.bridge.request !== "function") {
            report({ ok: false, error: "this Basecamp cannot open apps from a view" })
            return
        }
        try {
            root.bridge.request(action.intent, action.params || {}, function (res) { report(res) })
        } catch (e) {
            report({ ok: false, error: String(e) })
        }
    }

    function submit() {
        var text = input.text.trim()
        if (text.length === 0 || root.submitting) return
        root.submitting = true
        root.submittingSince = Date.now()
        root.showNotice("", false)
        root.core("submit", [text], function (raw) {
            root.submitting = false
            var r = root.parse(raw)
            if (r && r.ok) {
                input.text = ""
                root.followBottom = true
                root.poll()
            } else {
                root.showNotice(r && r.error ? r.error : "Could not reach basecamp_voice_core. Is it installed and running?", true)
            }
        })
    }

    function act(method, jobId, okText) {
        if (root.actingOn !== "") return
        root.actingOn = jobId
        root.actingSince = Date.now()
        root.core(method, [jobId], function (raw) {
            root.actingOn = ""
            var r = root.parse(raw)
            if (r && r.ok) root.showNotice(r.note || okText, false)
            else root.showNotice(r && r.error ? r.error : "No answer from basecamp_voice_core.", true)
            root.poll()
        })
    }

    // "read 1,240 tokens at 148 t/s, wrote 46 at 11.2 t/s"
    function modelLine(m) {
        if (!m || m.genTokens === undefined) return ""
        var rate = function (n, ms) { return ms > 0 ? " at " + (n * 1000 / ms).toFixed(n * 1000 / ms >= 100 ? 0 : 1) + " t/s" : "" }
        var s = "model: read " + Math.round(m.promptTokens || 0).toLocaleString(Qt.locale("en_US"), "f", 0) + " tokens" + rate(m.promptTokens || 0, m.promptMs || 0)
              + ", wrote " + Math.round(m.genTokens) + rate(m.genTokens, m.genMs || 0)
        if (root.model.mode === "local") s += root.model.gpu ? " on the GPU" : " on the CPU"
        if (m.promptMs !== undefined && m.genMs !== undefined) s += " (" + ((m.promptMs + m.genMs) / 1000).toFixed(1) + " s"
              + (m.requests > 1 ? ", " + m.requests + " requests" : "") + ")"
        return s
    }

    function stateLabel(state) {
        switch (state) {
        case "planning": return "Working out a plan..."
        case "awaiting_confirmation": return "This will change your Basecamp. Go ahead?"
        case "running": return "Working..."
        case "done": return "Done"
        case "failed": return "Failed"
        case "cancelled": return "Cancelled"
        }
        return state
    }

    function mark(status) {
        switch (status) {
        case "running": return "▶"
        case "done": return "✓"
        case "failed": return "✗"
        case "skipped": return "–"
        }
        return "•"
    }

    function markColor(status) {
        if (status === "done") return Theme.palette.success
        if (status === "failed") return Theme.palette.error
        if (status === "running") return Theme.palette.primary
        return Theme.palette.textTertiary
    }

    Timer {
        interval: root.active ? 500 : 2000
        running: true
        repeat: true
        onTriggered: root.poll()
    }

    Component.onCompleted: Qt.callLater(function () {
        root.core("snapshot", [], function (raw) {
            var r = root.parse(raw)
            if (r && r.ok !== false) {
                root.applyState(r)
                root.lastSeq = r.seq || 0
                endpointField.text = root.settings.endpoint || ""
                modelField.text = root.settings.model || ""
            } else {
                root.pollMisses++
            }
        })
    })

    // ---- the page: a conversation ------------------------------------------------

    // The requests since the last "New conversation", oldest first.
    readonly property var convoJobs: {
        var out = []
        for (var i = 0; i < root.jobs.length; i++)
            if (parseInt(String(root.jobs[i].id).substring(1)) > root.conversationStart) out.push(root.jobs[i])
        return out
    }
    // Keep the newest message in view unless the user scrolled up to read.
    property bool followBottom: true

    Rectangle {
        anchors.fill: parent
        color: Theme.palette.background
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.xlarge
        spacing: Theme.spacing.medium

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.small
            LogosText {
                textFormat: Text.PlainText
                text: "Basecamp Voice"
                font.pixelSize: Theme.typography.panelTitleText
                font.weight: Theme.typography.weightBold
            }
            Item { Layout.fillWidth: true }
            LogosButton {
                objectName: "newConversation"
                visible: root.convoJobs.length > 0
                compact: true
                text: "New conversation"
                onClicked: root.simple("newConversation", [], "")
            }
            LogosButton {
                compact: true
                text: root.showSettings ? "Hide settings" : "Settings"
                onClicked: root.showSettings = !root.showSettings
            }
        }

        ColumnLayout {
            visible: root.showSettings
            Layout.fillWidth: true
            spacing: Theme.spacing.small
            LogosText {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.palette.textSecondary
                text: "Leave empty to use the model on this computer. Or give an OpenAI-compatible server, for example Ollama at http://localhost:11434."
            }
            RowLayout {
                spacing: Theme.spacing.small
                visible: root.model.gpuAvailable === true
                LogosText { textFormat: Text.PlainText; color: Theme.palette.textSecondary; text: "Run the model on:" }
                Repeater {
                    model: [ { id: "gpu", label: "GPU (faster when idle)" }, { id: "cpu", label: "CPU (steadier)" } ]
                    delegate: LogosButton {
                        required property var modelData
                        objectName: "device_" + modelData.id
                        compact: true
                        variant: (root.settings.device || "cpu") === modelData.id ? LogosButton.Variant.Primary : LogosButton.Variant.Secondary
                        text: modelData.label
                        onClicked: root.simple("configure", [JSON.stringify({ device: modelData.id })],
                                               "The model will run on the " + modelData.id.toUpperCase() + " from the next request.")
                    }
                }
            }
            RowLayout {
                spacing: Theme.spacing.small
                visible: root.models.length > 1
                LogosText { textFormat: Text.PlainText; color: Theme.palette.textSecondary; text: "Model on this computer:" }
                Repeater {
                    model: root.models
                    delegate: LogosButton {
                        required property var modelData
                        compact: true
                        variant: root.settings.localModel === modelData.id ? LogosButton.Variant.Primary : LogosButton.Variant.Secondary
                        text: (modelData.id.indexOf("2b") >= 0 ? "Fast" : "Careful") + " - " + root.sizeText(modelData.size)
                        onClicked: root.simple("configure", [JSON.stringify({ localModel: modelData.id })], "")
                    }
                }
            }
            LogosTextField { id: endpointField; Layout.fillWidth: true; placeholderText: "Server URL (empty: this computer)" }
            LogosTextField { id: modelField; Layout.fillWidth: true; placeholderText: "Model name (for that server)" }
            LogosTextField { id: apiKeyField; Layout.fillWidth: true; placeholderText: root.settings.apiKey === "(set)" ? "API key (set; type to replace)" : "API key (if the server needs one)"; echoMode: TextInput.Password }
            RowLayout {
                spacing: Theme.spacing.small
                LogosButton { text: "Save"; onClicked: root.saveSettings() }
                LogosButton {
                    visible: root.settings.apiKey === "(set)"
                    text: "Forget the API key"
                    onClicked: root.simple("configure", [JSON.stringify({ apiKey: "" })], "The API key is gone.")
                }
            }
        }

        Flickable {
            id: page
            objectName: "conversation"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: column.implicitHeight
            clip: true
            onContentHeightChanged: if (root.followBottom) contentY = Math.max(0, contentHeight - height)
            onHeightChanged: if (root.followBottom) contentY = Math.max(0, contentHeight - height)
            onMovementEnded: root.followBottom = contentY >= contentHeight - height - 24

            ColumnLayout {
                id: column
                width: page.width
                spacing: Theme.spacing.large

                // Setup: what runs on this computer, and its size, before anything is fetched.
                LogosFrame {
                    objectName: "setupCard"
                    Layout.fillWidth: true
                    visible: root.setup.state !== "ready" && root.model.mode !== "remote" && !root.setupDismissed

                    ColumnLayout {
                        width: parent.width
                        spacing: Theme.spacing.small

                        LogosText {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: Theme.typography.subtitleText
                            text: root.setup.state === "downloading" ? "Downloading..." : "Set up Basecamp Voice"
                        }
                        LogosText {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: Theme.palette.textSecondary
                            visible: root.setup.state !== "downloading"
                            text: "Speech and the language model run on this computer; nothing you say leaves it. "
                                  + "They are downloaded once (" + root.sizeText(root.setup.missingBytes) + ") and checked before use. "
                                  + "Until then, typed commands work in a simple form."
                        }
                        // Which language model: careful (default) or fast (docs/model-eval.md).
                        RowLayout {
                            visible: root.setup.state !== "downloading" && root.models.length > 1
                            spacing: Theme.spacing.small
                            Repeater {
                                model: root.models
                                delegate: LogosButton {
                                    required property var modelData
                                    objectName: "modelChoice_" + modelData.id
                                    compact: true
                                    variant: root.settings.localModel === modelData.id ? LogosButton.Variant.Primary : LogosButton.Variant.Secondary
                                    text: (modelData.id.indexOf("2b") >= 0 ? "Fast" : "Careful") + " - " + root.sizeText(modelData.size)
                                    onClicked: root.simple("configure", [JSON.stringify({ localModel: modelData.id })], "")
                                }
                            }
                        }
                        Repeater {
                            model: root.setup.items || []
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 2
                                LogosText {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    color: modelData.have ? Theme.palette.textTertiary : Theme.palette.text
                                    text: (modelData.have ? "\u2713 " : "") + modelData.title + " - " + root.sizeText(modelData.size)
                                }
                                LogosProgressBar {
                                    Layout.fillWidth: true
                                    visible: root.setup.state === "downloading" && !modelData.have
                                    from: 0
                                    to: Math.max(1, modelData.size)
                                    value: modelData.received
                                }
                            }
                        }
                        LogosText {
                            objectName: "setupError"
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            visible: (root.setup.error || "").length > 0
                            color: Theme.palette.error
                            text: root.setup.error || ""
                        }
                        RowLayout {
                            spacing: Theme.spacing.small
                            LogosButton {
                                objectName: "setupButton"
                                visible: root.setup.state !== "downloading"
                                text: (root.setup.state === "failed" ? "Try again" : "Download") + " (" + root.sizeText(root.setup.missingBytes) + ")"
                                variant: LogosButton.Variant.Primary
                                onClicked: root.simple("startSetup", [], "")
                            }
                            LogosButton {
                                visible: root.setup.state === "downloading"
                                text: "Stop"
                                onClicked: root.simple("cancelSetup", [], "Stopped. The download continues from here next time.")
                            }
                            LogosButton {
                                visible: root.setup.state !== "downloading"
                                text: "Not now"
                                onClicked: root.setupDismissed = true
                            }
                        }
                    }
                }

                LogosText {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spacing.xxlarge
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    visible: root.convoJobs.length === 0
                    color: Theme.palette.textTertiary
                    text: "Ask for something, then follow up: \"add this repository and show me its apps\" ... \"install the third one\" ... \"open it\"."
                }

                Repeater {
                    model: root.convoJobs
                    delegate: ColumnLayout {
                        id: card
                        required property var modelData
                        readonly property var job: modelData
                        Layout.fillWidth: true
                        spacing: Theme.spacing.small

                        // What the user said.
                        Rectangle {
                            Layout.alignment: Qt.AlignRight
                            Layout.maximumWidth: column.width * 0.8
                            implicitWidth: said.implicitWidth + 2 * Theme.spacing.medium
                            implicitHeight: said.implicitHeight + 2 * Theme.spacing.small
                            radius: Theme.spacing.radiusLarge
                            color: Theme.palette.primary
                            LogosText {
                                id: said
                                textFormat: Text.PlainText
                                anchors.fill: parent
                                anchors.margins: Theme.spacing.small
                                anchors.leftMargin: Theme.spacing.medium
                                anchors.rightMargin: Theme.spacing.medium
                                wrapMode: Text.Wrap
                                width: Math.min(implicitWidth, column.width * 0.8 - 2 * Theme.spacing.medium)
                                color: "white"
                                text: card.job.text
                            }
                        }

                        // What came of it: plan, confirmation, steps, lists, summary.
                        LogosFrame {
                            objectName: "currentJob"
                            Layout.fillWidth: true
                            Layout.rightMargin: column.width * 0.1

                            ColumnLayout {
                                width: parent.width
                                spacing: Theme.spacing.medium


                                LogosText {
                                    objectName: "reply"
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    visible: (card.job.reply || "").length > 0 && card.job.state !== "done"
                                    color: Theme.palette.textSecondary
                                    text: card.job ? (card.job.reply || "") : ""
                                }

                                RowLayout {
                                    spacing: Theme.spacing.small
                                    LogosSpinner {
                                        visible: (card.job.state === "planning" || card.job.state === "running")
                                        Layout.preferredWidth: 18
                                        Layout.preferredHeight: 18
                                    }
                                    LogosText {
                                        objectName: "jobState"
                                        textFormat: Text.PlainText
                                        color: card.job.state === "failed" ? Theme.palette.error : Theme.palette.textSecondary
                                        text: card.job ? root.stateLabel(card.job.state) : ""
                                    }
                                }

                                Repeater {
                                    model: card.job ? card.job.steps : []
                                    delegate: ColumnLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: 2
                                        RowLayout {
                                            spacing: Theme.spacing.small
                                            LogosText {
                                                textFormat: Text.PlainText
                                                color: root.markColor(modelData.status)
                                                text: root.mark(modelData.status)
                                            }
                                            LogosText {
                                                textFormat: Text.PlainText
                                                Layout.fillWidth: true
                                                wrapMode: Text.Wrap
                                                text: modelData.description
                                            }
                                        }
                                        // A listing's entries, as a list (apps first).
                                        Column {
                                            objectName: "stepItems"
                                            Layout.fillWidth: true
                                            Layout.leftMargin: Theme.spacing.xlarge
                                            spacing: 2
                                            visible: modelData.items !== undefined && modelData.items.length > 0
                                            Repeater {
                                                model: modelData.items ? modelData.items.slice(0, 40) : []
                                                delegate: Row {
                                                    required property var modelData
                                                    required property int index
                                                    spacing: Theme.spacing.small
                                                    // Numbered like the model's copy, so "the third one" is unambiguous.
                                                    LogosText {
                                                        textFormat: Text.PlainText
                                                        color: Theme.palette.textTertiary
                                                        text: (index + 1) + "."
                                                    }
                                                    LogosText {
                                                        textFormat: Text.PlainText
                                                        text: modelData.name
                                                    }
                                                    LogosText {
                                                        textFormat: Text.PlainText
                                                        color: Theme.palette.textTertiary
                                                        text: modelData.note || ""
                                                    }
                                                }
                                            }
                                            LogosText {
                                                textFormat: Text.PlainText
                                                visible: modelData.items !== undefined && modelData.items.length > 40
                                                color: Theme.palette.textTertiary
                                                text: modelData.items ? "and " + (modelData.items.length - 40) + " more" : ""
                                            }
                                        }
                                        LogosText {
                                            textFormat: Text.PlainText
                                            Layout.fillWidth: true
                                            Layout.leftMargin: Theme.spacing.xlarge
                                            wrapMode: Text.Wrap
                                            visible: modelData.detail.length > 0 && modelData.status !== "done"
                                            color: modelData.status === "failed" ? Theme.palette.error : Theme.palette.textTertiary
                                            text: modelData.detail
                                        }
                                    }
                                }

                                RowLayout {
                                    visible: card.job.state === "awaiting_confirmation"
                                    spacing: Theme.spacing.small
                                    LogosButton {
                                        objectName: "confirmButton"
                                        text: "Do it"
                                        variant: LogosButton.Variant.Primary
                                        enabled: root.actingOn === ""
                                        onClicked: root.act("confirm", card.job.id, "Going ahead.")
                                    }
                                    LogosButton {
                                        objectName: "cancelButton"
                                        text: "Cancel"
                                        enabled: root.actingOn === ""
                                        onClicked: root.act("cancel", card.job.id, "Cancelled.")
                                    }
                                }

                                LogosButton {
                                    visible: card.job.state === "running"
                                    text: "Stop after this step"
                                    compact: true
                                    enabled: root.actingOn === ""
                                    onClicked: root.act("cancel", card.job.id, "Stopping after the current step.")
                                }

                                LogosText {
                                    objectName: "timing"
                                    textFormat: Text.PlainText
                                    visible: card.job.timing !== undefined && card.job.timing.planMs > 0
                                    color: Theme.palette.textTertiary
                                    font.pixelSize: 11
                                    text: card.job.timing
                                          ? "planned in " + (card.job.timing.planMs / 1000).toFixed(1) + " s"
                                            + " (+" + (card.job.timing.prepareMs / 1000).toFixed(1) + " s checking)"
                                            + (card.job.timing.runMs > 0 ? ", ran in " + (card.job.timing.runMs / 1000).toFixed(1) + " s" : "")
                                            + (root.modelLine(card.job.timing.model) ? "\n" + root.modelLine(card.job.timing.model) : "")
                                          : ""
                                }

                                LogosText {
                                    objectName: "summary"
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    visible: card.job.summary.length > 0
                                    color: card.job.state === "failed" ? Theme.palette.error : Theme.palette.text
                                    text: card.job ? card.job.summary : ""
                                }
                            }
                        }
                    }
                }
            }
        }

        // The bottom bar: speak (or type), and what is happening.
        LogosText {
            objectName: "notice"
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.notice.length > 0
            color: root.noticeIsError ? Theme.palette.error : Theme.palette.textSecondary
            text: root.notice
        }
        LogosText {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.palette.textTertiary
                text: root.voice.state === "recording" ? "Listening. Press Stop when you are done."
                  : root.voice.state === "transcribing" ? "Working out what you said..."
                  : (root.voice.error || "").length > 0 ? root.voice.error
                  : root.model.mode === "rules" ? "Simple commands until set up: open <app> - install <package> - what is installed - status - search <words>"
                  : root.typing ? "Type what you want, for example: install and open the blockchain app"
                  : "Press the button and say what you want, for example: install and open the blockchain app"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.large

            // The main control: one big button. Typing is the small alternative.
            Rectangle {
                id: micButton
                objectName: "micButton"
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.spacing.medium
                implicitWidth: 96
                implicitHeight: 96
                radius: width / 2
                enabled: root.speechReady && !root.voiceBusy && root.voice.state !== "transcribing"
                readonly property bool listening: root.voice.state === "recording"
                property string text: listening ? "Stop" : (root.voice.state === "transcribing" ? "Listening..." : "Speak")
                signal clicked()
                onClicked: root.toggleRecording()

                color: listening ? Theme.palette.error
                     : !enabled ? Theme.palette.backgroundMuted
                     : micArea.containsMouse ? Theme.palette.primaryHover : Theme.palette.primary
                opacity: enabled || listening ? 1 : 0.5

                // A slow pulse while listening.
                SequentialAnimation on scale {
                    running: micButton.listening
                    loops: Animation.Infinite
                    onRunningChanged: if (!running) micButton.scale = 1
                    NumberAnimation { to: 1.06; duration: 600; easing.type: Easing.InOutQuad }
                    NumberAnimation { to: 1.0; duration: 600; easing.type: Easing.InOutQuad }
                }

                Column {
                    anchors.centerIn: parent
                    spacing: 2
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        textFormat: Text.PlainText
                        font.pixelSize: 36
                        text: micButton.listening ? "\u23F9" : "\uD83C\uDFA4"
                    }
                    LogosText {
                        anchors.horizontalCenter: parent.horizontalCenter
                        textFormat: Text.PlainText
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                        color: "white"
                        text: micButton.text
                    }
                }

                MouseArea {
                    id: micArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: micButton.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: if (micButton.enabled) micButton.clicked()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.small
                LogosButton {
                    objectName: "typeToggle"
                    Layout.alignment: Qt.AlignLeft
                    visible: root.speechReady
                    compact: true
                    text: root.typing ? "\uD83C\uDFA4 Speak instead" : "Type instead"
                    onClicked: root.typing = !root.typing
                }
                // Typing: shown on request, or when there is no speech model yet.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small
                    visible: root.typing || !root.speechReady

                    LogosTextField {
                        id: input
                        objectName: "commandInput"
                        Layout.fillWidth: true
                        placeholderText: "What should Basecamp do? For example: open eth rpc"
                        Keys.onReturnPressed: root.submit()
                        Keys.onEnterPressed: root.submit()
                    }

                    LogosButton {
                        objectName: "goButton"
                        text: root.submitting ? "Sending..." : "Go"
                        variant: LogosButton.Variant.Primary
                        enabled: !root.submitting && input.text.trim().length > 0
                        onClicked: root.submit()
                    }
                }
            }
        }
        LogosText {
            objectName: "diagnostics"
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: root.pollMisses >= 3 ? Theme.palette.error : Theme.palette.textTertiary
            font.pixelSize: 12
            text: "core " + (root.coreVersion || "?") + " - view " + root.viewVersion
                  + " - " + (root.model.mode === "local" ? "model on this computer (" + root.model.server + ")"
                             : root.model.mode === "remote" ? "model at " + (root.settings.endpoint || "?")
                             : "simple commands")
                  + (root.intents.count === undefined ? ""
                     : root.intents.count > 0 ? " - app intents: " + root.intents.count + " (" + root.intents.apps.join(", ") + ")"
                     : " - no app intents: " + root.intents.problem)
                  + (root.pollMisses >= 3 ? " - can't reach basecamp_voice_core" : "")
        }
    }
}
