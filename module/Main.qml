import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
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
    function toast(msg, err) { root.toastMsg = msg; root.toastErr = !!err; toastTimer.restart() }
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
        var pending = 2 + (root.openId ? 1 : 0)
        var fin = function () { if (--pending > 0) return; root.busy = false; if (root.again) { root.again = false; root.refresh() } }
        root.core("snapshot", [], function (raw) { var s = root.parse(raw); if (s && s.ok) root.st = s; fin() })
        root.core("listModels", [JSON.stringify({ q: root.query, tag: root.tagFilter, sort: root.sortBy, mine: root.tab === "mine", limit: 200 })], function (raw) {
            var r = root.parse(raw); if (r && r.ok) { root.models = r.models; root.tags = r.tags; root.total = r.total } fin()
        })
        if (root.openId) root.core("getModel", [root.openId], function (raw) { var r = root.parse(raw); if (r && r.ok) root.model = r.model; fin() })
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

    function fileUrl(p) { return p ? ("file://" + p) : "" }
    function size(n) { return n > 1048576 ? (n / 1048576).toFixed(1) + " MB" : n > 1024 ? Math.round(n / 1024) + " KB" : n + " B" }
    function day(ms) { return ms ? new Date(ms).toISOString().slice(0, 10) : "" }
    function ver() { if (!root.model) return null; var vs = root.model.versions; var i = root.openVersion > 0 ? root.openVersion - 1 : vs.length - 1; return vs[Math.min(i, vs.length - 1)] }
    function openModel(id) { root.openId = id; root.openVersion = 0; root.model = null; root.refresh() }
    function startRemix() {
        var v = root.ver(); if (!v) return
        root.draftParents = [{ modelId: root.model.modelId, v: v.v }]; root.draftFor = ""
        pTitle.text = "Remix of " + root.model.title; pLicence.currentIndex = Math.max(0, root.licences.indexOf(v.licence))
        root.openId = ""; root.model = null; root.tab = "publish"
    }
    function startNewVersion() {
        var v = root.ver(); if (!v) return
        root.draftFor = root.model.modelId; root.draftParents = []
        pTitle.text = v.title; pSummary.text = v.summary || ""; pDesc.text = v.description || ""; pTags.text = (v.tags || []).join(", ")
        pLicence.currentIndex = Math.max(0, root.licences.indexOf(v.licence))
        root.openId = ""; root.model = null; root.tab = "publish"
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
    component Field: TextField {
        Layout.fillWidth: true; Layout.minimumWidth: 80; Layout.preferredWidth: 200
        color: root.cText; placeholderTextColor: root.cText3; font.pixelSize: 13
        background: Rectangle { color: root.cInset; radius: root.rad; border.color: parent.activeFocus ? root.cPrimary : root.cLine }
    }
    component Area: TextArea {
        Layout.fillWidth: true; Layout.preferredHeight: 90
        color: root.cText; placeholderTextColor: root.cText3; font.pixelSize: 13; wrapMode: TextEdit.Wrap
        background: Rectangle { color: root.cInset; radius: root.rad; border.color: parent.activeFocus ? root.cPrimary : root.cLine }
    }
    component Thumb: Rectangle {
        property string path: ""
        color: root.cInset; radius: root.rad; clip: true
        Image { anchors.fill: parent; anchors.margins: 4; source: root.fileUrl(parent.path); fillMode: Image.PreserveAspectFit; asynchronous: true; visible: status === Image.Ready }
        T3 { anchors.centerIn: parent; visible: !parent.path; text: "no preview yet" }
    }

    FileDialog { id: fileDlg; fileMode: FileDialog.OpenFiles; title: "Model files (STL, 3MF, STEP, ...)"
        onAccepted: { var a = root.draftFiles.slice(); for (var i = 0; i < selectedFiles.length; i++) a.push(root.pathOf(selectedFiles[i])); root.draftFiles = a } }
    FileDialog { id: imgDlg; fileMode: FileDialog.OpenFiles; title: "Photos (PNG, JPEG)"; nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)"]
        onAccepted: { var a = root.draftImages.slice(); for (var i = 0; i < selectedFiles.length; i++) a.push(root.pathOf(selectedFiles[i])); root.draftImages = a } }
    FileDialog { id: makeDlg; fileMode: FileDialog.OpenFile; title: "A photo of your print"; nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)"]
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

        Rectangle {
            visible: root.toastMsg !== ""
            Layout.fillWidth: true
            implicitHeight: toastT.implicitHeight + 16
            radius: root.rad
            color: root.toastErr ? Qt.rgba(0.9, 0.33, 0.3, 0.15) : Qt.rgba(0.37, 0.72, 0.35, 0.15)
            border.color: root.toastErr ? root.cErr : root.cOk
            Text { id: toastT; textFormat: Text.PlainText; anchors.fill: parent; anchors.margins: 8; text: root.toastMsg; color: root.cText; wrapMode: Text.Wrap; font.pixelSize: 13 }
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
                        Field { id: search; placeholderText: "Search models, tags..."; onAccepted: { root.query = text; root.refresh() } }
                        ComboBox { id: sortBox; model: ["Newest", "Most liked", "Most made"]; Layout.preferredWidth: 160
                            onActivated: { root.sortBy = ["new", "likes", "makes"][currentIndex]; root.refresh() } }
                        LogosButton { text: "Search"; onClicked: { root.query = search.text; root.refresh() } }
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
                            delegate: Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: 300
                                color: root.cCard; radius: root.rad + 4; border.color: hov.containsMouse ? root.cPrimary : root.cLine
                                ColumnLayout {
                                    anchors.fill: parent; anchors.margins: root.spS; spacing: 4
                                    Thumb { Layout.fillWidth: true; Layout.preferredHeight: 190; path: modelData.thumb || "" }
                                    T1 { Layout.fillWidth: true; text: modelData.title; font.pixelSize: 14; elide: Text.ElideRight; maximumLineCount: 2 }
                                    T3 { Layout.fillWidth: true; text: "by " + modelData.creatorName + "  ·  " + modelData.licence + (modelData.remix ? "  ·  remix" : "") }
                                    T3 { Layout.fillWidth: true; text: modelData.likes + " likes  ·  " + modelData.makes + " makes  ·  v" + modelData.latest }
                                }
                                MouseArea { id: hov; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.openModel(modelData.modelId) }
                            }
                        }
                    }
                    T2 { visible: root.models.length === 0; text: root.tab === "mine" ? "You haven't published anything yet. Publish your first model." : "Nothing here yet. The catalogue fills in as it syncs from other people, or publish the first model." }
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
                        T3 { visible: !root.model; text: "loading..." }
                    }
                    Card {
                        visible: !!root.model
                        RowLayout {
                            Layout.fillWidth: true; spacing: root.sp
                            Thumb { Layout.preferredWidth: 300; Layout.preferredHeight: 300; Layout.alignment: Qt.AlignTop
                                path: { var v = root.ver(); if (!v || !v.images) return ""; for (var i = 0; i < v.images.length; i++) if (v.images[i].local) return v.images[i].local; return "" } }
                            ColumnLayout {
                                Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; spacing: 6
                                T1 { Layout.fillWidth: true; font.pixelSize: 20; text: root.model ? root.model.title : "" }
                                T2 { Layout.fillWidth: true; text: root.model ? ("by " + root.model.creatorName + "  ·  " + (root.ver() ? root.ver().licence : "") + "  ·  published " + root.day(root.ver() ? root.ver().published : 0)) : "" }
                                T2 { Layout.fillWidth: true; visible: !!root.ver() && !!root.ver().summary; text: root.ver() ? (root.ver().summary || "") : "" }
                                T3 { Layout.fillWidth: true; visible: !!root.ver() && (root.ver().parents || []).length > 0; text: "Remix of " + ((root.ver() && root.ver().parents) ? root.ver().parents.map(function (p) { return p.modelId.slice(0, 8) + " v" + p.v }).join(", ") : "") }
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
                                RowLayout {
                                    spacing: root.spS
                                    LogosButton { text: { var d = root.ver() && root.ver().download; return d ? (d.status === "done" ? "Downloaded" : d.status === "failed" ? "Retry download" : "Downloading...") : "Download" }
                                        onClicked: root.act("download", [root.model.modelId, String(root.ver().v)], "Downloading into your Swamp folder") }
                                    LogosButton { visible: !!(root.ver() && root.ver().download && root.ver().download.status === "done"); text: "Open folder"
                                        onClicked: Qt.openUrlExternally(root.fileUrl(root.ver().download.dir)) }
                                    LogosButton { text: root.model && root.model.likedByMe ? "Unlike (" + root.model.likes + ")" : "Like (" + (root.model ? root.model.likes : 0) + ")"
                                        onClicked: root.act("like", [root.model.modelId, root.model.likedByMe ? "false" : "true"], "") }
                                    LogosButton { text: "Remix"; onClicked: root.startRemix() }
                                    LogosButton { visible: !!root.model && root.model.mine; text: "New version"; onClicked: root.startNewVersion() }
                                }
                                T3 { Layout.fillWidth: true; visible: !!(root.ver() && root.ver().download && root.ver().download.error); color: root.cErr; text: root.ver() && root.ver().download ? (root.ver().download.error || "") : "" }
                            }
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
                                Thumb { Layout.preferredWidth: 90; Layout.preferredHeight: 90; path: modelData.images && modelData.images.length && modelData.images[0].local ? modelData.images[0].local : "" }
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
                    T1 { text: root.draftFor ? "Publish a new version" : (root.draftParents.length ? "Publish a remix" : "Publish a model") }
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
                        T3 { text: "Licence" }
                        ComboBox { id: pLicence; model: root.licences; Layout.fillWidth: true }
                        T3 { text: "Files" }
                        ColumnLayout { Layout.fillWidth: true
                            Repeater { model: root.draftFiles; delegate: T2 { text: modelData } }
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
                                var d = { title: pTitle.text, summary: pSummary.text, description: pDesc.text, licence: pLicence.currentText,
                                          tags: pTags.text.split(",").map(function (t) { return t.trim() }).filter(function (t) { return t.length > 0 }),
                                          parents: root.draftParents, files: root.draftFiles.map(function (p) { return { path: p } }),
                                          images: root.draftImages.map(function (p) { return { path: p } }) }
                                if (root.draftFor) d.modelId = root.draftFor
                                root.act("publish", [JSON.stringify(d)], "Published - files are uploading in the background", function (r) {
                                    root.draftFiles = []; root.draftImages = []; root.draftParents = []; root.draftFor = ""
                                    pTitle.text = ""; pSummary.text = ""; pDesc.text = ""; pTags.text = ""
                                    root.tab = "mine"; root.openModel(r.modelId)
                                })
                            }
                        }
                        LogosButton { visible: !!root.draftFor || root.draftParents.length > 0; text: "Cancel"; onClicked: { root.draftFor = ""; root.draftParents = [] } }
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
                            Field { id: profName; placeholderText: "display name"; text: root.st.me && root.st.me.profile ? (root.st.me.profile.name || "") : "" }
                            Field { id: profBio; placeholderText: "a line about you (optional)"; text: root.st.me && root.st.me.profile ? (root.st.me.profile.bio || "") : "" }
                            LogosButton { text: "Save"; onClicked: root.act("setProfile", [JSON.stringify({ name: profName.text, bio: profBio.text })], "Profile saved") } }
                        RowLayout { Layout.fillWidth: true
                            T3 { text: "Your key:" }
                            Text { textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideMiddle; color: root.cText2; font.family: "monospace"; font.pixelSize: 11; text: root.st.me ? root.st.me.address : "" }
                            LogosButton { text: "Copy"; onClicked: root.copy(root.st.me.address, "Address") } }
                    }
                    Card {
                        T1 { text: "This node" }
                        T2 { Layout.fillWidth: true; text: (root.st.status || "?") + (root.st.hub ? "  ·  running as a pinning hub" : "") }
                        T3 { Layout.fillWidth: true; text: root.st.catalog ? (root.st.catalog.models + " models, " + root.st.catalog.events + " catalogue events, " + root.st.catalog.cids + " known files") : "" }
                        T3 { Layout.fillWidth: true; text: root.st.counters ? ("rx " + root.st.counters.rx + " / tx " + root.st.counters.tx + "  ·  uploaded " + root.st.counters.uploaded + "  ·  fetched " + root.st.counters.fetched + (root.st.counters.verifyFailed ? "  ·  rejected " + root.st.counters.verifyFailed + " bad files" : "")) : "" }
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
            color: root.st.status === "Connected" ? root.cOk : root.cWarn
            text: (root.st.status || "Connecting to swamp_core...") + "   |   core " + (root.st.version || "?") + "   |   " + (root.st.catalog ? root.st.catalog.models + " models" : "")
        }
    }
}
