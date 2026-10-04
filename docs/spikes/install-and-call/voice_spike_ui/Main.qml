import QtQuick

// Spike view. Its only jobs: being opened loads voice_spike (its dependency),
// and it performs the shell intents the core asks for, since intents are a
// view-side API. Polls on a Timer so no blocking call runs during load.
Item {
    id: root
    width: 480; height: 240
    property string lastAction: "none"

    function core(m, a) {
        if (typeof logos === "undefined" || !logos.callModule) return "";
        return String(logos.callModule("voice_spike", m, a || []));
    }
    function unwrap(r) {
        for (var i = 0; i < 2 && typeof r === "string"; i++) { try { r = JSON.parse(r) } catch (e) { return null } }
        return r;
    }

    Timer {
        interval: 1000; running: true; repeat: true
        onTriggered: {
            var a = root.unwrap(root.core("nextAction", [""]));
            if (!a || !a.intent) return;
            root.lastAction = a.intent;
            root.core("note", ["raising " + a.intent + " " + JSON.stringify(a.params)]);
            try {
                logos.request(a.intent, a.params || {}, function (res) {
                    root.core("note", ["intent " + a.intent + " replied " + JSON.stringify(res)]);
                });
            } catch (e) {
                root.core("note", ["intent " + a.intent + " threw " + e]);
            }
        }
    }

    Text { anchors.centerIn: parent; text: "voice_spike_ui - last action: " + root.lastAction }
}
