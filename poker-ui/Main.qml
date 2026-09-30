import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Item {
    id: root
    width: 760
    height: 720

    // Whole table snapshot, refreshed from the core each tick.
    property var st: ({})

    // ── Logos bridge ──────────────────────────────────────────────────
    //
    // Two entry points, and the distinction matters:
    //   logos.callModule(id, method, [])            — synchronous, no-arg only
    //   logos.callModuleAsync(id, method, args, cb) — anything with arguments
    // Calling the sync form with arguments is outside its contract, so every
    // method that takes parameters (joinTable, act) goes through the async one.
    // Nothing here needs the return value: the 1s poll re-reads tableState.
    function callPoker(method) {
        if (typeof logos === "undefined" || !logos.callModule) {
            console.log("poker: logos bridge unavailable")
            return null
        }
        return logos.callModule("poker", method, [])
    }
    function callPokerArgs(method, args) {
        if (typeof logos === "undefined" || !logos.callModuleAsync) {
            console.log("poker: logos.callModuleAsync unavailable")
            return
        }
        logos.callModuleAsync("poker", method, args, function () { refresh() })
    }

    // The bridge JSON-encodes whatever the module returned, so a method whose
    // own return value is already a JSON document comes back double-encoded:
    // tableState arrives as "\"{\\\"pot\\\":0,…}\"". One parse peels the
    // transport quoting and yields the inner JSON *text*; a second turns it
    // into an object. Scalar getters (myId, a hex colour) need exactly one
    // parse, so keep parsing while the result is still a string and stop as
    // soon as a parse fails — that failure means we already have the value.
    function unwrapRemote(raw, def) {
        if (raw === null || raw === undefined) return def
        var v = raw
        for (var i = 0; i < 3 && typeof v === "string"; ++i) {
            try {
                v = JSON.parse(v)
            } catch (e) {
                return (i === 0) ? def : v
            }
        }
        return v
    }
    function refresh() {
        var s = unwrapRemote(callPoker("tableState"), null)
        if (s && typeof s === "object") st = s
    }

    // ── Card helpers (id 0..51: rank=id%13, suit=id/13) ───────────────
    function rankStr(id) {
        return ["2","3","4","5","6","7","8","9","10","J","Q","K","A"][id % 13]
    }
    function suitGlyph(id) { return ["♣","♦","♥","♠"][Math.floor(id / 13)] }
    function suitColor(id) {
        var s = Math.floor(id / 13)
        return (s === 1 || s === 2) ? "#d11" : "#111"   // diamonds/hearts red
    }

    function statusText(s) {
        return s === 0 ? "Off" : s === 1 ? "Connecting…" : s === 2 ? "Connected" : "Error"
    }
    function statusColor(s) {
        return s === 2 ? "#34a853" : s === 1 ? "#fbbc04" : s === 3 ? "#ea4335" : "#9aa5b1"
    }
    function phaseLabel(p) {
        switch (p) {
        case "preflop": return "Pre-flop"
        case "flop":    return "Flop"
        case "turn":    return "Turn"
        case "river":   return "River"
        case "showdown":return "Showdown"
        case "handover":return "Hand over"
        default:        return ""
        }
    }
    function protoLabel(p) {
        switch (p) {
        case "shuffle": return "Shuffling deck (mental poker)…"
        case "lock":    return "Locking cards…"
        case "deal":    return "Dealing…"
        default:        return ""
        }
    }

    // Felt-green backdrop.
    Rectangle { anchors.fill: parent; color: "#0e6b43" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 14

        // ── Header ──
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Text {
                text: "♠ p2p Poker"
                color: "white"; font.pixelSize: 24; font.bold: true
                Layout.fillWidth: true
            }
            Rectangle { width: 10; height: 10; radius: 5; color: statusColor(st.status || 0) }
            Text { text: statusText(st.status || 0); color: "white"; opacity: 0.9; font.pixelSize: 13 }
            Button {
                text: (st.status || 0) === 0 ? "Start net" : "Stop net"
                onClicked: {
                    if ((st.status || 0) === 0) callPoker("startDelivery")
                    else                        callPoker("stopDelivery")
                    refresh()
                }
            }
        }

        // ── Join / lobby ──
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            radius: 10
            color: Qt.rgba(1,1,1,0.12)

            // Not joined yet → name + Join.
            RowLayout {
                anchors.fill: parent; anchors.margins: 10; spacing: 10
                visible: !(st.joined === true) && !(st.leaving === true)
                TextField {
                    id: nameField
                    Layout.fillWidth: true
                    placeholderText: "Your name"
                }
                Button {
                    text: "Join table"
                    onClicked: { callPokerArgs("joinTable", [nameField.text]) }
                }
            }

            // Joined → players count + (coordinator) Start hand.
            RowLayout {
                id: joinedView
                anchors.fill: parent; anchors.margins: 10; spacing: 10
                visible: st.joined === true
                Text {
                    Layout.fillWidth: true
                    color: "white"; font.pixelSize: 14
                    text: (st.players || 0) + " player(s) at the table"
                          + (protoLabel(st.proto) ? "  ·  " + protoLabel(st.proto) : "")
                }
                Text {
                    visible: !(st.isCoordinator === true)
                    color: "white"; opacity: 0.8; font.pixelSize: 12
                    text: "waiting for host to deal…"
                }
                Button {
                    visible: st.isCoordinator === true
                    enabled: (st.players || 0) >= 2
                            && (st.proto === "lobby" || st.proto === "done")
                    text: "Deal hand"
                    onClicked: { callPoker("startHand"); refresh() }
                }
                Button {
                    readonly property bool inHand: st.proto !== "lobby" && st.proto !== "done"
                    text: inHand ? "Leave after hand" : "Leave table"
                    onClicked: { callPoker("leaveTable"); refresh() }
                }
            }

            // Left mid-hand → the core folds us out, then gives up the seat.
            RowLayout {
                anchors.fill: parent; anchors.margins: 10; spacing: 10
                visible: st.leaving === true
                Text {
                    Layout.fillWidth: true
                    color: "white"; font.pixelSize: 14
                    text: "Leaving after this hand — folding automatically…"
                }
                Button {
                    text: "Stay"
                    onClicked: { callPokerArgs("joinTable", [st.myName || ""]) }
                }
            }
        }

        // ── Community cards + pot ──
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            radius: 12
            color: Qt.rgba(0,0,0,0.18)
            visible: st.proto !== "lobby"

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 8
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 14
                    Text {
                        text: phaseLabel(st.phase)
                        color: "white"; opacity: 0.9; font.pixelSize: 14; font.bold: true
                    }
                    Text {
                        text: "Pot: " + (st.pot || 0)
                        color: "#ffe082"; font.pixelSize: 16; font.bold: true
                    }
                }
                Row {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8
                    Repeater {
                        model: 5
                        delegate: Rectangle {
                            property var bd: st.board ? st.board : []
                            property bool has: index < bd.length
                            width: 50; height: 70; radius: 6
                            color: has ? "white" : Qt.rgba(1,1,1,0.10)
                            border.color: "#0a4a2e"; border.width: 1
                            Text {
                                anchors.centerIn: parent
                                visible: has
                                text: has ? rankStr(bd[index]) + " " + suitGlyph(bd[index]) : ""
                                color: has ? suitColor(bd[index]) : "transparent"
                                font.pixelSize: 16; font.bold: true
                            }
                        }
                    }
                }
            }
        }

        // ── Seats ──
        ListView {
            id: seatList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: st.seats ? st.seats : []
            delegate: Rectangle {
                id: seatRow
                property var seat: modelData
                width: seatList.width
                height: 56
                radius: 8
                color: seat.isToAct ? Qt.rgba(1,1,0.4,0.22)
                                    : Qt.rgba(1,1,1,0.10)
                border.color: seat.isMe ? "#ffe082" : "transparent"
                border.width: seat.isMe ? 2 : 0
                opacity: seat.folded ? 0.45 : 1.0

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 10

                    // Dealer button marker.
                    Rectangle {
                        width: 22; height: 22; radius: 11
                        color: seatRow.seat.isButton ? "white" : "transparent"
                        border.color: "white"; border.width: 1
                        Text {
                            anchors.centerIn: parent; text: "D"
                            visible: seatRow.seat.isButton
                            color: "#0e6b43"; font.pixelSize: 12; font.bold: true
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Text {
                            color: "white"; font.pixelSize: 14; font.bold: true
                            text: seatRow.seat.name + (seatRow.seat.isMe ? "  (you)" : "")
                        }
                        Text {
                            color: "#cfe9d8"; font.pixelSize: 12
                            text: "chips " + seatRow.seat.chips
                                  + (seatRow.seat.committed > 0 ? "   ·   bet " + seatRow.seat.committed : "")
                                  + (seatRow.seat.allIn ? "   ·   ALL-IN" : "")
                                  + (seatRow.seat.folded ? "   ·   folded" : "")
                        }
                    }

                    // Hole cards: shown only when revealed (mine always, others at showdown).
                    Row {
                        spacing: 5
                        Repeater {
                            model: 2
                            delegate: Rectangle {
                                property var hole: seatRow.seat.hole ? seatRow.seat.hole : []
                                property bool shown: hole.length === 2
                                width: 34; height: 48; radius: 5
                                color: shown ? "white" : "#13502f"
                                border.color: "#0a4a2e"; border.width: 1
                                Text {
                                    anchors.centerIn: parent
                                    visible: shown
                                    text: shown ? rankStr(hole[index]) + suitGlyph(hole[index]) : ""
                                    color: shown ? suitColor(hole[index]) : "transparent"
                                    font.pixelSize: 12; font.bold: true
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── My hole cards ──
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 10
            // `st.myHole && …` yields undefined before the first tableState
            // arrives, and `visible` is a bool — coerce so QML doesn't warn.
            visible: !!(st.myHole && st.myHole.length === 2)
            Text { text: "Your hand:"; color: "white"; font.pixelSize: 14 }
            Row {
                spacing: 6
                Repeater {
                    model: (st.myHole && st.myHole.length === 2) ? st.myHole : []
                    delegate: Rectangle {
                        width: 48; height: 66; radius: 6
                        color: "white"; border.color: "#0a4a2e"; border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: rankStr(modelData) + " " + suitGlyph(modelData)
                            color: suitColor(modelData); font.pixelSize: 16; font.bold: true
                        }
                    }
                }
            }
        }

        // ── Last winner banner ──
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            radius: 8
            color: Qt.rgba(1,1,1,0.16)
            visible: st.lastWinner !== undefined && st.proto === "done"
            Text {
                anchors.centerIn: parent
                color: "#ffe082"; font.pixelSize: 14; font.bold: true
                text: st.lastWinner
                      ? ("🏆 " + (st.lastWinner.names ? st.lastWinner.names.join(", ") : "")
                         + " wins " + (st.lastWinner.amount || 0)
                         + "  (" + (st.lastWinner.category || "") + ")")
                      : ""
            }
        }

        // ── Action bar ──
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: st.myTurn === true

            Button {
                text: "Fold"
                onClicked: { callPokerArgs("act", ["fold", 0]) }
            }
            Button {
                text: (st.toCall || 0) > 0 ? ("Call " + st.toCall) : "Check"
                onClicked: {
                    callPokerArgs("act", [(st.toCall || 0) > 0 ? "call" : "check", 0])
                }
            }
            Item { Layout.fillWidth: true }
            TextField {
                id: raiseField
                Layout.preferredWidth: 90
                placeholderText: "amount"
                text: "" + (st.minRaise || 10)
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1 }
            }
            Button {
                text: (st.currentBet || 0) > 0 ? "Raise" : "Bet"
                onClicked: {
                    var amt = parseInt(raiseField.text)
                    if (!isNaN(amt) && amt > 0) { callPokerArgs("act", ["raise", amt]) }
                }
            }
        }
    }

    Timer { interval: 1000; running: true; repeat: true; onTriggered: refresh() }
    Component.onCompleted: refresh()
}
