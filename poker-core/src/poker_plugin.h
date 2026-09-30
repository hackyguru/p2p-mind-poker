#ifndef POKER_PLUGIN_H
#define POKER_PLUGIN_H

#include <QObject>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QSet>
#include <QVariant>
#include <QJsonObject>

#include <map>
#include <memory>
#include <vector>

#include "poker_interface.h"
#include "poker_crypto.h"
#include "poker_game.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_object.h"
#include "logos_sdk.h"

// ─────────────────────────────────────────────────────────────────────────
// Trustless multiplayer Texas Hold'em over logos-delivery.
//
// Delivery wiring is lifted from part6 shared_color (createNode / start /
// subscribe, the connection-state race handling, the Instance-A/B static-node
// trick for two peers on one machine). On top of it sits a mental-poker (SRA)
// protocol that shuffles and deals without any trusted dealer — see
// poker_crypto.h for the cryptography and poker_game.h for the betting/eval
// engine. This class is the orchestrator that drives the protocol forward as
// messages arrive.
//
// Wire messages (all JSON on /p2p-poker/1/table/json), discriminated by "type":
//   join     {id, name}
//   start    {handId, players[], chips[], button}      — coordinator kicks a hand off
//   shuffle  {handId, step, deck[]}                     — encrypt+shuffle pass
//   lock     {handId, step, deck[]}                     — per-card re-encrypt pass
//   key      {handId, pos, seat, key}                   — a per-position decrypt share
//   action   {handId, seat, kind, amount}               — a betting action
// Every message also carries "mid" (sender id + counter) for dedup.
// ─────────────────────────────────────────────────────────────────────────

class PokerPlugin : public QObject, public PokerInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PokerInterface_iid FILE "metadata.json")
    Q_INTERFACES(PokerInterface PluginInterface)

public:
    explicit PokerPlugin(QObject* parent = nullptr);
    ~PokerPlugin() override;

    QString name() const override { return "poker"; }
    QString version() const override { return "0.1.0"; }

    Q_INVOKABLE void initLogos(LogosAPI* api);

    Q_INVOKABLE bool startDelivery() override;
    Q_INVOKABLE bool stopDelivery() override;
    Q_INVOKABLE int  deliveryStatus() override;

    Q_INVOKABLE bool    joinTable(const QString& name) override;
    Q_INVOKABLE bool    startHand() override;
    Q_INVOKABLE bool    act(const QString& kind, int amount) override;
    Q_INVOKABLE QString tableState() override;
    Q_INVOKABLE QString myId() override;

signals:
    void eventResponse(const QString& eventName, const QVariantList& args);

private:
    // ── delivery (mirrors shared_color) ──
    void handleMessageReceived(const QVariantList& data);
    void setDeliveryStatus(int status);
    bool invokeBool(const char* what, const QString& method, const QVariant& arg = QVariant());

    // ── protocol orchestration ──
    void onMessage(const QJsonObject& o);
    void ingestHandMessage(const QJsonObject& o);   // apply a message for m_handId (no progress)
    void adoptHand(const QJsonObject& o);
    void resetHandState();
    void progressProtocol();
    void performMyShuffleStep();
    void performMyLockStep();
    void publishDealKeys();
    void decodeMyHoles();
    void handleBetting();
    bool revealStreet(int stage);          // 0=flop 1=turn 2=river; true if street advanced
    void publishBoardKeys(int stage);
    void enterShowdown();
    void tryShowdown();
    void resolveByFold();
    void recordWinner(const std::vector<int>& winners, bool byFold);
    int  decodeCard(int pos);
    std::vector<int> stagePositions(int stage) const;

    // ── messaging helpers ──
    void    publish(QJsonObject obj);      // stamps mid, dedups self-echo, defers send
    void    sendKey(int pos);
    void    announceJoin();                // (re)broadcast our seat — joins are fire-and-forget
    QString midNext();

    // NB: do NOT redeclare `logosAPI` here. PluginInterface already provides a
    // public `LogosAPI* logosAPI` and the host reads *that* member to decide
    // whether the module is wired up. A local field of the same name shadows
    // it, so initLogos() would fill the shadow while the base stayed null and
    // every call in would be refused with "QtProviderObject::callMethod:
    // LogosAPI not available".
    LogosAPIClient* m_deliveryClient = nullptr;
    LogosObject*    m_deliveryObject = nullptr;
    int             m_deliveryStatus = 0;
    bool            m_started        = false;
    bool            m_subscribed     = false;
    bool            m_createNodeDone = false;

    // identity / lobby
    QString          m_myId;
    QString          m_myName;
    quint64          m_seq         = 0;
    int              m_handCounter = 0;
    int              m_lastButton  = -1;
    QSet<QString>    m_seen;            // seen message ids (dedup)
    QMap<QString, QString> m_joined;    // id -> display name
    QTimer*          m_announceTimer = nullptr;  // periodic join re-broadcast

    // current hand
    enum class Proto { Lobby, Shuffle, Lock, Deal, Play, Done };
    Proto                          m_proto = Proto::Lobby;
    QString                        m_handId;
    QSet<QString>                  m_pastHands;      // hands already adopted
    QList<QJsonObject>             m_early;          // messages that overtook their "start"
    QStringList                    m_participants;   // crypto seat order (== table seat order)
    int                            m_N      = 0;
    int                            m_mySeat = -1;    // my index in m_participants, -1 = spectator
    std::unique_ptr<poker::SraKeyset> m_keys;
    std::vector<std::string>       m_deck;           // evolving encrypted deck
    std::map<int, std::map<int, std::string>> m_keyShares;  // pos -> seat -> decrypt key
    std::map<int, std::vector<std::string>>   m_shufflePending;
    std::map<int, std::vector<std::string>>   m_lockPending;
    int   m_shuffleStep      = 0;
    int   m_lockStep         = 0;
    bool  m_shufflePerformed = false;
    bool  m_lockPerformed    = false;
    bool  m_dealKeysPublished = false;
    bool  m_boardPublished[3] = {false, false, false};
    bool  m_boardAdvanced[3]  = {false, false, false};
    std::vector<int> m_boardCards;
    bool  m_enteredShowdown  = false;
    bool  m_showdownPublished = false;
    bool  m_handResolved     = false;

    poker::PokerTable m_table;
    QJsonObject       m_lastWinner;
    bool              m_haveLastWinner = false;
};

#endif // POKER_PLUGIN_H
