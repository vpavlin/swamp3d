import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCore
import Logos.Theme
import Logos.Controls

// Swamp - pure-QML view over swamp_core (Basecamp 0.3). Every call goes through callVia() ->
// logos.callModuleAsync. Read state: snapshot()/listModels()/getModel() polled + stateChanged.
// Contract: swamp_core_impl.h (docs/SPEC.md section 6).
Item {
    id: root
    anchors.fill: parent

    readonly property color cBg: Theme.palette.background || "#10130f"
    readonly property color cCard: Theme.palette.backgroundElevated || "#181d16"
    readonly property color cInset: Theme.palette.backgroundInset || "#0b0d0a"
    readonly property color cLine: Theme.palette.borderHairline || "#2a3326"
    readonly property color cText: Theme.palette.text || "#eef2e8"
    readonly property color cText2: Theme.palette.textSecondary || "#c2cbb8"
    readonly property color cText3: Theme.palette.textTertiary || "#8b9682"
    readonly property color cPrimary: Theme.palette.primary || "#8fbf5a"
    readonly property color cOk: Theme.palette.success || "#5fb85a"
    readonly property color cWarn: Theme.palette.warning || "#e0a03a"
    readonly property color cErr: Theme.palette.error || "#e5534b"
    readonly property int sp: Theme.spacing.medium || 12
    readonly property int spS: Theme.spacing.small || 8
    readonly property int rad: Theme.spacing.radiusSmall || 6

    property string tab: "browse"
    property var st: ({})
    property var models: []
    property var tags: []
    property int total: 0
    property string query: ""
    property string tagFilter: ""
    property string catFilter: ""
    property string modelNote: ""
    readonly property bool printBusy: !!root.st.printJob && ["downloading", "slicing", "uploading", "starting"].indexOf(root.st.printJob.stage) >= 0
    property var globalRes: []
    property var globalInfo: null
    property bool globalPending: false
    // index results you don't already see in your own categories
    readonly property var globalOthers: root.globalRes.filter(function (r) { for (var i = 0; i < root.models.length; i++) if (root.models[i].modelId === r.modelId) return false; return true })     // Browse: one category, or all of yours
    property string sortBy: "new"
    property var model: null          // the open model page
    property string openId: ""
    property int openVersion: 0       // 0 = latest
    property string toastMsg: ""
    property bool toastErr: false
    property var draftParents: []
    property string draftFor: ""      // modelId when publishing a new version
    property var draftFiles: []
    property var draftImages: []
    property var draftKeep: []        // files carried over from the previous version: {sha256, name, size}
    property var draftParentInfo: null // the version being remixed: {modelId, v, title, creatorName, licence}
    property var draftBack: null      // where Remix / New version came from: {modelId, title, tab} - Back and Cancel return there
    property int shownImage: 0        // which picture of the open version is large

    readonly property var licences: ["CC-BY-4.0", "CC-BY-SA-4.0", "CC-BY-NC-4.0", "CC-BY-NC-SA-4.0", "CC-BY-ND-4.0", "CC-BY-NC-ND-4.0", "CC0-1.0", "MIT", "GPL-3.0-or-later"]

    // ── calls ──────────────────────────────────────────────────────────────────
    function callVia(mod, method, args, cb) {
        var a = args || []
        var done = function (raw) { if (cb) { try { cb(raw === undefined || raw === null ? "" : raw) } catch (e) { console.warn(e) } } }
        if (typeof logos === "undefined" || logos === null) { Qt.callLater(function () { done("") }); return }
        if (typeof logos.callModuleAsync === "function") {
            try { logos.callModuleAsync(mod, method, a, done, 20000) } catch (e) { Qt.callLater(function () { done("") }) }
            return
        }
        Qt.callLater(function () { var r = ""; try { r = logos.callModule(mod, method, a) } catch (e) {} done(r) })
    }
    function core(method, args, cb) { root.callVia("swamp_core", method, args, cb) }
    function parse(raw) {
        var v = raw
        for (var i = 0; i < 3 && typeof v === "string"; i++) { try { v = JSON.parse(v) } catch (e) { return null } }
        return (v && typeof v === "object") ? v : null
    }
    function toast(msg, err) { root.toastMsg = msg; root.toastErr = !!err; toastTimer.interval = err ? 15000 : 5000; toastTimer.restart() }
    function act(method, args, okMsg, onOk) {
        root.core(method, args, function (raw) {
            var r = root.parse(raw)
            if (!r) { root.toast("Request failed - is swamp_core loaded?", true); return }
            if (r.ok === false) { root.toast(r.error || "Failed", true); return }
            if (okMsg) root.toast(okMsg, false)
            if (onOk) onOk(r)
            root.refresh()
        })
    }
    property bool busy: false
    property bool again: false
    function refresh() {
        if (root.busy) { root.again = true; return }
        root.busy = true
        busyGuard.restart()
        var global = root.tab === "browse" && !root.openId && !!root.query
        var pending = 2 + (root.openId ? 1 : 0) + (global ? 1 : 0)
        var fin = function () { if (--pending > 0) return; root.busy = false; if (root.again) { root.again = false; root.refresh() } }
        root.core("snapshot", [], function (raw) { var s = root.parse(raw); if (s && s.ok) root.st = s; fin() })
        root.core("listModels", [JSON.stringify({ q: root.query, tag: root.tagFilter, category: root.catFilter, sort: root.sortBy, mine: root.tab === "mine", limit: 200 })], function (raw) {
            var r = root.parse(raw); if (r && r.ok) { root.models = r.models; root.tags = r.tags; root.total = r.total } fin()
        })
        if (global) root.core("globalSearch", [JSON.stringify({ q: root.query, category: root.catFilter, limit: 60 })], function (raw) {
            var r = root.parse(raw); if (r && r.ok) { root.globalRes = r.results; root.globalInfo = r.index; root.globalPending = r.pending } fin()
        })
        if (!root.query) { root.globalRes = []; root.globalPending = false }
        if (root.openId) root.core("getModel", [root.openId], function (raw) { var r = root.parse(raw); if (r && r.ok) { root.model = r.model; root.modelNote = "" } else if (r) root.modelNote = r.error || ""; fin() })
    }
    Timer { id: busyGuard; interval: 45000; onTriggered: root.busy = false }
    Timer { interval: 2500; running: true; repeat: true; onTriggered: root.refresh() }
    Timer { id: toastTimer; interval: 5000; onTriggered: root.toastMsg = "" }
    Component.onCompleted: {
        if (typeof logos !== "undefined" && logos && logos.onModuleEvent) logos.onModuleEvent("swamp_core", "stateChanged")
        root.refresh()
    }
    Connections {
        target: typeof logos !== "undefined" ? logos : null
        ignoreUnknownSignals: true
        function onModuleEventReceived(module, event, data) { if (module === "swamp_core" && event === "stateChanged") root.refresh() }
    }
    TextEdit { id: clip; visible: false }
    function copy(t, what) { clip.text = t; clip.selectAll(); clip.copy(); root.toast((what || "Text") + " copied", false) }

    // Basecamp 0.3 sandboxes a view: it may load only files under its own plugin dir (no file://
    // elsewhere, no data: URLs). The core copies each picture into <this dir>/cache/ on request.
    readonly property string viewDir: { var u = String(Qt.resolvedUrl(".")); u = u.indexOf("file://") === 0 ? decodeURIComponent(u.slice(7)) : u; return u.replace(/\/+$/, "") }
    property var imgCache: ({})
    property int imgRev: 0
    property var imgRetry: ({})
    Timer { interval: 4000; running: true; repeat: true; onTriggered: root.imgRev++ }   // re-ask for pending pictures
    function catLabel(id) { var cs = root.st.categories || []; for (var i = 0; i < cs.length; i++) if (cs[i].id === id) return cs[i].label; return id }
    function imageUrl(p) {
        if (!p) return ""
        var sha = String(p).split("/").pop()
        var c = root.imgCache[sha]
        if (c) return c
        // pictures are fetched when they're looked at (ADR 0014): ask, then ask again a bit later
        var now = Date.now()
        if (c !== undefined && now < (root.imgRetry[sha] || 0)) return ""
        root.imgCache[sha] = ""; root.imgRetry[sha] = now + 4000
        root.core("cacheImage", [sha, root.viewDir], function (raw) {
            var r = root.parse(raw)
            if (r && r.ok && r.path) { root.imgCache[sha] = "file://" + r.path; root.imgRev++ }
        })
        return ""
    }
    function plural(n, one, many) { return n + " " + (n === 1 ? one : many) }
    function size(n) { return n > 1048576 ? (n / 1048576).toFixed(1) + " MB" : n > 1024 ? Math.round(n / 1024) + " KB" : n + " B" }
    function day(ms) { return ms ? new Date(ms).toISOString().slice(0, 10) : "" }
    function ver() { if (!root.model) return null; var vs = root.model.versions; var i = root.openVersion > 0 ? root.openVersion - 1 : vs.length - 1; return vs[Math.min(i, vs.length - 1)] }
    function shareLink() { return root.model ? "swamp://model/" + root.model.modelId + "?c=" + encodeURIComponent(root.model.category || "other") : "" }
    function shareText() { return root.model ? root.model.title + " by " + (root.model.creatorName || "someone") + " on Swamp: " + root.shareLink() : "" }
    // a pasted share link (or a whole shared message containing one) opens the model instead of searching
    function openOrSearch(text) {
        if (text.indexOf("swamp://model/") >= 0) {
            root.core("openLink", [text], function (raw) {
                var r = root.parse(raw)
                if (!r || r.ok === false) { root.toast(r ? r.error : "Request failed - is swamp_core loaded?", true); return }
                root.query = ""; root.openModel(r.modelId)
            })
            return
        }
        root.query = text; root.refresh()
    }
    function openModel(id) { root.openId = id; root.openVersion = 0; root.shownImage = 0; root.model = null; root.refresh() }
    function presetCategory(id) { var cs = root.st.categories || []; for (var i = 0; i < cs.length; i++) if (cs[i].id === id) { pCategory.currentIndex = i; return } }
    function clearDraft() {
        root.draftFor = ""; root.draftParents = []; root.draftParentInfo = null; root.draftFiles = []; root.draftImages = []; root.draftKeep = []
        pTitle.text = ""; pSummary.text = ""; pDesc.text = ""; pTags.text = ""; pLicence.currentIndex = 0
    }
    function startRemix() {
        var v = root.ver(); if (!v) return
        var info = { modelId: root.model.modelId, v: v.v, title: root.model.title, creatorName: root.model.creatorName, licence: v.licence }
        root.clearDraft()
        root.draftParents = [{ modelId: info.modelId, v: info.v }]; root.draftParentInfo = info
        root.presetCategory(root.model.category)
        pTitle.text = "Remix of " + info.title; pLicence.currentIndex = Math.max(0, root.licences.indexOf(v.licence))
        root.draftBack = { modelId: info.modelId, title: info.title, tab: root.tab }
        root.openId = ""; root.model = null; root.tab = "publish"
    }
    // leave a remix / new-version draft and go back to the model it started from
    function leaveDraft() {
        var back = root.draftBack
        root.clearDraft(); root.draftBack = null
        root.tab = back && back.tab !== "publish" ? back.tab : "browse"
        if (back) root.openModel(back.modelId)
    }
    function startNewVersion() {
        var v = root.ver(); if (!v) return
        var mid = root.model.modelId
        root.clearDraft()
        root.draftFor = mid
        root.presetCategory(root.model.category)
        root.draftParents = (v.parents || []).map(function (p) { return { modelId: p.modelId, v: p.v } })   // still a remix of the same original
        pTitle.text = v.title; pSummary.text = v.summary || ""; pDesc.text = v.description || ""; pTags.text = (v.tags || []).join(", ")
        pLicence.currentIndex = Math.max(0, root.licences.indexOf(v.licence))
        // keep the files you already published; remove the ones you're replacing
        root.draftKeep = (v.files || []).filter(function (f) { return !!f.local }).map(function (f) { return { sha256: f.sha256, name: f.name, size: f.size } })
        root.draftBack = { modelId: mid, title: root.model.title, tab: root.tab }
        root.openId = ""; root.model = null; root.tab = "publish"
    }
    // ND forbids derivatives; SA wants the same licence on the remix
    function licenceNote(parentLic, mine) {
        if (!parentLic) return ""
        if (parentLic.indexOf("-ND") >= 0) return "The original is " + parentLic + ": its author doesn't allow derivatives. Ask them first, or don't publish this remix."
        if (parentLic.indexOf("-SA") >= 0 && mine !== parentLic) return "The original is " + parentLic + ": a remix has to use the same licence."
        if (parentLic.indexOf("-NC") >= 0 && mine.indexOf("-NC") < 0) return "The original is " + parentLic + ": a remix has to stay non-commercial."
        return ""
    }
    function pathOf(u) { var s = String(u); return s.indexOf("file://") === 0 ? decodeURIComponent(s.slice(7)) : s }

    // ── pieces ─────────────────────────────────────────────────────────────────
    component T1: Text { textFormat: Text.PlainText; color: root.cText; font.pixelSize: 16; font.weight: Font.DemiBold; wrapMode: Text.Wrap }
    component T2: Text { textFormat: Text.PlainText; color: root.cText2; font.pixelSize: 13; wrapMode: Text.Wrap }
    component T3: Text { textFormat: Text.PlainText; color: root.cText3; font.pixelSize: 11; wrapMode: Text.Wrap }
    component Card: Rectangle {
        default property alias content: inner.data
        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 2 * root.sp
        color: root.cCard; radius: root.rad + 4; border.color: root.cLine
        ColumnLayout { id: inner; anchors.fill: parent; anchors.margins: root.sp; spacing: root.spS }
    }
    // a command to run: shown as-is, selectable, with a Copy button - nobody retypes a command
    component CommandBox: Rectangle {
        property string cmd: ""
        Layout.fillWidth: true; visible: cmd !== ""
        implicitHeight: Math.max(cmdT.implicitHeight, cmdCopy.implicitHeight) + 12
        color: root.cInset; radius: root.rad; border.color: root.cLine
        TextEdit { id: cmdT; readOnly: true; selectByMouse: true; textFormat: TextEdit.PlainText; wrapMode: TextEdit.WrapAnywhere
            anchors.left: parent.left; anchors.right: cmdCopy.left; anchors.verticalCenter: parent.verticalCenter; anchors.margins: 8
            text: parent.cmd; color: root.cText; font.family: "monospace"; font.pixelSize: 12 }
        LogosButton { id: cmdCopy; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.rightMargin: 4
            text: "Copy"; onClicked: root.copy(parent.cmd, "Command") }
    }
    // an error you can copy (to search for it, or to send it to someone)
    component ErrorLine: RowLayout {
        property string msg: ""
        Layout.fillWidth: true; visible: msg !== ""; spacing: root.spS
        TextEdit { Layout.fillWidth: true; readOnly: true; selectByMouse: true; textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap
            text: parent.msg; color: root.cErr; font.pixelSize: 13 }
        LogosButton { text: "Copy"; onClicked: root.copy(parent.msg, "Error") }
    }
    // "here's what's wrong and what to do": {title, text, steps:[{text, command}], action}
    component FixPanel: ColumnLayout {
        property var fix: null
        property var retry: null   // function: try the thing again once it's fixed
        readonly property var inst: root.st.slicerInstall || null
        readonly property string ist: inst ? (inst.stage || "") : ""
        Layout.fillWidth: true; visible: !!fix; spacing: root.spS
        T1 { Layout.fillWidth: true; text: parent.fix ? parent.fix.title : ""; color: root.cWarn }
        T2 { Layout.fillWidth: true; text: parent.fix ? parent.fix.text : ""; color: root.cText }
        RowLayout { spacing: root.spS; visible: !!parent.fix && parent.fix.action === "installSlicer"
            LogosButton { text: "Set up OrcaSlicer 2.4.2 for me"; enabled: ["downloading", "unpacking", "checking"].indexOf(parent.parent.ist) < 0 && parent.parent.ist !== "done"
                onClicked: root.act("installSlicer", [], "") }
            LogosButton { visible: parent.parent.ist === "done" && !!parent.parent.retry; text: "Try again"; onClicked: parent.parent.retry() }
        }
        ProgressBar { Layout.fillWidth: true; visible: parent.ist === "downloading"; from: 0; to: parent.inst && parent.inst.total ? parent.inst.total : 1; value: parent.inst ? (parent.inst.bytes || 0) : 0 }
        T2 { Layout.fillWidth: true; visible: !!parent.inst && parent.ist !== ""; color: parent.ist === "failed" ? root.cErr : (parent.ist === "done" ? root.cOk : root.cText2)
            text: parent.inst ? (parent.inst.message || "") + (parent.ist === "downloading" && parent.inst.total ? "  " + Math.round(100 * (parent.inst.bytes || 0) / parent.inst.total) + " %" : "") : "" }
        Repeater { model: parent.fix && parent.fix.steps ? parent.fix.steps : []
            delegate: ColumnLayout { Layout.fillWidth: true; spacing: 4
                T2 { Layout.fillWidth: true; text: modelData.text || "" }
                CommandBox { cmd: modelData.command || "" } } }
        Repeater { model: parent.inst && parent.inst.fix && parent.inst.fix.steps ? parent.inst.fix.steps : []   // after setting up: what's still missing
            delegate: ColumnLayout { Layout.fillWidth: true; spacing: 4
                T2 { Layout.fillWidth: true; text: modelData.text || "" }
                CommandBox { cmd: modelData.command || "" } } }
    }
    component Field: TextField {
        // the core's value for this field; it fills the field only while you're not editing it
        // (binding `text` to the polled state would overwrite what you type on every refresh)
        property string saved: ""
        property bool edited: false
        onSavedChanged: if (!activeFocus && !edited) text = saved
        onTextEdited: edited = true
        Component.onCompleted: if (saved !== "") text = saved
        Layout.fillWidth: true; Layout.minimumWidth: 80; Layout.preferredWidth: 200
        color: root.cText; placeholderTextColor: root.cText3; font.pixelSize: 13
        background: Rectangle { color: root.cInset; radius: root.rad; border.color: parent.activeFocus ? root.cPrimary : root.cLine }
    }
    component Area: TextArea {
        Layout.fillWidth: true; Layout.preferredHeight: 90
        color: root.cText; placeholderTextColor: root.cText3; font.pixelSize: 13; wrapMode: TextEdit.Wrap
        background: Rectangle { color: root.cInset; radius: root.rad; border.color: parent.activeFocus ? root.cPrimary : root.cLine }
    }
    component ModelCard: Rectangle {
        property var m: ({})
        Layout.fillWidth: true; Layout.maximumWidth: 420; Layout.preferredHeight: 300
        color: root.cCard; radius: root.rad + 4; border.color: hov.containsMouse ? root.cPrimary : root.cLine
        ColumnLayout {
            anchors.fill: parent; anchors.margins: root.spS; spacing: 4
            Thumb { Layout.fillWidth: true; Layout.preferredHeight: 190; path: m.thumb || m.thumbSha || "" }
            T1 { Layout.fillWidth: true; text: m.title || ""; font.pixelSize: 14; elide: Text.ElideRight; maximumLineCount: 2 }
            T3 { Layout.fillWidth: true; text: "by " + m.creatorName + "  ·  " + root.catLabel(m.category) + "  ·  " + m.licence + (m.remix ? "  ·  remix" : "") + (m.retracted ? "  ·  retracted" : "") }
            T3 { Layout.fillWidth: true; text: root.plural(m.likes || 0, "like", "likes") + "  ·  " + root.plural(m.makes || 0, "make", "makes") + "  ·  v" + m.latest }
        }
        MouseArea { id: hov; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.openModel(m.modelId) }
    }
    component Thumb: Rectangle {
        property string path: ""
        color: root.cInset; radius: root.rad; clip: true
        Image { id: thumbImg; anchors.fill: parent; anchors.margins: 4; source: { root.imgRev; return root.imageUrl(parent.path) } fillMode: Image.PreserveAspectFit; asynchronous: true; visible: status === Image.Ready }
        T3 { anchors.centerIn: parent; visible: !thumbImg.visible; text: parent.path ? "loading picture..." : "no preview yet" }
    }

    FileDialog { id: fileDlg; fileMode: FileDialog.OpenFiles; title: "Model files (STL, 3MF, STEP, ...)"; currentFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)
        nameFilters: ["3D models and sources (*.stl *.3mf *.obj *.step *.stp *.scad *.f3d)", "All files (*)"]
        onAccepted: { var a = root.draftFiles.slice(); for (var i = 0; i < selectedFiles.length; i++) a.push(root.pathOf(selectedFiles[i])); root.draftFiles = a } }
    FileDialog { id: imgDlg; fileMode: FileDialog.OpenFiles; title: "Photos (PNG, JPEG)"; currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation); nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)"]
        onAccepted: { var a = root.draftImages.slice(); for (var i = 0; i < selectedFiles.length; i++) a.push(root.pathOf(selectedFiles[i])); root.draftImages = a } }
    FileDialog { id: makeDlg; fileMode: FileDialog.OpenFile; title: "A photo of your print"; currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation); nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)"]
        onAccepted: makePhoto.text = root.pathOf(selectedFile) }

    Rectangle { anchors.fill: parent; color: root.cBg }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.sp + 4
        spacing: root.sp

        RowLayout {
            Layout.fillWidth: true
            spacing: root.sp
            ColumnLayout {
                spacing: 0
                Text { textFormat: Text.PlainText; text: "Swamp"; color: root.cText; font.pixelSize: 22; font.weight: Font.Bold }
                T3 { text: "3D models nobody can take down - built on the Logos tech stack, not affiliated with Logos" }
            }
            Item { Layout.fillWidth: true }
            Repeater {
                model: [["browse", "Browse"], ["publish", "Publish"], ["mine", "My models"], ["me", "Me"]]
                delegate: LogosButton { Layout.preferredWidth: 110; text: (root.tab === modelData[0] && !root.openId ? "> " : "") + modelData[1]
                    onClicked: { root.openId = ""; root.model = null; root.tab = modelData[0]; root.refresh() } }
            }
        }

        ScrollView {
            id: scroller
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; contentWidth: availableWidth

            ColumnLayout {
                width: scroller.availableWidth
                spacing: root.sp

                // ════ BROWSE / MINE ════
                ColumnLayout {
                    visible: (root.tab === "browse" || root.tab === "mine") && !root.openId
                    Layout.fillWidth: true
                    spacing: root.sp
                    RowLayout {
                        Layout.fillWidth: true
                        Field { id: search; placeholderText: "Search models, tags... or paste a swamp:// link"; onAccepted: root.openOrSearch(text) }
                        ComboBox { id: sortBox; model: ["Newest", "Most liked", "Most made"]; Layout.preferredWidth: 160
                            onActivated: { root.sortBy = ["new", "likes", "makes"][currentIndex]; root.refresh() } }
                        LogosButton { text: "Search"; onClicked: root.openOrSearch(search.text) }
                    }
                    Flow {
                        Layout.fillWidth: true; spacing: 6
                        visible: root.tab === "browse"
                        Repeater {
                            model: [{ id: "", label: "All my categories" }].concat((root.st.categories || []).filter(function (c) { return c.subscribed && (c.models > 0 || c.id === root.catFilter) }))
                            delegate: Rectangle {
                                width: ct.implicitWidth + 20; height: 28; radius: 14
                                color: root.catFilter === modelData.id ? root.cPrimary : root.cInset; border.color: root.cLine
                                Text { id: ct; textFormat: Text.PlainText; anchors.centerIn: parent; text: modelData.label + (modelData.models !== undefined ? "  " + modelData.models : ""); color: root.catFilter === modelData.id ? root.cBg : root.cText; font.pixelSize: 12 }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { root.catFilter = modelData.id; root.refresh() } }
                            }
                        }
                        Text { textFormat: Text.PlainText; text: "choose categories..."; color: root.cPrimary; font.pixelSize: 12; font.underline: true; topPadding: 6
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { root.tab = "me"; root.refresh() } } }
                    }
                    Flow {
                        Layout.fillWidth: true; spacing: 6
                        visible: root.tags.length > 0
                        Repeater {
                            model: root.tags
                            delegate: Rectangle {
                                width: tt.implicitWidth + 18; height: 26; radius: 13
                                color: root.tagFilter === modelData.tag ? root.cPrimary : root.cCard; border.color: root.cLine
                                Text { id: tt; textFormat: Text.PlainText; anchors.centerIn: parent; text: modelData.tag + " " + modelData.count; color: root.tagFilter === modelData.tag ? root.cBg : root.cText2; font.pixelSize: 12 }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { root.tagFilter = root.tagFilter === modelData.tag ? "" : modelData.tag; root.refresh() } }
                            }
                        }
                    }
                    T3 { text: root.total + (root.total === 1 ? " model" : " models") + (root.tab === "mine" ? " published by you" : " in your catalogue") + (root.tagFilter ? " tagged " + root.tagFilter : "") }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: Math.max(1, Math.floor(scroller.availableWidth / 250))
                        columnSpacing: root.sp; rowSpacing: root.sp
                        Repeater {
                            model: root.models
                            delegate: ModelCard { m: modelData }
                        }
                    }
                    T2 { visible: root.models.length === 0 && !root.query; text: root.tab === "mine" ? "You haven't published anything yet. Publish your first model." : "Nothing here yet. The catalogue fills in as it syncs from other people, or publish the first model." }
                    T2 { visible: root.models.length === 0 && !!root.query && root.tab === "browse"; text: "Nothing in the categories you follow." }
                    // everything else, from the newest index snapshot (ADR 0015)
                    ColumnLayout {
                        visible: root.tab === "browse" && !!root.query
                        Layout.fillWidth: true; spacing: root.spS
                        Rectangle { Layout.fillWidth: true; height: 1; color: root.cLine }
                        T1 { text: "Everywhere" }
                        T3 { Layout.fillWidth: true
                            text: !root.globalInfo ? "No index yet - an indexing hub publishes one every half hour or so." :
                                  "From the index by " + root.globalInfo.indexerName + ", " + Math.round(root.globalInfo.ageMs / 60000) + " min old, " +
                                  root.plural(root.globalInfo.models, "model", "models") + "  ·  " + root.globalInfo.agreeing + " of " + root.plural(root.globalInfo.indexers, "indexer", "indexers") + " agree" +
                                  (root.globalInfo.private ? (root.globalInfo.privacyDowngrades ? "  ·  " + root.globalInfo.privacyDowngrades + " fetches fell back to non-private" : "  ·  fetched privately") : "  ·  not private yet: the storage network's mixnet isn't working, so the hub serving the index can see which piece you fetched") }
                        T2 { visible: root.globalPending; text: "Fetching the part of the index this search needs..." }
                        T2 { visible: !root.globalPending && !!root.globalInfo && root.globalOthers.length === 0; text: "No other matches." }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: Math.max(1, Math.floor(scroller.availableWidth / 250))
                            columnSpacing: root.sp; rowSpacing: root.sp
                            Repeater { model: root.globalOthers; delegate: ModelCard { m: modelData } }
                        }
                    }
                }

                // ════ MODEL PAGE ════
                ColumnLayout {
                    visible: !!root.openId
                    Layout.fillWidth: true
                    spacing: root.sp
                    RowLayout {
                        Layout.fillWidth: true
                        LogosButton { text: "< Back"; onClicked: { root.openId = ""; root.model = null; root.refresh() } }
                        Item { Layout.fillWidth: true }
                        T3 { visible: !root.model; text: root.modelNote || "loading..." }
                    }
                    Card {
                        visible: !!root.model
                        RowLayout {
                            Layout.fillWidth: true; spacing: root.sp
                            ColumnLayout { Layout.alignment: Qt.AlignTop; spacing: root.spS
                                Thumb { Layout.preferredWidth: 300; Layout.preferredHeight: 300
                                    path: { var v = root.ver(); if (!v || !v.images || !v.images.length) return ""; var im = v.images[Math.min(root.shownImage, v.images.length - 1)]; return im.local || im.sha256 || "" } }
                                // every picture of this version: the thumbnail plus the creator's photos
                                Flow { Layout.preferredWidth: 300; spacing: 6; visible: !!root.ver() && (root.ver().images || []).length > 1
                                    Repeater { model: root.ver() ? (root.ver().images || []) : []
                                        delegate: Thumb { width: 66; height: 66; path: modelData.local || modelData.sha256 || ""
                                            border.color: index === root.shownImage ? root.cPrimary : root.cLine
                                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.shownImage = index } } } }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; spacing: 6
                                T1 { Layout.fillWidth: true; font.pixelSize: 20; text: root.model ? root.model.title : "" }
                                T2 { Layout.fillWidth: true; text: root.model ? ("by " + root.model.creatorName + "  ·  " + root.catLabel(root.model.category) + "  ·  " + (root.ver() ? root.ver().licence : "") + "  ·  published " + root.day(root.ver() ? root.ver().published : 0)) : "" }
                                T2 { Layout.fillWidth: true; visible: !!root.ver() && !!root.ver().summary; text: root.ver() ? (root.ver().summary || "") : "" }
                                Flow { Layout.fillWidth: true; spacing: 4; visible: !!root.ver() && (root.ver().parents || []).length > 0
                                    T3 { text: "Remix of" }
                                    Repeater { model: root.ver() ? (root.ver().parents || []) : []
                                        delegate: Text { textFormat: Text.PlainText; color: root.cPrimary; font.pixelSize: 11; font.underline: true
                                            text: (modelData.title || modelData.modelId.slice(0, 8)) + " v" + modelData.v + (modelData.creatorName ? " by " + modelData.creatorName : "")
                                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.openModel(modelData.modelId) } } } }
                                T3 { Layout.fillWidth: true; visible: !!root.model && root.model.retracted; color: root.cWarn; text: "The creator retracted this model" + (root.model && root.model.retractReason ? ": " + root.model.retractReason : "") }
                                RowLayout {
                                    T3 { text: "Version" }
                                    ComboBox { id: verBox; Layout.preferredWidth: 120
                                        model: root.model ? root.model.versions.map(function (v) { return "v" + v.v }) : []
                                        currentIndex: root.model ? (root.openVersion > 0 ? root.openVersion - 1 : root.model.versions.length - 1) : 0
                                        onActivated: root.openVersion = currentIndex + 1 }
                                }
                                Flow { Layout.fillWidth: true; spacing: 6
                                    Repeater { model: root.ver() ? (root.ver().tags || []) : []
                                        delegate: Rectangle { width: tg.implicitWidth + 14; height: 22; radius: 11; color: root.cInset; border.color: root.cLine
                                            Text { id: tg; textFormat: Text.PlainText; anchors.centerIn: parent; text: modelData; color: root.cText2; font.pixelSize: 11 } } } }
                                // fixed slots: a button never moves because another one appeared
                                RowLayout {
                                    spacing: root.spS
                                    LogosButton { Layout.preferredWidth: 130; text: { var d = root.ver() && root.ver().download; return d ? (d.status === "done" ? "Download again" : d.status === "failed" ? "Retry download" : "Downloading...") : "Download" }
                                        onClicked: root.act("download", [root.model.modelId, String(root.ver().v)], "Downloading into your Swamp folder") }
                                    LogosButton { Layout.preferredWidth: 110; enabled: !!(root.ver() && root.ver().download && root.ver().download.status === "done"); text: "Open folder"
                                        onClicked: Qt.openUrlExternally(root.fileUrl(root.ver().download.dir)) }
                                    // hand the verified model files to the slicer you already use; print from there
                                    LogosButton { Layout.preferredWidth: 170
                                        enabled: !!root.st.slicer
                                        text: root.st.slicer ? "Open in " + root.st.slicer.name.replace(" (Flatpak)", "").replace(" (AppImage)", "") : "Open in slicer"
                                        ToolTip.visible: hovered; ToolTip.text: root.st.slicer ? "Downloads and verifies the files if needed, then opens them in " + root.st.slicer.name : "Install OrcaSlicer, Bambu Studio or PrusaSlicer to open models in it from here"
                                        onClicked: root.act("openInSlicer", [root.model.modelId, String(root.ver().v)], "Opening in " + (root.st.slicer ? root.st.slicer.name : "your slicer") + "...") }
                                    LogosButton { Layout.preferredWidth: 150
                                        visible: !!root.st.printer && !!root.st.experimentalPrint
                                        enabled: !!root.st.printer && root.st.printer.supported && !root.printBusy
                                        text: root.st.printer ? "Print on " + root.st.printer.name.replace("Bambu Lab ", "") : "Print"
                                        ToolTip.visible: hovered; ToolTip.text: "Slices with the printer's default profile (0.20 mm, PLA) and sends it over your LAN after you confirm"
                                        onClicked: root.act("preparePrint", [root.model.modelId, String(root.ver().v)], "") }
                                    LogosButton { Layout.preferredWidth: 110; text: root.model && root.model.likedByMe ? "Unlike (" + root.model.likes + ")" : "Like (" + (root.model ? root.model.likes : 0) + ")"
                                        onClicked: root.act("like", [root.model.modelId, root.model.likedByMe ? "false" : "true"], "") }
                                    LogosButton { Layout.preferredWidth: 100; text: "Remix"; onClicked: root.startRemix() }
                                    LogosButton { Layout.preferredWidth: 100; text: "Share"
                                        ToolTip.visible: hovered; ToolTip.text: "Copies a link. Anyone with Swamp can paste it into the search box to open this model."
                                        onClicked: root.copy(root.shareText(), "Link") }
                                    LogosButton { Layout.preferredWidth: 120; visible: !!root.model && root.model.mine; text: "New version"; onClicked: root.startNewVersion() }
                                    LogosButton { Layout.preferredWidth: 100; visible: !!root.model && root.model.mine && !root.model.retracted; text: "Retract"; onClicked: retractDlg.open() }
                                }
                                ErrorLine { msg: root.ver() && root.ver().download ? (root.ver().download.error || "") : "" }
                            }
                        }
                    }
                    Card {
                        visible: !!root.model && !!root.st.printJob && root.st.printJob.modelId === root.model.modelId
                        T1 { text: "Print on " + (root.st.printJob ? root.st.printJob.printer : "") }
                        T2 { Layout.fillWidth: true; visible: !(root.st.printJob && root.st.printJob.stage === "failed"); text: root.st.printJob ? root.st.printJob.message : ""
                             color: root.st.printJob && root.st.printJob.stage === "sent" ? root.cOk : root.cText2 }
                        ErrorLine { msg: root.st.printJob && root.st.printJob.stage === "failed" ? (root.st.printJob.message || "") : "" }
                        FixPanel { fix: root.st.printJob && root.st.printJob.stage === "failed" ? (root.st.printJob.fix || null) : null
                            retry: function () { root.act("preparePrint", [root.model.modelId, String(root.ver().v)], "") } }
                        LogosButton { visible: !!root.st.printJob && root.st.printJob.stage === "failed" && !!root.st.printJob.log; text: "Copy the slicer's output"
                            onClicked: root.copy(root.st.printJob.log, "Slicer output") }
                        T2 { Layout.fillWidth: true; visible: !!root.st.printJob && !!root.st.printJob.estimate
                             text: root.st.printJob && root.st.printJob.estimate ? "About " + root.st.printJob.estimate.time + "  ·  " + (root.st.printJob.estimate.filamentG || "?") + " g of PLA (" + (root.st.printJob.estimate.filamentM || "?") + " m)  ·  " + (root.st.printJob.estimate.layers || "?") + " layers  ·  " + root.st.printJob.profile : "" }
                        ProgressBar { Layout.fillWidth: true; visible: !!root.st.printJob && root.st.printJob.stage === "uploading"; from: 0; to: 100; value: root.st.printJob ? (root.st.printJob.progress || 0) : 0 }
                        RowLayout { spacing: root.spS
                            LogosButton { visible: !!root.st.printJob && root.st.printJob.stage === "ready"; text: "Start print"; onClicked: printConfirm.open() }
                            LogosButton { visible: !!root.st.printJob && ["ready", "failed", "sent", "downloading", "slicing"].indexOf(root.st.printJob.stage) >= 0
                                text: root.st.printJob && (root.st.printJob.stage === "sent" || root.st.printJob.stage === "failed") ? "Close" : "Cancel"
                                onClicked: root.act("cancelPrint", [], "") }
                        }
                    }
                    Card {
                        visible: !!root.ver() && !!root.ver().description
                        T1 { text: "About" }
                        T2 { Layout.fillWidth: true; text: root.ver() ? (root.ver().description || "") : "" }
                    }
                    Card {
                        visible: !!root.ver()
                        T1 { text: "Files" }
                        Repeater {
                            model: root.ver() ? root.ver().files : []
                            delegate: RowLayout {
                                Layout.fillWidth: true
                                T2 { Layout.fillWidth: true; text: modelData.name + "  (" + root.size(modelData.size) + ", " + modelData.kind + ")" }
                                T3 { color: modelData.local ? root.cOk : (modelData.cids > 0 ? root.cText3 : root.cWarn)
                                    text: modelData.local ? "on this device" : modelData.fetching ? "fetching..." : modelData.cids > 0 ? "available" : "not uploaded yet" }
                            }
                        }
                        T3 { Layout.fillWidth: true; text: "Every file is checked against its SHA-256 before it's saved. Open the folder and drop the STL/3MF into your slicer." }
                    }
                    Card {
                        visible: !!root.model
                        T1 { text: "Makes (" + (root.model ? root.model.makesList.length : 0) + ")" }
                        Repeater {
                            model: root.model ? root.model.makesList : []
                            delegate: RowLayout {
                                Layout.fillWidth: true; spacing: root.sp
                                Thumb { Layout.preferredWidth: 90; Layout.preferredHeight: 90; path: modelData.images && modelData.images.length ? (modelData.images[0].local || modelData.images[0].sha256 || "") : "" }
                                ColumnLayout { Layout.fillWidth: true
                                    T2 { Layout.fillWidth: true; text: modelData.text || "(photo)" }
                                    T3 { text: modelData.authorName + "  ·  " + root.day(modelData.at) } }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Field { id: makeText; placeholderText: "Printed it? Settings, material, how it went" }
                            Field { id: makePhoto; placeholderText: "photo path"; Layout.preferredWidth: 160 }
                            LogosButton { text: "Photo..."; onClicked: makeDlg.open() }
                            LogosButton { text: "Post make"
                                onClicked: root.act("postMake", [root.model.modelId, JSON.stringify({ text: makeText.text, v: root.ver().v, images: makePhoto.text ? [makePhoto.text] : [] })], "Make posted",
                                                    function () { makeText.text = ""; makePhoto.text = "" }) }
                        }
                    }
                    Card {
                        visible: !!root.model && root.model.remixes.length > 0
                        T1 { text: "Remixes" }
                        Repeater { model: root.model ? root.model.remixes : []
                            delegate: T2 { text: modelData.title + "  by " + modelData.creatorName
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.openModel(modelData.modelId) } } }
                    }
                    Card {
                        visible: !!root.model
                        T1 { text: "Comments (" + (root.model ? root.model.comments.length : 0) + ")" }
                        Repeater { model: root.model ? root.model.comments : []
                            delegate: ColumnLayout { Layout.fillWidth: true; spacing: 0
                                T2 { Layout.fillWidth: true; text: modelData.text }
                                T3 { text: modelData.authorName + "  ·  " + root.day(modelData.at) } } }
                        RowLayout { Layout.fillWidth: true
                            Field { id: commentText; placeholderText: "Say something nice (or useful)" }
                            LogosButton { text: "Comment"; onClicked: root.act("comment", [root.model.modelId, commentText.text], "", function () { commentText.text = "" }) } }
                    }
                }

                // ════ PUBLISH ════
                Card {
                    visible: root.tab === "publish" && !root.openId
                    LogosButton { visible: !!root.draftBack; text: "< Back to " + (root.draftBack ? root.draftBack.title : ""); onClicked: root.leaveDraft() }
                    T1 { text: root.draftFor ? "Publish a new version" : (root.draftParents.length ? "Publish a remix" : "Publish a model") }
                    T2 { Layout.fillWidth: true; visible: !!root.draftParentInfo; color: root.cText
                        text: root.draftParentInfo ? "Remixing " + root.draftParentInfo.title + " v" + root.draftParentInfo.v + " by " + root.draftParentInfo.creatorName + " (" + root.draftParentInfo.licence + "). The original is credited and linked." : "" }
                    T2 { Layout.fillWidth: true; color: root.cWarn; visible: text !== ""
                        text: root.draftParentInfo ? root.licenceNote(root.draftParentInfo.licence, pLicence.currentText) : "" }
                    T2 { Layout.fillWidth: true; text: "Your files stay on this device and are shared through Logos Storage. A thumbnail and a shape fingerprint are made from the first STL. Published versions can't be edited - publish a new one instead." }
                    GridLayout {
                        columns: 2; columnSpacing: root.sp; rowSpacing: root.spS; Layout.fillWidth: true
                        T3 { text: "Title" }
                        Field { id: pTitle; placeholderText: "What is it?" }
                        T3 { text: "Summary" }
                        Field { id: pSummary; placeholderText: "One line (optional)" }
                        T3 { text: "Description" }
                        Area { id: pDesc; placeholderText: "Print settings, assembly, what it's for..." }
                        T3 { text: "Tags" }
                        Field { id: pTags; placeholderText: "comma separated, e.g. tool, organizer, gridfinity" }
                        T3 { text: "Category" }
                        ComboBox { id: pCategory; Layout.fillWidth: true; enabled: !root.draftFor
                            // a lone & is a keyboard-mnemonic marker in the list items, but not in the closed box
                            model: (root.st.categories || []).map(function (c) { return c.label.replace(/&/g, "&&") })
                            displayText: currentText.replace(/&&/g, "&")
                            ToolTip.visible: hovered && !enabled; ToolTip.text: "A model keeps the category it was created in" }
                        T3 { text: "Licence" }
                        ComboBox { id: pLicence; model: root.licences; Layout.fillWidth: true }
                        T3 { text: "Files" }
                        ColumnLayout { Layout.fillWidth: true
                            Repeater { model: root.draftKeep
                                delegate: RowLayout { T2 { text: modelData.name + "  (" + root.size(modelData.size) + ", from the previous version)" }
                                    Text { textFormat: Text.PlainText; text: "remove"; color: root.cPrimary; font.pixelSize: 11; font.underline: true
                                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { var a = root.draftKeep.slice(); a.splice(index, 1); root.draftKeep = a } } } } }
                            Repeater { model: root.draftFiles; delegate: T2 { text: modelData.split("/").pop() + "   " + modelData } }
                            RowLayout { LogosButton { text: "Add files..."; onClicked: fileDlg.open() }
                                        LogosButton { visible: root.draftFiles.length > 0; text: "Clear"; onClicked: root.draftFiles = [] } }
                            Field { id: pPath; placeholderText: "...or paste a file path and press Enter"; onAccepted: { if (text) { var a = root.draftFiles.slice(); a.push(text); root.draftFiles = a; text = "" } } }
                        }
                        T3 { text: "Photos" }
                        ColumnLayout { Layout.fillWidth: true
                            Repeater { model: root.draftImages; delegate: T2 { text: modelData } }
                            RowLayout { LogosButton { text: "Add photos..."; onClicked: imgDlg.open() }
                                        LogosButton { visible: root.draftImages.length > 0; text: "Clear"; onClicked: root.draftImages = [] } }
                        }
                    }
                    RowLayout {
                        LogosButton {
                            text: "Publish"
                            onClicked: {
                                var d = { title: pTitle.text, summary: pSummary.text, description: pDesc.text, licence: pLicence.currentText, category: ((root.st.categories || [])[pCategory.currentIndex] || { id: "other" }).id,
                                          tags: pTags.text.split(",").map(function (t) { return t.trim() }).filter(function (t) { return t.length > 0 }),
                                          parents: root.draftParents, files: root.draftKeep.map(function (f) { return { sha256: f.sha256, name: f.name } }).concat(root.draftFiles.map(function (p) { return { path: p } })),
                                          images: root.draftImages.map(function (p) { return { path: p } }) }
                                if (root.draftFor) d.modelId = root.draftFor
                                root.act("publish", [JSON.stringify(d)], "Published - files are uploading in the background", function (r) {
                                    root.clearDraft()
                                    pTitle.text = ""; pSummary.text = ""; pDesc.text = ""; pTags.text = ""
                                    root.draftBack = null; root.tab = "mine"; root.openModel(r.modelId)
                                })
                            }
                        }
                        LogosButton { visible: !!root.draftFor || root.draftParents.length > 0; text: "Cancel"; onClicked: root.leaveDraft() }
                    }
                }

                // ════ ME ════
                ColumnLayout {
                    visible: root.tab === "me" && !root.openId
                    Layout.fillWidth: true
                    spacing: root.sp
                    Card {
                        T1 { text: "Your profile" }
                        T2 { Layout.fillWidth: true; text: "Shown next to your models and comments. It's a name, not an account - your identity is a key on this device." }
                        RowLayout { Layout.fillWidth: true
                            Field { id: profName; objectName: "profName"; placeholderText: "display name"; saved: root.st.me && root.st.me.profile ? (root.st.me.profile.name || "") : "" }
                            Field { id: profBio; placeholderText: "a line about you (optional)"; saved: root.st.me && root.st.me.profile ? (root.st.me.profile.bio || "") : "" }
                            LogosButton { text: "Save"; onClicked: root.act("setProfile", [JSON.stringify({ name: profName.text, bio: profBio.text })], "Profile saved", function () { profName.edited = false; profBio.edited = false }) } }
                        RowLayout { Layout.fillWidth: true
                            T3 { text: "Your key:" }
                            Text { textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideMiddle; color: root.cText2; font.family: "monospace"; font.pixelSize: 11; text: root.st.me ? root.st.me.address : "" }
                            LogosButton { text: "Copy"; onClicked: root.copy(root.st.me.address, "Address") } }
                    }
                    Card {
                        T1 { text: "Categories you follow" }
                        T2 { Layout.fillWidth: true; text: "Your node keeps a full copy of these categories: browsing and searching them works offline and nobody learns what you look at. Each one costs disk space and bandwidth as it grows - unfollow what you don't need; search still finds everything else. Categories you publish in stay on, so replies to your models reach you." }
                        Flow { Layout.fillWidth: true; spacing: 6
                            Repeater { model: root.st.categories || []
                                delegate: Rectangle {
                                    width: sc.implicitWidth + 22; height: 30; radius: 15
                                    color: modelData.subscribed ? root.cPrimary : root.cInset; border.color: root.cLine
                                    Text { id: sc; textFormat: Text.PlainText; anchors.centerIn: parent; text: (modelData.subscribed ? "✓ " : "") + modelData.label; color: modelData.subscribed ? root.cBg : root.cText; font.pixelSize: 12 }
                                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            var ids = []
                                            var cs = root.st.categories || []
                                            for (var i = 0; i < cs.length; i++) {
                                                var on = cs[i].id === modelData.id ? !cs[i].subscribed : cs[i].subscribed   // flip the clicked one
                                                if (on) ids.push(cs[i].id)
                                            }
                                            root.act("setCategories", [JSON.stringify(ids)], "")
                                        } } } } }
                        RowLayout { spacing: root.spS
                            LogosButton { text: "Follow all"; onClicked: root.act("setCategories", [JSON.stringify(["all"])], "") }
                            LogosButton { text: "Only mine"; onClicked: root.act("setCategories", [JSON.stringify([])], "") } }
                    }
                    Card {
                        T1 { text: "Slicer" }
                        T2 { Layout.fillWidth: true; text: "\"Open in slicer\" on a model page downloads its files (checked against their hashes) and opens them in your slicer, where you check the preview and print the way you always do." }
                        T2 { Layout.fillWidth: true; visible: !!root.st.slicer; color: root.cOk; text: root.st.slicer ? "Opens in " + root.st.slicer.name + "." : "" }
                        FixPanel { fix: root.st.slicer ? null : (root.st.printSlicerFix || null) }
                    }
                    Card {
                        visible: !!root.st.experimentalPrint   // SWAMP_EXPERIMENTAL_PRINT=1 only
                        T1 { text: "Printer (experimental)" }
                        T2 { Layout.fillWidth: true; color: root.cErr
                             text: "Unfinished experiment. In its first real test the printer drove its head against the top of the frame. Only use it if you can watch the printer and stop it." }
                        T2 { Layout.fillWidth: true
                             text: "Print straight from Swamp to a Bambu Lab A1 or A1 mini on your network - no cloud, no account. The printer has to be in LAN-only mode with Developer Mode on (printer screen: Settings > Network/LAN; then restart it). That switches off Bambu's cloud printing while it's on. Needs OrcaSlicer 2.4+ or Bambu Studio. Only pick a printer you recognise: Swamp remembers its certificate on first contact and refuses an impostor later, but it can't tell which device is yours the first time." }
                        T2 { Layout.fillWidth: true; visible: !!root.st.printSlicer; color: root.cOk; text: root.st.printSlicer ? "Slicing with " + root.st.printSlicer.name + "." : "" }
                        FixPanel { fix: root.st.printSlicer ? null : (root.st.printSlicerFix || null) }
                        T2 { Layout.fillWidth: true; visible: !!root.st.printer; color: root.cText
                             text: root.st.printer ? root.st.printer.name + "  ·  " + root.st.printer.ip + "  ·  " + root.st.printer.serial + (root.st.printer.supported ? "" : "  ·  not supported yet") : "" }
                        T3 { Layout.fillWidth: true; visible: !!root.st.printer
                             text: !root.st.printer ? "" : root.st.printer.state ? ("Status: " + root.st.printer.state.state + (root.st.printer.state.state === "RUNNING" ? "  ·  " + root.st.printer.state.percent + "%  ·  " + root.st.printer.state.remainingMin + " min left" : "") +
                                   "  ·  nozzle " + Math.round(root.st.printer.state.nozzle) + "°C  ·  bed " + Math.round(root.st.printer.state.bed) + "°C" + (root.st.printer.state.job ? "  ·  " + root.st.printer.state.job : ""))
                                   : (root.st.printer.stateError ? "Can't reach it: " + root.st.printer.stateError : "Checking...")
                             color: !!root.st.printer && !!root.st.printer.stateError ? root.cErr : root.cText3 }
                        RowLayout { spacing: root.spS
                            LogosButton { text: root.st.discovering ? "Looking..." : "Find printers"; enabled: !root.st.discovering; onClicked: root.act("findPrinters", [], "") }
                            LogosButton { visible: !!root.st.printer; text: "Check status"; onClicked: root.core("printerStatus", [], function () { root.refresh() }) }
                        }
                        Repeater { model: root.st.discovered || []
                            delegate: RowLayout { spacing: root.spS
                                T2 { text: (modelData.name || "Bambu Lab printer") + "  ·  " + modelData.ip + "  ·  " + modelData.serial }
                                Text { textFormat: Text.PlainText; text: "use this one"; color: root.cPrimary; font.pixelSize: 12; font.underline: true
                                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { pIp.text = modelData.ip; pSerial.text = modelData.serial; pModel.text = modelData.model; pName.text = modelData.name || ""; pIp.edited = pSerial.edited = pModel.edited = true } } } } }
                        GridLayout { columns: 4; columnSpacing: root.spS; rowSpacing: root.spS; Layout.fillWidth: true
                            Field { id: pIp; placeholderText: "IP address"; saved: root.st.printer ? root.st.printer.ip : "" }
                            Field { id: pSerial; placeholderText: "serial number"; saved: root.st.printer ? root.st.printer.serial : "" }
                            Field { id: pModel; placeholderText: "model: A1 or A1 mini"; saved: root.st.printer ? root.st.printer.model : "" }
                            Field { id: pName; placeholderText: "name (optional)" }
                            Field { id: pCode; placeholderText: root.st.printer && root.st.printer.hasAccessCode ? "access code (saved)" : "access code (on the printer's screen)"; echoMode: TextInput.Password }
                            LogosButton { text: "Save printer"; onClicked: root.act("setPrinter", [JSON.stringify({ ip: pIp.text, serial: pSerial.text, model: pModel.text, name: pName.text, accessCode: pCode.text })], "Printer saved", function () { pCode.text = ""; pIp.edited = false; pSerial.edited = false; pModel.edited = false }) }
                        }
                    }
                    Card {
                        T1 { text: "Search indexes" }
                        T2 { Layout.fillWidth: true; text: root.st.index ? root.plural(root.st.index.known || 0, "index", "indexes") + " known. Your node checks that each one includes your own models; one that leaves something out without saying so loses your trust." : "" }
                        Repeater { model: root.st.index ? root.st.index.omissions : []
                            delegate: T2 { Layout.fillWidth: true; color: root.cErr
                                text: "Caught: " + modelData.indexerName + " left out your \"" + modelData.title + "\" without declaring it. Your searches no longer prefer this index." } }
                        Repeater { model: root.st.index ? root.st.index.suspects : []
                            delegate: T2 { Layout.fillWidth: true; color: root.cWarn
                                text: "\"" + modelData.title + "\" isn't in " + modelData.indexerName + "'s index yet. Your node re-sent it; if a newer index still leaves it out, that index gets flagged." } }
                        Repeater { model: root.st.index ? root.st.index.excludedMine : []
                            delegate: T2 { Layout.fillWidth: true; color: root.cWarn
                                text: modelData.indexerName + " declines to carry your \"" + modelData.title + "\"" + (modelData.why ? ": " + modelData.why : "") + ". That's its published policy; other indexes still carry it." } }
                        T3 { visible: !!root.st.index && root.st.index.omissions.length === 0 && root.st.index.excludedMine.length === 0 && (root.st.index.suspects || []).length === 0; text: "No problems found." }
                    }
                    Card {
                        T1 { text: "This node" }
                        T2 { Layout.fillWidth: true; text: (root.st.status || "?") + (root.st.hub ? "  ·  running as a pinning hub" : "") }
                        T2 { Layout.fillWidth: true; color: root.cWarn; visible: !!root.st.transport && !root.st.transport.ok
                             text: root.st.transport ? "Posted on these topics for 10+ minutes without hearing anything back: " + root.st.transport.silentTopics.join(", ") + ". Either nobody else follows them yet, or the network isn't carrying them - what you publish there may not be reaching anyone." : "" }
                        T3 { Layout.fillWidth: true; text: root.st.catalog ? (root.st.catalog.models + " models, " + root.st.catalog.events + " catalogue events, " + root.st.catalog.cids + " known files") : "" }
                        T3 { Layout.fillWidth: true; text: root.st.counters ? ("since start: rx " + root.st.counters.rx + " / tx " + root.st.counters.tx + "  ·  uploaded " + root.st.counters.uploaded + "  ·  fetched " + root.st.counters.fetched + (root.st.counters.verifyFailed ? "  ·  rejected " + root.st.counters.verifyFailed + " bad files" : "")) : "" }
                        T3 { Layout.fillWidth: true; text: root.st.storage ? ("Storage: " + (root.st.storage.hostOwned ? "Basecamp's node" : "own node") + "  ·  downloads go to " + root.st.storage.downloads) : "" }
                        RowLayout {
                            LogosButton { text: "Sync now"; onClicked: root.act("resync", [], "Asked peers for anything missing") }
                            LogosButton { text: "Copy diagnostics"; onClicked: root.copy(JSON.stringify(root.st), "Diagnostics") } }
                    }
                }
            }
        }

        T3 {
            Layout.fillWidth: true
            readonly property bool silent: !!root.st.transport && !root.st.transport.ok
            color: root.st.status === "Connected" && !silent ? root.cOk : root.cWarn
            text: (silent ? "Connected, but nobody has answered on " + root.st.transport.silentTopics.length + " of your topics - your posts may not be reaching anyone" : (root.st.status || "Connecting to swamp_core...")) +
                  "   |   core " + (root.st.version || "?") + "   |   " + (root.st.catalog ? root.plural(root.st.catalog.models, "model", "models") : "")
        }
    }
    Rectangle {
        // floats over the page so showing it doesn't move what you're about to click
        visible: root.toastMsg !== ""; z: 10
        anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 40
        width: Math.min(parent.width - 80, 720)
        implicitHeight: toastT.implicitHeight + 16
        radius: root.rad
        color: root.toastErr ? Qt.rgba(0.9, 0.33, 0.3, 0.15) : Qt.rgba(0.37, 0.72, 0.35, 0.15)
        border.color: root.toastErr ? root.cErr : root.cOk
        Text { id: toastT; textFormat: Text.PlainText; anchors.fill: parent; anchors.margins: 8; text: root.toastMsg + (root.toastErr ? "   (click to copy)" : ""); color: root.cText; wrapMode: Text.Wrap; font.pixelSize: 13 }
        MouseArea { anchors.fill: parent; enabled: root.toastErr; cursorShape: Qt.PointingHandCursor; onClicked: root.copy(root.toastMsg, "Error") }
        }


    Dialog {
        id: printConfirm
        modal: true; anchors.centerIn: parent; width: 520; padding: root.sp
        background: Rectangle { color: root.cCard; radius: root.rad + 4; border.color: root.cLine }
        header: T1 { text: "Start the print?"; padding: root.sp; bottomPadding: 0 }
        ColumnLayout { anchors.fill: parent; spacing: root.spS
            T2 { Layout.fillWidth: true; text: "The printer starts heating and printing right away. Check that PLA is loaded, the plate is clean and nothing is on the bed." +
                 (root.st.printJob && root.st.printJob.estimate ? "\n\nAbout " + root.st.printJob.estimate.time + ", " + (root.st.printJob.estimate.filamentG || "?") + " g of PLA." : "") } }
        footer: RowLayout { spacing: root.spS
            Item { Layout.fillWidth: true }
            LogosButton { text: "Not yet"; onClicked: printConfirm.reject() }
            LogosButton { text: "Print it"; onClicked: printConfirm.accept() }
            Item { width: root.sp } }
        onAccepted: root.act("startPrint", ["yes"], "Sending to the printer...")
    }
    Dialog {
        id: retractDlg
        title: "Retract this model?"
        modal: true; anchors.centerIn: parent; width: 520; padding: root.sp
        background: Rectangle { color: root.cCard; radius: root.rad + 4; border.color: root.cLine }
        header: T1 { text: "Retract this model?"; padding: root.sp; bottomPadding: 0 }
        footer: RowLayout { spacing: root.spS
            Item { Layout.fillWidth: true }
            LogosButton { text: "Cancel"; onClicked: retractDlg.reject() }
            LogosButton { text: "Retract"; onClicked: retractDlg.accept() }
            Item { width: root.sp } }
        ColumnLayout { anchors.fill: parent; spacing: root.spS
            T2 { Layout.fillWidth: true; text: "It disappears from listings everywhere it syncs. Copies people already downloaded stay theirs, and anyone looking at the model sees that you retracted it." }
            Field { id: retractReason; placeholderText: "Why (optional) - e.g. replaced by a better model" } }
        onAccepted: { root.act("retract", [root.model.modelId, retractReason.text], "Retracted", function () { retractReason.text = ""; root.openId = ""; root.model = null }) }
    }
}
