#include "LogPanel.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include "Theme.h"
#include <QFormLayout>
#include <QScrollArea>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include "QrzClient.h"
#include "ReceiverPanel.h"
using namespace fireside::logbook;

namespace {
QString envPath() { return QDir::homePath() + "/.config/fireside/wavelog.env"; }
}  // namespace

LogPanel::LogPanel(ReceiverPanel* rx, QWidget* parent) : QWidget(parent), rx_(rx), net_(new QNetworkAccessManager(this)) {
    dir_ = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/fireside";
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS"))                                     // tests: never the user's log.adi / unsent.adi (a test QSO landed there 32 times)
        dir_ = QDir::tempPath() + QString("/fireside-test-%1").arg(QCoreApplication::applicationPid());
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(16, 16, 16, 16); outer->setSpacing(12);
    // ---- connection
    auto* cf = new QGridLayout; cf->setHorizontalSpacing(10); cf->setVerticalSpacing(8); int crow = 0; auto addRow = [&](const QString& l, QWidget* w) { cf->addWidget(new QLabel(l), crow, 0); cf->addWidget(w, crow++, 1); };
    url_ = new QLineEdit; url_->setPlaceholderText("https://log.example.com"); token_ = new QLineEdit; token_->setEchoMode(QLineEdit::Password); token_->setPlaceholderText("API v2 token (wl2_...)");
    station_ = new QSpinBox; station_->setRange(1, 9999); station_box_ = new QComboBox; station_box_->setToolTip("Station profiles found on the server (after Test connection)");
    my_call_ = new QLineEdit; my_call_->setReadOnly(true); my_grid_ = new QLineEdit; my_grid_->setReadOnly(true);
    auto* test = new QPushButton("Test connection"); state_ = new QLabel("Not configured"); state_->setObjectName("muted");
    auto* srow = new QHBoxLayout; srow->addWidget(station_); srow->addWidget(station_box_, 1);
    addRow("Server", url_); addRow("Token", token_); { auto* w = new QWidget; srow->setContentsMargins(0, 0, 0, 0); w->setLayout(srow); addRow("Station profile", w); }
    auto* trow = new QHBoxLayout; trow->addWidget(test); trow->addWidget(state_, 1); cf->addLayout(trow, crow++, 1); cf->setColumnStretch(1, 1);
    // ---- radio push
    radio_name_ = new QLineEdit("FLEX-5000A"); push_ = new QCheckBox("Send frequency, mode and power to Wavelog");
    push_->setToolTip("Wavelog's live QSO page fills in band, frequency, mode and power by itself: pick this radio name there. Sent on every change and every 5 minutes.\n"
                      "Power is the forward power measured on your last over (left out until you have transmitted). The token needs the radio:write scope.");
    push_state_ = new QLabel("Not sending"); push_state_->setObjectName("muted"); push_state_->setWordWrap(true);
    auto* rf = new QGridLayout; rf->setHorizontalSpacing(10); rf->setVerticalSpacing(8); rf->addWidget(new QLabel("Radio name"), 0, 0); rf->addWidget(radio_name_, 0, 1); rf->addWidget(push_, 1, 0, 1, 2); rf->addWidget(push_state_, 2, 0, 1, 2); rf->setColumnStretch(1, 1);
    // ---- QRZ.com callbook (straight from QRZ, not through Wavelog): name, town, country, grid and distance for callers and decoded stations
    qrz_ = new QrzClient(this);
    qrz_user_ = new QLineEdit(qrz_->user()); qrz_user_->setPlaceholderText("QRZ.com username (your call)");
    qrz_pass_ = new QLineEdit; qrz_pass_->setEchoMode(QLineEdit::Password); qrz_pass_->setPlaceholderText(qrz_->configured() ? "saved (type to change)" : "QRZ.com password");
    qrz_every_ = new QCheckBox("Also look up stations calling CQ"); qrz_every_->setToolTip("Stations calling you and the entry are always looked up. With this on, FT8/FT4 stations calling CQ are too. Other decodes show only what is already cached (30 days); at most one request a second.");
    auto* qrz_btn = new QPushButton("Save and log in"); qrz_btn->setToolTip("Saves the username and password in ~/.config/fireside/qrz.env (readable only by you) and logs in to check them");
    qrz_state_ = new QLabel(qrz_->status()); qrz_state_->setObjectName("muted"); qrz_state_->setWordWrap(true);
    auto* qf = new QGridLayout; qf->setHorizontalSpacing(10); qf->setVerticalSpacing(8);
    qf->addWidget(new QLabel("Username"), 0, 0); qf->addWidget(qrz_user_, 0, 1); qf->addWidget(new QLabel("Password"), 1, 0); qf->addWidget(qrz_pass_, 1, 1);
    qf->addWidget(qrz_every_, 2, 0, 1, 2); qf->addWidget(qrz_btn, 3, 0); qf->addWidget(qrz_state_, 3, 1); qf->setColumnStretch(1, 1);
    connect(qrz_btn, &QPushButton::clicked, this, [this] { qrzSaveAndLogin(); });
    connect(qrz_pass_, &QLineEdit::returnPressed, this, [this] { qrzSaveAndLogin(); });
    connect(qrz_every_, &QCheckBox::toggled, this, [this](bool on) { qrz_->setEveryDecode(on); if (!loading_ && !qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) QSettings("fireside", "log").setValue("qrz_every", on); });
    connect(qrz_, &QrzClient::statusChanged, this, [this](const QString& t) { qrz_state_->setText(t); });
    connect(qrz_, &QrzClient::infoReady, this, [this](const QString& call, const fireside::qrz::Info&) {
        if (rerender_ && QString::fromStdString(fireside::qrz::base_call(call_->text().trimmed().toStdString())) == call) rerender_();   // the entry's station: draw the lookup again with it
    });
    // ---- highlight on decodes: what a station would add to the log (from the whole Wavelog log, read at start-up and every 30 minutes)
    auto* hf = new QGridLayout; hf->setHorizontalSpacing(10); hf->setVerticalSpacing(4);
    {   struct H { const char* key; const char* text; const char* tip; bool fireside::award::Options::* field; };
        static const H hs[] = {{"hl_dxcc", "New DXCC entity", "A country you have never worked", &fireside::award::Options::dxcc},
                               {"hl_dxcc_band", "New DXCC on this band", "Worked, but never on this band (any mode)", &fireside::award::Options::dxcc_band},
                               {"hl_cont", "New continent", "", &fireside::award::Options::cont},
                               {"hl_cq", "New CQ zone", "Approximate: cty.dat gives one zone for all of the US, Canada, Russia, Australia and a few other big countries", &fireside::award::Options::cq},
                               {"hl_itu", "New ITU zone", "Approximate, as for CQ zones", &fireside::award::Options::itu},
                               {"hl_grid", "New grid square", "From the locator in their message (CQs and first replies carry one)", &fireside::award::Options::grid},
                               {"hl_grid_band", "New grid on this band", "", &fireside::award::Options::grid_band}};
        int i = 0;
        for (const H& h : hs) {
            auto* b = new QCheckBox(h.text); if (*h.tip) b->setToolTip(h.tip);
            const bool on = qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS") ? hl_.*h.field : QSettings("fireside", "log").value(h.key, hl_.*h.field).toBool();
            hl_.*h.field = on; b->setChecked(on);
            connect(b, &QCheckBox::toggled, this, [this, h](bool v) { hl_.*h.field = v; if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) QSettings("fireside", "log").setValue(h.key, v); });
            hf->addWidget(b, i / 2, i % 2); ++i;
        }
        hl_state_ = new QLabel("Log not read yet"); hl_state_->setObjectName("muted"); hl_state_->setWordWrap(true); hf->addWidget(hl_state_, (i + 1) / 2, 0, 1, 2);
        hf->setColumnStretch(1, 1); }
    { auto* top = new QHBoxLayout; top->setSpacing(12); top->addWidget(theme::makeCard("Wavelog connection", cf), 3); top->addWidget(theme::makeCard("QRZ.com callbook", qf), 3); top->addWidget(theme::makeCard("Follow the dial", rf), 2); top->addWidget(theme::makeCard("Highlight on decodes", hf), 3); outer->addLayout(top); }
    // ---- two widgets for the Receiver tab (take*Widget): a small LOOKUP box (callsign + Enter -> who is it, have I worked them), and the large LOG form that replaces the Band and Mode cards
    call_ = new QLineEdit; call_->setPlaceholderText("callsign, then Enter"); call_->setMaxLength(14); call_->setMinimumWidth(150); rst_sent_ = new QLineEdit; rst_rcvd_ = new QLineEdit; name_ = new QLineEdit; grid_ = new QLineEdit; comment_ = new QLineEdit;
    for (auto* e : {rst_sent_, rst_rcvd_}) { e->setMaximumWidth(80); e->setPlaceholderText("59"); } name_->setPlaceholderText("name"); grid_->setMinimumWidth(80); grid_->setPlaceholderText("grid"); comment_->setPlaceholderText("comment");
    freq_mode_ = new QLabel; freq_mode_->setObjectName("muted"); worked_ = new QLabel(" "); worked_->setTextFormat(Qt::RichText); worked_->setWordWrap(true);  worked_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    look_btn_ = new QPushButton("Look up"); look_btn_->setToolTip("First press: look the callsign up. Second press (or Enter again): open the log form"); unsent_btn_ = new QPushButton("Send unsent"); unsent_btn_->setVisible(false);
    auto* clear_btn = new QPushButton("Clear"); clear_btn->setToolTip("Forget this callsign");
    entry_status_ = new QLabel(" "); entry_status_->setObjectName("muted"); entry_status_->setWordWrap(true);
    lookup_w_ = new QWidget; { auto* eg = new QVBoxLayout(lookup_w_); eg->setContentsMargins(0, 0, 0, 0); eg->setSpacing(6);
        auto* r1 = new QHBoxLayout; r1->setSpacing(6); r1->addWidget(call_, 1); r1->addWidget(look_btn_); r1->addWidget(clear_btn);
        auto* r4 = new QHBoxLayout; r4->setSpacing(6); r4->addWidget(entry_status_, 1); r4->addWidget(unsent_btn_);
        last_title_ = new QLabel("Last contacts"); last_title_->setObjectName("muted"); last_qsos_ = new QLabel; last_qsos_->setTextFormat(Qt::RichText); last_qsos_->setAlignment(Qt::AlignLeft | Qt::AlignTop); last_qsos_->setWordWrap(true);
        eg->addLayout(r1);
        // Everything below the callsign row is one fixed-height feed: a lookup result pushes the previous contacts down and off the bottom of the tile instead of resizing the tile.
        auto* feed = new QWidget; auto* fl = new QVBoxLayout(feed); fl->setContentsMargins(0, 0, 0, 0); fl->setSpacing(4); fl->setSizeConstraint(QLayout::SetNoConstraint);
        fl->addLayout(r4); fl->addWidget(worked_); fl->addSpacing(2); fl->addWidget(last_title_); fl->addWidget(last_qsos_); fl->addStretch(1);
        auto* sa = new QScrollArea; sa->setFrameShape(QFrame::NoFrame); sa->setWidgetResizable(true); sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); sa->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); sa->setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");
        sa->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored); sa->setMinimumHeight(0); sa->setWidget(feed); eg->addWidget(sa, 1); }       // a scroll area (no bars) is what keeps the wrapped text's height-for-width out of the tile's size
    log_btn_ = new QPushButton("Log QSO"); log_btn_->setToolTip("Enter in any field does the same"); auto* cancel_btn = new QPushButton("Cancel"); cancel_btn->setToolTip("Close the form without logging (Esc)");
    form_title_ = new QLabel; form_title_->setTextFormat(Qt::RichText); form_info_ = new QLabel; form_info_->setTextFormat(Qt::RichText); form_info_->setWordWrap(true); form_status_ = new QLabel(" "); form_status_->setObjectName("muted"); form_status_->setWordWrap(true);
    form_w_ = new QWidget; { auto* fg = new QVBoxLayout(form_w_); fg->setContentsMargins(0, 0, 0, 0); fg->setSpacing(8);
        auto* r2 = new QHBoxLayout; r2->setSpacing(6); auto* l1 = new QLabel("sent"); l1->setObjectName("muted"); auto* l2 = new QLabel("rcvd"); l2->setObjectName("muted"); auto* l3 = new QLabel("grid"); l3->setObjectName("muted");
        r2->addWidget(l1); r2->addWidget(rst_sent_); r2->addWidget(l2); r2->addWidget(rst_rcvd_); r2->addWidget(l3); r2->addWidget(grid_, 1);
        auto* r3 = new QHBoxLayout; r3->setSpacing(6); name_->setMinimumWidth(120); r3->addWidget(name_, 1); r3->addWidget(comment_, 3);
        auto* r5 = new QHBoxLayout; r5->setSpacing(6); r5->addWidget(log_btn_); r5->addWidget(cancel_btn); r5->addWidget(form_status_, 1);
        fg->addWidget(form_title_); fg->addWidget(form_info_); fg->addLayout(r2); fg->addLayout(r3); fg->addLayout(r5); fg->addStretch(1);
        auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), form_w_); esc->setContext(Qt::WidgetWithChildrenShortcut); connect(esc, &QShortcut::activated, this, [this] { closeForm(); }); }
    recent_ = new QPlainTextEdit; recent_->setReadOnly(true); recent_->setMaximumBlockCount(200); recent_->setPlaceholderText("Logged contacts appear here"); { auto* rl = new QVBoxLayout; rl->addWidget(recent_); outer->addWidget(theme::makeCard("Logged contacts", rl, true), 1); }
    push_timer_ = new QTimer(this); push_timer_->setInterval(3000);
    connect(test, &QPushButton::clicked, this, [this] { testConnection(); });
    connect(log_btn_, &QPushButton::clicked, this, [this] { logNow(); });
    connect(look_btn_, &QPushButton::clicked, this, [this] { enterPressed(); });
    connect(cancel_btn, &QPushButton::clicked, this, [this] { closeForm(); });
    connect(clear_btn, &QPushButton::clicked, this, [this] { clearForm(); setStatus("Cleared"); });
    for (auto* e : {rst_sent_, rst_rcvd_, name_, grid_, comment_}) connect(e, &QLineEdit::returnPressed, this, [this] { logNow(); });
    connect(unsent_btn_, &QPushButton::clicked, this, [this] { sendUnsent(); });
    connect(call_, &QLineEdit::textChanged, this, [this](const QString& t) { const QString u = t.toUpper(); if (u != t) { QSignalBlocker b(call_); const int c = call_->cursorPosition(); call_->setText(u); call_->setCursorPosition(c); } ++lookup_serial_; looked_up_.clear(); worked_->setText(" "); look_btn_->setText("Look up"); });
    connect(call_, &QLineEdit::returnPressed, this, [this] { enterPressed(); });
    connect(push_timer_, &QTimer::timeout, this, [this] { freq_mode_->setText(QString("%1 kHz  %2").arg(rx_->dialHz() / 1e3, 0, 'f', 3).arg(currentMode())); pushRadioIfChanged(); });
    connect(push_, &QCheckBox::toggled, this, [this](bool on) { if (loading_) return; setRadioPush(on); if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) QSettings("fireside", "log").setValue("push_radio", on); });
    connect(radio_name_, &QLineEdit::editingFinished, this, [this] { last_push_hz_ = -1; pushRadioIfChanged(); });   // a new name is a new radio in Wavelog: send it at once
    connect(station_box_, &QComboBox::activated, this, [this](int i) { if (i >= 0) station_->setValue(station_box_->itemData(i).toInt()); });
    loadFromFiles(); refreshLastContacts(); push_timer_->start();
    { auto* st = new QTimer(this); st->setSingleShot(true); st->setInterval(800); connect(st, &QTimer::timeout, this, [this] { fetchStation(); });
      connect(station_, &QSpinBox::valueChanged, this, [this, st] { if (!loading_) st->start(); });
      if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) QTimer::singleShot(1000, this, [this] { fetchStation(); }); }   // the profile's call / grid at start-up
    recent_timer_ = new QTimer(this); recent_timer_->setInterval(5 * 60 * 1000); connect(recent_timer_, &QTimer::timeout, this, [this] { refreshFromWavelog(); }); recent_timer_->start(); QTimer::singleShot(800, this, [this] { refreshFromWavelog(); });
    { auto* it = new QTimer(this); it->setInterval(10 * 60 * 1000); connect(it, &QTimer::timeout, this, [this] { loadWorkedIndex(); }); it->start();
      if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) QTimer::singleShot(1500, this, [this] { loadWorkedIndex(); }); }
    freq_mode_->setText(QString("%1 kHz  %2").arg(rx_->dialHz() / 1e3, 0, 'f', 3).arg(currentMode()));
}

// ---------------------------------------------------------------- settings: URL + token in a 0600 file outside the repo, the rest in QSettings (the token never goes to QSettings)
void LogPanel::loadFromFiles() {
    loading_ = true;
    QFile f(envPath());
    if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS") && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QByteArray& l : f.readAll().split('\n')) {
            const int eq = l.indexOf('='); if (eq < 0) continue; const QString k = QString::fromUtf8(l.left(eq)).trimmed(), v = QString::fromUtf8(l.mid(eq + 1)).trimmed();
            if (k == "WAVELOG_URL") url_->setText(v); else if (k == "WAVELOG_KEY") token_->setText(v);
        }
    }
    if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) {
        QSettings q("fireside", "log"); station_->setValue(q.value("station", 1).toInt()); radio_name_->setText(q.value("radio", "FLEX-5000A").toString());
        my_call_->setText(q.value("my_call").toString()); my_grid_->setText(q.value("my_grid").toString());
        push_->setChecked(q.value("push_radio", false).toBool());           // not a transmit permission: remembered
    }
    { const bool every = qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS") ? true : QSettings("fireside", "log").value("qrz_every", true).toBool(); qrz_every_->setChecked(every); qrz_->setEveryDecode(every); }
    loading_ = false; state_->setText(configured() ? "Configured: press Test connection" : "Not configured");
    if (push_->isChecked()) push_state_->setText(configured() ? "Sending shortly" : "Set up the Wavelog connection first");
}
void LogPanel::writeTokenFile() {
    QDir().mkpath(QFileInfo(envPath()).absolutePath()); QFile f(envPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);                               // before the secret goes in
    f.write(("WAVELOG_URL=" + url_->text().trimmed() + "\nWAVELOG_KEY=" + token_->text().trimmed() + "\n").toUtf8());
}
void LogPanel::saveSettings() {
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    if (configured()) writeTokenFile();
    QSettings q("fireside", "log"); q.setValue("station", station_->value()); q.setValue("radio", radio_name_->text()); q.setValue("my_call", my_call_->text()); q.setValue("my_grid", my_grid_->text());
}
void LogPanel::setServer(const QString& url, const QString& token, int station) { url_->setText(url); token_->setText(token); station_->setValue(station); state_->setText(configured() ? "Configured" : "Not configured"); }
bool LogPanel::configured() const { return !wavelog_normalise_base_url(url_->text().toStdString()).empty() && token_->text().trimmed().startsWith("wl2_"); }
void LogPanel::setLocalDir(const QString& dir) { dir_ = dir; }
void LogPanel::setQrzCredentials(const QString& user, const QString& password) { qrz_user_->setText(user); qrz_pass_->setText(password); }
void LogPanel::qrzSaveAndLogin() {
    const QString pass = qrz_pass_->text().isEmpty() ? QString() : qrz_pass_->text();
    if (qrz_user_->text().trimmed().isEmpty()) { qrz_state_->setText("Enter your QRZ.com username"); return; }
    if (!pass.isEmpty() || !qrz_->configured() || qrz_user_->text().trimmed() != qrz_->user()) {
        if (pass.isEmpty()) { qrz_state_->setText("Enter your QRZ.com password"); return; }
        qrz_->setCredentials(qrz_user_->text(), pass);
    }
    qrz_pass_->clear(); qrz_pass_->setPlaceholderText("saved (type to change)");
    const bool saved = qrz_->saveCredentials(); qrz_->testLogin();
    if (!saved && !qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) qrz_state_->setText("Could not write " + QrzClient::envPath());
}
QString LogPanel::qrzStatus() const { return qrz_->status(); }
QString LogPanel::myGrid() const { return my_grid_->text(); }
void LogPanel::setMyGrid(const QString& g) { my_grid_->setText(g.trimmed().toUpper()); }
void LogPanel::setStatus(const QString& s) { status_ = s; state_->setText(s); entry_status_->setText(s); form_status_->setText(s); emit statusChanged(s); }
// the last few contacts from the local ADIF safety copy, newest first (fills the lookup box under the lookup result)
void LogPanel::refreshLastContacts() {
    QFile f(dir_ + "/log.adi"); QList<QStringList> rows;
    if (f.open(QIODevice::ReadOnly)) {
        const QString all = QString::fromUtf8(f.readAll()); const int eoh = all.indexOf("<EOH>", 0, Qt::CaseInsensitive);
        const QStringList recs = all.mid(eoh < 0 ? 0 : eoh + 5).split("<EOR>", Qt::SkipEmptyParts, Qt::CaseInsensitive);
        static const QRegularExpression tag("<(\\w+):(\\d+)(?::\\w+)?>", QRegularExpression::CaseInsensitiveOption);
        for (int i = recs.size() - 1; i >= 0 && rows.size() < 6; --i) {
            QMap<QString, QString> m; const QString& r = recs[i]; auto it = tag.globalMatch(r);
            while (it.hasNext()) { const auto mt = it.next(); m[mt.captured(1).toUpper()] = r.mid(mt.capturedEnd(), mt.captured(2).toInt()); }
            if (!m.contains("CALL")) continue;
            const QString d = m.value("QSO_DATE"), t = m.value("TIME_ON");
            const QString when = d.size() == 8 && t.size() >= 4 ? QString("%1-%2-%3 %4:%5").arg(d.left(4), d.mid(4, 2), d.mid(6, 2), t.left(2), t.mid(2, 2)) : QString();
            rows << (QStringList() << when << m.value("CALL") << m.value("BAND") << (m.value("SUBMODE").isEmpty() ? m.value("MODE") : m.value("SUBMODE")) << m.value("NAME"));   // FT4 = MFSK / FT4
        }
    }
    if (!wavelog_recent_ok_) showLastRows(rows, "this PC");                                                    // once Wavelog has answered, its list is the better one
}
QString LogPanel::lastContactsText() const { return last_title_->text() + "\n" + last_qsos_->text(); }
void LogPanel::showLastRows(const QList<QStringList>& rows, const QString& source) {     // each row: when, call, band, mode, name  ->  an aligned table
    last_title_->setText("Last contacts  (" + source + ")"); const QList<QStringList> shown = rows.mid(0, 6);
    if (rows.isEmpty()) { last_qsos_->setText("<span style='color:#8b93a7'>No contacts logged yet</span>"); return; }
    QString t = "<table cellspacing='0' cellpadding='1'>";
    for (const QStringList& r : shown) t += QString("<tr><td style='color:#8b93a7'>%1&nbsp;&nbsp;&nbsp;</td><td><b>%2</b>&nbsp;&nbsp;&nbsp;</td><td>%3&nbsp;&nbsp;&nbsp;</td><td>%4&nbsp;&nbsp;&nbsp;</td><td style='color:#8b93a7'>%5</td></tr>").arg(r[0].toHtmlEscaped(), r[1].toHtmlEscaped(), r[2].toHtmlEscaped(), r[3].toHtmlEscaped(), r[4].toHtmlEscaped());
    last_qsos_->setText(t + "</table>");
}
// the newest QSOs on the Wavelog account (read-only); on any failure the local list stays
void LogPanel::refreshFromWavelog() {
    if (!configured()) { wavelog_recent_ok_ = false; return; }
    auto* r = get(QString::fromStdString(wavelog_recent_path(12)));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater(); if (r->error() != QNetworkReply::NoError) { wavelog_recent_ok_ = false; return; }
        QList<QJsonObject> v; for (const auto& x : QJsonDocument::fromJson(r->readAll()).object().value("data").toArray()) v << x.toObject();
        std::stable_sort(v.begin(), v.end(), [](const QJsonObject& a, const QJsonObject& b) { return a.value("qso_date").toString() > b.value("qso_date").toString(); });
        QList<QStringList> rows;
        for (const QJsonObject& o : v) {
            const QString call = o.value("call").toString(); if (call.isEmpty()) continue;
            const QString mode = o.value("submode").toString().isEmpty() ? o.value("mode").toString() : o.value("submode").toString(), name = o.value("name").toString();
            rows << (QStringList() << o.value("qso_date").toString().left(16) << call << o.value("band").toString() << mode << name);
        }
        wavelog_recent_ok_ = true; showLastRows(rows, "Wavelog");
    });
}
QWidget* LogPanel::takeLookupWidget() { lookup_w_->setParent(nullptr); return lookup_w_; }
QWidget* LogPanel::takeFormWidget() { form_w_->setParent(nullptr); return form_w_; }

// ---------------------------------------------------------------- network (the token only ever goes in the Authorization header, and never into a status line)
QNetworkReply* LogPanel::get(const QString& path) {
    QNetworkRequest r(QUrl(QString::fromStdString(wavelog_normalise_base_url(url_->text().toStdString())) + path));
    r.setRawHeader("Authorization", "Bearer " + token_->text().trimmed().toUtf8()); r.setRawHeader("Accept", "application/json"); r.setTransferTimeout(10000);
    return net_->get(r);
}
QNetworkReply* LogPanel::post(const QString& path, const QByteArray& body) {
    QNetworkRequest r(QUrl(QString::fromStdString(wavelog_normalise_base_url(url_->text().toStdString())) + path));
    r.setRawHeader("Authorization", "Bearer " + token_->text().trimmed().toUtf8()); r.setRawHeader("Accept", "application/json"); r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json"); r.setTransferTimeout(10000);
    return net_->post(r, body);
}
static QString failure(QNetworkReply* r) {
    const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object(); const QString why = o.value("reason").toString(o.value("error").toObject().value("message").toString());
    const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 401 || code == 403) return QString("Wavelog refused the token (%1)%2").arg(code).arg(why.isEmpty() ? "" : ": " + why);
    if (code > 0) return QString("Wavelog error %1%2").arg(code).arg(why.isEmpty() ? "" : ": " + why);
    return "Cannot reach Wavelog: " + r->errorString();
}

void LogPanel::testConnection() {
    if (!configured()) { setStatus("Enter the server URL and an API v2 token (wl2_...) first"); return; }
    setStatus("Testing...");
    auto* r = get(kWavelogTokenPath);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) { setStatus(failure(r)); return; }
        const QJsonObject d = QJsonDocument::fromJson(r->readAll()).object().value("data").toObject(); QStringList sc; for (const auto& v : d.value("scopes").toArray()) sc << v.toString();
        QStringList missing; for (const char* need : {"qso:write", "lookup:read"}) if (!sc.contains(need)) missing << need;
        setStatus(QString("Connected as %1").arg(d.value("owner").toString()) + (missing.isEmpty() ? "" : " (token lacks " + missing.join(", ") + ")"));
        fetchStation();
    });
}
// The station profiles (callsign, grid square): at start-up, after Test connection and when the station number changes, so the Log tab's
// callsign / locator (used for MY_GRIDSQUARE, FT8 and PSK Reporter) always match the Wavelog profile. Read-only.
void LogPanel::fetchStation() {
    if (!configured()) return;
    auto* s = get(kWavelogStationPath);
    connect(s, &QNetworkReply::finished, this, [this, s] {
        s->deleteLater(); if (s->error() != QNetworkReply::NoError) return;
        QSignalBlocker b(station_box_); station_box_->clear(); bool found = false;
        for (const auto& v : QJsonDocument::fromJson(s->readAll()).object().value("data").toArray()) {
            const QJsonObject o = v.toObject(); station_box_->addItem(QString("%1: %2 %3").arg(o.value("id").toInt()).arg(o.value("callsign").toString(), o.value("gridsquare").toString()), o.value("id").toInt());
            if (o.value("id").toInt() == station_->value()) {
                station_box_->setCurrentIndex(station_box_->count() - 1); found = true;
                if (!o.value("callsign").toString().isEmpty()) my_call_->setText(o.value("callsign").toString().trimmed().toUpper());
                if (!o.value("gridsquare").toString().isEmpty()) my_grid_->setText(o.value("gridsquare").toString().trimmed().toUpper());
            }
        }
        if (found) saveSettings();
    });
}

QString LogPanel::currentMode() const {
    static const char* modes[] = {"USB", "LSB", "AM", "CW", "FM"}; static const char* dec[] = {"", "CW", "PSK31", "RTTY", "FT8", "FT4"};
    const int d = rx_->decoderIndex(); if (d >= 1 && d <= 5) return dec[d];
    const int m = rx_->modeIndex(); return (m >= 0 && m < 5) ? modes[m] : "USB";
}
void LogPanel::showLookupResult(const QString& html) { worked_->setText(html); }
QSize LogPanel::lookupSizeHint() const { return lookup_w_->sizeHint(); }
QString LogPanel::workedText() const { return worked_->text(); }

namespace {
// "2026-11-10 12:30:00" (UTC, as Wavelog stores it) -> "2026-11-10 12:30 UTC (5 days ago)"
QString when(const QString& stamp) {
    QDateTime t = QDateTime::fromString(stamp.left(19), "yyyy-MM-dd HH:mm:ss"); if (!t.isValid()) return stamp.toHtmlEscaped(); t.setTimeZone(QTimeZone::utc());
    const qint64 mins = t.secsTo(QDateTime::currentDateTimeUtc()) / 60; QString ago;
    if (mins < 1) ago = "just now"; else if (mins < 90) ago = QString("%1 min ago").arg(mins); else if (mins < 48 * 60) ago = QString("%1 h ago").arg(mins / 60); else if (mins < 90 * 24 * 60) ago = QString("%1 days ago").arg(mins / (24 * 60)); else ago = QString("%1 months ago").arg(mins / (30 * 24 * 60));
    return QString("%1 UTC (%2)").arg(t.toString("yyyy-MM-dd HH:mm"), ago);
}
struct LookupParts { QJsonObject info, last; int total = 0; bool info_ok = false, hist_ok = false, hist_done = false, info_done = false; QString info_error; };
}  // namespace

void LogPanel::lookupNow() {
    const QString call = call_->text().trimmed();
    if (!configured() || !valid_call(call.toStdString())) { worked_->setText(" "); return; }
    const std::string band = band_for_hz(rx_->dialHz()); const int serial = ++lookup_serial_;
    auto parts = std::make_shared<LookupParts>();
    auto render = [this, parts, call, serial] {
        if (!parts->info_done || !parts->hist_done) return;
        if (serial != lookup_serial_ || call != call_->text().trimmed()) return;                                           // an older answer: the callsign has changed since
        looked_up_ = call; look_btn_->setText("Log");                                                                       // a second Enter now opens the form, even when the lookup failed
        if (!parts->info_ok) { worked_->setText("<span style='color:#ff7a7a'>Lookup failed: " + parts->info_error.toHtmlEscaped() + "</span>"); return; }
        const QJsonObject& d = parts->info; const QJsonObject& l = parts->last;
        const QString country = d.value("dxcc").toString().toHtmlEscaped(), cont = d.value("cont").toString();
        const QJsonObject cb = d.value("callbook").toObject();                                                              // Wavelog's own callbook, if it sends one (its e-mail address is never shown or used)
        const fireside::qrz::Info* qi = qrz_->cached(call); if (qi && qi->not_found) qi = nullptr;                              // QRZ.com directly: first choice
        QString name = qi ? QString::fromStdString(qi->first_name.empty() || qi->name.empty() ? qi->first_name + qi->name : qi->first_name + " " + qi->name) : QString();
        if (name.isEmpty()) name = cb.value("name").toString(); if (name.isEmpty()) name = d.value("name").toString(); if (name.isEmpty()) name = l.value("name").toString();   // then Wavelog's callbook, its record, what we logged last time
        const QString heard = rx_->heardGrid(call);                                                                       // the grid they sent on the air: where they are now
        QString grid = heard; if (grid.isEmpty() && qi) grid = QString::fromStdString(qi->grid); if (grid.isEmpty()) grid = d.value("gridsquare").toString(); if (grid.isEmpty()) grid = cb.value("gridsquare").toString(); if (grid.isEmpty()) grid = l.value("gridsquare").toString();
        QString qth = qi ? QString::fromStdString(qi->town) : cb.value("city").toString(); { const QString st = qi ? QString::fromStdString(qi->state) : cb.value("state").toString(); if (!st.isEmpty() && !qth.isEmpty()) qth += ", " + st; }
        QString t = QString("<span style='font-size:17px;font-weight:600'>%1</span>").arg(name.isEmpty() ? QString("<span style='color:#8b93a7'>name unknown</span>") : name.toHtmlEscaped());
        if (!grid.isEmpty()) t += QString("  <span style='color:#8b93a7'>%1</span>").arg(grid.toHtmlEscaped());
        if (qth.size() > 30) qth = qth.left(29) + QString::fromUtf8("\u2026");
        if (!qth.isEmpty()) t += QString("  <span style='color:#8b93a7'>%1</span>").arg(qth.toHtmlEscaped());
        t += QString("<br>%1%2").arg(country.isEmpty() ? "Unknown DXCC" : country, cont.isEmpty() ? "" : QString("  <span style='color:#8b93a7'>%1</span>").arg(cont.toHtmlEscaped()));
        if (qi) {                                                                                                           // QRZ: distance / bearing from my grid, QSL hints
            double la1, lo1, la2 = 0, lo2 = 0, km, deg; bool theirs = fireside::qrz::grid_latlon(heard.toStdString(), la2, lo2), from_qrz = false;   // heard grid first, QRZ's record (home, maybe stale) after
            if (!theirs) { theirs = qi->lat && qi->lon && (*qi->lat != 0 || *qi->lon != 0); if (theirs) { la2 = *qi->lat; lo2 = *qi->lon; } else theirs = fireside::qrz::grid_latlon(qi->grid, la2, lo2); from_qrz = theirs; }
            if (theirs && fireside::qrz::grid_latlon(my_grid_->text().toStdString(), la1, lo1)) { fireside::qrz::distance_bearing(la1, lo1, la2, lo2, km, deg); t += QString("  <span style='color:#8b93a7'>%1 km, bearing %2°%3</span>").arg(km, 0, 'f', 0).arg(deg, 0, 'f', 0).arg(from_qrz ? " (QRZ address)" : ""); }
            QStringList q; if (qi->lotw) q << "LoTW"; if (qi->eqsl) q << "eQSL"; if (qi->paper_qsl) q << "paper QSL"; if (!qi->qsl_manager.empty()) q << QString::fromStdString(qi->qsl_manager).left(30);
            if (!q.isEmpty()) t += QString("  <span style='color:#8b93a7'>%1</span>").arg(q.join(" · ").toHtmlEscaped());
        } else { const QString bearing = d.value("bearing").toString(); if (!bearing.isEmpty()) t += QString("  <span style='color:#8b93a7'>bearing %1</span>").arg(bearing.toHtmlEscaped()); }
        QString state, colour;
        if (!d.value("call_worked").toBool()) { state = "NEW callsign"; colour = "#3ddc84"; }
        else if (!d.value("call_worked_band_mode").toBool()) { state = d.value("call_worked_band").toBool() ? "worked on this band, new mode" : "worked before, new band"; colour = "#ffb454"; }
        else { state = "already worked on this band and mode"; colour = "#ff7a7a"; }
        t += QString("<br><span style='color:%1;font-weight:600'>%2</span>").arg(colour, state);
        if (!d.value("dxcc_confirmed").toBool() && !country.isEmpty()) t += "  <span style='color:#8b93a7'>(DXCC not confirmed yet)</span>";
        if (!l.isEmpty()) {
            const QString bm = l.value("band").toString() + " " + (l.value("submode").toString().isEmpty() ? l.value("mode").toString() : l.value("submode").toString());
            t += QString("<br>Last worked %1 on <b>%2</b>%3").arg(when(l.value("qso_date").toString()), bm.trimmed().toHtmlEscaped(), parts->total > 1 ? QString(" · %1 QSOs in all").arg(parts->total) : QString());
        } else if (d.value("call_worked").toBool()) t += "<br><span style='color:#8b93a7'>Last worked: (history not available)</span>";
        if (name_->text().isEmpty() && !name.isEmpty()) name_->setText(name);                                              // the lookup fills the blanks, never overwrites what was typed
        if (grid_->text().isEmpty() && !grid.isEmpty()) grid_->setText(grid);
        worked_->setText(t); if (form_open_) form_info_->setText(t);
    };
    rerender_ = render; qrz_->want(call, true);                                                                            // QRZ.com in parallel (cached: at once)
    auto* r = get(QString::fromStdString(wavelog_lookup_path(call.toStdString(), band, currentMode().toStdString(), false)));   // Wavelog: worked-before only (no second callbook lookup)
    connect(r, &QNetworkReply::finished, this, [r, parts, render] {
        r->deleteLater(); parts->info_done = true;
        if (r->error() == QNetworkReply::NoError) { parts->info = QJsonDocument::fromJson(r->readAll()).object().value("data").toObject(); parts->info_ok = true; } else parts->info_error = failure(r);
        render();
    });
    auto* h = get(QString::fromStdString(wavelog_history_path(call.toStdString())));
    connect(h, &QNetworkReply::finished, this, [h, parts, render] {
        h->deleteLater(); parts->hist_done = true;
        if (h->error() == QNetworkReply::NoError) { const QJsonObject o = QJsonDocument::fromJson(h->readAll()).object(); const QJsonArray a = o.value("data").toArray(); if (!a.isEmpty()) parts->last = a.first().toObject(); parts->total = o.value("meta").toObject().value("total").toInt(int(a.size())); parts->hist_ok = true; }
        render();
    });
}

Qso LogPanel::formQso() const {
    Qso q; q.call = call_->text().trimmed().toStdString(); q.freq_hz = rx_->dialHz(); q.mode = currentMode().toStdString(); q.time_on_utc = QDateTime::currentSecsSinceEpoch();
    q.rst_sent = rst_sent_->text().trimmed().toStdString(); q.rst_rcvd = rst_rcvd_->text().trimmed().toStdString(); q.name = name_->text().trimmed().toStdString(); q.grid = grid_->text().trimmed().toStdString();
    q.comment = comment_->text().trimmed().toStdString(); q.station_callsign = my_call_->text().toStdString(); q.my_grid = my_grid_->text().toStdString(); q.tx_pwr_w = tx_pwr_ ? tx_pwr_() : 0.0; return q;
}
void LogPanel::clearForm() { for (auto* e : {call_, rst_sent_, rst_rcvd_, name_, grid_, comment_}) e->clear(); worked_->setText(" "); looked_up_.clear(); look_btn_->setText("Look up"); closeForm(); }
void LogPanel::setCallText(const QString& call) { call_->setText(call.trimmed().toUpper()); }
void LogPanel::enterPressed() {
    const QString call = call_->text().trimmed();
    if (!valid_call(call.toStdString())) { setStatus("Enter a callsign first"); return; }
    if (form_open_) return;
    if (!configured() || looked_up_ == call) openForm(); else lookupNow();                                                 // first Enter: who is it?  second Enter: log them
}
void LogPanel::openForm() {
    const QString call = call_->text().trimmed(); if (form_open_ || !valid_call(call.toStdString())) return;
    const QString mode = currentMode(); const bool voice = mode == "USB" || mode == "LSB" || mode == "AM" || mode == "FM", morse = mode == "CW" || mode == "PSK31" || mode == "RTTY";
    if (rst_sent_->text().isEmpty() && (voice || morse)) rst_sent_->setText(voice ? "59" : "599");
    if (rst_rcvd_->text().isEmpty() && (voice || morse)) rst_rcvd_->setText(voice ? "59" : "599");
    form_title_->setText(QString("<span style='font-size:17px;font-weight:600'>Log %1</span>  <span style='color:#8b93a7'>%2 kHz  %3  %4 UTC</span>").arg(call.toHtmlEscaped()).arg(rx_->dialHz() / 1e3, 0, 'f', 3).arg(mode, QDateTime::currentDateTimeUtc().toString("HH:mm")));
    form_info_->setText(worked_->text().trimmed().isEmpty() ? QString("<span style='color:#8b93a7'>No lookup</span>") : worked_->text());
    form_open_ = true; emit activeChanged(true); rst_rcvd_->setFocus(); rst_rcvd_->selectAll();
}
void LogPanel::closeForm() { if (!form_open_) return; form_open_ = false; emit activeChanged(false); call_->setFocus(); }
void LogPanel::setCall(const QString& call) { call_->setText(call.trimmed().toUpper()); lookupNow(); }

int LogPanel::unsentCount() const {
    QFile f(dir_ + "/unsent.adi"); if (!f.open(QIODevice::ReadOnly)) return 0;
    return f.readAll().count("<EOR>");
}

void LogPanel::logNow() {
    const Qso q = formQso(); std::string rec;
    try { rec = adif_record(q); } catch (const std::exception& e) { setStatus(QString("Not logged: ") + e.what()); return; }
    QDir().mkpath(dir_);
    try { append_adif_file((dir_ + "/log.adi").toStdString(), q, "fireside", "1"); } catch (const std::exception& e) { setStatus(QString("Not logged: cannot write the local copy: ") + e.what()); return; }   // the safety copy comes first
    if (index_ready_) index_.add(q.call, band_for_hz(q.freq_hz), q.grid, false);                            // worked now, whether or not Wavelog takes it (counted when Wavelog reports it)
    refreshLastContacts(); for (const QString& k : worked_cache_.keys()) if (k.startsWith(QString::fromStdString(q.call).toUpper() + "|")) worked_cache_.remove(k);
    const QString line = QString("%1  %2  %3 kHz  %4").arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss"), QString::fromStdString(q.call)).arg(q.freq_hz / 1e3, 0, 'f', 3).arg(QString::fromStdString(q.mode));
    if (!configured()) { try { append_adif_file((dir_ + "/unsent.adi").toStdString(), q, "fireside", "1"); } catch (...) {} recent_->appendPlainText(line + "   [saved locally only]"); setStatus("Saved locally (Wavelog not configured)"); last_logged_ = QString::fromStdString(q.call).toUpper(); last_logged_at_ = QDateTime::currentDateTimeUtc(); auto_call_.clear(); clearForm(); unsent_btn_->setVisible(unsentCount() > 0); return; }
    auto* r = post(kWavelogQsoPath, QByteArray::fromStdString(wavelog_adif_import_body(station_->value(), rec, false)));
    last_logged_ = QString::fromStdString(q.call).toUpper(); last_logged_at_ = QDateTime::currentDateTimeUtc(); auto_call_.clear();
    clearForm();
    connect(r, &QNetworkReply::finished, this, [this, r, q, line] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            const QString why = failure(r); try { append_adif_file((dir_ + "/unsent.adi").toStdString(), q, "fireside", "1"); } catch (...) {}
            recent_->appendPlainText(line + "   [NOT SENT, kept locally]"); setStatus(why + ": saved locally, use Send unsent later"); unsent_btn_->setVisible(true); return;
        }
        const QJsonObject d = QJsonDocument::fromJson(r->readAll()).object().value("data").toObject();
        recent_->appendPlainText(line + (d.value("imported").toInt() > 0 ? "   [sent]" : "   [Wavelog skipped it: duplicate]")); refreshFromWavelog(); setStatus(d.value("imported").toInt() > 0 ? "Logged to Wavelog" : "Wavelog skipped it as a duplicate");
    });
}

void LogPanel::sendUnsent() {
    QFile f(dir_ + "/unsent.adi"); if (!configured() || !f.open(QIODevice::ReadOnly)) return;
    const QByteArray all = f.readAll(); f.close(); if (!all.contains("<EOR>")) return;
    auto* r = post(kWavelogQsoPath, QByteArray::fromStdString(wavelog_adif_import_body(station_->value(), all.toStdString(), false)));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater(); if (r->error() != QNetworkReply::NoError) { setStatus(failure(r)); return; }
        const QJsonObject d = QJsonDocument::fromJson(r->readAll()).object().value("data").toObject();
        QFile::remove(dir_ + "/unsent.adi"); unsent_btn_->setVisible(false);
        setStatus(QString("Sent the saved contacts: %1 new, %2 already in Wavelog").arg(d.value("imported").toInt()).arg(d.value("skipped").toInt()));
    });
}

void LogPanel::setRadioPush(bool on) {
    QSignalBlocker b(push_); push_->setChecked(on); last_push_hz_ = -1; push_retry_ms_ = 0;
    if (!on) push_state_->setText("Not sending"); else if (!configured()) push_state_->setText("Set up the Wavelog connection first"); else pushRadioIfChanged();
}
QString LogPanel::radioPushStatus() const { return push_state_->text(); }
void LogPanel::pushRadioIfChanged() {
    if (!push_->isChecked() || !configured() || radio_name_->text().trimmed().isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch(); if (now < push_retry_ms_) return;                 // after an error: wait a minute, not 3 s
    const double hz = std::round(rx_->dialHz()); const QString mode = currentMode();
    const double pw = tx_pwr_ ? tx_pwr_() : 0.0; const int w = pw >= 0.5 ? int(std::lround(pw)) : -1;          // -1: not known (no over in the last 10 minutes); keep what Wavelog has
    const bool changed = hz != last_push_hz_ || mode != last_push_mode_ || (w > 0 && w != last_push_w_);
    if (!changed && now - last_push_ms_ < 5 * 60 * 1000) return;                                               // heartbeat: Wavelog counts a radio as current for 15 minutes
    last_push_hz_ = hz; last_push_mode_ = mode; if (w > 0) last_push_w_ = w; last_push_ms_ = now;
    auto* r = post(kWavelogRadioPath, QByteArray::fromStdString(wavelog_radio_body(radio_name_->text().trimmed().toStdString(), hz, mode.toStdString(), w > 0 ? w : 0)));
    const QString what = QString("%1 kHz %2%3").arg(hz / 1e3, 0, 'f', 3).arg(mode).arg(w > 0 ? QString(" %1 W").arg(w) : QString());
    connect(r, &QNetworkReply::finished, this, [this, r, what] {
        r->deleteLater();
        if (r->error() == QNetworkReply::NoError) { ++radio_pushes_; push_retry_ms_ = 0; push_state_->setText(QString("Sent %1 UTC: %2").arg(QDateTime::currentDateTimeUtc().toString("HH:mm:ss"), what)); return; }
        const QString why = failure(r); last_push_hz_ = -1; push_retry_ms_ = QDateTime::currentMSecsSinceEpoch() + 60 * 1000;   // send again in a minute
        push_state_->setText(why + (why.contains("403") ? " (the token needs the radio:write scope)" : "") + ". Trying again in a minute.");
    });
}

// ---------------------------------------------------------------- worked-before tags for decoded FT8/FT4 stations: one cached, read-only lookup per callsign (no callbook), one request at a time
void LogPanel::checkWorked(const QString& callIn) {
    const QString call = callIn.trimmed().toUpper();
    if (!configured() || !valid_call(call.toStdString()) || call == my_call_->text().toUpper()) return;
    const QString key = call + "|" + QString::fromStdString(band_for_hz(rx_->dialHz())) + "|" + currentMode();
    const auto it = worked_cache_.constFind(key); if (it != worked_cache_.constEnd()) { emitWorked(call, it.value()); return; }
    if (!worked_queue_.contains(key) && worked_queue_.size() < 40) worked_queue_ << key;
    pumpWorked();
}
void LogPanel::pumpWorked() {
    if (worked_busy_ || worked_queue_.isEmpty()) return;
    const QString key = worked_queue_.takeFirst(); const QStringList p = key.split('|'); worked_busy_ = true;
    auto* r = get(QString::fromStdString(wavelog_lookup_path(p[0].toStdString(), p[1].toStdString(), p[2].toStdString(), false)));
    connect(r, &QNetworkReply::finished, this, [this, r, key, p] {
        r->deleteLater();
        if (r->error() == QNetworkReply::NoError) {
            const QJsonObject d = QJsonDocument::fromJson(r->readAll()).object().value("data").toObject();
            const int st = !d.value("call_worked").toBool() ? 0 : !d.value("call_worked_band_mode").toBool() ? 1 : 2;
            worked_cache_.insert(key, st); emitWorked(p[0], st);
        }
        QTimer::singleShot(250, this, [this] { worked_busy_ = false; pumpWorked(); });                              // gentle on the server: at most four lookups a second
    });
}
void LogPanel::emitWorked(const QString& call, int st) {
    const int a = index_ready_ ? index_.tag(call.toStdString(), band_for_hz(rx_->dialHz()), rx_->heardGrid(call).toStdString(), hl_) : fireside::award::kNone;
    emit workedKnown(call, a != fireside::award::kNone ? a : st);
}
// ---------------------------------------------------------------- the whole log, for the new DXCC / grid tags: built aside and swapped in only when every page has arrived
void LogPanel::loadWorkedIndex() {
    if (!configured() || index_busy_) return;
    index_busy_ = true; if (!index_ready_) hl_state_->setText("Reading the log from Wavelog...");
    auto load = std::make_shared<IndexLoad>();
    if (index_ready_) { load->idx = index_; load->since = load->max_id = index_max_id_; }                  // after the first read: only what is new (usually nothing)
    fetchIndexPage(1, load);
}
void LogPanel::fetchIndexPage(int page, std::shared_ptr<IndexLoad> load) {
    auto* r = get(QString::fromStdString(wavelog_all_qsos_path(page, load->since)));
    connect(r, &QNetworkReply::finished, this, [this, r, page, load] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) { index_busy_ = false; hl_state_->setText(failure(r) + (index_ready_ ? ": showing the log read earlier" : ": no DXCC or grid tags until it can be read")); return; }
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        for (const auto& x : o.value("data").toArray()) {
            const QJsonObject q = x.toObject(); const long long id = q.value("id").toVariant().toLongLong();
            if (load->since > 0 && id <= load->since) continue;                                                   // a server that ignores since_id: no double count
            load->idx.add(q.value("call").toString().toStdString(), q.value("band").toString().toStdString(), q.value("gridsquare").toString().toStdString());
            load->max_id = std::max(load->max_id, id);
        }
        if (o.value("meta").toObject().value("has_more").toBool() && page < 200) { fetchIndexPage(page + 1, load); return; }
        index_ = load->idx; index_max_id_ = load->max_id; index_ready_ = true; index_busy_ = false;         // swapped in only when every page has arrived
        hl_state_->setText(QString("Log read %1 UTC: %2 QSOs, %3 DXCC entities, %4 grids").arg(QDateTime::currentDateTimeUtc().toString("HH:mm")).arg(index_.qsos()).arg(index_.dxcc_count()).arg(index_.grid_count()));
    });
}
QString LogPanel::highlightStatus() const { return hl_state_->text(); }
bool LogPanel::fillFromDecode(const QString& line) {
    int snr = 0; const std::string text = ftx_line_text(line.toStdString(), &snr); const FtxMessage m = parse_ftx_message(text);
    if (!m.ok) { setStatus("That line has no usable callsign"); return false; }
    clearForm(); call_->setText(QString::fromStdString(m.from)); if (!m.grid.empty()) grid_->setText(QString::fromStdString(m.grid));
    char b[8]; std::snprintf(b, sizeof b, "%+03d", std::clamp(snr, -30, 30)); rst_sent_->setText(b);                    // the report we would send: how well we hear them
    if (!m.report.empty() && m.to == my_call_->text().toStdString()) rst_rcvd_->setText(QString::fromStdString(m.report));   // they sent us a report
    comment_->setText(QString::fromStdString(text)); setStatus(QString("Filled from the decode: %1 (Enter again to log)").arg(QString::fromStdString(m.from)));
    lookupNow(); return true;
}
void LogPanel::callerDecoded(const QString& line) {
    int snr = 0; const FtxMessage m = parse_ftx_message(ftx_line_text(line.toStdString(), &snr)); if (!m.ok || m.from.empty()) return;
    const QString from = QString::fromStdString(m.from), cur = call_->text().trimmed().toUpper();
    if (from == last_logged_ && last_logged_at_.isValid() && last_logged_at_.secsTo(QDateTime::currentDateTimeUtc()) < 600) return;   // just logged: their RR73 / 73
    if (form_open_ || (!cur.isEmpty() && cur != from)) { setStatus(QString("%1 is also calling you (the entry holds %2: left alone; double-click the line to switch)").arg(from, cur.isEmpty() ? "an open form" : cur)); return; }
    if (cur == from) { if (!m.report.empty() && m.to == my_call_->text().toUpper().toStdString()) rst_rcvd_->setText(QString::fromStdString(m.report)); return; }   // the same caller again: only their report
    auto_call_ = from; fillFromDecode(line); setStatus(QString("%1 is calling you: looked up (Enter to log)").arg(from));
}
void LogPanel::logAuto(const QString& call, const QString& grid, const QString& sent, const QString& rcvd) {
    const bool same = call_->text().trimmed().toUpper() == call.toUpper();
    const QString name = same ? name_->text() : QString(), comment = same ? comment_->text() : QString();
    if (form_open_) closeForm();
    clearForm(); call_->setText(call.toUpper()); grid_->setText(grid); rst_sent_->setText(sent); rst_rcvd_->setText(rcvd);
    if (!name.isEmpty()) name_->setText(name); else if (const auto* qi = qrz_->cached(call); qi && !qi->not_found) name_->setText(QString::fromStdString(qi->first_name));
    comment_->setText(comment);
    logNow(); setStatus(QString("%1 logged automatically (sent %2, rcvd %3) - ").arg(call, sent, rcvd) + status_);
}
QString LogPanel::callText() const { return call_->text(); }
QString LogPanel::rstRcvdText() const { return rst_rcvd_->text(); }
