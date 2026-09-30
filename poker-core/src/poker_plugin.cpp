#include "poker_plugin.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_object.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <random>
#include <utility>

using poker::Phase;
using poker::Seat;
using poker::SraKeyset;

// Single shared table topic — every peer joins the same one (LIP-23).
static const QString TOPIC = "/p2p-poker/1/table/json";

// ── helpers ────────────────────────────────────────────────────────────────

namespace {

QJsonArray deckToJson(const std::vector<std::string>& deck)
{
    QJsonArray a;
    for (const std::string& s : deck) a.append(QString::fromStdString(s));
    return a;
}

std::vector<std::string> jsonToDeck(const QJsonArray& a)
{
    std::vector<std::string> v;
    v.reserve(a.size());
    for (const QJsonValue& x : a) v.push_back(x.toString().toStdString());
    return v;
}

} // namespace

PokerPlugin::PokerPlugin(QObject* parent)
    : QObject(parent)
    , m_myId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    qDebug() << "PokerPlugin: created, myId =" << m_myId;
}

PokerPlugin::~PokerPlugin() = default;

void PokerPlugin::initLogos(LogosAPI* api)
{
    logosAPI = api;
    qDebug() << "PokerPlugin: LogosAPI wired up";
}

// ── Delivery lifecycle (mirrors part6 shared_color) ──────────────────────────

bool PokerPlugin::startDelivery()
{
    if (m_started) return true;
    setDeliveryStatus(1);   // before start() — connectionStateChanged may fire inside it

    m_deliveryClient = logosAPI->getClient("delivery_module");
    if (!m_deliveryClient) {
        qWarning() << "PokerPlugin: delivery_module client unavailable";
        setDeliveryStatus(3);
        return false;
    }

    if (!m_createNodeDone) {
        // Deterministic Instance-A/B keys + staticNodes so two peers on one box
        // dial each other directly over loopback (the logos.dev bootstrap peers
        // don't currently handshake). POKER_TCPPORT unset → A; set → B.
        const int  customPort  = qEnvironmentVariableIntValue("POKER_TCPPORT");
        const bool isInstanceB = (customPort > 0);
        const int  tcpPort     = isInstanceB ? customPort : 60000;
        const int  udpPort     = isInstanceB ? 9000 + (tcpPort - 60000) : 9000;

        static const QString KEY_A    = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
        static const QString KEY_B    = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f21";
        static const QString PEERID_A = "16Uiu2HAm4Ms862Gnqafssgvik4JJ1LuqWMcKNipq4nm2UaoLRbeP";
        // PEERID_A/B are the libp2p identities delivery_module derives from the
        // nodeKeys above. Verified against a live run: instance A announces
        // …oLRbeP and instance B announces …u8AMiG. (The value part6 carries for
        // B is stale — dialing it fails the noise handshake, so only B→A ever
        // connected and the pair depended on B starting second.)
        static const QString PEERID_B = "16Uiu2HAmKCaJ7sfcm1aHY8TAShdsKttUHvDwTPbgTkJhLQu8AMiG";

        const QString nodeKey = isInstanceB ? KEY_B : KEY_A;
        const QString peerMultiAddr = isInstanceB
            ? QString("/ip4/127.0.0.1/tcp/60000/p2p/%1").arg(PEERID_A)
            : QString("/ip4/127.0.0.1/tcp/60001/p2p/%1").arg(PEERID_B);

        QJsonObject cfgObj;
        cfgObj["logLevel"]      = "INFO";
        cfgObj["mode"]          = "Core";
        cfgObj["preset"]        = "logos.dev";
        cfgObj["relay"]         = true;   // gossipsub relay — required for same-shard delivery
        cfgObj["tcpPort"]       = tcpPort;
        cfgObj["discv5UdpPort"] = udpPort;
        cfgObj["nodeKey"]       = nodeKey;
        cfgObj["staticNodes"]   = QJsonArray{ peerMultiAddr };

        const QString cfg = QString::fromUtf8(QJsonDocument(cfgObj).toJson(QJsonDocument::Compact));
        if (!invokeBool("createNode", "createNode", cfg)) { setDeliveryStatus(3); return false; }
        m_createNodeDone = true;
    }

    // Register handlers BEFORE start() so we don't miss the first events.
    m_deliveryObject = m_deliveryClient->requestObject("delivery_module");
    if (m_deliveryObject) {
        m_deliveryClient->onEvent(m_deliveryObject, "messageReceived",
            [this](const QString&, const QVariantList& data) { handleMessageReceived(data); });

        m_deliveryClient->onEvent(m_deliveryObject, "connectionStateChanged",
            [this](const QString&, const QVariantList& data) {
                if (data.isEmpty()) return;
                const QString status = data[0].toString();
                if (status.contains("Connected", Qt::CaseInsensitive)) setDeliveryStatus(2);
                else if (!status.isEmpty())                            setDeliveryStatus(1);
            });

        m_deliveryClient->onEvent(m_deliveryObject, "messageError",
            [](const QString&, const QVariantList& data) {
                if (data.size() >= 3) qWarning() << "poker: delivery send error:" << data[2];
            });
    } else {
        qWarning() << "PokerPlugin: no delivery_module object — events will be missed";
    }

    if (!invokeBool("start", "start")) { setDeliveryStatus(3); return false; }

    // Subscribe synchronously right after start (matches shared_color: avoids the
    // asymmetric gossipsub mesh bug from deferring subscribe).
    if (!invokeBool("subscribe", "subscribe", TOPIC)) { setDeliveryStatus(3); return false; }
    m_subscribed = true;
    m_started    = true;

    if (m_deliveryStatus < 2) setDeliveryStatus(2);   // optimistic flip for solo runs
    return true;
}

bool PokerPlugin::stopDelivery()
{
    if (!m_started) return true;
    if (m_announceTimer) m_announceTimer->stop();
    if (m_deliveryClient) {
        if (m_subscribed) {
            m_deliveryClient->invokeRemoteMethod("delivery_module", "unsubscribe", TOPIC);
            m_subscribed = false;
        }
        invokeBool("stop", "stop");
    }
    m_deliveryObject = nullptr;
    m_started        = false;
    setDeliveryStatus(0);
    return true;
}

int PokerPlugin::deliveryStatus() { return m_deliveryStatus; }
QString PokerPlugin::myId()       { return m_myId; }

// ── Public table API ─────────────────────────────────────────────────────────

bool PokerPlugin::joinTable(const QString& name)
{
    m_myName = name.trimmed().isEmpty() ? ("Player-" + m_myId.left(4)) : name.trimmed();
    if (!m_started && !startDelivery()) return false;

    m_joined.insert(m_myId, m_myName);
    m_table.upsertSeat(m_myId.toStdString(), m_myName.toStdString());

    announceJoin();

    // A join published before the other peer's node is up is simply lost —
    // relay has no history — so keep re-announcing while we sit at the table.
    if (!m_announceTimer) {
        m_announceTimer = new QTimer(this);
        m_announceTimer->setInterval(5000);
        connect(m_announceTimer, &QTimer::timeout, this, [this]() {
            if (m_started && m_joined.contains(m_myId)) announceJoin();
        });
    }
    m_announceTimer->start();

    emit eventResponse("tableChanged", {});
    return true;
}

void PokerPlugin::announceJoin()
{
    QJsonObject o;
    o["type"] = "join";
    o["id"]   = m_myId;
    o["name"] = m_myName;
    publish(o);
}

bool PokerPlugin::startHand()
{
    if (!m_started) return false;

    QStringList players = m_joined.keys();
    players.sort();
    if (players.size() < 2) {
        qWarning() << "PokerPlugin: need >= 2 players to start";
        return false;
    }
    if ((2 * players.size() + 5) > poker::kDeckSize) {
        qWarning() << "PokerPlugin: too many players for one deck";
        return false;
    }
    // Only the coordinator (lowest id) may kick off a hand — avoids two peers
    // racing to start different hands.
    if (m_myId != players.first()) {
        qWarning() << "PokerPlugin: not coordinator; only" << players.first() << "can start";
        return false;
    }

    const QString hid = m_myId + "#" + QString::number(++m_handCounter);
    const int button  = (m_lastButton + 1) % players.size();

    QJsonObject o;
    o["type"]   = "start";
    o["handId"] = hid;
    QJsonArray pj, cj;
    for (const QString& id : players) {
        pj.append(id);
        const int idx = m_table.seatIndex(id.toStdString());
        const long chips = (idx >= 0) ? m_table.seats()[idx].chips : poker::kStartingChips;
        cj.append(static_cast<double>(chips));
    }
    o["players"] = pj;
    o["chips"]   = cj;
    o["button"]  = button;

    publish(o);            // others adopt
    adoptHand(o);          // adopt locally (publish self-echo is deduped)
    m_lastButton = button;
    return true;
}

bool PokerPlugin::act(const QString& kind, int amount)
{
    if (m_proto != Proto::Play) return false;
    const int idx = m_table.seatIndex(m_myId.toStdString());
    if (idx < 0 || idx != m_table.toAct()) return false;
    if (!m_table.applyAction(m_myId.toStdString(), kind.toStdString(), amount)) return false;

    QJsonObject o;
    o["type"]   = "action";
    o["handId"] = m_handId;
    o["seat"]   = m_mySeat;
    o["kind"]   = kind;
    o["amount"] = amount;
    publish(o);
    progressProtocol();
    emit eventResponse("tableChanged", {});
    return true;
}

// ── Inbound dispatch ─────────────────────────────────────────────────────────

void PokerPlugin::handleMessageReceived(const QVariantList& data)
{
    // delivery_module.messageReceived: [hash, contentTopic, payload, ts_ns]
    if (data.size() < 3) return;
    if (data[1].toString() != TOPIC) return;

    // delivery_module ≥0.2.0 emits the payload as raw bytes (QByteArray, or a
    // QVariantList of byte values over IPC); older builds sent base64 text.
    // Our messages are always JSON objects, so sniff the leading brace.
    QByteArray payload;
    if (data[2].userType() == QMetaType::QVariantList) {
        for (const QVariant& b : data[2].toList())
            payload.append(char(b.toUInt()));
    } else {
        payload = data[2].toByteArray();
    }
    if (!payload.startsWith('{'))
        payload = QByteArray::fromBase64(payload);

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    onMessage(doc.object());
}

void PokerPlugin::onMessage(const QJsonObject& o)
{
    const QString mid = o.value("mid").toString();
    if (!mid.isEmpty()) {
        if (m_seen.contains(mid)) return;   // dedup (incl. our own echo)
        m_seen.insert(mid);
    }

    const QString type = o.value("type").toString();

    if (type == "join") {
        const QString id   = o.value("id").toString();
        const QString name = o.value("name").toString();
        if (!id.isEmpty()) {
            const bool isNew = !m_joined.contains(id);
            m_joined.insert(id, name);
            // A new seat re-sorts the table and would shift toAct/button indices
            // mid-hand; newcomers get seated when the next hand is adopted.
            const bool midHand = m_proto != Proto::Lobby && m_proto != Proto::Done;
            if (!isNew || !midHand)
                m_table.upsertSeat(id.toStdString(), name.toStdString());
            // Greet a newcomer right away so they don't wait for our next heartbeat.
            if (isNew && id != m_myId && m_joined.contains(m_myId)) announceJoin();
        }
        emit eventResponse("tableChanged", {});
        return;
    }

    if (type == "start") {
        adoptHand(o);
        return;
    }

    // Everything below is scoped to a hand. Relay doesn't preserve order — a
    // 27 KB shuffle can overtake the small "start" that precedes it — so park
    // messages for a hand we haven't adopted yet and replay them in adoptHand.
    const QString hid = o.value("handId").toString();
    if (hid != m_handId) {
        if (!hid.isEmpty() && !m_pastHands.contains(hid) && m_early.size() < 2000)
            m_early.append(o);
        return;
    }

    ingestHandMessage(o);
    progressProtocol();
    emit eventResponse("tableChanged", {});
}

void PokerPlugin::ingestHandMessage(const QJsonObject& o)
{
    const QString type = o.value("type").toString();
    if (type == "shuffle") {
        m_shufflePending[o.value("step").toInt()] = jsonToDeck(o.value("deck").toArray());
    } else if (type == "lock") {
        m_lockPending[o.value("step").toInt()] = jsonToDeck(o.value("deck").toArray());
    } else if (type == "key") {
        const int pos  = o.value("pos").toInt();
        const int seat = o.value("seat").toInt();
        m_keyShares[pos][seat] = o.value("key").toString().toStdString();
    } else if (type == "action") {
        const int seat = o.value("seat").toInt();
        if (seat >= 0 && seat < m_participants.size()) {
            const QString id = m_participants[seat];
            m_table.applyAction(id.toStdString(),
                                 o.value("kind").toString().toStdString(),
                                 o.value("amount").toInt());
        }
    }
}

// ── Hand setup ───────────────────────────────────────────────────────────────

void PokerPlugin::adoptHand(const QJsonObject& o)
{
    resetHandState();

    m_handId = o.value("handId").toString();
    m_pastHands.insert(m_handId);
    m_participants.clear();
    const QJsonArray pj = o.value("players").toArray();
    const QJsonArray cj = o.value("chips").toArray();
    for (int i = 0; i < pj.size(); ++i) {
        const QString id = pj[i].toString();
        m_participants.append(id);
        const QString nm = m_joined.value(id, id);
        m_table.upsertSeat(id.toStdString(), nm.toStdString());
        if (i < cj.size())
            m_table.setChips(id.toStdString(), static_cast<long>(cj[i].toDouble()));
    }
    m_N      = m_participants.size();
    m_mySeat = m_participants.indexOf(m_myId);

    m_keys = std::make_unique<SraKeyset>();
    m_deck = SraKeyset::cardCodes();          // plaintext codes — seat 0 encrypts first

    if (!m_table.startHand(o.value("button").toInt())) {
        qWarning() << "PokerPlugin: startHand rejected (insufficient funded players)";
        m_proto = Proto::Lobby;
        return;
    }
    m_proto = Proto::Shuffle;

    // Replay anything for this hand that overtook its "start"; drop the rest
    // (they belong to hands we'll never adopt now).
    const QList<QJsonObject> early = std::exchange(m_early, {});
    for (const QJsonObject& e : early)
        if (e.value("handId").toString() == m_handId) ingestHandMessage(e);

    progressProtocol();
    emit eventResponse("tableChanged", {});
}

void PokerPlugin::resetHandState()
{
    m_deck.clear();
    m_keyShares.clear();
    m_shufflePending.clear();
    m_lockPending.clear();
    m_shuffleStep = 0;
    m_lockStep    = 0;
    m_shufflePerformed = false;
    m_lockPerformed    = false;
    m_dealKeysPublished = false;
    for (int i = 0; i < 3; ++i) { m_boardPublished[i] = false; m_boardAdvanced[i] = false; }
    m_boardCards.clear();
    m_enteredShowdown   = false;
    m_showdownPublished = false;
    m_handResolved      = false;
    m_keys.reset();
}

// ── Protocol engine ──────────────────────────────────────────────────────────

void PokerPlugin::progressProtocol()
{
    if (m_proto == Proto::Shuffle) {
        bool changed = true;
        while (changed) {
            changed = false;
            while (m_shufflePending.count(m_shuffleStep)) {
                m_deck = m_shufflePending[m_shuffleStep];
                m_shufflePending.erase(m_shuffleStep);
                ++m_shuffleStep;
                changed = true;
            }
            if (m_shuffleStep < m_N && m_mySeat == m_shuffleStep && !m_shufflePerformed) {
                performMyShuffleStep();
                changed = true;
            }
        }
        if (m_shuffleStep >= m_N) m_proto = Proto::Lock;
    }

    if (m_proto == Proto::Lock) {
        bool changed = true;
        while (changed) {
            changed = false;
            while (m_lockPending.count(m_lockStep)) {
                m_deck = m_lockPending[m_lockStep];
                m_lockPending.erase(m_lockStep);
                ++m_lockStep;
                changed = true;
            }
            if (m_lockStep < m_N && m_mySeat == m_lockStep && !m_lockPerformed) {
                performMyLockStep();
                changed = true;
            }
        }
        if (m_lockStep >= m_N) m_proto = Proto::Deal;
    }

    if (m_proto == Proto::Deal) {
        publishDealKeys();
        m_proto = Proto::Play;
    }

    if (m_proto == Proto::Play) {
        decodeMyHoles();
        handleBetting();
    }
}

void PokerPlugin::performMyShuffleStep()
{
    std::vector<std::string> enc;
    enc.reserve(m_deck.size());
    for (const std::string& v : m_deck) enc.push_back(m_keys->encryptShuffle(v));

    // Secret shuffle — every peer reorders with its own entropy.
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(enc.begin(), enc.end(), g);

    m_deck = enc;
    m_shufflePerformed = true;
    ++m_shuffleStep;

    QJsonObject o;
    o["type"]   = "shuffle";
    o["handId"] = m_handId;
    o["step"]   = m_mySeat;
    o["deck"]   = deckToJson(enc);
    publish(o);
}

void PokerPlugin::performMyLockStep()
{
    // Remove our whole-deck key, re-encrypt each position with its per-card key.
    // No reshuffle here: positions are now fixed and shared.
    std::vector<std::string> out;
    out.reserve(m_deck.size());
    for (int j = 0; j < static_cast<int>(m_deck.size()); ++j) {
        std::string v = m_keys->decryptShuffle(m_deck[j]);
        v = m_keys->encryptCard(j, v);
        out.push_back(v);
    }
    m_deck = out;
    m_lockPerformed = true;
    ++m_lockStep;

    QJsonObject o;
    o["type"]   = "lock";
    o["handId"] = m_handId;
    o["step"]   = m_mySeat;
    o["deck"]   = deckToJson(out);
    publish(o);
}

void PokerPlugin::publishDealKeys()
{
    if (m_dealKeysPublished) return;
    m_dealKeysPublished = true;
    if (m_mySeat < 0) return;   // spectator has no keys

    // Locally we know our own decrypt key for every position (needed to read our
    // own hole cards and, later, the board).
    const int used = 2 * m_N + 5;
    for (int p = 0; p < used; ++p)
        m_keyShares[p][m_mySeat] = m_keys->cardDecryptKeyHex(p);

    // Publish our keys for every *other* player's hole positions so they can read
    // their own cards. We withhold our own holes (and the board, until its street).
    for (int s = 0; s < m_N; ++s) {
        if (s == m_mySeat) continue;
        sendKey(2 * s);
        sendKey(2 * s + 1);
    }
}

void PokerPlugin::decodeMyHoles()
{
    if (m_mySeat < 0) return;
    const int idx = m_table.seatIndex(m_myId.toStdString());
    if (idx < 0 || m_table.seats()[idx].hole0 >= 0) return;   // already known
    const int c0 = decodeCard(2 * m_mySeat);
    const int c1 = decodeCard(2 * m_mySeat + 1);
    if (c0 >= 0 && c1 >= 0) m_table.setHole(m_myId.toStdString(), c0, c1);
}

void PokerPlugin::handleBetting()
{
    while (true) {
        if (m_proto == Proto::Done) return;

        if (m_table.phase() == Phase::Showdown) { tryShowdown(); return; }
        if (!m_table.roundComplete()) return;          // someone still to act

        if (m_table.liveCount() <= 1) { resolveByFold(); return; }

        const Phase ph = m_table.phase();
        const int stage = (ph == Phase::Preflop) ? 0
                        : (ph == Phase::Flop)    ? 1
                        : (ph == Phase::Turn)    ? 2
                        : -1;
        if (stage >= 0) {
            if (!revealStreet(stage)) return;          // waiting on key shares
            continue;                                   // advanced — maybe cascade (all-in)
        }
        // River betting complete → showdown.
        enterShowdown();
        continue;
    }
}

std::vector<int> PokerPlugin::stagePositions(int stage) const
{
    const int b = 2 * m_N;   // board cards start right after the hole cards
    if (stage == 0) return { b, b + 1, b + 2 };   // flop
    if (stage == 1) return { b + 3 };             // turn
    return { b + 4 };                              // river
}

bool PokerPlugin::revealStreet(int stage)
{
    publishBoardKeys(stage);
    if (m_boardAdvanced[stage]) return true;

    std::vector<int> cards;
    for (int p : stagePositions(stage)) {
        const int c = decodeCard(p);
        if (c < 0) return false;                  // not all keys in yet
        cards.push_back(c);
    }
    for (int c : cards) m_boardCards.push_back(c);
    m_table.setBoard(m_boardCards);
    m_table.advanceStreet();
    m_boardAdvanced[stage] = true;
    return true;
}

void PokerPlugin::publishBoardKeys(int stage)
{
    if (m_boardPublished[stage]) return;
    m_boardPublished[stage] = true;
    if (m_mySeat < 0) return;
    for (int p : stagePositions(stage)) sendKey(p);   // even folded players must reveal board keys
}

void PokerPlugin::enterShowdown()
{
    if (!m_enteredShowdown) {
        m_table.advanceStreet();      // River → Showdown (collects the river bets)
        m_enteredShowdown = true;
    }
    if (!m_showdownPublished) {
        m_showdownPublished = true;
        if (m_mySeat >= 0) {
            const int idx = m_table.seatIndex(m_myId.toStdString());
            // Only players still live reveal their holes; folded hands stay mucked.
            if (idx >= 0 && m_table.seats()[idx].inHand && !m_table.seats()[idx].folded) {
                sendKey(2 * m_mySeat);
                sendKey(2 * m_mySeat + 1);
            }
        }
    }
    tryShowdown();
}

void PokerPlugin::tryShowdown()
{
    if (m_handResolved) return;

    // Decode every live player's hole cards once all their key shares are in.
    for (int s = 0; s < m_N; ++s) {
        const QString id = m_participants[s];
        const int tidx = m_table.seatIndex(id.toStdString());
        if (tidx < 0) continue;
        const Seat& st = m_table.seats()[tidx];
        if (!st.inHand || st.folded || st.hole0 >= 0) continue;
        const int c0 = decodeCard(2 * s);
        const int c1 = decodeCard(2 * s + 1);
        if (c0 >= 0 && c1 >= 0) m_table.setHole(id.toStdString(), c0, c1);
    }

    int live = 0;
    bool allKnown = true;
    for (const Seat& st : m_table.seats()) {
        if (!st.inHand || st.folded) continue;
        ++live;
        if (st.hole0 < 0 || st.hole1 < 0) allKnown = false;
    }
    if (live == 0 || !allKnown) return;   // still waiting on reveals

    const std::vector<int> winners = m_table.showdownWinners();
    recordWinner(winners, /*byFold=*/false);
    m_table.endHand(winners);
    m_proto = Proto::Done;
    m_handResolved = true;
    emit eventResponse("tableChanged", {});
}

void PokerPlugin::resolveByFold()
{
    if (m_handResolved) return;
    std::vector<int> winners;
    for (int i = 0; i < m_table.seatCount(); ++i) {
        const Seat& s = m_table.seats()[i];
        if (s.inHand && !s.folded) winners.push_back(i);
    }
    recordWinner(winners, /*byFold=*/true);
    m_table.endHand(winners);
    m_proto = Proto::Done;
    m_handResolved = true;
    emit eventResponse("tableChanged", {});
}

void PokerPlugin::recordWinner(const std::vector<int>& winners, bool byFold)
{
    QJsonObject w;
    QJsonArray ids, names;
    const long amount = m_table.pot();   // captured before endHand zeroes it
    for (int i : winners) {
        ids.append(QString::fromStdString(m_table.seats()[i].id));
        names.append(QString::fromStdString(m_table.seats()[i].name));
    }
    QString category = byFold ? "(everyone else folded)" : "High Card";
    if (!byFold && !winners.empty()) {
        const Seat& s = m_table.seats()[winners[0]];
        const std::vector<int>& b = m_table.board();
        if (b.size() == 5 && s.hole0 >= 0 && s.hole1 >= 0) {
            const int cards[7] = { s.hole0, s.hole1, b[0], b[1], b[2], b[3], b[4] };
            category = QString::fromStdString(poker::handCategoryName(poker::evaluate7(cards)));
        }
    }
    w["ids"]      = ids;
    w["names"]    = names;
    w["amount"]   = static_cast<double>(amount);
    w["category"] = category;
    m_lastWinner     = w;
    m_haveLastWinner = true;
}

int PokerPlugin::decodeCard(int pos)
{
    auto it = m_keyShares.find(pos);
    if (it == m_keyShares.end()) return -1;
    if (static_cast<int>(it->second.size()) < m_N) return -1;     // need every seat's key
    if (pos < 0 || pos >= static_cast<int>(m_deck.size())) return -1;

    std::string v = m_deck[pos];
    for (const auto& kv : it->second) {
        v = SraKeyset::applyKey(v, kv.second);
        if (v.empty()) return -1;
    }
    return SraKeyset::cardIdForCode(v);
}

// ── tableState JSON for the UI ───────────────────────────────────────────────

QString PokerPlugin::tableState()
{
    QJsonObject st;
    st["status"]  = m_deliveryStatus;
    st["myId"]    = m_myId;
    st["myName"]  = m_myName;
    st["joined"]  = m_joined.contains(m_myId);
    st["players"] = m_joined.size();

    QStringList sorted = m_joined.keys();
    sorted.sort();
    st["isCoordinator"] = (!sorted.isEmpty() && sorted.first() == m_myId);

    const char* protoNames[] = { "lobby", "shuffle", "lock", "deal", "play", "done" };
    st["proto"] = QString::fromLatin1(protoNames[static_cast<int>(m_proto)]);

    QString phase = "idle";
    switch (m_table.phase()) {
        case Phase::Preflop:  phase = "preflop";  break;
        case Phase::Flop:     phase = "flop";     break;
        case Phase::Turn:     phase = "turn";     break;
        case Phase::River:    phase = "river";    break;
        case Phase::Showdown: phase = "showdown"; break;
        case Phase::HandOver: phase = "handover"; break;
        default:                                   break;
    }
    st["phase"]      = phase;
    st["handId"]     = m_handId;
    st["pot"]        = static_cast<double>(m_table.pot());
    st["currentBet"] = static_cast<double>(m_table.currentBet());
    st["minRaise"]   = static_cast<double>(m_table.minRaise());

    const int toAct = m_table.toAct();
    QString toActId;
    if (toAct >= 0 && toAct < m_table.seatCount())
        toActId = QString::fromStdString(m_table.seats()[toAct].id);
    st["toActId"] = toActId;

    const int myIdx = m_table.seatIndex(m_myId.toStdString());
    const bool myTurn = (m_proto == Proto::Play) && (toAct >= 0) && (toAct == myIdx);
    st["myTurn"] = myTurn;
    st["toCall"] = static_cast<double>(myIdx >= 0 ? m_table.toCall(myIdx) : 0);

    QJsonArray board;
    for (int c : m_table.board()) board.append(c);
    st["board"] = board;

    QJsonArray myHole;
    if (myIdx >= 0) {
        const Seat& s = m_table.seats()[myIdx];
        if (s.hole0 >= 0 && s.hole1 >= 0) { myHole.append(s.hole0); myHole.append(s.hole1); }
    }
    st["myHole"] = myHole;

    QJsonArray seats;
    for (int i = 0; i < m_table.seatCount(); ++i) {
        const Seat& s = m_table.seats()[i];
        QJsonObject so;
        so["id"]        = QString::fromStdString(s.id);
        so["name"]      = QString::fromStdString(s.name);
        so["chips"]     = static_cast<double>(s.chips);
        so["committed"] = static_cast<double>(s.committed);
        so["inHand"]    = s.inHand;
        so["folded"]    = s.folded;
        so["allIn"]     = s.allIn;
        so["isMe"]      = (s.id == m_myId.toStdString());
        so["isButton"]  = (m_proto != Proto::Lobby && i == m_table.button());
        so["isToAct"]   = (i == toAct);
        QJsonArray hole;
        // Reveal a seat's hole cards only to its owner, or once shown down.
        if (s.hole0 >= 0 && s.hole1 >= 0) { hole.append(s.hole0); hole.append(s.hole1); }
        so["hole"] = hole;
        seats.append(so);
    }
    st["seats"] = seats;

    if (m_haveLastWinner) st["lastWinner"] = m_lastWinner;

    return QString::fromUtf8(QJsonDocument(st).toJson(QJsonDocument::Compact));
}

// ── messaging helpers ────────────────────────────────────────────────────────

QString PokerPlugin::midNext()
{
    return m_myId + ":" + QString::number(++m_seq);
}

void PokerPlugin::publish(QJsonObject obj)
{
    const QString mid = midNext();
    obj["mid"] = mid;
    m_seen.insert(mid);   // pre-mark so our own gossipsub echo is ignored

    const QString payload = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));

    // Defer the send: publish() is frequently called from inside the
    // messageReceived handler, and invoking delivery_module synchronously from
    // there re-enters it and deadlocks (delivery-guide Gotcha #9).
    QTimer::singleShot(0, this, [this, payload]() {
        if (m_deliveryClient)
            m_deliveryClient->invokeRemoteMethod("delivery_module", "send", TOPIC, payload);
    });
}

void PokerPlugin::sendKey(int pos)
{
    if (m_mySeat < 0 || !m_keys) return;
    QJsonObject o;
    o["type"]   = "key";
    o["handId"] = m_handId;
    o["pos"]    = pos;
    o["seat"]   = m_mySeat;
    o["key"]    = QString::fromStdString(m_keys->cardDecryptKeyHex(pos));
    publish(o);
}

// ── delivery plumbing ────────────────────────────────────────────────────────

void PokerPlugin::setDeliveryStatus(int status)
{
    if (m_deliveryStatus == status) return;
    m_deliveryStatus = status;
    emit eventResponse("deliveryStatusChanged", QVariantList{ status });
}

bool PokerPlugin::invokeBool(const char* what, const QString& method, const QVariant& arg)
{
    const QVariant r = arg.isValid()
        ? m_deliveryClient->invokeRemoteMethod("delivery_module", method, arg)
        : m_deliveryClient->invokeRemoteMethod("delivery_module", method);
    if (!r.isValid()) {
        if (method == QStringLiteral("start")) {
            qDebug() << "PokerPlugin: start RPC returned void (v1.0.0); waiting on connectionStateChanged";
            return true;
        }
        qWarning() << "PokerPlugin:" << what << "RPC failed (invalid QVariant)";
        return false;
    }
    if (r.canConvert<LogosResult>()) {
        const LogosResult lr = r.value<LogosResult>();
        if (!lr.success) {
            qWarning() << "PokerPlugin:" << what << "failed:" << lr.error.toString();
            return false;
        }
        return true;
    }
    if (!r.toBool()) {
        qWarning() << "PokerPlugin:" << what << "returned false:" << r;
        return false;
    }
    return true;
}
