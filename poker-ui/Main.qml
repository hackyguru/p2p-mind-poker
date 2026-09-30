import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Item {
    id: root
    width: 960
    height: 760

    // Whole table snapshot, refreshed from the core each tick.
    property var st: ({})
    property bool showRankings: false

    // ── Palette ───────────────────────────────────────────────────────
    readonly property color cBg:      "#0f1411"
    readonly property color cBar:     "#161d19"
    readonly property color cFeltHi:  "#23915a"
    readonly property color cFeltLo:  "#0c5534"
    readonly property color cRailHi:  "#7b5230"
    readonly property color cRailLo:  "#3e2715"
    readonly property color cGold:    "#f3c969"
    readonly property color cText:    "#f4f1e8"
    readonly property color cMuted:   "#9fb2a6"
    readonly property color cGreen:   "#2f9e5f"
    readonly property color cPlate:   "#1b2420"

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

    // ── Table state helpers ───────────────────────────────────────────
    readonly property var seats: st.seats ? st.seats : []
    readonly property bool inHandPhase: !!st.proto && st.proto !== "lobby" && st.proto !== "done"
    readonly property int myIndex: {
        for (var i = 0; i < seats.length; ++i) if (seats[i].isMe) return i
        return 0
    }
    function nameOf(id) {
        for (var i = 0; i < seats.length; ++i) if (seats[i].id === id) return seats[i].name
        return ""
    }
    function statusText(s) {
        return s === 0 ? "Offline" : s === 1 ? "Connecting…" : s === 2 ? "Connected" : "Error"
    }
    function statusColor(s) {
        return s === 2 ? "#34c26b" : s === 1 ? "#f2b93b" : s === 3 ? "#e5533d" : "#6f7d74"
    }
    function phaseLabel(p) {
        switch (p) {
        case "preflop":  return "PRE-FLOP"
        case "flop":     return "FLOP"
        case "turn":     return "TURN"
        case "river":    return "RIVER"
        case "showdown": return "SHOWDOWN"
        case "handover": return "HAND OVER"
        default:         return ""
        }
    }
    function protoLabel(p) {
        switch (p) {
        case "shuffle": return "Shuffling: every player encrypts and reshuffles the deck…"
        case "lock":    return "Locking card positions…"
        case "deal":    return "Dealing…"
        default:        return ""
        }
    }

    // Hand-rankings cheat sheet. Cards are "<rank><suit>"; only the first
    // `used` cards make the hand, the rest are kickers and drawn faded.
    // `key` matches the category name the core reports for a winner.
    readonly property var rankings: [
        { name: "Royal Flush",     key: "",                cards: ["As","Ks","Qs","Js","Ts"], used: 5 },
        { name: "Straight Flush",  key: "Straight Flush",  cards: ["4h","5h","6h","7h","8h"], used: 5 },
        { name: "Four of a Kind",  key: "Four of a Kind",  cards: ["As","Ah","Ac","Ad","2s"], used: 4 },
        { name: "Full House",      key: "Full House",      cards: ["Qd","Qs","Qh","Ts","Th"], used: 5 },
        { name: "Flush",           key: "Flush",           cards: ["Ks","Js","9s","6s","3s"], used: 5 },
        { name: "Straight",        key: "Straight",        cards: ["4s","5h","6s","7h","8s"], used: 5 },
        { name: "Three of a Kind", key: "Three of a Kind", cards: ["Kh","Kc","Kd","7s","9d"], used: 3 },
        { name: "Two Pair",        key: "Two Pair",        cards: ["8h","8c","7h","7c","Kh"], used: 4 },
        { name: "One Pair",        key: "Pair",            cards: ["Js","Jh","As","8d","2s"], used: 2 },
        { name: "High Card",       key: "High Card",       cards: ["Ah","8s","7h","2s","4h"], used: 1 }
    ]
    // Card id 0..51: rank = id % 13 (0 = "2" … 12 = "A"), suit = id / 13 (♣ ♦ ♥ ♠).
    function cardId(code) {
        return "cdhs".indexOf(code[1]) * 13 + "23456789TJQKA".indexOf(code[0])
    }

    Rectangle { anchors.fill: parent; color: root.cBg }

    // ── Reusable pieces ───────────────────────────────────────────────

    // A playing card; card < 0 draws the back.
    component PlayingCard: Rectangle {
        id: pc
        property int card: -1
        property real s: 1.0
        property bool dim: false
        readonly property bool faceUp: card >= 0
        readonly property int rank: card % 13
        readonly property int suit: Math.floor(card / 13)
        readonly property color ink: (suit === 1 || suit === 2) ? "#d23b2f" : "#1d1d1d"
        width: 46 * s; height: 66 * s; radius: 6 * s
        color: faceUp ? "#fbfaf5" : "#9c2a24"
        border.color: faceUp ? "#d4d1c4" : "#f1dfbf"
        border.width: 1
        opacity: dim ? 0.3 : 1.0

        // Back: inset frame + pip.
        Rectangle {
            visible: !pc.faceUp
            anchors.fill: parent; anchors.margins: 4 * pc.s
            radius: 4 * pc.s; color: "transparent"
            border.color: "#f1dfbf"; border.width: 1; opacity: 0.55
        }
        Text {
            visible: !pc.faceUp
            anchors.centerIn: parent
            text: "♠"; color: "#f1dfbf"; opacity: 0.55
            font.pixelSize: 22 * pc.s
        }
        // Face: corner rank + suit, big suit bottom-right.
        Column {
            visible: pc.faceUp
            x: 4 * pc.s; y: 2 * pc.s
            spacing: -4 * pc.s
            Text {
                text: pc.faceUp ? ["2","3","4","5","6","7","8","9","10","J","Q","K","A"][pc.rank] : ""
                color: pc.ink; font.pixelSize: 16 * pc.s; font.bold: true
            }
            Text {
                text: pc.faceUp ? ["♣","♦","♥","♠"][pc.suit] : ""
                color: pc.ink; font.pixelSize: 12 * pc.s
            }
        }
        Text {
            visible: pc.faceUp
            anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.rightMargin: 5 * pc.s; anchors.bottomMargin: 1 * pc.s
            text: pc.faceUp ? ["♣","♦","♥","♠"][pc.suit] : ""
            color: pc.ink; font.pixelSize: 26 * pc.s
        }
    }

    // Pill button in the table's palette.
    component PillButton: Rectangle {
        id: pb
        property string label: ""
        property color tint: "#2b3530"
        signal clicked()
        implicitWidth: Math.max(88, lbl.implicitWidth + 30)
        implicitHeight: 38
        radius: height / 2
        color: !enabled ? Qt.darker(tint, 1.4)
             : ma.pressed ? Qt.darker(tint, 1.2)
             : ma.containsMouse ? Qt.lighter(tint, 1.18) : tint
        opacity: enabled ? 1.0 : 0.45
        border.color: Qt.rgba(1, 1, 1, 0.10); border.width: 1
        Text {
            id: lbl
            anchors.centerIn: parent
            text: pb.label; color: "white"
            font.pixelSize: 13; font.bold: true
        }
        MouseArea {
            id: ma
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: pb.clicked()
        }
    }

    // A chip with an amount next to it.
    component ChipAmount: Row {
        id: ca
        property real amount: 0
        property color chip: "#c8372d"
        spacing: 5
        Rectangle {
            width: 18; height: 18; radius: 9
            color: ca.chip
            border.color: "white"; border.width: 2
            anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                anchors.centerIn: parent
                width: 8; height: 8; radius: 4
                color: "transparent"; border.color: "white"; border.width: 1
            }
        }
        Text {
            text: ca.amount
            color: "white"; font.pixelSize: 13; font.bold: true
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    // ── Layout ────────────────────────────────────────────────────────
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Top bar ──
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            color: root.cBar

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18; anchors.rightMargin: 18
                spacing: 12

                Text {
                    text: "♠ p2p Poker"
                    color: root.cText; font.pixelSize: 20; font.bold: true
                }
                Rectangle {
                    width: 9; height: 9; radius: 5
                    color: root.statusColor(st.status || 0)
                }
                Text {
                    text: root.statusText(st.status || 0)
                    color: root.cMuted; font.pixelSize: 12
                }
                PillButton {
                    implicitHeight: 32
                    label: (st.status || 0) === 0 ? "Start net" : "Stop net"
                    onClicked: {
                        if ((st.status || 0) === 0) root.callPoker("startDelivery")
                        else                        root.callPoker("stopDelivery")
                        root.refresh()
                    }
                }

                Item { Layout.fillWidth: true }

                // Not seated → name + Join.
                Rectangle {
                    id: nameBox
                    visible: !(st.joined === true) && !(st.leaving === true)
                    implicitWidth: 160; implicitHeight: 32; radius: 16
                    color: "#0f1512"
                    border.color: nameField.activeFocus ? root.cGold : "#33413a"
                    border.width: 1
                    TextInput {
                        id: nameField
                        anchors.fill: parent
                        anchors.leftMargin: 14; anchors.rightMargin: 14
                        verticalAlignment: TextInput.AlignVCenter
                        color: root.cText; font.pixelSize: 13
                        selectByMouse: true
                        clip: true
                        onAccepted: root.callPokerArgs("joinTable", [text])
                    }
                    Text {
                        anchors.fill: nameField
                        verticalAlignment: Text.AlignVCenter
                        visible: nameField.text === "" && !nameField.activeFocus
                        text: "Your name"
                        color: root.cMuted; font.pixelSize: 13
                    }
                }
                PillButton {
                    visible: nameBox.visible
                    implicitHeight: 32
                    tint: root.cGreen
                    label: "Join table"
                    onClicked: root.callPokerArgs("joinTable", [nameField.text])
                }
                // Seated → leave (deferred to the end of a hand).
                PillButton {
                    visible: st.joined === true
                    implicitHeight: 32
                    label: root.inHandPhase ? "Leave after hand" : "Leave table"
                    onClicked: { root.callPoker("leaveTable"); root.refresh() }
                }
                PillButton {
                    visible: st.leaving === true
                    implicitHeight: 32
                    tint: root.cGreen
                    label: "Stay"
                    onClicked: root.callPokerArgs("joinTable", [st.myName || ""])
                }
                PillButton {
                    implicitHeight: 32
                    tint: root.showRankings ? "#8a6d2b" : "#2b3530"
                    label: "Hand rankings"
                    onClicked: root.showRankings = !root.showRankings
                }
            }
        }

        // ── Table ──
        Item {
            id: tableArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            // The table itself; narrows when the rankings drawer is open.
            Item {
                id: tableRegion
                anchors.left: parent.left
                anchors.top: parent.top; anchors.bottom: parent.bottom
                width: parent.width - (root.showRankings ? drawer.width : 0)
                Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

            // Wooden rail.
            Rectangle {
                id: rail
                // Leave room outside the rail for the seats (cards + name plate).
                width: Math.max(320, Math.min(tableRegion.width - 190,
                                              (tableRegion.height - 250) * 1.9,
                                              tableRegion.height * 0.64 * 1.9))
                height: width / 1.9
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                radius: height / 2
                gradient: Gradient {
                    GradientStop { position: 0.0; color: root.cRailHi }
                    GradientStop { position: 1.0; color: root.cRailLo }
                }
                border.color: "#20130a"; border.width: 2

                // Felt.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 18
                    radius: height / 2
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: root.cFeltHi }
                        GradientStop { position: 1.0; color: root.cFeltLo }
                    }
                    border.color: "#0a3d25"; border.width: 3

                    // Betting line.
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 22
                        radius: height / 2
                        color: "transparent"
                        border.color: Qt.rgba(1, 1, 1, 0.13); border.width: 2
                    }
                }
            }

            // ── Centre: board, pot, status ──
            Column {
                anchors.centerIn: rail
                spacing: 10

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: text !== ""
                    text: root.phaseLabel(st.phase)
                    color: Qt.rgba(1, 1, 1, 0.75)
                    font.pixelSize: 12; font.bold: true; font.letterSpacing: 3
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 8
                    visible: !!st.proto && st.proto !== "lobby"
                    Repeater {
                        model: 5
                        delegate: Item {
                            width: 50; height: 72
                            property var bd: st.board ? st.board : []
                            Rectangle {
                                anchors.fill: parent
                                visible: index >= bd.length
                                radius: 6; color: Qt.rgba(0, 0, 0, 0.12)
                                border.color: Qt.rgba(1, 1, 1, 0.22); border.width: 1
                            }
                            PlayingCard {
                                visible: index < bd.length
                                s: 50 / 46
                                card: index < bd.length ? bd[index] : -1
                            }
                        }
                    }
                }

                // Pot.
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: (st.pot || 0) > 0
                    width: potRow.implicitWidth + 24; height: 30; radius: 15
                    color: Qt.rgba(0, 0, 0, 0.35)
                    Row {
                        id: potRow
                        anchors.centerIn: parent
                        spacing: 6
                        Text {
                            text: "POT"; color: root.cGold
                            font.pixelSize: 11; font.bold: true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        ChipAmount { amount: st.pot || 0; chip: "#2f6fd1" }
                    }
                }

                // Shuffle/lock progress and lobby hints.
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    horizontalAlignment: Text.AlignHCenter
                    visible: text !== ""
                    color: root.cText; opacity: 0.85; font.pixelSize: 13
                    text: {
                        var p = root.protoLabel(st.proto)
                        if (p) return "🔐 " + p
                        if (!st.proto || st.proto === "lobby") {
                            if ((st.status || 0) === 0) return "Start the network, then join the table"
                            if (!(st.joined === true)) return "Enter a name and join the table"
                            if ((st.players || 0) < 2) return "Waiting for another player…"
                            return st.isCoordinator ? "Everyone's here. Deal when ready"
                                                    : "Waiting for the host to deal…"
                        }
                        return ""
                    }
                }

                // Winner banner.
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: st.lastWinner !== undefined && st.proto === "done"
                    width: winText.implicitWidth + 32; height: 36; radius: 18
                    color: Qt.rgba(0, 0, 0, 0.45)
                    border.color: root.cGold; border.width: 1
                    Text {
                        id: winText
                        anchors.centerIn: parent
                        color: root.cGold; font.pixelSize: 14; font.bold: true
                        text: st.lastWinner
                              ? ("🏆 " + (st.lastWinner.names ? st.lastWinner.names.join(", ") : "")
                                 + " wins " + (st.lastWinner.amount || 0)
                                 + " · " + (st.lastWinner.category || ""))
                              : ""
                    }
                }
            }

            // ── Seats around the rail (you at the bottom) ──
            Repeater {
                model: root.seats
                delegate: Item {
                    id: seatItem
                    property var seat: modelData
                    readonly property int n: Math.max(root.seats.length, 1)
                    readonly property real angle: Math.PI / 2 + 2 * Math.PI * (index - root.myIndex) / n
                    readonly property real cx: rail.x + rail.width / 2
                    readonly property real cy: rail.y + rail.height / 2
                    // Seats sit just outside the rail so they never cover the board.
                    readonly property real px: cx + Math.cos(angle) * (rail.width / 2 + 22)
                    readonly property real py: cy + Math.sin(angle) * (rail.height / 2 + 44)
                    readonly property bool dealtIn: (seat.inHand && root.inHandPhase)
                                                    || (!!seat.hole && seat.hole.length === 2)
                    width: 170; height: 118
                    x: px - width / 2
                    y: py - height / 2
                    z: 2

                    // Bet in front of the seat, pushed toward the centre.
                    ChipAmount {
                        visible: seatItem.seat.committed > 0
                        amount: seatItem.seat.committed
                        x: seatItem.width / 2 - width / 2 + (seatItem.cx - seatItem.px) * 0.34
                        y: seatItem.height / 2 - height / 2 + (seatItem.cy - seatItem.py) * 0.48
                    }

                    // Hole cards, fanned.
                    Item {
                        id: holeCards
                        width: 90; height: 64
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        visible: seatItem.dealtIn && !seatItem.seat.folded
                        property var hole: seatItem.seat.hole ? seatItem.seat.hole : []
                        PlayingCard {
                            s: seatItem.seat.isMe ? 0.95 : 0.8
                            x: 10; y: 2; rotation: -7
                            card: holeCards.hole.length === 2 ? holeCards.hole[0] : -1
                        }
                        PlayingCard {
                            s: seatItem.seat.isMe ? 0.95 : 0.8
                            x: 38; y: 2; rotation: 7
                            card: holeCards.hole.length === 2 ? holeCards.hole[1] : -1
                        }
                    }

                    // Name plate.
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        width: 164; height: 52; radius: 26
                        color: root.cPlate
                        opacity: seatItem.seat.folded ? 0.55 : 1.0
                        border.width: seatItem.seat.isToAct ? 3 : (seatItem.seat.isMe ? 2 : 1)
                        border.color: seatItem.seat.isToAct ? root.cGold
                                    : seatItem.seat.isMe ? "#5fbf8a" : "#33413a"

                        // Avatar.
                        Rectangle {
                            x: 6; anchors.verticalCenter: parent.verticalCenter
                            width: 40; height: 40; radius: 20
                            color: seatItem.seat.isMe ? "#2f7d57" : "#3a4a8a"
                            Text {
                                anchors.centerIn: parent
                                text: (seatItem.seat.name || "?").charAt(0).toUpperCase()
                                color: "white"; font.pixelSize: 17; font.bold: true
                            }
                        }
                        Column {
                            x: 52; anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 60
                            spacing: 1
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: seatItem.seat.name + (seatItem.seat.isMe ? " (you)" : "")
                                color: root.cText; font.pixelSize: 13; font.bold: true
                            }
                            Text {
                                text: seatItem.seat.folded ? "Folded"
                                    : seatItem.seat.allIn  ? "ALL-IN"
                                    : "◉ " + seatItem.seat.chips
                                color: seatItem.seat.allIn ? root.cGold : root.cMuted
                                font.pixelSize: 12
                            }
                        }

                        // Dealer button.
                        Rectangle {
                            visible: !!seatItem.seat.isButton
                            anchors.right: parent.right; anchors.top: parent.top
                            anchors.rightMargin: -6; anchors.topMargin: -8
                            width: 22; height: 22; radius: 11
                            color: "white"; border.color: "#bbbbbb"; border.width: 1
                            Text {
                                anchors.centerIn: parent; text: "D"
                                color: "#111111"; font.pixelSize: 11; font.bold: true
                            }
                        }
                    }
                }
            }

            } // tableRegion

            // ── Hand-rankings drawer ──
            Rectangle {
                id: drawer
                width: 300
                anchors.top: parent.top; anchors.bottom: parent.bottom
                x: root.showRankings ? parent.width - width : parent.width
                visible: x < parent.width
                z: 10
                color: "#131a16"
                border.color: "#2a3a31"; border.width: 1
                Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

                Flickable {
                    anchors.fill: parent
                    anchors.margins: 14
                    contentHeight: sheet.implicitHeight
                    clip: true

                    Column {
                        id: sheet
                        width: parent.width
                        spacing: 8

                        RowLayout {
                            width: parent.width
                            Text {
                                Layout.fillWidth: true
                                text: "HAND RANKINGS"
                                color: root.cText; font.pixelSize: 18; font.bold: true
                                font.letterSpacing: 1
                            }
                            Text {
                                text: "✕"; color: root.cMuted; font.pixelSize: 16
                                MouseArea {
                                    anchors.fill: parent; anchors.margins: -8
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.showRankings = false
                                }
                            }
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: "Best hand first. Faded cards are kickers that don't make the hand."
                            color: root.cMuted; font.pixelSize: 11
                        }

                        Repeater {
                            model: root.rankings
                            delegate: Rectangle {
                                id: rankRow
                                readonly property int usedCount: modelData.used
                                readonly property bool isWin: st.proto === "done" && !!st.lastWinner
                                                             && modelData.key !== ""
                                                             && st.lastWinner.category === modelData.key
                                width: sheet.width
                                height: 96
                                radius: 10
                                color: index === 0 ? "#1d6b45" : (isWin ? "#3a3218" : "#19221e")
                                border.color: isWin ? root.cGold : "transparent"
                                border.width: isWin ? 2 : 0

                                Text {
                                    x: 10; y: 7
                                    text: (index + 1) + ". " + modelData.name.toUpperCase()
                                    color: root.cText; font.pixelSize: 12; font.bold: true
                                    font.letterSpacing: 0.5
                                }
                                Row {
                                    x: 10; y: 30
                                    spacing: 6
                                    Repeater {
                                        model: modelData.cards
                                        delegate: PlayingCard {
                                            s: 0.84
                                            card: root.cardId(modelData)
                                            dim: index >= rankRow.usedCount
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Action bar ──
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 70
            color: root.cBar

            // My turn → betting controls.
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18; anchors.rightMargin: 18
                spacing: 10
                visible: st.myTurn === true

                Text {
                    text: "Your move"
                    color: root.cGold; font.pixelSize: 14; font.bold: true
                }
                Item { Layout.fillWidth: true }
                PillButton {
                    tint: "#7a2a24"
                    label: "Fold"
                    onClicked: root.callPokerArgs("act", ["fold", 0])
                }
                PillButton {
                    tint: "#2d5f8f"
                    label: (st.toCall || 0) > 0 ? ("Call " + st.toCall) : "Check"
                    onClicked: root.callPokerArgs("act", [(st.toCall || 0) > 0 ? "call" : "check", 0])
                }
                Rectangle {
                    implicitWidth: 150; implicitHeight: 38; radius: 19
                    color: "#0f1512"; border.color: "#33413a"; border.width: 1
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                        spacing: 2
                        Text {
                            text: "−"; color: root.cText; font.pixelSize: 18
                            MouseArea {
                                anchors.fill: parent; anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: raiseField.text = "" + Math.max(st.minRaise || 10,
                                                   (parseInt(raiseField.text) || 0) - (st.minRaise || 10))
                            }
                        }
                        TextInput {
                            id: raiseField
                            Layout.fillWidth: true
                            horizontalAlignment: TextInput.AlignHCenter
                            text: "" + (st.minRaise || 10)
                            color: root.cText; font.pixelSize: 14; font.bold: true
                            selectByMouse: true
                            inputMethodHints: Qt.ImhDigitsOnly
                            validator: IntValidator { bottom: 1 }
                        }
                        Text {
                            text: "+"; color: root.cText; font.pixelSize: 18
                            MouseArea {
                                anchors.fill: parent; anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: raiseField.text = "" + ((parseInt(raiseField.text) || 0) + (st.minRaise || 10))
                            }
                        }
                    }
                }
                PillButton {
                    tint: "#8a6d2b"
                    label: (st.currentBet || 0) > 0 ? "Raise" : "Bet"
                    onClicked: {
                        var amt = parseInt(raiseField.text)
                        if (!isNaN(amt) && amt > 0) root.callPokerArgs("act", ["raise", amt])
                    }
                }
            }

            // Otherwise → status + deal.
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18; anchors.rightMargin: 18
                spacing: 10
                visible: st.myTurn !== true

                Text {
                    Layout.fillWidth: true
                    color: root.cMuted; font.pixelSize: 13
                    elide: Text.ElideRight
                    text: st.leaving === true ? "Leaving after this hand. Folding automatically…"
                        : (st.proto === "play" && st.toActId) ? ("Waiting for " + root.nameOf(st.toActId) + "…")
                        : (st.joined === true) ? ((st.players || 0) + " player(s) seated")
                        : "Not seated"
                }
                PillButton {
                    visible: st.isCoordinator === true && st.joined === true
                    enabled: (st.players || 0) >= 2 && (st.proto === "lobby" || st.proto === "done")
                    tint: root.cGreen
                    label: st.proto === "done" ? "Deal next hand" : "Deal hand"
                    onClicked: { root.callPoker("startHand"); root.refresh() }
                }
            }
        }
    }

    Timer { interval: 1000; running: true; repeat: true; onTriggered: root.refresh() }
    Component.onCompleted: refresh()
}
