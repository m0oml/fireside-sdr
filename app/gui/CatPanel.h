#pragma once
// The Device tab's CAT card: a Hamlib "NET rigctl" server (fireside/rigctl.h) on 127.0.0.1 only (default port 4532, as rigctld), so
// WSJT-X, JTDX, fldigi, GridTracker, WaveLogGate, CQRLOG ... can follow and set this app's dial and mode (Hamlib rig model 2).
// It tunes through ReceiverPanel::setDialHz, the same path as the digits and the band buttons. It cannot transmit: rigctl.h refuses PTT.
// On by default; remembered in QSettings "fireside/cat" (never with FIRESIDE_NO_SETTINGS: tests start it themselves).
#include <QWidget>
#include <functional>
#include <map>
#include <memory>
#include "fireside/rigctl.h"
class QCheckBox; class QLabel; class QSpinBox; class QTcpServer; class QTcpSocket; class QTimer; class ReceiverPanel;

class CatPanel : public QWidget {
    Q_OBJECT
public:
    CatPanel(ReceiverPanel* rx, std::function<bool()> keyed, std::function<bool()> tx_busy, QWidget* parent = nullptr);
    ~CatPanel() override;
    bool startServer(int port);                         // false + status text if the port is taken; 0 = any free port (tests)
    void stopServer();
    bool listening() const;
    int port() const;                                   // the port in use (0 if not listening)
    int clients() const { return int(sessions_.size()); }
    uint64_t refusedPtt() const { return refused_ptt_; }
    QString statusText() const;
    QCheckBox* enableBox() const { return enable_; }
private:
    void onNewConnection(); void onReadyRead(QTcpSocket* s); void updateStatus(); void save();
    struct Rx;                                          // the Backend over the ReceiverPanel
    std::unique_ptr<Rx> backend_; QTcpServer* server_; std::map<QTcpSocket*, std::unique_ptr<fireside::rigctl::Session>> sessions_;
    QCheckBox* enable_; QSpinBox* port_; QLabel* status_; QTimer* tick_; QString last_, error_; uint64_t refused_ptt_ = 0, commands_ = 0; bool loading_ = false;
};
