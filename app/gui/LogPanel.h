#pragma once
// The Log tab: this app keeps no logbook of its own. It hands QSOs to the user's Wavelog instance (REST API v2) and keeps a local ADIF safety copy.
// Receive-only helper: it reads the dial/mode from the receiver and never touches the radio or the transmitter.
#include <QHash>
#include <QStringList>
#include <QDateTime>
#include <QWidget>
#include <functional>
#include "fireside/logbook.h"
#include "fireside/qrz.h"
class QCheckBox; class QComboBox; class QLabel; class QLineEdit; class QNetworkAccessManager; class QNetworkReply; class QPlainTextEdit; class QPushButton; class QSpinBox; class QTimer;
class ReceiverPanel; class QrzClient;

class LogPanel : public QWidget {
    Q_OBJECT
public:
    explicit LogPanel(ReceiverPanel* rx, QWidget* parent = nullptr);
    void setServer(const QString& url, const QString& token, int station);    // also what the connection fields show
    bool configured() const;
    void setLocalDir(const QString& dir);                                     // where log.adi / unsent.adi live (default ~/.local/share/fireside); tests redirect it
    QString localDir() const { return dir_; }
    void setCall(const QString& call);                                        // fills the form (e.g. from a decoded FT8 line) and runs the worked-before lookup
    bool fillFromDecode(const QString& decoderLine);                          // FT8/FT4 line -> callsign, grid, our report, their report; false when the line has no callsign
    QString callText() const;
    QString rstRcvdText() const;                                              // tests
    void refreshLastContacts();                                               // from the local ADIF copy at once, then from Wavelog when it answers (QSOs logged elsewhere show too)
    void refreshFromWavelog();                                               // last logged contacts, shown under the lookup result
    QWidget* takeLookupWidget();                                              // the small lookup box (callsign + Enter) for the Receiver tab
    QWidget* takeFormWidget();                                                // the large log form that takes over the Band and Mode cards while open
    void setCallText(const QString& call);                                    // typing a callsign without looking it up (tests)
    void enterPressed();                                                      // Enter in the callsign box: first press looks it up, the second opens the log form
    void openForm(); void closeForm(); bool formOpen() const { return form_open_; }
    void clearForm();
    void testConnection();
    void fetchStation();                                // the Wavelog station profile's callsign / grid square (read-only)
    void lookupNow();
    void logNow();                                                            // log the form's QSO now (UTC time of the click)
    void sendUnsent();
    void pushRadioIfChanged();
    int unsentCount() const;
    QString status() const { return status_; }
    QString workedText() const;
    QSize lookupSizeHint() const;                       // the small box's preferred size
    void showLookupResult(const QString& html);         // the lookup result line(s) in the small box (tests use it to check the box never changes size)
    QString lastContactsText() const;
    uint64_t radioPushes() const { return radio_pushes_; }
    void setRadioPush(bool on);
    // A decoded FT8/FT4 line addressed to us (someone answering): fills the entry from it and looks the caller up, like a double-click, unless the entry already
    // holds another callsign (typed, or an earlier caller: the first one stays until logged or cleared) or a form is open, or the caller was logged in the last
    // 10 minutes (their RR73 / 73 after the QSO). The same caller again only updates the report they sent.
    void callerDecoded(const QString& line);
    void logAuto(const QString& call, const QString& grid, const QString& rstSent, const QString& rstRcvd);   // a QSO the FT8 sequencer completed: logged now (Wavelog + the local copy)
    QString autoCall() const { return auto_call_; }
    void checkWorked(const QString& call);                                    // FT8/FT4 decode: is this station new / new on this band or mode / already worked? (answered through workedKnown; read-only, cached, one request at a time)
    QrzClient* qrz() const { return qrz_; }                                    // QRZ.com callbook (its settings card is on this tab)
    void setQrzCredentials(const QString& user, const QString& password);   // what the QRZ fields show (tests)
    void qrzSaveAndLogin();                                                   // the card's button: saves qrz.env (0600), logs in
    QString qrzStatus() const;
    QString myGrid() const;
    void setMyGrid(const QString& g);
    void setTxPowerSource(std::function<double()> f) { tx_pwr_ = std::move(f); }   // our power for TX_PWR (0 = unknown)                                          // tools / tests (normally from the Wavelog station profile)
    void saveSettings();                                                      // station id, radio name; the token goes to a 0600 file, never to QSettings
signals:
    void statusChanged(const QString&);
    void workedKnown(const QString& call, int state);                         // 0 = new callsign, 1 = worked, but not on this band and mode, 2 = already worked on this band and mode
    void activeChanged(bool open);                                            // the large log form opened / closed
private:
    void showLastRows(const QList<QStringList>& rows, const QString& source);
    QTimer* recent_timer_ = nullptr; bool wavelog_recent_ok_ = false;
    void pumpWorked();
    QHash<QString, int> worked_cache_; QStringList worked_queue_; bool worked_busy_ = false;
    QNetworkReply* post(const QString& path, const QByteArray& body);
    QNetworkReply* get(const QString& path);
    void setStatus(const QString& s);
    void loadFromFiles();
    void writeTokenFile();
    fireside::logbook::Qso formQso() const;
    QString currentMode() const;
    ReceiverPanel* rx_; QNetworkAccessManager* net_;
    QLineEdit *url_, *token_, *call_, *rst_sent_, *rst_rcvd_, *name_, *grid_, *comment_, *my_call_, *my_grid_, *radio_name_; QSpinBox* station_; QComboBox* station_box_;
    QLabel *worked_, *freq_mode_, *state_, *entry_status_; QWidget *lookup_w_, *form_w_; QLabel *form_title_, *form_info_, *form_status_; QPushButton* look_btn_; QString looked_up_; bool form_open_ = false; QCheckBox* push_; QPushButton *log_btn_, *unsent_btn_; QPlainTextEdit* recent_; QLabel *last_title_, *last_qsos_; QTimer* push_timer_;
    QrzClient* qrz_ = nullptr; QLineEdit *qrz_user_ = nullptr, *qrz_pass_ = nullptr; QCheckBox* qrz_every_ = nullptr; QLabel* qrz_state_ = nullptr;
    std::function<void()> rerender_; std::function<double()> tx_pwr_;                     // the current lookup's renderer (QRZ answers after Wavelog: draw again)
    QString auto_call_, last_logged_; QDateTime last_logged_at_;
    QString status_, dir_, last_looked_up_; double last_push_hz_ = -1; QString last_push_mode_; uint64_t radio_pushes_ = 0; int lookup_serial_ = 0; bool loading_ = false;
};
