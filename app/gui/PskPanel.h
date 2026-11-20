#pragma once
// The PSK Reporter tab: where this station is being heard, live, from the PSK Reporter MQTT feed (fireside/pskr.h).
//   * subscribes only to reports of MY call as the sender (the topic carries the callsign; no account)
//   * optional (off by default): uploads my live FT8 / FT4 decodes to report.pskreporter.info (fireside/pskr.h Uploader: IPFIX over UDP, PSK Reporter's rules)
//   * table of receivers (distance / bearing from the Log tab's grid), a summary over a chosen window, a one-line summary for the Radio screen
//   * every report is appended to ~/.local/share/fireside/pskreporter-heard.csv with the "Antenna note" typed here, for comparing antennas later
//   * reconnects by itself (10 s .. 5 min back-off); never with FIRESIDE_NO_SETTINGS (tests) unless a test broker is set
#include <QWidget>
#include <functional>
#include <memory>
#include "fireside/pskr.h"
class QCheckBox; class QComboBox; class QLabel; class QLineEdit; class QTableWidget; class QTimer; class QSslSocket; class QUdpSocket;
#include <QHostAddress>

class PskrClient : public QObject {
    Q_OBJECT
public:
    explicit PskrClient(QObject* parent = nullptr);
    ~PskrClient() override;
    void setBroker(const QString& host, int port, bool tls) { host_ = host; port_ = port; tls_ = tls; }   // tests: a local fake broker
    void start(const QString& call);                    // (re)connects and subscribes to reports of `call` as the sender
    void stop();
    bool running() const { return running_; }
    QString status() const { return status_; }
    int received() const { return received_; }
signals:
    void spot(const fireside::pskr::Spot& s);
    void statusChanged(const QString& s);
private:
    void connectNow(); void onConnected(); void onReadyRead(); void onDropped(const QString& why); void setStatus(const QString& s);
    QSslSocket* sock_ = nullptr; QTimer *ping_, *retry_; fireside::pskr::mqtt::Decoder dec_;
    QString host_ = fireside::pskr::kBroker, call_, status_ = "off", client_id_; int port_ = fireside::pskr::kPortTls; bool tls_ = true, running_ = false, subscribed_ = false;
    int backoff_s_ = 10, received_ = 0;
};

class PskPanel : public QWidget {
    Q_OBJECT
public:
    PskPanel(std::function<QString()> my_call, std::function<QString()> my_grid, std::function<QString()> dial_band, QWidget* parent = nullptr);
    PskrClient* client() const { return client_; }
    QCheckBox* enableBox() const { return enable_; }
    void setCsvPath(const QString& p) { csv_ = p; }      // tests; default ~/.local/share/fireside/pskreporter-heard.csv (not with FIRESIDE_NO_SETTINGS)
    void setNowForTests(std::function<qint64()> f) { now_ = std::move(f); refresh(); }
    int rows() const;
    QString summaryText() const;
    QString shortSummary() const { return short_; }
    QLineEdit* antennaBox() const { return ant_; }
    void addSpotForTests(const fireside::pskr::Spot& s) { onSpot(s); }
    // Uploading my decodes (off by default): one decoder line heard live with the dial at dial_hz.
    void onDecodeLine(const QString& line, double dial_hz, const QString& mode);
    QCheckBox* uploadBox() const { return upload_; }
    void setUploadTarget(const QHostAddress& a, quint16 port) { up_addr_ = a; up_port_ = port; }   // tests: a local UDP listener
    void flushUploads();                                 // send what is waiting now (closing the app)
    QString uploadStatus() const;
    const fireside::pskr::Uploader& uploader() const { return *up_; }
signals:
    void shortSummaryChanged(const QString& text);       // "Heard by 12 in 15 min, furthest DL1ABC 1104 km" (Radio screen status)
private:
    void onSpot(const fireside::pskr::Spot& s); void refresh(); void syncStation(); void load(); void save();
    std::function<QString()> my_call_, my_grid_, dial_band_; std::function<qint64()> now_;
    PskrClient* client_; fireside::pskr::Heard heard_; QString call_, grid_, csv_, short_;
    QCheckBox *enable_, *this_band_; QComboBox* window_; QLineEdit* ant_; QLabel *status_, *summary_; QTableWidget* table_; QTimer* tick_;
    bool loading_ = false;
    QCheckBox* upload_; QLabel* up_status_; QUdpSocket* udp_ = nullptr; std::unique_ptr<fireside::pskr::Uploader> up_; QHostAddress up_addr_; quint16 up_port_ = 0;
    qint64 last_upload_ = 0; int up_errors_ = 0; void uploadTick(); void sendDatagram(int64_t now);
};
