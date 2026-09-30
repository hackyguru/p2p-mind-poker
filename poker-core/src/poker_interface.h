#ifndef POKER_INTERFACE_H
#define POKER_INTERFACE_H

#include <QObject>
#include <QString>
#include "interface.h"

class PokerInterface : public PluginInterface
{
public:
    virtual ~PokerInterface() = default;

    // Delivery lifecycle — 0=off, 1=connecting, 2=connected, 3=error
    Q_INVOKABLE virtual bool startDelivery() = 0;
    Q_INVOKABLE virtual bool stopDelivery() = 0;
    Q_INVOKABLE virtual int  deliveryStatus() = 0;

    // Table actions
    Q_INVOKABLE virtual bool    joinTable(const QString& name) = 0;   // announce + claim a seat
    Q_INVOKABLE virtual bool    startHand() = 0;                      // coordinator only
    Q_INVOKABLE virtual bool    act(const QString& kind, int amount) = 0; // fold/check/call/raise
    Q_INVOKABLE virtual QString tableState() = 0;                     // JSON snapshot for the UI
    Q_INVOKABLE virtual QString myId() = 0;
};

#define PokerInterface_iid "org.logos.PokerInterface"
Q_DECLARE_INTERFACE(PokerInterface, PokerInterface_iid)

#endif // POKER_INTERFACE_H
