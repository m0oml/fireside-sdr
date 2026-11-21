#include <QApplication>
#include <chrono>
#include <cmath>
#include <sys/resource.h>
#include <QFileDialog>
#include <QGuiApplication>
#include <QScreen>
#include <QElapsedTimer>
#include <QStyleFactory>
#include <QMouseEvent>
#include <QPainter>
#include <QAbstractButton>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QSettings>
#include <QShortcut>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QEventLoop>
#include <QLineEdit>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <cmath>
#include <set>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include "fireside/decode.h"
#include "fireside/nr2.h"
#include "fireside/dsp.h"
#include "MainWindow.h"
#include <QUdpSocket>
#include "PskPanel.h"
#include "CatPanel.h"
#include "AppLog.h"
#include <sys/socket.h>
#include <csignal>
#include <QSocketNotifier>
#include "LogPanel.h"
#include "QrzClient.h"
#include "AudioPanel.h"
#include "PowerMeter.h"
#include <QDateTime>
#include <QTimeZone>
#include <QStandardPaths>
#include "TxPanel.h"
#include "TransmitCard.h"
#include "ReceiverPanel.h"
#include "RadioPanel.h"
#include "RecordController.h"
#include "Theme.h"
#include "FrequencyDisplay.h"
#include "SMeter.h"
#include "SpectrumWidget.h"
#include "WaterfallWidget.h"
#include <QWheelEvent>
#include "fireside/opcodes.h"
#include <QThread>
#include <QSlider>
#include "AntennaPanel.h"
#include "fireside/rxant.h"
#include "fireside/display.h"

// --smoke           build/link/start check for CTest; no hardware involved
// --smoke-receiver  drives the receiver panel on the synthetic demo (offscreen): checks waterfall, peak position and a non-blank render; optional --screenshot out.png
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if ((!std::strncmp(argv[i], "--smoke", 7) && std::strcmp(argv[i], "--smoke-settings")) || !std::strcmp(argv[i], "--screenshot-tabs")) qputenv("FIRESIDE_NO_SETTINGS", "1");      // smoke runs never read or write the user's saved settings
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion")); app.setStyleSheet(theme::stylesheet());     // one consistent look everywhere
    if (argc > 1 && !std::strcmp(argv[1], "--smoke-log")) {               // the Log tab against a fake Wavelog on localhost (no real server, no real files)
        QTemporaryDir tmp; std::string failed; auto step = [&](const char* n, bool good) { if (!good && failed.empty()) failed = n; };
        struct Seen { QString method, path, auth, body; }; std::vector<Seen> seen; int mode = 0; QString qrz_key = "KEY1"; bool qrz_expire_once = false;                  // mode 0 = healthy, 1 = refuses the token (401)
        QTcpServer srv; srv.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&srv, &QTcpServer::newConnection, [&] {
            QTcpSocket* c = srv.nextPendingConnection(); auto* buf = new QByteArray;
            QObject::connect(c, &QTcpSocket::readyRead, [&, c, buf] {
                *buf += c->readAll(); const int he = buf->indexOf("\r\n\r\n"); if (he < 0) return;
                const QString head = QString::fromLatin1(buf->left(he)); int cl = 0; QString auth; for (const QString& l : head.split("\r\n")) { if (l.startsWith("Content-Length:", Qt::CaseInsensitive)) cl = l.mid(15).trimmed().toInt(); if (l.startsWith("Authorization:", Qt::CaseInsensitive)) auth = l.mid(14).trimmed(); }
                if (buf->size() < he + 4 + cl) return;
                const QStringList rl = head.split("\r\n").first().split(' '); seen.push_back({rl[0], rl[1], auth, QString::fromUtf8(buf->mid(he + 4, cl))});
                QByteArray body; int code = 200; const QString& p = rl[1];
                if (p.contains("/xml/current/")) {                                                                  // fake QRZ.com XML
                    auto sess = [](const QString& key, const QString& err) { return QString("<Session>%1%2<Count>42</Count><SubExp>Wed Jan 1 12:34:03 2027</SubExp></Session>").arg(key.isEmpty() ? "" : "<Key>" + key + "</Key>", err.isEmpty() ? "" : "<Error>" + err + "</Error>"); };
                    QString x;
                    if (p.contains("username=")) x = p.contains("password=good") ? sess(qrz_key, "") : sess("", "Username/password incorrect");
                    else if (!p.contains("s=" + qrz_key)) x = sess("", "Invalid session key");
                    else if (qrz_expire_once) { qrz_expire_once = false; qrz_key = "KEY2"; x = sess("", "Session Timeout"); }
                    else if (p.contains("callsign=DK0ULR")) x = "<Callsign><call>DK0ULR</call><fname>Klaus</fname><name>Club</name><addr1>Secret Street 1</addr1><addr2>Ulm</addr2><country>Germany</country><lat>48.4</lat><lon>9.98</lon><grid>JN58ri</grid><email>secret@example.com</email><lotw>1</lotw></Callsign>" + sess(qrz_key, "");
                    else if (p.contains("callsign=SP9ABC")) x = "<Callsign><call>SP9ABC</call><fname>Jan</fname><addr2>Krakow</addr2><country>Poland</country><grid>KN09</grid></Callsign>" + sess(qrz_key, "");
                    else { const int at = p.indexOf("callsign="); x = sess(qrz_key, "Not found: " + p.mid(at + 9)); }
                    body = ("<?xml version=\"1.0\"?><QRZDatabase version=\"1.34\">" + x + "</QRZDatabase>").toUtf8();
                }
                else if (mode == 1) { code = 401; body = "{\"status\":\"failed\",\"reason\":\"missing or invalid api key\"}"; }
                else if (p.contains("/api/v2/token")) body = "{\"data\":{\"owner\":\"M0OML\",\"scopes\":[\"qso:write\",\"lookup:read\",\"radio:write\"]}}";
                else if (p.contains("/api/v2/station")) body = "{\"data\":[{\"id\":3,\"callsign\":\"M0OML\",\"gridsquare\":\"IO92AR\"}]}";
                else if (p.contains("/api/v2/lookup")) body = "{\"data\":{\"dxcc\":\"FEDERAL REPUBLIC OF GERMANY\",\"call_worked\":true,\"call_worked_band\":true,\"call_worked_band_mode\":false,\"dxcc_confirmed\":true,\"callbook\":{\"name\":\"Klaus Meyer\",\"gridsquare\":\"JO62\",\"city\":\"Hamburg\",\"email\":\"secret@example.com\"}}}";
                else if (p.contains("/api/v2/qso") && rl[0] == "GET") body = "{\"data\":[{\"qso_date\":\"2026-11-10 12:30:00\",\"band\":\"20m\",\"mode\":\"FT8\",\"submode\":null,\"name\":\"Klaus\",\"call\":\"DL2AL\"}],\"meta\":{\"total\":2}}";
                else if (p.contains("/api/v2/qso")) { code = 201; body = "{\"data\":{\"parsed\":1,\"imported\":1,\"skipped\":0,\"messages\":[]}}"; }
                else if (p.contains("/api/v2/radio")) { code = 201; body = "{\"data\":{\"id\":1}}"; }
                c->write(QString("HTTP/1.1 %1 X\r\nContent-Type: application/json\r\nContent-Length: %2\r\nConnection: close\r\n\r\n").arg(code).arg(body.size()).toLatin1() + body); c->disconnectFromHost();
            });
        });
        auto waitFor = [&](auto cond) { QEventLoop l; QTimer t; t.setInterval(20); QElapsedTimer e; e.start(); QObject::connect(&t, &QTimer::timeout, [&] { if (cond() || e.elapsed() > 4000) l.quit(); }); t.start(); l.exec(); return cond(); };
        const QString url = QString("http://127.0.0.1:%1/").arg(srv.serverPort()); const QString token = "wl2_testtoken0123456789";
        MainWindow w(false, false, true); auto* lp = w.logPanel(); auto* r = w.receiver(); lp->setLocalDir(tmp.path());
        step("not configured at first", !lp->configured()); lp->setServer(url, token, 3); step("configured", lp->configured()); r->setModeIndex(0); r->setDialHz(14.074e6);
        lp->testConnection(); step("connects", waitFor([&] { return lp->status().startsWith("Connected as M0OML"); }));
        step("bearer token sent", !seen.empty() && seen[0].auth == "Bearer " + token); step("token never shown in status", !lp->status().contains(token));
        lp->refreshFromWavelog(); step("last contacts come from Wavelog", waitFor([&] { return lp->lastContactsText().contains("(Wavelog)") && lp->lastContactsText().contains("DL2AL"); }));
        { int ws = -1; QString wc; QObject scope; QObject::connect(lp, &LogPanel::workedKnown, &scope, [&](const QString& c, int s) { wc = c; ws = s; }); /* scoped: disconnects at the end of this block */ lp->checkWorked("DL2AL"); step("worked-before check answers: worked, but not on this band and mode", waitFor([&] { return ws == 1 && wc == "DL2AL"; }));
          const size_t n = seen.size(); ws = -1; lp->checkWorked("DL2AL"); step("the answer is cached (no second request)", ws == 1 && seen.size() == n); }
        lp->setCallText("DL2AL"); lp->enterPressed(); step("first Enter looks up", waitFor([&] { return lp->workedText().contains("worked on this band"); })); step("lookup alone does not open the form", !lp->formOpen() && !r->logActive());
        lp->enterPressed(); step("second Enter opens the log form over Band and Mode", lp->formOpen() && r->logActive()); lp->closeForm(); step("Cancel closes it again", !lp->formOpen() && !r->logActive()); lp->enterPressed(); step("reopened", r->logActive()); step("callbook name and town shown", lp->workedText().contains("Klaus Meyer") && lp->workedText().contains("Hamburg")); step("callbook e-mail never shown", !lp->workedText().contains("secret@")); step("last worked time, band and mode shown", lp->workedText().contains("2026-11-10 12:30 UTC") && lp->workedText().contains("20m FT8") && lp->workedText().contains("2 QSOs in all")); step("lookup path", std::any_of(seen.begin(), seen.end(), [](const Seen& s) { return s.path.contains("callsign=DL2AL") && s.path.contains("band=20m"); }));
        lp->logNow(); step("logged", waitFor([&] { return lp->status() == "Logged to Wavelog"; })); step("form closes after logging", !r->logActive() && !lp->formOpen());
        QString qsoBody; for (const auto& s : seen) if (s.method == "POST" && s.path.contains("/api/v2/qso")) qsoBody = s.body;
        step("qso body", qsoBody.contains("\"station_profile_id\":3") && qsoBody.contains("<CALL:5>DL2AL") && qsoBody.contains("<BAND:3>20m") && !qsoBody.contains("dryrun")); step("local copy written", QFile(tmp.path() + "/log.adi").exists() && lp->unsentCount() == 0);
        lp->logNow(); step("empty call refused", lp->status().startsWith("Not logged"));
        step("fill from a CQ decode", lp->fillFromDecode("110130  -6  0.7  683 ~  CQ TA6CQ KN70") && lp->callText() == "TA6CQ");
        step("a line without a callsign is refused", !lp->fillFromDecode("110130  -6  0.7  683 ~  TNX 73"));
        step("fill from a directed decode", lp->fillFromDecode("110145  -9  0.3 1200 ~  M0OML DL2AL -07") && lp->callText() == "DL2AL");
        lp->logNow(); step("decode-filled QSO logged", waitFor([&] { return lp->status() == "Logged to Wavelog"; }));
        { QString b2; for (const auto& s2 : seen) if (s2.method == "POST" && s2.path.contains("/api/v2/qso")) b2 = s2.body;
          step("decode-filled fields", b2.contains("<CALL:5>DL2AL") && b2.contains("<RST_SENT:3>-09") && b2.contains("<RST_RCVD:3>-07")); }
        {   // the callers area: whoever calls me stays, fading, until I answer them or they have gone unanswered for 6 FT8 / 10 FT4 cycles
            qint64 clk = 1000000; r->setCallersClockForTests([&clk] { return clk; });
            r->setDecoderIndex(4); r->myCallBox()->setText("M0OML"); r->clearDecoded();
            r->addDecodedText("093300  -3  0.2 1200 ~  CQ DL2AL JO31\n093300  -8  0.1  800 ~  SP9ABC DL2AL -05\n093315  -6  0.1 2576 ~  M0OML DK0ULR JN58\n");
            step("callers: someone calling me is listed; CQs and other traffic are not", r->callersText().contains("DK0ULR") && !r->callersText().contains("DL2AL") && !r->callersText().contains("SP9ABC"));
            r->showCallInfo("DK0ULR", "Klaus, Ulm, Germany, 770 km"); r->markWorked("DK0ULR", 0);
            step("callers: the callbook info and the NEW tag show in the callers area", r->callersText().contains("Klaus, Ulm, Germany, 770 km") && r->callersText().contains("NEW"));
            r->addDecodedText("093325  -5  0.1 1500 ~  M0OML DK0ULR JN58\n"); step("callers: a repeat call keeps the info", r->callersText().contains("Klaus, Ulm") && r->callersText().count("DK0ULR") == 1);
            clk += 5 * 15000; r->tickCallers(); step("callers: still listed after 5 FT8 cycles", r->callersText().contains("DK0ULR"));
            clk += 15000; r->tickCallers(); step("callers: gone after 6 unanswered FT8 cycles", r->callersText().isEmpty());
            r->addDecodedText("093330  -6  0.1 1500 ~  M0OML EA3ABC JN11\n"); clk += 4 * 15000;
            r->addDecodedText("093430  -5  0.1 1500 ~  M0OML EA3ABC JN11\n"); clk += 4 * 15000; r->tickCallers();
            step("callers: a repeat call refreshes the entry (listed once)", r->callersText().count("EA3ABC") == 1);
            r->addQsoTx("093445", 1500, "EA3ABC M0OML -05"); step("callers: our over to them answers them (removed at once)", r->callersText().isEmpty());
            for (int k = 1; k <= 6; ++k) { r->addDecodedText(QString("0935%1  -6  0.1 1500 ~  M0OML CALL%2X JN11\n").arg(10 + k).arg(k)); clk += 1000; }
            step("callers: only the newest four are shown", r->callersText().split('\n').size() == 4 && r->callersText().contains("CALL6X") && !r->callersText().contains("CALL2X"));
            r->setQsoPartner("CALL6X"); step("callers: replying to a line (partner set) removes that caller", !r->callersText().contains("CALL6X")); r->setQsoPartner(QString());
            r->setDecoderIndex(5); r->addDecodedText("093700  -9  0.2 1200 ~  M0OML DK0ULR JN58\n");
            clk += 9 * 7500; r->tickCallers(); step("callers: still listed after 9 FT4 cycles", r->callersText().contains("DK0ULR"));
            clk += 7500; r->tickCallers(); step("callers: gone after 10 unanswered FT4 cycles", r->callersText().isEmpty());
            r->setCallersClockForTests({}); r->setDecoderIndex(4); r->clearDecoded(); lp->clearForm();   // (the Log tab entry was filled by those callers)
        }
        {   // someone answering me: the line is highlighted, the entry filled and the caller looked up; others calling meanwhile are left alone
            r->setDecoderIndex(4); r->myCallBox()->setText("M0OML"); r->clearDecoded();
            r->addDecodedText("093300  -3  0.2 1200 ~  CQ DL2AL JO31\n093300  -8  0.1  800 ~  SP9ABC DL2AL -05\n");
            step("lines not to me are not highlighted", r->highlightedLines() == 0 && lp->callText().isEmpty());
            const size_t n0 = seen.size();
            r->addDecodedText("093315  -6  0.1 2576 ~  M0OML DK0ULR JN58\n093315 -12  0.4 1900 ~  M0OML/P SP9ABC JO90\n");
            step("both lines to me highlighted (portable form too)", r->highlightedLines() == 2);
            step("the first caller fills the entry", lp->callText() == "DK0ULR");
            step("the first caller is looked up", waitFor([&] { return std::any_of(seen.begin() + long(n0), seen.end(), [](const Seen& s) { return s.path.contains("callsign=DK0ULR"); }); }));
            step("the second caller leaves the entry alone and says so", lp->callText() == "DK0ULR" && lp->status().contains("SP9ABC is also calling you"));
            r->addDecodedText("093345  -5  0.1 2576 ~  M0OML DK0ULR R-07\n"); step("the same caller again: their report", lp->rstRcvdText() == "R-07" || lp->rstRcvdText() == "-07");
            lp->logNow(); waitFor([&] { return lp->status() == "Logged to Wavelog"; });
            r->addDecodedText("093415  -5  0.1 2576 ~  M0OML DK0ULR RR73\n"); step("their RR73 after logging does not refill the entry", lp->callText().isEmpty());
            r->addDecodedText("093430  -9  0.1 1900 ~  M0OML SP9ABC -11\n"); step("the next caller then fills it", lp->callText() == "SP9ABC");
            step("Wavelog is asked for worked-before only (no callbook through it)", std::none_of(seen.begin(), seen.end(), [](const Seen& s) { return s.path.contains("callbook=true"); }));
            r->setDecoderIndex(0); r->clearDecoded();
        }
        {   // QRZ.com straight from QRZ: login, lookups queued (callers first), cached, shown on the decoder lines and in the lookup box
            auto* q = lp->qrz(); q->setBaseUrl(url + "xml/current/"); q->setCachePath(tmp.path() + "/qrz-cache.tsv");
            auto qrzReqs = [&] { return std::count_if(seen.begin(), seen.end(), [](const Seen& s) { return s.path.contains("/xml/current/"); }); };
            step("QRZ not configured at first", !q->configured());
            lp->setQrzCredentials("m0oml", "wrong"); lp->qrzSaveAndLogin();
            step("a wrong password is reported", waitFor([&] { return lp->qrzStatus().contains("refused") && lp->qrzStatus().contains("incorrect"); }));
            lp->setQrzCredentials("m0oml", "good"); lp->qrzSaveAndLogin();
            step("logged in, subscription and QRZ's count shown", waitFor([&] { return lp->qrzStatus().contains("logged in as m0oml") && lp->qrzStatus().contains("2027") && lp->qrzStatus().contains("42 lookups"); }));
            step("the password is never shown", !lp->qrzStatus().contains("good"));
            r->setDecoderIndex(4); r->myCallBox()->setText("M0OML"); r->clearDecoded(); lp->clearForm();
            r->addDecodedText("101500  -6  0.1 2576 ~  M0OML DK0ULR JN58\n");
            step("a caller's callbook info goes on their decoder line", waitFor([&] { return r->decodedText().contains("Klaus, Ulm, Germany, ") && r->decodedText().contains(" km"); }));
            step("the distance comes from the grid they sent (JN58), not QRZ's record", !r->decodedText().split('\n').first().contains("(QRZ)") && r->heardGrid("DK0ULR") == "JN58");
            step("the street and e-mail are never shown", !r->decodedText().contains("Secret") && !r->decodedText().contains("secret@") && !lp->workedText().contains("secret@"));
            lp->setCall("DK0ULR");                                                               // (logged above, so their call did not refill the entry by itself)
            step("the lookup box uses QRZ (name, distance, LoTW)", waitFor([&] { return lp->workedText().contains("Klaus Club") && lp->workedText().contains("Ulm") && lp->workedText().contains("bearing") && lp->workedText().contains("LoTW"); }));
            step("a line with callbook info still parses", lp->fillFromDecode(r->decodedText().split('\n').first()) && lp->callText() == "DK0ULR");
            const auto n1 = qrzReqs(); r->addDecodedText("101515  -6  0.1 2576 ~  M0OML DK0ULR R-05\n"); QCoreApplication::processEvents();
            step("cached: no second request, the info shown at once", qrzReqs() == n1 && r->decodedText().count("Klaus, Ulm") == 2);
            r->addDecodedText("101530 -10  0.2 1900 ~  CQ SP9ABC KN09\n101530 -14  0.3  800 ~  CQ XX9XX AA00\n");
            step("stations calling CQ looked up (one a second)", waitFor([&] { return r->decodedText().contains("Jan, Krakow, Poland"); }));
            step("not found: cached", waitFor([&] { return q->cached("XX9XX") != nullptr; })); 
            step("not found: no callbook details (only the distance from the grid they sent)", [&] { for (const QString& l : r->decodedText().split('\n')) if (l.contains("XX9XX AA00") && l.section(QChar(0x2502), 1).contains(',')) return false; return true; }());
            q->setEveryDecode(false); const auto n2 = qrzReqs(); r->addDecodedText("101545  -9  0.1 1100 ~  CQ OH2NEW KP20\n"); QCoreApplication::processEvents();
            step("with the CQ option off, plain CQs are not looked up", qrzReqs() == n2); q->setEveryDecode(true);
            { const auto n3 = qrzReqs(); r->addDecodedText("101600  -9  0.1 1300 ~  G4AAA SP9NEW KO00\n"); QCoreApplication::processEvents(); waitFor([&] { return false; });
              step("a station working someone else (not CQ, not me) is not looked up", qrzReqs() == n3 && q->cached("SP9NEW") == nullptr); }
            qrz_expire_once = true; q->want("SP9ZZZ", true);
            step("an expired session logs in again and the lookup goes on", waitFor([&] { return q->cached("SP9ZZZ") != nullptr; }) && q->loggedIn());
            QFile cf(tmp.path() + "/qrz-cache.tsv"); step("cache written without e-mail or street", cf.open(QIODevice::ReadOnly) && [&] { const QByteArray a = cf.readAll(); return a.contains("DK0ULR") && !a.contains("secret") && !a.contains("Secret"); }());
            QrzClient q2; q2.setCachePath(tmp.path() + "/qrz-cache.tsv"); step("the cache is read back next time", q2.cached("DK0ULR") && q2.cached("DK0ULR")->town == "Ulm");
            r->setDecoderIndex(0); r->clearDecoded(); lp->clearForm();
        }
        lp->setRadioPush(true); step("radio pushed", waitFor([&] { return lp->radioPushes() >= 1; })); const uint64_t n = lp->radioPushes(); lp->pushRadioIfChanged(); QCoreApplication::processEvents(); step("no repeat push when unchanged", lp->radioPushes() == n);
        r->setDialHz(7.074e6); lp->pushRadioIfChanged(); step("push on change", waitFor([&] { return lp->radioPushes() > n; }));
        QString radioBody; for (const auto& s : seen) if (s.path.contains("/api/v2/radio")) radioBody = s.body; step("radio body has no power", radioBody.contains("\"frequency\":7074000") && !radioBody.contains("power"));
        mode = 1; lp->setCall("pa4ps"); waitFor([&] { return lp->workedText().contains("refused"); }); step("401 reported in lookup", lp->workedText().contains("refused"));
        lp->setCall("pa4ps"); lp->logNow(); step("refused QSO kept locally", waitFor([&] { return lp->unsentCount() == 1; }) && lp->status().contains("saved locally"));
        mode = 0; lp->sendUnsent(); step("unsent resent", waitFor([&] { return lp->unsentCount() == 0; }));
        srv.close(); lp->setCall("k1abc"); lp->logNow(); step("unreachable server keeps the QSO", waitFor([&] { return lp->unsentCount() == 1; }) && lp->status().contains("Cannot reach"));
        if (!failed.empty()) { std::printf("log smoke FAILED at: %s (status: %s)\n", failed.c_str(), lp->status().toStdString().c_str()); return 1; }
        if (argc > 3 && !std::strcmp(argv[2], "--screenshot")) { w.resize(900, 900); w.show(); w.findChild<QTabWidget*>()->setCurrentWidget(lp); lp->setCall("DL2AL"); waitFor([&] { return false; }); w.grab().save(argv[3]); }
        for (int i = 2; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--screenshot")) { lp->resize(1700, 900); lp->grab().save(QString::fromUtf8(argv[i + 1])); }
        std::printf("log smoke ok: %zu requests\n", seen.size()); return 0;
    }
    if (argc > 3 && !std::strcmp(argv[1], "--screenshot-tabs")) {          // pictures of the Receiver tab (USB, 20 m) and the Log tab; argv[2] = output prefix, argv[3] = "log" to use the real Wavelog from ~/.config/fireside/wavelog.env
        MainWindow w(false, false, true); w.resize(1500, 900); w.show(); auto* r = w.receiver(); auto* lp = w.logPanel(); auto* tabs = w.findChild<QTabWidget*>();
        r->loadDemo(); r->setPlaying(false); r->setModeIndex(0); r->setDialHz(14.2e6); r->setNotchesHz({1000, 2200}); r->setPassbandCuts(300, 2700); for (int i = 0; i < 40; ++i) r->advance(4800); QApplication::processEvents();
        if (!std::strcmp(argv[3], "log")) { QFile f(QDir::homePath() + "/.config/fireside/wavelog.env"); QString url, key; if (f.open(QIODevice::ReadOnly)) for (const QByteArray& l : f.readAll().split('\n')) { if (l.startsWith("WAVELOG_URL=")) url = QString::fromUtf8(l.mid(12)).trimmed(); if (l.startsWith("WAVELOG_KEY=")) key = QString::fromUtf8(l.mid(12)).trimmed(); } lp->setServer(url, key, 1); lp->testConnection(); }
        { QEventLoop l; QTimer::singleShot(2500, &l, &QEventLoop::quit); l.exec(); }
        lp->fillFromDecode("123045  -9  0.3 1200 ~  CQ G4ABC IO92");   { QEventLoop l; QTimer::singleShot(2500, &l, &QEventLoop::quit); l.exec(); } QApplication::processEvents();
        w.grab().save(QString(argv[2]) + "_lookup.png");                                          // stage 1: callsign + Enter -> lookup info in the small box
        lp->enterPressed(); QApplication::processEvents(); w.grab().save(QString(argv[2]) + "_receiver.png");     // stage 2: Enter again -> the large log form takes over Band and Mode
        tabs->setCurrentWidget(lp); QApplication::processEvents(); w.grab().save(QString(argv[2]) + "_log.png");
        { auto* at = w.antennas(); at->bandBox(4)->setCurrentIndex(2); at->bandBox(6)->setCurrentIndex(4); at->bandBox(1)->setCurrentIndex(3); at->onDial(7.074e6); tabs->setCurrentWidget(at); QApplication::processEvents(); w.grab().save(QString(argv[2]) + "_antennas.png"); }
        return 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "--smoke-settings")) {          // remembered settings, memories, keyboard helpers: with a TEMPORARY settings folder, never the user's
        QTemporaryDir tmp; QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path()); std::string failed;
        auto step = [&](const char* n, bool good) { if (!good && failed.empty()) failed = n; };
        {
            MainWindow w(false, false, true); auto* r = w.receiver(); r->autoSidebandBox()->setChecked(false); r->setModeIndex(1); r->setAgcIndex(3); r->setFilterWidth(2400); r->setPassbandCuts(300, 2700); r->setFilterShape(2); step("independent cuts", r->cutLow() == 300 && r->cutHigh() == 2700 && r->filterWidth() == 2400); r->setPassbandCuts(5000, 5050); step("cuts keep a sane minimum width", r->cutHigh() - r->cutLow() >= 100); r->setPassbandCuts(300, 2700); r->nrBox()->setChecked(true); r->notchBox()->setValue(1500); step("notch box is notch 1", r->notchesHz().size() == 1 && r->notchesHz()[0] == 1500); r->setNotchesHz({1500, 900, 2200}); step("three notches", r->notchesHz().size() == 3 && r->notchBox()->value() == 1500); r->squelchBox()->setValue(-80);
            r->setDialHz(14.2e6); r->saveMemory("20m test"); r->setModeIndex(3); r->setDialHz(7.03e6); r->saveMemory("CW 40m"); r->setModeIndex(1); r->setFilterWidth(2400); r->setPassbandCuts(300, 2700); r->setDialHz(14.2e6);
            step("shortcuts exist", r->findChildren<QShortcut*>().size() >= 8);
            step("step 100 Hz default", r->stepHz() == 100.0); r->stepDial(+3); step("stepDial", std::fabs(r->dialHz() - 14200300.0) < 1);
            r->setDialHz(14.2e6); r->nextBand(+1); step("band up from 20m", std::fabs(r->dialHz() - 18.1e6) < 1); r->nextBand(-1); r->nextBand(-1); step("band down twice", std::fabs(r->dialHz() - 10.12e6) < 1);
            r->setDialHz(6.0e6); r->nextBand(+1); step("between bands goes up to 40m, where it was left (7.03 LSB)", std::fabs(r->dialHz() - 7.03e6) < 1 && r->modeIndex() == 1); r->setDialHz(6.0e6); r->nextBand(-1); step("between bands goes down to 60m", std::fabs(r->dialHz() - 5.357e6) < 1);
            r->setDialHz(1.9e6); r->nextBand(-1); step("lowest band stays", std::fabs(r->dialHz() - 1.9e6) < 1);
            r->setPalette(2); r->setFloorOffsetDb(7); step("axis settling defaults to normal", r->axisSettling() == 1); r->setAxisSettling(2); r->setAxisSettling(9); step("axis settling is clamped", r->axisSettling() == 2);
            step("two memories", r->memories().size() == 2); r->setModeIndex(0); r->setDialHz(3.7e6); step("recall", r->recallMemory(0) && std::fabs(r->dialHz() - 14.2e6) < 1 && r->modeIndex() == 1 && r->filterWidth() == 2400);
            step("recall CW memory", r->recallMemory(1) && r->modeIndex() == 3 && std::fabs(r->dialHz() - 7.03e6) < 1); step("bad recall", !r->recallMemory(5));
            r->saveMemory("CW 40m"); step("same name replaces", r->memories().size() == 2);
            {   // band memory: each band goes back where it was left; a band never visited starts at its usual place (its FT8 frequency under FT8)
                r->setDecoderIndex(0); r->setDialHz(7.155e6); r->setModeIndex(1); r->setFilterWidth(1800);
                r->setDialHz(14.1e6); step("FT8 button on 20 m", r->selectFt(false) && std::fabs(r->dialHz() - 14.074e6) < 1 && r->decoderIndex() == 4);
                QPushButton* b40 = nullptr; for (auto* b : r->findChildren<QPushButton*>()) if (b->text() == "40") b40 = b;
                if (b40) b40->click();
                step("40 m button: back to 7.155 LSB 1800, decoder off", b40 && std::fabs(r->dialHz() - 7.155e6) < 1 && r->modeIndex() == 1 && r->filterWidth() == 1800 && r->decoderIndex() == 0);
                step("40 m memory kept", r->bandMemory(3).set && std::fabs(r->bandMemory(3).hz - 7.155e6) < 1);
                step("20 m: back to FT8", r->recallBand(5) && std::fabs(r->dialHz() - 14.074e6) < 1 && r->decoderIndex() == 4 && r->modeIndex() == 0 && r->filterWidth() == 3600);
                step("20 m memory not disturbed by leaving 40", std::fabs(r->bandMemory(5).hz - 14.074e6) < 1 && r->bandMemory(5).decoder == 4);
                step("15 m never visited, FT8 on: its FT8 frequency", r->recallBand(7) && std::fabs(r->dialHz() - 21.074e6) < 1 && r->decoderIndex() == 4);
                r->recallBand(3); step("same band again: no change", r->recallBand(3) && std::fabs(r->dialHz() - 7.155e6) < 1 && r->modeIndex() == 1);
                r->setDialHz(7.160e6); step("tuning inside the band updates its memory", std::fabs(r->bandMemory(3).hz - 7.160e6) < 1);
                r->recallBand(5); r->nextBand(-1); r->nextBand(-1); step("band down 20 -> 30 -> 40 recalls 40", std::fabs(r->dialHz() - 7.160e6) < 1 && r->decoderIndex() == 0);
                step("bad band", !r->recallBand(99));
                r->setDecoderIndex(0); r->setDialHz(14.2e6);                                       // leave 40 m as it is for the restart check
            }
            {   // NR2 (WDSP spectral noise reduction): a button on the Noise card, voice only, not under a data decoder, remembered
                r->setDecoderIndex(0); r->setModeIndex(0); r->nr2Box()->setChecked(true);
                step("NR2 button follows the build (hidden without libfftw3)", r->nr2Box()->isVisibleTo(r) == fireside::dsp::spectral_nr_available());
                step("NR2 on: passed to the receive chain (voice)", r->options().dc.spectral_nr);
                r->setDecoderIndex(4); step("NR2 not applied while FT8 is decoding", !r->options().dc.spectral_nr);
                r->setDecoderIndex(0); r->setModeIndex(1); step("NR2 applied again for SSB with the decoder off", r->options().dc.spectral_nr);
                r->nr2Box()->setChecked(true);                                                     // stays ticked for the restart check
            }
            r->setModeIndex(1); r->setFilterWidth(2400); r->setPassbandCuts(300, 2700); r->setDialHz(14.2e6); w.radio()->allowWritesBox()->setChecked(true);                    // must NOT be remembered
        }                                                                                                                                      // window closes: settings written
        {
            MainWindow w(false, false, true); auto* r = w.receiver();
            step("auto sideband off restored", !r->autoSidebandBox()->isChecked()); step("mode restored", r->modeIndex() == 1); step("agc restored", r->agcIndex() == 3); step("filter restored", r->filterWidth() == 2400); step("notches restored", r->notchesHz().size() == 3 && r->notchesHz()[1] == 900 && r->notchesHz()[2] == 2200); step("cuts and shape restored", r->cutLow() == 300 && r->cutHigh() == 2700 && r->filterShape() == 2); step("dial restored", std::fabs(r->dialHz() - 14.2e6) < 1);
            step("nr restored", r->nrBox()->isChecked()); step("notch restored", r->notchBox()->value() == 1500); step("squelch restored", r->squelchBox()->value() == -80);
            step("memories restored", r->memories().size() == 2 && r->memories()[0].name == "20m test" && r->memories()[1].mode == 3);
            step("palette restored", r->paletteIndex() == 2 && r->waterfall()->palette() == fireside::display::Palette::Viridis);
            step("axis settling restored", r->axisSettling() == 2);
            step("writes are never remembered", !w.radio()->allowWritesBox()->isChecked()); step("NR2 restored", r->nr2Box()->isChecked());
            step("band memory restored", r->bandMemory(3).set && std::fabs(r->bandMemory(3).hz - 7.160e6) < 1 && r->bandMemory(3).mode == 1 && r->bandMemory(3).width == 1800);
            step("band memory recalled after a restart", r->recallBand(3) && std::fabs(r->dialHz() - 7.160e6) < 1 && r->filterWidth() == 1800);
        }
        {   // the Radio tab remembers device / byte order / antenna (not "Allow writes"); a simulated window never touches those settings, so use a real (unconnected) one
            { MainWindow w(false, false, false); w.radio()->orderBox()->setCurrentIndex(1); w.radio()->antBox()->setCurrentIndex(2); }
            MainWindow w(false, false, false); step("radio byte order restored", w.radio()->orderBox()->currentIndex() == 1); step("antenna restored", w.radio()->antBox()->currentIndex() == 2);
            step("radio tab never remembers writes", !w.radio()->allowWritesBox()->isChecked());
        }
        std::printf("%s%s%s\n", failed.empty() ? "settings smoke ok" : "settings smoke FAILED at '", failed.c_str(), failed.empty() ? "" : "'");
        return failed.empty() ? 0 : 1;
    }
    if (argc > 2 && !std::strcmp(argv[1], "--screenshot-dialog")) {        // the themed Open-file dialog, for a picture (argv[2] = output png)
        qputenv("FIRESIDE_NO_SETTINGS", "1"); MainWindow w(false, false, true); QFileDialog d(&w, "Open I/Q recording", QDir::homePath() + "/fireside/recordings", "WAV files (*.wav);;All files (*)"); d.setOption(QFileDialog::DontUseNativeDialog); d.show();
        for (int i = 0; i < 20; ++i) { QApplication::processEvents(); QThread::msleep(30); } d.grab().save(QString::fromUtf8(argv[2])); return 0;
    }
    if (argc > 3 && !std::strcmp(argv[1], "--screenshot-file")) {          // fireside --screenshot-file in.wav out.png [ft8|ft4|cw]: open a recording (e.g. from firesidetx), play it through, save a picture of the window
        qputenv("FIRESIDE_NO_SETTINGS", "1"); MainWindow w(false, false, true); w.resize(1500, 900); w.show(); auto* r = w.receiver();
        if (!r->loadFile(QString::fromUtf8(argv[2]))) { std::printf("cannot open %s\n", argv[2]); return 1; }
        r->setPlaying(false); r->clearDecoded();
        if (argc > 4) { const bool ft4 = !std::strcmp(argv[4], "ft4"); r->setDecoderIndex(ft4 ? 5 : !std::strcmp(argv[4], "cw") ? 1 : 4); }
        for (int i = 0; i < 160; ++i) { r->advance(4800); if (i > 20) QThread::msleep(8); QApplication::processEvents(); }
        { QEventLoop l; QTimer::singleShot(1500, &l, &QEventLoop::quit); l.exec(); } QApplication::processEvents();
        return w.grab().save(QString::fromUtf8(argv[3])) ? 0 : 1;
    }
    if (argc > 3 && !std::strcmp(argv[1], "--bench-direct")) {             // fireside --bench-direct <rate> <seconds> [out.png]: the real window on the radio's direct FireWire stream; does it keep up?
        qputenv("FIRESIDE_NO_SETTINGS", "1"); MainWindow w(false, true, false); w.resize(1500, 900); w.show(); auto* r = w.receiver();
        const unsigned rate = unsigned(std::atoi(argv[2])); const double secs = std::atof(argv[3]);
        if (!r->loadLiveDirect(rate)) { std::printf("%s\n", r->status().toUtf8().constData()); return 1; }
        rusage u0{}; getrusage(RUSAGE_SELF, &u0); QElapsedTimer t; t.start();
        while (t.elapsed() < qint64(secs * 1000)) { QApplication::processEvents(); QThread::msleep(5); }
        rusage u1{}; getrusage(RUSAGE_SELF, &u1); const double wall = double(t.elapsed()) / 1000.0;
        auto cpu = [](const rusage& a) { return double(a.ru_utime.tv_sec + a.ru_stime.tv_sec) + double(a.ru_utime.tv_usec + a.ru_stime.tv_usec) / 1e6; };
        const auto st = r->engine().status();
        std::printf("rate %u Hz, %.1f s: %llu samples processed (%.0f/s = %.1f%% of real time), xruns %llu, spectrum frames %llu (dropped %llu), audio %llu (dropped %llu), CPU %.0f%% of one core\n",
                    rate, wall, (unsigned long long)st.samples, double(st.samples) / wall, 100.0 * double(st.samples) / wall / rate, (unsigned long long)st.xruns, (unsigned long long)st.frames,
                    (unsigned long long)st.frames_dropped, (unsigned long long)st.audio_samples, (unsigned long long)st.audio_dropped, 100.0 * (cpu(u1) - cpu(u0)) / wall);
        std::printf("status: %s\n", r->status().toUtf8().constData());
        if (argc > 4) w.grab().save(QString::fromUtf8(argv[4]));
        r->closeLive(); return 0;
    }
    if (argc > 4 && !std::strcmp(argv[1], "--decode-lookup")) {            // fireside --decode-lookup in.wav MYCALL MYGRID: FT8 through the whole receive path + QRZ lookups as the app does them
        qputenv("FIRESIDE_NO_SETTINGS", "1"); MainWindow w(false, false, true); auto* r = w.receiver(); auto* lp = w.logPanel(); auto* q = lp->qrz();   // no window settings read or written
        QFile ef(QrzClient::envPath()); if (!ef.open(QIODevice::ReadOnly)) { std::printf("no %s\n", QrzClient::envPath().toUtf8().constData()); return 1; }
        const auto env = fireside::qrz::parse_env(ef.readAll().toStdString()); auto g = [&](const char* k) { auto it = env.find(k); return it == env.end() ? QString() : QString::fromStdString(it->second); };
        q->setCredentials(g("QRZ_USER"), g("QRZ_PASSWORD")); q->setCachePath(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/fireside/qrz-cache.tsv");   // the real cache, as the app uses it
        r->myCallBox()->setText(QString::fromUtf8(argv[3]).toUpper()); lp->setMyGrid(QString::fromUtf8(argv[4]));
        if (!r->loadFile(QString::fromUtf8(argv[2]))) { std::printf("cannot open %s\n", argv[2]); return 1; }
        r->setPlaying(false); r->clearDecoded(); r->setDecoderIndex(4);
        const double fs = r->engine().status().sample_rate > 0 ? r->engine().status().sample_rate : 48000.0; const size_t step = size_t(4800.0 * fs / 48000.0);
        QElapsedTimer t; t.start(); const int n = argc > 5 ? std::atoi(argv[5]) : 700;
        for (int i = 0; i < n; ++i) { r->advance(step); QApplication::processEvents(); if (i > 200) QThread::msleep(15); }
        for (int idle = 0; idle < 30 && t.elapsed() < 600000; ) { QApplication::processEvents(); QThread::msleep(20); if (q->queued() == 0) ++idle; else idle = 0; }   // let the queue drain (1 a second)
        std::printf("%s", r->decodedText().toUtf8().constData());
        std::fprintf(stderr, "decoder: %s\nhighlighted (to %s): %d | QRZ: %s | network lookups: %d\n", r->engine().status().decoder.c_str(), argv[3], r->highlightedLines(), q->status().toUtf8().constData(), q->lookups());
        return 0;
    }
    if (argc > 3 && !std::strcmp(argv[1], "--decode-file")) {              // fireside --decode-file in.wav ft8|ft4: decode a recording through the whole receive path and print the lines
        qputenv("FIRESIDE_NO_SETTINGS", "1"); MainWindow w(false, false, true); auto* r = w.receiver(); const bool ft4 = !std::strcmp(argv[3], "ft4"); const int dIdx = ft4 ? 5 : !std::strcmp(argv[3], "cw") ? 1 : !std::strcmp(argv[3], "psk31") ? 2 : !std::strcmp(argv[3], "rtty") ? 3 : 4; const int tone = argc > 5 ? std::atoi(argv[5]) : 0;
        if (!r->loadFile(QString::fromUtf8(argv[2]))) { std::printf("cannot open %s\n", argv[2]); return 1; }
        const bool rt = argc > 4 && !std::strcmp(argv[4], "realtime");                      // realtime: let the receiver run on its own thread for 40 s, as in the window
        r->setPlaying(rt); r->clearDecoded();
        if (argc > 4 && !std::strcmp(argv[4], "late")) for (int i = 0; i < 77; ++i) r->advance(4800);   // the decoder is chosen part-way through the file, as when you pick it after Open
        r->setDecoderIndex(dIdx); if (tone > 0) r->setDecoderTone(tone); if (argc > 6) r->setDialHz(r->dialHz() + std::atof(argv[6]));   // argv[6]: tune this many Hz up inside the recording
        if (rt) { QElapsedTimer t; t.start(); while (t.elapsed() < 40000) { QApplication::processEvents(); QThread::msleep(10); } }
        else { const double fs = r->engine().status().sample_rate > 0 ? r->engine().status().sample_rate : 48000.0; const size_t step = size_t(4800.0 * fs / 48000.0);   // the same 70 s of signal at any rate
               for (int i = 0; i < 700; ++i) { r->advance(step); if (i > 200) QThread::msleep(15); } }
        std::printf("%s", r->decodedText().toUtf8().constData()); if (rt) std::fprintf(stderr, "decoder: %s | %s\n", "-", r->status().toUtf8().constData()); return 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "--smoke-tx")) {                // the offline transmit tab: render, limits, errors, two-tone figures, preview on the receiver (decodes what was rendered)
        qputenv("FIRESIDE_NO_SETTINGS", "1"); MainWindow w(false, false, true); w.resize(1500, 900); w.show(); auto* t = w.txPanel(); auto* r = w.receiver(); std::string failed;
        auto step = [&](const char* n, bool good) { if (!good && failed.empty()) failed = n; };
        auto wait = [&] { QElapsedTimer tm; tm.start(); while (t->busy() && tm.elapsed() < 60000) { QApplication::processEvents(); QThread::msleep(5); } QApplication::processEvents(); };
        auto peak = [&] { double p = 0; for (auto& v : t->iq()) p = std::max(p, double(std::abs(v))); return p; };
        step("panel exists", t != nullptr); { auto* lp = w.logPanel(); QApplication::processEvents(); const int before = lp->lookupSizeHint().height(); lp->showLookupResult("<b>G4ABC</b><br>John Smith<br>Somewhere, Hampshire<br>JO01aa  distance 1200 km<br>Worked before on 20m SSB<br>QSL: LoTW, eQSL<br>another line<br>and another"); QApplication::processEvents(); step("a lookup result does not resize the receiver screen", lp->lookupSizeHint().height() == before);
          for (int i = 2; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--screenshot")) { r->setTransmitOpen(false); lp->fillFromDecode("123045  -9  0.3 1200 ~  CQ G4ABC IO92"); lp->showLookupResult("<b>G4ABC</b><br>John Smith<br>Somewhere, Hampshire<br>JO01aa  distance 1200 km<br>Worked before on 20m SSB<br>QSL: LoTW, eQSL<br>another line<br>and another"); w.resize(1500, 900); for (int k = 0; k < 10; ++k) QApplication::processEvents(); w.grab().save(QString::fromStdString(argv[i + 1]).replace(".png", "_lookup.png")); lp->clearForm(); }
          lp->showLookupResult(" "); }
        step("no separate transmit tab (Radio, Antennas, Device, Log, Audio, PSK Reporter)", w.findChild<QTabWidget*>()->count() == 6); step("transmit section starts closed", !r->transmitOpen()); r->setTransmitOpen(true); step("transmit section opens on the receiver screen", r->transmitOpen() && t->isVisibleTo(r)); r->setTransmitOpen(false); step("and closes", !r->transmitOpen()); r->setTransmitOpen(true); if (!t) { std::puts("tx smoke FAILED: no panel"); return 1; }
        t->setMode(TxPanel::Ft8); t->setMessage("CQ M0OML IO91"); t->setAudioHz(1200);
        step("FT8 message is encodable", t->encodableText().contains("can be sent")); step("slot clock shown", t->slotText().contains("UTC"));
        step("render starts", t->render()); step("a second render is refused while one runs", !t->render()); wait();
        step("FT8 rendered", t->hasResult() && t->iq().size() == 720000); step("FT8 under the -1 dBFS ceiling", peak() <= 0.8913 && peak() > 0.45); step("FT8 summary names USB", t->summary().contains("USB")); step("the result is drawn as a spectrogram", t->plot()->hasImage() && t->plot()->imageSize().height() > 200 && t->plot()->imageSize().width() > 500);
        bool asked = false; QString asked_path; int asked_dec = -1; QObject::connect(t, &TxPanel::previewRequested, [&](const QString& p, int d) { asked = true; asked_path = p; asked_dec = d; });
        QMetaObject::invokeMethod(t, "onPreviewPressed", Qt::DirectConnection);
        step("preview requested with the FT8 decoder", asked && asked_dec == 4 && QFile::exists(asked_path));
        step("receiver loaded the preview and picked the FT8 decoder", r->decoderIndex() == 4);
        r->setPlaying(false); r->clearDecoded(); for (int i = 0; i < 400 && !r->decodedText().contains("CQ M0OML IO91"); ++i) { r->advance(4800); if (i > 150) QThread::msleep(10); }
        step("the receiver decodes what the transmit tab rendered", r->decodedText().contains("CQ M0OML IO91"));
        t->setSidebandLsb(true); t->render(); wait(); step("LSB render", t->hasResult() && t->summary().contains("LSB")); t->setSidebandLsb(false);
        t->setMessage("THIS TEXT IS WAY TOO LONG TO ENCODE AT ALL"); step("bad message is flagged", t->encodableText().contains("cannot")); t->render(); wait();
        step("bad message gives an error and no result", !t->hasResult() && t->summary().contains("Could not render") && !t->plot()->hasImage()); t->setMessage("CQ M0OML IO91");
        t->setMode(TxPanel::Cw); t->setCwText("CQ DE M0OML"); t->render(); wait(); step("CW rendered at the default -6 dBFS", t->hasResult() && std::fabs(peak() - 0.501) < 0.01);
        step("CW memory defaults", t->memoryText(0) == "CQ DE M0OML" && t->memoryText(9).isEmpty() && !t->setMemory(9, "X") && !t->setMemory(-1, "X"));
        t->setCwText("TEST DE M0OML"); step("store text in M2", t->storeMemory(1) && t->memoryText(1) == "TEST DE M0OML"); t->setCwText("X"); step("recall M2", t->recallMemory(1)); t->render(); wait(); step("memory text is what gets sent", t->hasResult());
        step("empty memory cannot be recalled", t->setMemory(2, "") && !t->recallMemory(2)); t->setMemory(1, "M0OML"); t->setMemory(2, "R R 5NN TU");
        t->setMode(TxPanel::Keyer); t->render(); wait(); step("keyer with no recording is refused", !t->hasResult() && t->summary().contains("record some paddle input"));
        { auto* pd = t->paddlePad(); t->setKeyerOptions(20, true, false); const double u = 0.06; pd->paddle(true, false, 0.0); pd->paddle(false, false, 4.5 * u); step("pad records two changes", pd->events().size() == 2);
          t->render(); wait(); step("keyer renders the recording", t->hasResult() && peak() <= 0.8913 && peak() > 0.45 && t->decoderIndexFor(TxPanel::Keyer) == 1);
          size_t on = 0; bool up = false; const double th = 0.5 * peak(); for (auto& v : t->iq()) { const bool o = std::abs(v) > th; if (o && !up) ++on; up = o; } step("dit paddle held gives three elements", on == 3);
          pd->clear(); auto kev = [&](QEvent::Type ty, int k, bool rep = false) { QKeyEvent e(ty, k, Qt::NoModifier, QString(), rep); QApplication::sendEvent(pd, &e); return e.isAccepted(); };
          step("Z press is taken", kev(QEvent::KeyPress, Qt::Key_Z)); kev(QEvent::KeyPress, Qt::Key_Z, true); step("auto-repeat is ignored", pd->events().size() == 1);
          kev(QEvent::KeyPress, Qt::Key_X); kev(QEvent::KeyRelease, Qt::Key_Z); kev(QEvent::KeyRelease, Qt::Key_X); step("Z then X squeeze and release are recorded", pd->events().size() == 4 && pd->events()[1].dit && pd->events()[1].dah && !pd->events()[3].dit && !pd->events()[3].dah);
          step("other keys are not taken", !kev(QEvent::KeyPress, Qt::Key_Q) && pd->events().size() == 4);
          pd->clear(); step("clear empties the recording", pd->events().empty()); }
        t->setMode(TxPanel::Psk31); t->setDigitalText("CQ DE M0OML"); t->render(); wait(); step("PSK31 rendered under the ceiling", t->hasResult() && peak() <= 0.8913 && peak() > 0.45 && t->previewToneHz() == 1000);
        { QMetaObject::invokeMethod(t, "onPreviewPressed", Qt::DirectConnection); r->setPlaying(false); r->clearDecoded(); for (int i = 0; i < 400 && !r->decodedText().contains("CQ DE M0OML"); ++i) { r->advance(4800); if (i > 150) QThread::msleep(10); }
          step("the receiver decodes the PSK31 the tab rendered", r->decoderIndex() == 2 && r->decodedText().contains("CQ DE M0OML")); }
        t->setMode(TxPanel::Rtty); t->setDigitalText("CQ TEST DE M0OML"); t->render(); wait(); step("RTTY rendered with mark 2125", t->hasResult() && peak() <= 0.8913 && t->previewToneHz() == 2125 && TxPanel::decoderIndexFor(TxPanel::Rtty) == 3);
        { QMetaObject::invokeMethod(t, "onPreviewPressed", Qt::DirectConnection); r->setPlaying(false); r->clearDecoded(); for (int i = 0; i < 400 && !r->decodedText().contains("CQ TEST DE M0OML"); ++i) { r->advance(4800); if (i > 150) QThread::msleep(10); }
          step("the receiver decodes the RTTY the tab rendered", r->decoderIndex() == 3 && r->decodedText().contains("CQ TEST DE M0OML")); }
        t->setMode(TxPanel::TwoTone); t->render(); wait(); step("two-tone shows distortion figures", t->hasResult() && t->imdText().contains("third order") && !t->imdText().contains("nan"));
        t->setMode(TxPanel::Voice); t->setVoiceFile(""); t->render(); wait(); step("voice without a file is refused", !t->hasResult() && t->summary().contains("choose an audio file"));
        { const std::string wavp = "/tmp/fireside_txvoice_" + std::to_string(::getpid()) + ".wav"; std::vector<float> a(96000); for (size_t i = 0; i < a.size(); ++i) a[i] = float(0.3 * std::sin(2 * 3.14159265358979 * 1000.0 * double(i) / 48000.0)); fireside::dsp::write_wav_pcm16_mono(wavp, 48000, a);
          t->setVoiceFile(QString::fromStdString(wavp)); t->render(); wait(); step("voice file rendered", t->hasResult() && t->iq().size() == a.size() && peak() <= 0.8913); std::remove(wavp.c_str()); }
        { const QString p = t->previewPath(); step("save writes a readable I/Q WAV", !p.isEmpty() && fireside::dsp::read_iq_wav(p.toStdString(), 0, 1).samples.size() == t->iq().size()); }
        { std::string shot; for (int i = 2; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--screenshot")) shot = argv[i + 1];
          if (!shot.empty()) { t->setMode(TxPanel::Ft8); t->setMessage("CQ M0OML IO91"); t->render(); wait(); r->setTransmitOpen(true); for (int i = 0; i < 10; ++i) QApplication::processEvents(); w.grab().save(QString::fromStdString(shot));
            for (auto [m, tag] : {std::pair{TxPanel::Cw, "_cw"}, {TxPanel::Keyer, "_keyer"}, {TxPanel::TwoTone, "_twotone"}, {TxPanel::Rtty, "_rtty"}}) { t->setMode(m); if (m == TxPanel::Keyer) { t->paddlePad()->clear(); double tt = 0.0; for (int k = 0; k < 6; ++k) { t->paddlePad()->paddle(k % 2 == 0, k % 2 == 1, tt); tt += 0.09; } t->paddlePad()->paddle(false, false, tt); } t->render(); wait(); for (int i = 0; i < 10; ++i) QApplication::processEvents(); w.grab().save(QString::fromStdString(shot).replace(".png", tag + QStringLiteral(".png"))); } } }
        if (failed.empty()) { std::puts("tx smoke ok"); return 0; } std::printf("tx smoke FAILED: %s\n", failed.c_str()); return 1;
    }
    if (argc > 2 && !std::strcmp(argv[1], "--demo-qso")) {               // a picture of the FT8 callers area and QSO pane with made-up lines (checking the layout): fireside --demo-qso out.png
        qputenv("FIRESIDE_NO_SETTINGS", "1"); MainWindow w(false, false, true); w.resize(1700, 1000); w.show(); auto* r = w.receiver(); qint64 clk = 0; r->setCallersClockForTests([&clk] { return clk; });
        r->setDecoderIndex(4); r->myCallBox()->setText("M0OML"); r->clearDecoded();
        r->addDecodedText("093300  -3  0.2 1200 ~  CQ DL2AL JO31\n093300  -8  0.1  800 ~  SP9ABC DL2AL -05\n093300 -11  0.2 1900 ~  CQ DX K1ABC FN42\n");
        r->addDecodedText("093315  -6  0.1 2576 ~  M0OML DK0ULR JN58\n"); clk += 25000; r->addDecodedText("093340  -9  0.1 1700 ~  M0OML EA3ABC JN11\n"); clk += 25000;
        r->addDecodedText("093405 -12  0.3  900 ~  M0OML SP9XYZ JO90\n"); clk += 25000; r->addDecodedText("093430  -4  0.1 2200 ~  M0OML G4TEST IO91\n"); clk += 5000; r->tickCallers();
        r->showCallInfo("DK0ULR", "Klaus, Ulm, Germany, 770 km"); r->markWorked("DK0ULR", 2); r->showCallInfo("EA3ABC", "Jordi, Sabadell, Spain, 1210 km"); r->markWorked("EA3ABC", 1); r->showCallInfo("SP9XYZ", "Piotr, Katowice, Poland, 1180 km"); r->markWorked("SP9XYZ", 0); r->showCallInfo("G4TEST", "John, Oxford, England, 180 km");
        for (int k = 0; k < 40; ++k) r->addDecodedText(QString("0935%1  -%2  0.1 %3 ~  CQ %4%5 IO%6\n").arg(10 + k % 40).arg(5 + k % 15).arg(400 + k * 55).arg(char('A' + k % 20)).arg(char('B' + k % 7)).arg(80 + k % 15));
        r->setQsoPartner("DL2AL"); r->addDecodedText("093445  -7  0.2 1200 ~  M0OML DL2AL JO31\n"); r->addQsoTx("093500", 1500, "DL2AL M0OML -07"); r->addDecodedText("093515  -6  0.2 1200 ~  M0OML DL2AL R-09\n"); r->addQsoTx("093530", 1500, "DL2AL M0OML RR73");
        for (int k = 0; k < 10; ++k) QApplication::processEvents(); w.grab().save(QString::fromUtf8(argv[2])); return 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "--smoke-cat")) {              // the CAT server (Hamlib NET rigctl) over a real local socket, against the simulated radio
        MainWindow w(false, false, true); auto* r = w.receiver(); CatPanel* c = w.catPanel(); std::string failed;
        auto step = [&](const char* n, bool good) { if (!good && failed.empty()) failed = n; };
        step("CAT card on the Device tab, off in tests", c && !c->listening());
        if (!c) { std::puts("cat smoke FAILED: no panel"); return 1; }
        step("server starts on 127.0.0.1", c->startServer(0) && c->listening() && c->port() > 0);
        QTcpSocket s; s.connectToHost(QHostAddress::LocalHost, quint16(c->port()));
        for (int i = 0; i < 200 && s.state() != QAbstractSocket::ConnectedState; ++i) { QApplication::processEvents(); QThread::msleep(5); }
        QByteArray got;
        auto ask = [&](const QByteArray& cmd, const QByteArray& until) { got.clear(); s.write(cmd);
            for (int i = 0; i < 400 && !got.contains(until); ++i) { QApplication::processEvents(); s.waitForReadyRead(5); got += s.readAll(); } return got; };
        step("connected", s.state() == QAbstractSocket::ConnectedState);
        for (int i = 0; i < 50 && c->clients() < 1; ++i) { QApplication::processEvents(); QThread::msleep(5); }
        step("one client", c->clients() == 1);
        step("chk_vfo + dump_state (what Hamlib asks first)", ask("\\chk_vfo\n\\dump_state\n", "done\n").startsWith("0\n1\n2\n0\n") && got.contains("ptt_type=0x0"));
        r->setDialHz(7.1e6); step("get_freq follows the dial", ask("f\n", "\n") == "7100000\n");
        step("set_freq tunes the receiver", ask("F 14074000\n", "\n") == "RPRT 0\n" && std::fabs(r->dialHz() - 14.074e6) < 1);
        step("set_mode LSB 2400", ask("M LSB 2400\n", "\n") == "RPRT 0\n" && r->modeIndex() == 1 && r->filterWidth() == 2400);
        step("get_mode", ask("m\n", "2400\n") == "LSB\n2400\n");
        r->setDecoderIndex(4); step("FT8 on, the program asks for PKTUSB: USB here, PKTUSB reported", ask("M PKTUSB 0\n", "\n") == "RPRT 0\n" && r->modeIndex() == 0 && r->decoderIndex() == 4 && ask("m\n", "\n").startsWith("PKTUSB\n"));
        step("transmit refused", ask("T 1\n", "\n") == "RPRT -9\n" && !w.simRadio()->keyed && c->refusedPtt() == 1);
        step("get_ptt", ask("t\n", "\n") == "0\n");
        step("out of range refused", ask("F 99000000\n", "\n") == "RPRT -1\n" && std::fabs(r->dialHz() - 14.074e6) < 1);
        ask("F 7074000\n", "\n"); step("CAT tuning feeds band memory", std::fabs(r->bandMemory(3).hz - 7.074e6) < 1);
        for (int i = 0; i < 120; ++i) { QApplication::processEvents(); QThread::msleep(10); if (c->statusText().contains("commands")) break; }
        step("status shows the client and the refusal", c->statusText().contains("1 program connected") && c->statusText().contains("refused: 1"));
        s.write("q\n"); for (int i = 0; i < 200 && c->clients() > 0; ++i) { QApplication::processEvents(); s.waitForReadyRead(5); }
        step("quit closes the connection", c->clients() == 0);
        c->stopServer(); step("stops", !c->listening());
        std::printf("%s%s%s\n", failed.empty() ? "cat smoke ok" : "cat smoke FAILED at '", failed.c_str(), failed.empty() ? "" : "'");
        return failed.empty() ? 0 : 1;
    }
    if (argc > 1 && !std::strcmp(argv[1], "--smoke-pskr")) {             // PSK Reporter tab against a local fake MQTT broker (never the real feed)
        QTemporaryDir tmp; std::string failed; auto step = [&](const char* n, bool good) { if (!good && failed.empty()) failed = n; };
        QTcpServer srv; srv.listen(QHostAddress::LocalHost, 0); QString topic; int conns = 0; QTcpSocket* peer = nullptr;
        auto pub = [](const std::string& t, const std::string& pl) { std::string b; b += char(0); b += char(t.size()); b += t; b += pl; std::string out; out += char(0x30); size_t n = b.size();
                                                                    do { uint8_t x = uint8_t(n % 128); n /= 128; if (n) x |= 0x80; out += char(x); } while (n); return QByteArray::fromStdString(out + b); };
        QObject::connect(&srv, &QTcpServer::newConnection, [&] {
            peer = srv.nextPendingConnection(); ++conns;
            QObject::connect(peer, &QTcpSocket::readyRead, [&, sock = peer] {
                const QByteArray b = sock->readAll(); int i = 0;
                while (i < b.size()) {
                    const uint8_t t = uint8_t(b[i]) >> 4; int len = 0, mul = 1, j = i + 1; while (uint8_t(b[j]) & 0x80) { len += (uint8_t(b[j]) & 0x7F) * mul; mul *= 128; ++j; } len += uint8_t(b[j]) * mul; ++j;
                    if (t == 1) sock->write(QByteArray("\x20\x02\x00\x00", 4));                                         // CONNECT -> CONNACK accepted
                    if (t == 8) { const int tl = uint8_t(b[j + 2]) << 8 | uint8_t(b[j + 3]); topic = QString::fromLatin1(b.mid(j + 4, tl)); sock->write(QByteArray("\x90\x03\x00\x01\x00", 5));
                        sock->write(pub("pskr/filter/v2/40m/FT8/M0OML/DL1ABC/IO92/JO62/223/230", R"({"sq":11,"f":7075512,"md":"FT8","rp":-12,"t":1700000000,"sc":"M0OML","sl":"IO92ar","rc":"DL1ABC","rl":"JO62qm","sa":223,"ra":230,"b":"40m"})"));
                        sock->write(pub("pskr/filter/v2/40m/FT8/M0OML/EA8XYZ/IO92/IL18/223/29", R"({"sq":12,"f":7075510,"md":"FT8","rp":-20,"t":1700000060,"sc":"M0OML","rc":"EA8XYZ","rl":"IL18","b":"40m"})"));
                        sock->write(pub("pskr/filter/v2/40m/FT8/G0XXX/DL1ABC/IO91/JO62/223/230", R"({"sq":13,"f":7075000,"md":"FT8","rp":0,"t":1700000060,"sc":"G0XXX","rc":"DL1ABC","b":"40m"})"));
                        sock->write(pub("pskr/filter/v2/40m/FT8/M0OML/DL1ABC/IO92/JO62/223/230", R"({"sq":11,"f":7075512,"md":"FT8","rp":-12,"t":1700000000,"sc":"M0OML","rc":"DL1ABC","b":"40m"})")); }   // a repeat
                    i = j + len;
                }
            });
        });
        PskPanel pp([] { return QString("m0oml"); }, [] { return QString("IO92ar"); }, [] { return QString("40m"); });
        pp.setCsvPath(tmp.filePath("heard.csv")); pp.setNowForTests([] { return qint64(1700000300); }); pp.antennaBox()->setText("test end-fed");
        pp.client()->setBroker("127.0.0.1", srv.serverPort(), false);
        step("off by default in tests (never the real feed)", !pp.enableBox()->isChecked() && pp.client()->status() == "off");
        pp.enableBox()->setChecked(true);
        auto pump = [&](int ms, const std::function<bool()>& until) { QElapsedTimer t; t.start(); while (t.elapsed() < ms && !until()) { QApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(5); } };
        pump(5000, [&] { return pp.rows() >= 2; });
        step("subscribed to my call only", topic == "pskr/filter/v2/+/+/M0OML/#");
        step("status live", pp.client()->status().startsWith("live"));
        step("two receivers, not the other station's report", pp.rows() == 2);
        step("summary", pp.summaryText().contains("<b>2</b> stations in <b>2</b> countries") && pp.summaryText().contains("Furthest: EA8XYZ"));
        step("short summary for the Radio screen", pp.shortSummary().startsWith("Heard by 2") && pp.shortSummary().contains("km"));
        { QFile f(tmp.filePath("heard.csv")); f.open(QIODevice::ReadOnly); const QStringList l = QString(f.readAll()).split('\n', Qt::SkipEmptyParts);
          step("csv: header + one line per new report, with the antenna note", l.size() == 3 && l[0].startsWith("utc_epoch") && l[1].startsWith("1700000000,40m,FT8,7075512,-12,DL1ABC,JO62qm,") && l[1].endsWith(",test end-fed")); }
        if (peer) peer->abort(); pump(3000, [&] { return pp.client()->status().contains("retrying"); });
        step("a dropped connection is retried", pp.client()->status().contains("retrying"));
        pp.enableBox()->setChecked(false); step("unticked: off", pp.client()->status() == "off" && !pp.client()->running());
        {   // uploading my decodes: off until ticked; then to a local UDP listener (never the real collector in tests)
            QUdpSocket rx; rx.bind(QHostAddress::LocalHost, 0); pp.setUploadTarget(QHostAddress::LocalHost, rx.localPort());
            pp.onDecodeLine("203822 -12  0.1 1200 ~  CQ DL1ABC JO62", 7.074e6, "FT8"); pp.flushUploads(); pump(300, [&] { return rx.hasPendingDatagrams(); });
            step("upload off by default: nothing sent", !rx.hasPendingDatagrams() && pp.uploader().pending() == 0 && pp.uploadStatus().startsWith("off"));
            pp.uploadBox()->setChecked(true);
            pp.onDecodeLine("203822 -12  0.1 1200 ~  CQ DL1ABC JO62", 7.074e6, "FT8"); pp.onDecodeLine("203822  -3  0.2  850 ~  G4AAA EA8XYZ IL18", 7.074e6, "FT8");
            pp.onDecodeLine("203822 -12  0.1 1200 ~  CQ DL1ABC JO62", 7.074e6, "FT8"); pp.onDecodeLine("203822  -9  0.1  900 ~  DL1ABC M0OML IO92", 7.074e6, "FT8");
            step("two reports queued (repeat and my own call left out)", pp.uploader().pending() == 2 && pp.uploadStatus().contains("2 waiting"));
            pp.flushUploads(); pump(2000, [&] { return rx.hasPendingDatagrams(); });
            QByteArray d; if (rx.hasPendingDatagrams()) { d.resize(int(rx.pendingDatagramSize())); rx.readDatagram(d.data(), d.size()); }
            step("one IPFIX datagram: version 10, its own length, my call, both stations, the antenna note",
                 d.size() > 16 && uint8_t(d[0]) == 0 && uint8_t(d[1]) == 10 && ((uint8_t(d[2]) << 8) | uint8_t(d[3])) == d.size() && d.contains("M0OML") && d.contains("IO92ar") &&
                 d.contains("DL1ABC") && d.contains("EA8XYZ") && d.contains("test end-fed") && d.contains("fireside"));
            step("status counts the reports sent", pp.uploader().sent_reports() == 2 && pp.uploadStatus().startsWith("2 reports sent in 1 datagram"));
            pp.uploadBox()->setChecked(false);
        }
        std::printf("pskr smoke %s%s (connections %d, topic %s)\n", failed.empty() ? "ok" : "FAILED at '", failed.empty() ? "" : (failed + "'").c_str(), conns, topic.toStdString().c_str());
        return failed.empty() ? 0 : 1;
    }
    if (argc > 1 && !std::strcmp(argv[1], "--smoke-txmem")) {             // CW memories survive a restart (private settings dir, the real one is untouched)
        QTemporaryDir cfg; if (!cfg.isValid()) { std::puts("txmem FAILED: no temp dir"); return 1; } qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8()); qunsetenv("FIRESIDE_NO_SETTINGS"); QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, cfg.path());
        { TxPanel a; if (!a.setMemory(3, "HELLO DE TEST")) { std::puts("txmem FAILED: store"); return 1; } }
        { TxPanel b; if (b.memoryText(3) != "HELLO DE TEST" || b.memoryText(0) != "CQ DE M0OML") { std::puts("txmem FAILED: not restored"); return 1; } }
        std::puts("txmem ok"); return 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "--smoke-decode")) {            // CW, PSK31 and RTTY through the whole app: I/Q file -> receiver -> audio -> decoder -> text window
        MainWindow w(false, false, true); w.show(); auto* r = w.receiver(); std::string failed; auto step = [&](const char* n, bool good) { if (!good && failed.empty()) failed = n; };
        auto write = [&](const std::string& name, const std::vector<std::complex<float>>& z) { std::vector<float> il; for (auto& v : z) { il.push_back(v.real()); il.push_back(v.imag()); } fireside::dsp::write_wav_f32(name, 48000, 2, il); };
        auto run = [&](const std::string& file, int decoder, const char* want, const char* label) {
            step(label, r->loadFile(QString::fromStdString(file))); r->setPlaying(false); r->clearDecoded(); r->setDecoderIndex(decoder);
            for (int i = 0; i < 400; ++i) { r->advance(4800); if (r->decodedText().contains(want)) break; }
            step(label, r->decodedText().contains(want)); std::remove(file.c_str()); };
        const std::string base = "/tmp/fireside_decode_" + std::to_string(::getpid());
        { auto env = fireside::decode::morse_envelope("VVV CQ DE M0OML K ", 20); std::vector<std::complex<float>> z(env.size()); for (size_t i = 0; i < env.size(); ++i) z[i] = {0.3f * env[i], 0.0f}; write(base + "_cw.wav", z); }
        { auto p = fireside::decode::synth_psk31("CQ DE M0OML K ", 1000, 48000, 0.6, 3.0); std::vector<std::complex<float>> z(p.size()); for (size_t i = 0; i < p.size(); ++i) z[i] = {p[i], 0.0f}; write(base + "_psk.wav", z); }
        { auto t = fireside::decode::synth_rtty("CQ DE M0OML K ", 2125, 170, 45.45, 48000, 0.6, 1.0); std::vector<std::complex<float>> z(t.size()); for (size_t i = 0; i < t.size(); ++i) z[i] = {t[i], 0.0f}; write(base + "_rtty.wav", z); }
        run(base + "_cw.wav", 1, "CQ DE M0OML K", "CW"); step("CW switched the receiver to CW mode", r->modeIndex() == 3); step("CW status shown", r->status().size() >= 0);
        run(base + "_psk.wav", 2, "CQ DE M0OML K", "PSK31"); step("PSK31 switched to USB", r->modeIndex() == 0);
        { std::string shot; for (int i = 2; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--screenshot")) shot = argv[i + 1]; if (!shot.empty()) { w.resize(1500, 900); for (int i = 0; i < 30; ++i) r->advance(4800); QApplication::processEvents(); w.grab().save(QString::fromStdString(shot)); } }
        run(base + "_rtty.wav", 3, "CQ DE M0OML K", "RTTY");
        {   // FT8: a slot recording; the decode runs on a background thread right after the slot ends, so keep feeding and polling
            auto slot = fireside::decode::synth_ft8("CQ M0OML IO91", 1000, 48000, 0.3, 0.5); std::vector<std::complex<float>> z(slot.size()); for (size_t i = 0; i < slot.size(); ++i) z[i] = {slot[i], 0.0f}; write(base + "_ft8.wav", z);
            step("FT8 file", r->loadFile(QString::fromStdString(base + "_ft8.wav"))); r->setPlaying(false); r->clearDecoded(); r->setDecoderIndex(4);
            step("FT8 set USB and the wide filter", r->modeIndex() == 0 && r->filterWidth() == 3600);
            for (int i = 0; i < 700 && !r->decodedText().contains("CQ M0OML IO91"); ++i) { r->advance(4800); if (i > 150) QThread::msleep(15); }
            step("FT8 decoded", r->decodedText().contains("CQ M0OML IO91"));
            r->setDecoderIndex(0); r->setModeIndex(1); r->clearDecoded(); step("FT8 file again", r->loadFile(QString::fromStdString(base + "_ft8.wav"))); r->setPlaying(false); for (int i = 0; i < 40; ++i) r->advance(4800);      // part-way in, then pick the decoder: it must restart the recording on a slot boundary
            r->setDecoderIndex(4); for (int i = 0; i < 700 && !r->decodedText().contains("CQ M0OML IO91"); ++i) { r->advance(4800); if (i > 150) QThread::msleep(15); }
            step("FT8 chosen part-way through a file, from LSB, still decodes (and switches to USB)", r->decodedText().contains("CQ M0OML IO91") && r->modeIndex() == 0);
            r->markWorked("M0OML", 0); step("decoded line tagged NEW", r->decodedText().contains(QString::fromUtf8("\u2502 NEW")));
            { const QStringList ls = r->decodedText().split('\n', Qt::SkipEmptyParts); int col = -1; bool same = true; for (const QString& l : ls) if (l.contains(" ~ ")) { const int c = l.indexOf(QChar(0x2502)); if (col < 0) col = c; else if (c != col) same = false; }
              step("decoder columns fixed: the separator in one column on every line", col > 40 && same); } r->markWorked("M0OML", 2); step("tag added only once per line", !r->decodedText().contains("worked") && r->decodedText().count("NEW") == 1); std::remove((base + "_ft8.wav").c_str()); }
        {   // FT4: the same, 7.5 s slots
            auto slot = fireside::decode::synth_ft4("CQ M0OML IO92", 1000, 48000, 0.3, 0.3); std::vector<std::complex<float>> z(slot.size()); for (size_t i = 0; i < slot.size(); ++i) z[i] = {slot[i], 0.0f}; write(base + "_ft4.wav", z);
            step("FT4 file", r->loadFile(QString::fromStdString(base + "_ft4.wav"))); r->setPlaying(false); r->clearDecoded(); r->setDecoderIndex(5);
            step("FT4 set USB and the wide filter", r->modeIndex() == 0 && r->filterWidth() == 3600);
            for (int i = 0; i < 700 && !r->decodedText().contains("CQ M0OML IO92"); ++i) { r->advance(4800); if (i > 100) QThread::msleep(15); }
            step("FT4 decoded", r->decodedText().contains("CQ M0OML IO92")); std::remove((base + "_ft4.wav").c_str()); }
        { std::string demo, shot; for (int i = 2; i + 1 < argc; ++i) { if (!std::strcmp(argv[i], "--ft8-demo")) demo = argv[i + 1]; if (!std::strcmp(argv[i], "--screenshot")) shot = argv[i + 1]; }
          if (!demo.empty() && !shot.empty() && r->loadFile(QString::fromStdString(demo))) { r->setPlaying(false); r->clearDecoded(); r->setDecoderIndex(4);
              for (int i = 0; i < 600; ++i) { r->advance(4800); if (i > 150) QThread::msleep(15); if (r->decodedText().count('\n') >= 14) break; }
              w.resize(1500, 900); QApplication::processEvents(); w.grab().save(QString::fromStdString(shot).replace(".png", "_ft8.png")); } }
        r->setDecoderIndex(0); step("decoder off", r->decoderIndex() == 0);
        std::printf("%s%s%s\n", failed.empty() ? "decode smoke ok" : "decode smoke FAILED at '", failed.c_str(), failed.empty() ? "" : "'");
        return failed.empty() ? 0 : 1;
    }
    const bool smoke = argc > 1 && !std::strcmp(argv[1], "--smoke"), smokeRx = argc > 1 && !std::strcmp(argv[1], "--smoke-receiver"),
               smokeTh = argc > 1 && !std::strcmp(argv[1], "--smoke-threaded"), smokeRadio = argc > 1 && !std::strcmp(argv[1], "--smoke-radio");
    bool simulate = smokeRadio; for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--simulate")) simulate = true;      // demo/rehearsal radio, no hardware
    QTemporaryDir backup_tmp;                                      // the radio smoke keeps EEPROM backups in a temp dir: it never reads or writes the real data folder
    if (smokeRadio) { const QString real = TransmitCard::findBackup(); qputenv("FIRESIDE_BACKUP_DIR", backup_tmp.path().toUtf8());
                      if (!real.isEmpty()) QFile::copy(real, backup_tmp.path() + "/" + QFileInfo(real).fileName()); }   // with a real backup present the whole transmit rehearsal runs
    MainWindow w(!(smoke || smokeRx || smokeTh || smokeRadio), !smokeRx, simulate);                              // never open an audio device in smoke runs
    if (smoke) {
        w.show(); QTimer::singleShot(50, &app, &QApplication::quit); app.exec();
        std::printf("smoke ok: %s\n", w.deviceSummary().toStdString().c_str());
        return 0;
    }
    if (smokeRadio) {                                                        // the whole radio-session path against the simulated radio, including click-to-tune
        w.show(); auto* rp = w.radio(); auto* rx = w.receiver(); auto* bus = w.simBus(); std::string failed;
        auto step = [&](const char* name, bool good) { if (!good && failed.empty()) failed = name; };
        auto ops = [&] { std::vector<uint32_t> v; for (auto& w : bus->writes) { const uint32_t code = fireside::sim::be_word(w.second, 0) & 0x7FFFFFFFu; const fireside::OpInfo* o = fireside::find_opcode(code); if (!o || !fireside::is_read(*o)) v.push_back(code); } return v; };   // what was SENT (the sim executes lazily)
        auto pump = [&](int n) { for (int i = 0; i < n; ++i) { QApplication::processEvents(); QThread::msleep(30); } };
        step("sim bus", bus != nullptr);
        step("connect read-only", rp->connectRadio() && ops().empty());
        step("start refused without allow-writes", !rp->startRadio(true) && ops().empty());
        rp->disconnectRadio(); rp->allowWritesBox()->setChecked(true);
        rx->setDialHz(7.1e6); step("connect again", rp->connectRadio()); step("start", rp->startRadio(true));
        step("reviewed start-up sequence", ops() == std::vector<uint32_t>{1219, 1247, 1320, 1278, 1347});
        QPushButton* pre = nullptr; for (auto* b : rx->findChildren<QPushButton*>()) if (b->text() == "Preamp") pre = b; step("preamp button exists", pre != nullptr);
        step("preamp button usable once the radio is started", pre && pre->isEnabled() && !pre->isChecked());
        step("IF offset locked to 9 kHz while started (DDS 9 kHz below the dial)", rx->ifBox()->currentIndex() == 0 && !rx->ifBox()->isEnabled());
        { const std::string f0 = "/tmp/fireside_smoke_if0_" + std::to_string(::getpid()) + ".wav"; std::vector<float> z(2 * 4800, 0.0f); fireside::dsp::write_wav_f32(f0, 48000, 2, z);
          rx->loadFile(QString::fromStdString(f0)); rx->setPlaying(false); std::remove(f0.c_str()); }      // G1: a new source that wants IF 0 (a file) must not unlock or change it
        step("IF lock survives a new source", rx->ifBox()->currentIndex() == 0 && !rx->ifBox()->isEnabled() && ops().size() == 5);
        rx->loadDemo(); rx->setPlaying(false);
        std::string shot; for (int i = 2; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--screenshot")) shot = argv[i + 1];
        if (!shot.empty()) { w.resize(1040, 780); w.showRadio(); pump(3); w.grab().save(QString::fromStdString(shot)); w.showReceiver(); pump(2); }
        const double before = rx->dialHz(); rx->setDialHz(14.074e6); pump(5);               // the dial box tunes the radio now
        step("dial retunes the radio", rx->dialHz() == 14.074e6 && ops().size() == 6 && ops().back() == 1347);
        rx->setDialHz(14.075e6); rx->setDialHz(14.076e6); rx->setDialHz(14.077e6); pump(10);   // a spinning knob: never a write per step
        const size_t after_spin = ops().size(); step("spinning knob is rate limited", after_spin >= 7 && after_spin <= 8);
        { const size_t n = ops().size(); pre->click(); pump(3);
          step("preamp ON is exactly one SET_TRX_PREAMP write", ops().size() == n + 1 && ops().back() == 1247 && rp->session().preamp_on());
          pre->click(); pump(3); step("preamp off is one more", ops().size() == n + 2 && ops().back() == 1247 && !rp->session().preamp_on());
          pre->click(); pump(3); step("preamp on again before stopping", rp->session().preamp_on() && ops().size() == n + 3); }
        {   // the radio's transmit card on the simulated radio (tuner, PA and EEPROM modelled: fireside/sim_radio.h)
            auto* tc = w.transmitCard(); step("transmit card exists", tc != nullptr);
            auto wait_idle = [&](int ms) { for (int i = 0; i < ms / 30 && (tc->busy() || tc->statusLabel()->text().endsWith("...")); ++i) pump(1); };
            auto count = [&](uint32_t code) { size_t n = 0; for (auto c : ops()) n += c == code; return n; };
            if (tc) {
                tc->setConfirmForTests(true);
                step("arm usable once the radio is started", tc->armButton()->isEnabled() && !tc->tuneButton()->isEnabled() && !tc->measureButton()->isEnabled());
                const size_t n = ops().size(); tc->autoArm(); wait_idle(20000);                    /* what runs by itself once a real radio starts */
                const QString st = tc->statusLabel()->text();
                if (TransmitCard::findBackup().isEmpty()) {
                    step("no backup: arming refused and nothing written", st.contains("No EEPROM backup") && ops().size() == n && !tc->armButton()->isChecked());
                    tc->setBackupOfferForTests(true); tc->autoArm(); pump(30); wait_idle(40000);      // the offer is accepted (tests answer yes): this simulated radio carries no usable EEPROM
                    step("no backup: the offer saves nothing from an unusable EEPROM and writes nothing to the radio", QDir(TransmitCard::backupDir()).entryList({"F5K_*"}, QDir::Files).isEmpty() && ops().size() == n && !tc->armButton()->isChecked());
                } else {
                    step("transmit ready against the simulated radio carrying the backup's EEPROM", st.startsWith("Transmit ready") && tc->armButton()->isChecked() && !tc->armButton()->isVisible());
                    step("arming wrote exactly the 8 PA bias pots", ops().size() == n + 8 && count(1237) == 8);
                    rx->setDialHz(14.336e6); pump(5);
                    step("tune usable before a measurement (EEPROM table drive)", tc->tuneButton()->isEnabled() && tc->measureButton()->isEnabled() && tc->testToneButton()->isEnabled());
                    tc->measureButton()->click(); wait_idle(20000);
                    step("drive measured on the simulated PA", tc->statusLabel()->text().contains("Drive measured") && !w.simRadio()->keyed && tc->tuneButton()->isEnabled());
                    const size_t mox = count(1292); tc->tuneButton()->click(); wait_idle(40000);
                    step("ATU tune passes on the simulated radio", tc->statusLabel()->text().contains("PASS") && tc->memoryLabel()->text().contains("ind 30 cap 7"));
                    step("the tune keyed once and unkeyed", count(1292) == mox + 2 && !w.simRadio()->keyed);
                    {   // FT4: a queued send goes out in its slot; a queued one is cancelled by Stop without keying
                        const auto modes = tc->digitalPart()->findChildren<QComboBox*>(); for (auto* c : modes) if (c->count() == 2 && c->itemText(0) == "FT8") c->setCurrentIndex(1);
                        const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
                        const long long idx = (long long)std::floor(now / 7.5) + ((std::fmod(now, 7.5) > 5.0) ? 2 : 1);   // a slot >= 2.5 s away
                        tc->parityBox()->setCurrentIndex(int(idx % 2)); rx->myCallBox()->setText("M0OML"); tc->messageBox()->setText("CQ M0OML IO91");   // the station is the user's own: there is no default call
                        const size_t mox = count(1292); tc->sendButton()->click(); pump(2);
                        step("FT4 send queued", tc->sendQueued());
                        for (int i = 0; i < 900 && (tc->sendQueued() || tc->busy() || tc->statusLabel()->text().endsWith("...")); ++i) pump(1);
                        step("FT4 over sent on the simulated radio", tc->statusLabel()->text().contains("FT4 sent") && count(1292) == mox + 2 && !w.simRadio()->keyed);
                        const size_t mox2 = count(1292); tc->sendButton()->click(); pump(2);
                        QPushButton* stopb = nullptr; for (auto* b : tc->digitalPart()->findChildren<QPushButton*>()) if (b->text() == "Stop") stopb = b;
                        if (stopb) stopb->click();
                        pump(20); step("a queued send is cancelled by Stop without keying", !tc->sendQueued() && count(1292) == mox2 && !w.simRadio()->keyed);
                        tc->messageBox()->setText("   "); const size_t mox3 = count(1292); tc->sendButton()->click(); pump(2);
                        step("an empty message is refused, nothing queued or keyed", !tc->sendQueued() && tc->statusLabel()->text().contains("Cannot send") && count(1292) == mox3);
                        QCheckBox* rep = nullptr; for (auto* c : tc->digitalPart()->findChildren<QCheckBox*>()) if (c->text().startsWith("Repeat")) rep = c;
                        step("repeat box exists", rep != nullptr);
                        if (rep) {   // Repeat: after an over the next slot of the same parity is queued by itself; Stop ends the calling
                            const double now2 = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
                            const long long i2 = (long long)std::floor(now2 / 7.5) + ((std::fmod(now2, 7.5) > 5.0) ? 2 : 1);
                            tc->parityBox()->setCurrentIndex(int(i2 % 2)); tc->messageBox()->setText("CQ M0OML IO91"); rep->setChecked(true);
                            const size_t m4 = count(1292); tc->sendButton()->click(); pump(2);
                            step("repeat: no question, the status says repeating", tc->sendQueued() && tc->statusLabel()->text().contains("repeating"));
                            for (int i = 0; i < 900 && !(count(1292) >= m4 + 2 && !tc->busy()); ++i) pump(1);
                            for (int i = 0; i < 50 && !tc->sendQueued(); ++i) pump(1);
                            step("repeat: after the over the next slot of that parity is queued", count(1292) == m4 + 2 && tc->sendQueued() && !w.simRadio()->keyed
                                 && tc->statusLabel()->text().contains(tc->parityBox()->currentIndex() ? "odd" : "even"));
                            if (stopb) stopb->click();
                            pump(20); rep->setChecked(false);
                            step("repeat: Stop ends the calling without another key", !tc->sendQueued() && count(1292) == m4 + 2 && !w.simRadio()->keyed);
                        }
                    }
                    {   // click to reply, auto sequence and auto log: a whole FT4 QSO with DK0ULR on the simulated radio
                        const auto modes = tc->digitalPart()->findChildren<QComboBox*>(); for (auto* c : modes) if (c->count() == 2 && c->itemText(0) == "FT8") c->setCurrentIndex(1);
                        rx->setDecoderIndex(5); rx->myCallBox()->setText("M0OML"); tc->messageBox()->setText("CQ M0OML IO92");
                        QString logged; QObject scope; QObject::connect(tc, &TransmitCard::logQso, &scope, [&](const QString& c, const QString& g, const QString& a, const QString& b) { logged = c + "|" + g + "|" + a + "|" + b; });
                        auto wall = [] { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); };
                        auto label = [](double t) { return QDateTime::fromMSecsSinceEpoch(qint64(std::floor(t / 7.5) * 7500), QTimeZone::UTC).toString("HHmmss"); };
                        const double t0 = wall(); const long long k = (long long)std::floor(t0 / 7.5) - 1;                       // their CQ: the slot that just ended
                        const size_t m0 = count(1292); tc->replyTo(label(k * 7.5) + " -10  0.1 1200 ~  CQ DK0ULR JN58"); pump(2);
                        step("click to reply: queued in the other parity with our grid", tc->sendQueued() && tc->parityBox()->currentIndex() == int((k + 1) % 2) && tc->messageText() == "DK0ULR M0OML IO92");
                        auto over = [&](size_t n) { for (int i = 0; i < 1500 && !(count(1292) >= m0 + n && !tc->busy()); ++i) pump(1); pump(30); return count(1292) >= m0 + n; };
                        step("reply sent", over(2) && !w.simRadio()->keyed);
                        rx->addDecodedText(label(wall()) + " -12  0.1 1200 ~  M0OML DK0ULR -15\n"); pump(2);
                        step("their report moves the QSO on: our R-report", tc->messageText() == "DK0ULR M0OML R-12" && tc->sendQueued());
                        step("R-report sent", over(4));
                        rx->addDecodedText(label(wall()) + " -11  0.1 1200 ~  M0OML DK0ULR RR73\n"); pump(2);
                        step("their RR73: our 73", tc->messageText() == "DK0ULR M0OML 73");
                        step("73 sent", over(6));
                        step("the QSO is logged by itself: call, grid, both reports", logged == "DK0ULR|JN58|-12|-15");
                        step("a test QSO never reaches the user's own log files", !w.logPanel()->localDir().startsWith(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)));
                        step("the log gets our measured power (TX_PWR)", tc->recentTxPowerW() > 3.5 && tc->recentTxPowerW() < 6.5);
                        for (int i = 0; i < 300 && tc->sendQueued(); ++i) pump(1);
                        step("done: nothing more queued or keyed", !tc->sendQueued() && !w.simRadio()->keyed && count(1292) == m0 + 6 && tc->qsoText().contains("done"));
                        const QString qp = rx->qsoPaneText();
                        step("QSO pane: the partner, their messages to me and our transmissions", qp.contains("QSO with DK0ULR") && qp.contains("M0OML DK0ULR -15") && qp.contains("M0OML DK0ULR RR73")
                             && qp.contains("TX") && qp.contains("DK0ULR M0OML R-12") && qp.contains("DK0ULR M0OML 73") && !qp.contains("CQ DK0ULR"));
                        rx->addDecodedText("093500  -9  0.2 1200 ~  DK0ULR EA3XYZ -10\n093515 -12  0.2 1300 ~  CQ DK0ULR JN58\n093530  -8  0.1 1500 ~  EA3XYZ DL1ABC 73\n093545  -7  0.1 1600 ~  M0OML DK0ULR 73\n");
                        const QString qp2 = rx->qsoPaneText();
                        step("QSO pane: the partner's calls to others and unrelated traffic are dropped, a line to me still shows", !qp2.contains("EA3XYZ") && !qp2.contains("CQ DK0ULR") && !qp2.contains("DL1ABC") && qp2.contains("M0OML DK0ULR 73"));
                        for (int i = 0; i < 80; ++i) rx->addQsoTx("093600", 1500, QString("DK0ULR M0OML R-%1").arg(10 + i % 20));
                        pump(3); { int mx = 0; const bool bottom = rx->qsoPaneAtBottom(&mx); step("QSO pane stays scrolled to the newest line", bottom && (mx > 0 || !rx->isVisible())); }
                        step("Clear QSO button shown for FT8 / FT4", rx->clearQsoButton() && rx->clearQsoButton()->isVisibleTo(rx));
                        rx->clearQsoButton()->click(); step("Clear QSO empties the pane", rx->qsoPaneText().trimmed().isEmpty());
                        step("power meter shows the last over", tc->powerMeter()->text().startsWith("last over: fwd"));
                        w.logPanel()->setMyGrid("IO92AR");   // the Log tab's station profile is where the locator comes from: there is no built-in default
                        const size_t m1 = count(1292); tc->cqButton()->click(); pump(2);
                        step("CQ button: calls CQ with my grid (queued, or already joining this slot)", (tc->sendQueued() || tc->busy()) && tc->messageText() == "CQ M0OML IO92" && tc->qsoText().startsWith("Calling CQ"));
                        QPushButton* sb = nullptr; for (auto* b : tc->digitalPart()->findChildren<QPushButton*>()) if (b->text() == "Stop") sb = b;
                        if (sb) sb->click();
                        for (int i = 0; i < 200 && tc->busy(); ++i) pump(1); pump(10);
                        step("Stop ends the CQ: nothing queued, unkeyed", !tc->sendQueued() && !tc->busy() && !w.simRadio()->keyed && (count(1292) == m1 || count(1292) == m1 + 2) && !tc->qsoText().startsWith("Calling"));
                    }
                    {   // voice: PTT keys an SSB over with the Audio tab's input + processing; release unkeys (a generated voice: no mic here)
                        struct GenVoice : fireside::txc::IAudioIn {
                            size_t pos = 0; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
                            size_t read(float* x, size_t n, double) override { std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(1e6 * double(pos + n) / 48000)));
                                for (size_t i = 0; i < n; ++i, ++pos) { const double t = double(pos) / 48000; x[i] = float(0.2 * (0.5 - 0.5 * std::cos(2 * M_PI * 3 * t)) * (std::sin(2 * M_PI * 600 * t) + std::sin(2 * M_PI * 1500 * t))); } return n; }
                            std::string describe() const override { return "generated voice"; }
                        };
                        w.audioPanel()->setInputFactoryForTests([] { return std::unique_ptr<fireside::txc::IAudioIn>(new GenVoice); });
                        rx->setDialHz(14.200e6); pump(5); const size_t m0 = count(1292);
                        step("PTT usable once transmit is ready", tc->pttButton()->isEnabled());
                        tc->setPttForTests(true); for (int i = 0; i < 300 && !w.simRadio()->keyed; ++i) pump(1);
                        step("PTT keys a voice over", w.simRadio()->keyed && tc->statusLabel()->text().startsWith("Voice USB on 14.2000 MHz"));
                        for (int i = 0; i < 80; ++i) pump(1);
                        step("the power meter shows the voice over", tc->powerMeter()->text().startsWith("fwd"));
                        tc->setPttForTests(false); for (int i = 0; i < 300 && (tc->busy() || w.simRadio()->keyed); ++i) pump(1);
                        step("releasing PTT unkeys", !w.simRadio()->keyed && count(1292) == m0 + 2 && tc->statusLabel()->text().contains("PTT released"));
                        w.audioPanel()->setInputFactoryForTests({});
                    }
                    {   // keying by another program over CAT (Hamlib set_ptt): only with the tick, only the owning client, unkeys on T 0 / untick / disconnect / silence
                        struct GenTone : fireside::txc::IAudioIn {
                            size_t pos = 0; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
                            size_t read(float* x, size_t n, double) override { std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(1e6 * double(pos + n) / 48000)));
                                for (size_t i = 0; i < n; ++i, ++pos) x[i] = float(0.9 * std::sin(2 * M_PI * 1500 * double(pos) / 48000)); return n; }
                            std::string describe() const override { return "generated FT8-like tone"; }
                        };
                        w.audioPanel()->setExternalInputFactoryForTests([] { return std::unique_ptr<fireside::txc::IAudioIn>(new GenTone); });
                        CatPanel* cp = w.catPanel(); step("CAT server for the keying test", cp && cp->startServer(0));
                        auto open_client = [&](QTcpSocket& so) { so.connectToHost(QHostAddress::LocalHost, quint16(cp->port())); for (int i = 0; i < 200 && so.state() != QAbstractSocket::ConnectedState; ++i) { QApplication::processEvents(); QThread::msleep(5); } };
                        auto ask = [&](QTcpSocket& so, const QByteArray& cmd) { QByteArray got; so.write(cmd);
                            for (int i = 0; i < 400 && !got.endsWith("\n"); ++i) { QApplication::processEvents(); so.waitForReadyRead(5); got += so.readAll(); } return got; };
                        auto wait_keyed = [&](bool k) { for (int i = 0; i < 300 && w.simRadio()->keyed != k; ++i) pump(1); for (int i = 0; i < 300 && !k && tc->busy(); ++i) pump(1); return w.simRadio()->keyed == k; };
                        QTcpSocket a, b; open_client(a); open_client(b);
                        rx->setDialHz(14.074e6); pump(5); const size_t m0 = count(1292);
                        step("the tick starts off (every new window)", !tc->extAllowBox()->isChecked());
                        step("CAT T 1 refused without the tick", ask(a, "T 1\n") == "RPRT -9\n" && !w.simRadio()->keyed && count(1292) == m0);
                        tc->extAllowBox()->setChecked(true);
                        step("CAT T 1 accepted with the tick", ask(a, "T 1\n") == "RPRT 0\n");
                        step("the program's over keys", wait_keyed(true) && tc->statusLabel()->text().startsWith("Keyed by another program over CAT"));
                        step("get_ptt reports it", ask(a, "t\n") == "1\n");
                        step("a second program cannot key or release it", ask(b, "T 1\n") == "RPRT -9\n" && ask(b, "T 0\n") == "RPRT 0\n" && w.simRadio()->keyed);
                        step("the dial is locked while keyed", ask(a, "F 14080000\n") == "RPRT -9\n" && std::fabs(rx->dialHz() - 14.074e6) < 1);
                        step("T 0 unkeys", ask(a, "T 0\n") == "RPRT 0\n" && wait_keyed(false) && tc->statusLabel()->text().contains("PTT released"));
                        step("T 1 keys again, unticking unkeys", ask(a, "T 1\n") == "RPRT 0\n" && wait_keyed(true));
                        tc->extAllowBox()->setChecked(false); step("unticking unkeys", wait_keyed(false));
                        tc->extAllowBox()->setChecked(true);
                        step("T 1 keys again, the program disconnecting unkeys", ask(a, "T 1\n") == "RPRT 0\n" && wait_keyed(true));
                        a.abort(); step("a disconnect unkeys", wait_keyed(false));
                        tc->setExtWatchdogForTests(600);
                        step("T 1 keys again, a program that goes quiet is unkeyed", ask(b, "T 1\n") == "RPRT 0\n" && wait_keyed(true));
                        for (int i = 0; i < 100 && w.simRadio()->keyed; ++i) pump(1); pump(10);
                        step("the watchdog unkeys a quiet program", !w.simRadio()->keyed && tc->statusLabel()->text().contains("stopped talking"));
                        tc->setExtWatchdogForTests(15000);
                        rx->setDialHz(14.400e6); pump(5);
                        step("outside the amateur bands refused", ask(b, "T 1\n") == "RPRT -9\n" && !w.simRadio()->keyed);
                        rx->setDialHz(14.074e6); pump(5);
                        step("every over keyed and unkeyed through the gate", (count(1292) - m0) % 2 == 0 && !w.simRadio()->keyed);
                        tc->extAllowBox()->setChecked(false); b.abort(); cp->stopServer(); w.audioPanel()->setExternalInputFactoryForTests({});
                    }
                    {   // a safety stop (here: a power limit on the fixed simulated reading) shows the red box above the tabs for 5 s; a normal end does not
                        for (int i = 0; i < 250 && w.tripBox() && w.tripBox()->isVisible(); ++i) pump(1);                  // an earlier step may have tripped on purpose: let its box expire
                        step("no red box once earlier trips have expired", w.tripBox() && !w.tripBox()->isVisible());
                        const int fwd0 = w.simRadio()->adc_fwd, rev0 = w.simRadio()->adc_rev; const bool pfl = w.simRadio()->pa_from_level;
                        w.simRadio()->pa_from_level = false; w.simRadio()->adc_fwd = 966; w.simRadio()->adc_rev = 800;       // a strong forward reading: the power limit trips
                        rx->setDialHz(14.100e6); pump(5); tc->testToneButton()->click();
                        for (int i = 0; i < 300 && !w.tripBox()->isVisible(); ++i) pump(1);
                        for (int i = 0; i < 300 && tc->busy(); ++i) pump(1); pump(3);                                         // the card's own status line follows the box
                        step("a limit stop shows the red box with the reason", w.tripBox()->isVisible() && w.tripBox()->text().startsWith("TRANSMITTER STOPPED: ") && w.tripBox()->text().contains(" above ") && tc->statusLabel()->text().contains(w.tripBox()->text().mid(21).left(20)));
                        step("and the radio is unkeyed", !w.simRadio()->keyed);
                        pump(60); step("the box is still there 2 s later", w.tripBox()->isVisible());
                        for (int i = 0; i < 200 && w.tripBox()->isVisible(); ++i) pump(1);
                        step("it goes away after 5 s", !w.tripBox()->isVisible());
                        w.simRadio()->adc_fwd = fwd0; w.simRadio()->adc_rev = rev0; w.simRadio()->pa_from_level = pfl;
                    }
                    w.simRadio()->silent = true; tc->tuneButton()->click();                    // a tuner that never answers: Stop unkeys it
                    for (int i = 0; i < 300 && !w.simRadio()->keyed; ++i) pump(1);
                    step("the never-ending tune is keyed", w.simRadio()->keyed); pump(10);
                    QPushButton* stop = nullptr; for (auto* b : tc->digitalPart()->findChildren<QPushButton*>()) if (b->text() == "Stop") stop = b;
                    step("stop is usable while tuning", stop && stop->isEnabled());
                    if (stop) stop->click();
                    wait_idle(20000); w.simRadio()->silent = false;
                    step("stop unkeyed the tune", !w.simRadio()->keyed && !tc->busy() && (tc->statusLabel()->text().contains("abort") || tc->statusLabel()->text().contains("stopped")));
                }
                {   // the master switch: Voice / CW / Digital decide which cards exist (nothing here transmits)
                    w.showReceiver(); rx->setModeGroup(0); pump(3);
                    {   // the S-meter: the factory RX level table per band, the preamp row only while the preamp is on, the placeholder without a table
                        std::array<double, 11> mt{}, pa{}; mt.fill(-33.0); pa.fill(-14.0); mt[5] = -32.7; const double dial0 = rx->dialHz();
                        rx->setRxLevelCal(true, mt, pa); rx->setDialHz(14.2e6); rx->setPreampChecked(false); pump(1);
                        step("S-meter: the 20 m factory meter offset", rx->meterCalibrated() && std::fabs(rx->meterOffsetDb() + 32.7) < 1e-9);
                        rx->setPreampChecked(true); step("S-meter: preamp on subtracts its gain", std::fabs(rx->meterOffsetDb() + 46.7) < 1e-9); rx->setPreampChecked(false);
                        rx->setDialHz(5.0e6); step("S-meter: outside the bands the placeholder", !rx->meterCalibrated());
                        rx->setRxLevelCal(false, mt, pa); rx->setDialHz(14.2e6); step("S-meter: no table, no calibration", !rx->meterCalibrated()); rx->setDialHz(dial0); pump(1);
                    }
                    rx->setMeterTx(true); step("meter tile: the power meter replaces the signal meter while keyed", rx->powerMeterShown());
                    step("Voice: USB-type mode, no decoder, voice strip shown", rx->modeGroup() == 0 && rx->decoderIndex() == 0 && rx->modeIndex() != 3 && !rx->decoderRowVisible() && rx->voiceStripVisible() && !rx->sendCardVisible());
                    rx->setModeGroup(2); pump(3);
                    step("Digital: FT8 decoder, message controls shown, voice strip gone", rx->modeGroup() == 2 && rx->decoderIndex() >= 4 && rx->decoderRowVisible() && rx->sendCardVisible() && !rx->voiceStripVisible());
                    rx->setModeGroup(1); pump(3);
                    step("CW: CW mode, decoder card without the message controls", rx->modeGroup() == 1 && rx->modeIndex() == 3 && rx->decoderRowVisible() && !rx->sendCardVisible() && !rx->voiceStripVisible());
                    rx->setDecoderIndex(2); pump(3);
                    step("picking PSK31 moves the switch to Digital", rx->modeGroup() == 2 && rx->sendCardVisible());
                    rx->setModeGroup(0); pump(3);
                    step("back to Voice: decoder off again", rx->modeGroup() == 0 && rx->decoderIndex() == 0 && !rx->decoderRowVisible());
                }
                if (!shot.empty()) { w.resize(2536, 1377); w.showReceiver(); rx->setDecoderIndex(4); rx->myCallBox()->setText("M0OML"); rx->addDecodedText("093300  -3  0.2 1200 ~  CQ DL2AL JO31\n093315  -6  0.1 2576 ~  M0OML DK0ULR JN58\n"); pump(4); w.grab().save(QString::fromStdString(shot).replace(".png", "-transmit.png")); w.resize(1400, 860); pump(4); w.grab().save(QString::fromStdString(shot).replace(".png", "-small.png")); rx->setModeGroup(0); pump(4); w.grab().save(QString::fromStdString(shot).replace(".png", "-voice.png")); rx->setModeGroup(1); pump(4); w.grab().save(QString::fromStdString(shot).replace(".png", "-cw.png")); if (auto* ap = w.audioPanel()) { ap->resize(2000, 700); ap->grab().save(QString::fromStdString(shot).replace(".png", "-audio.png")); } rx->setDecoderIndex(0); }
            }
        }
        step("stop radio", rp->stopRadio());
        step("stop leaves a non-live source alone", !rx->closeLive());
        step("preamp button off and unusable after stop", !pre->isChecked() && !pre->isEnabled() && !rp->session().preamp_on());
        const auto v = ops(); step("preamp switched off first in the shutdown", v.size() >= 4 && v[v.size() - 4] == 1247); step("shutdown last, PA bias off at the very end", v.size() >= 3 && v[v.size() - 3] == 1292 && v[v.size() - 2] == 1320 && v.back() == 1285);
        step("stopped state", w.radio()->session().state() == fireside::RadioSession::State::Connected);
        if (auto* tc = w.transmitCard()) step("transmit disarmed and unusable after stop", !tc->armButton()->isChecked() && !tc->armButton()->isEnabled() && !tc->tuneButton()->isEnabled());
        step("IF offset unlocked after stop", rx->ifBox()->isEnabled());
        step("nothing left to shut down after a clean stop", rp->shutdownOnExit() && ops().size() == v.size());
        {   // a start that fails part-way (mailbox wedges after INITIALIZE): closing the window must still try the shutdown
            bus->dynamic[fireside::find_opcode(std::string("RDAL_OP_SET_TRX_PREAMP"))->value] = [&] { bus->stuck = true; bus->mem[fireside::kPalCmd] = fireside::sim::be_words({fireside::kPalBusy}); return 0u; };
            const size_t n0 = ops().size(); const bool started = rp->startRadio(true);
            step("partial start reported", !started && rp->statusText().size() > 0);
            bus->dynamic.clear(); bus->stuck = false; bus->mem[fireside::kPalCmd] = fireside::sim::be_words({0});      // radio answers again
            step("close after a partial start shuts the radio down", rp->shutdownOnExit() && ops().size() > n0 && ops().back() == 1285);
        }
        {   // the Antennas tab: the RX antenna follows the band while the radio runs (SET_RX1_ANT 1..4 only), and the start-up uses the band's antenna
            {   // Mode card: FT8 / FT4 buttons and the automatic sideband (no radio writes beyond tuning)
                const int dec0 = rx->decoderIndex(); rx->setDecoderIndex(0); rx->autoSidebandBox()->setChecked(true);
                rx->setModeIndex(0); rx->setDialHz(14.2e6); rx->setDialHz(7.1e6); step("auto sideband: 40 m is LSB", rx->modeIndex() == 1);
                rx->setDialHz(14.2e6); step("auto sideband: 20 m is USB", rx->modeIndex() == 0);
                rx->setDialHz(14.25e6); rx->setModeIndex(1); rx->setDialHz(14.3e6); step("auto sideband: inside a band, your choice stays", rx->modeIndex() == 1);
                rx->autoSidebandBox()->setChecked(false); rx->setDialHz(7.1e6); rx->setDialHz(14.2e6); step("auto sideband off: the mode stays", rx->modeIndex() == 1); rx->autoSidebandBox()->setChecked(true);
                rx->setDialHz(7.1e6); step("FT8 on 40 m: 7.074 MHz, USB, the FT8 decoder", rx->selectFt(false) && std::fabs(rx->dialHz() - 7.074e6) < 1 && rx->modeIndex() == 0 && rx->decoderIndex() == 4);
                step("FT4 on 40 m: 7.0475 MHz, the FT4 decoder", rx->selectFt(true) && std::fabs(rx->dialHz() - 7.0475e6) < 1 && rx->decoderIndex() == 5);
                { auto* tm = w.findChild<QComboBox*>("ftModeBox"); step("the Transmit card sends FT4 too", tm && tm->currentIndex() == 1); }
                rx->setDialHz(14.2e6); step("FT8 keeps USB while the dial changes band", rx->modeIndex() == 0);
                step("FT8 on 20 m: 14.074 MHz", rx->selectFt(false) && std::fabs(rx->dialHz() - 14.074e6) < 1);
                rx->setDialHz(5.36e6); step("60 m has no FT4 frequency", !rx->selectFt(true));
                rx->setDecoderIndex(dec0); rx->setDialHz(7.1e6);
            }
            auto* at = w.antennas(); step("antennas tab exists with 12 band rows", at && at->bandCount() == 12);
            auto ant_writes = [&] { std::vector<uint32_t> v; for (auto& wr : bus->writes) if ((fireside::sim::be_word(wr.second, 0) & 0x7FFFFFFFu) == 1278u) v.push_back(fireside::sim::be_word(wr.second, 4)); return v; };
            at->autoBox()->setChecked(true); at->defaultBox()->setCurrentIndex(0);                                        // default ANT1
            size_t n40 = 0; const fireside::RxBand* bands = fireside::rx_bands(n40); int i40 = -1, i20 = -1; for (size_t i = 0; i < n40; ++i) { if (std::string(bands[i].id) == "40m") i40 = int(i); if (std::string(bands[i].id) == "20m") i20 = int(i); }
            at->bandBox(i40)->setCurrentIndex(2); at->bandBox(i20)->setCurrentIndex(4);                                   // 40 m -> ANT2, 20 m -> RX1 IN (items: Default, ANT1, ANT2, ANT3, RX1 IN)
            step("antenna lookup by dial", at->antennaFor(7.074e6) == "ANT2" && at->antennaFor(14.074e6) == "RX1IN" && at->antennaFor(9.6e6) == "ANT1" && at->antennaFor(3.573e6) == "ANT1");
            rp->disconnectRadio(); rx->setDialHz(7.1e6); const size_t w0 = ant_writes().size();
            step("no antenna writes while the radio is not running", ant_writes().size() == w0);
            step("quick start", rp->quickStart(true) && rp->session().state() == fireside::RadioSession::State::Started);
            { const auto v = ant_writes(); step("the start-up used the band's antenna (ANT2)", v.size() == w0 + 1 && v.back() == 2u); }
            step("status line shows the antenna", rx->status().contains("RX ANT2"));
            rx->setDialHz(7.2e6); step("same band: no write", ant_writes().size() == w0 + 1);
            rx->setDialHz(14.074e6); { const auto v = ant_writes(); step("moving to 20 m switches to RX1 IN (4)", v.size() == w0 + 2 && v.back() == 4u); }
            rx->setDialHz(14.2e6); step("inside 20 m: still no write", ant_writes().size() == w0 + 2);
            rx->setDialHz(9.6e6); { const auto v = ant_writes(); step("outside every band: the default (ANT1)", v.size() == w0 + 3 && v.back() == 1u); }
            at->autoBox()->setChecked(false); rx->setDialHz(7.1e6); step("auto switching off: the dial moves, the antenna stays", ant_writes().size() == w0 + 3);
            at->autoBox()->setChecked(true); { const auto v = ant_writes(); step("auto switching on again applies the band's antenna", v.size() == w0 + 4 && v.back() == 2u); }
            at->bandBox(i40)->setCurrentIndex(0); { const auto v = ant_writes(); step("setting the band to Default re-evaluates (ANT1)", v.size() == w0 + 5 && v.back() == 1u); }
            at->bandBox(i40)->setCurrentIndex(3); { const auto v = ant_writes(); step("a table change takes effect at once (ANT3)", v.size() == w0 + 6 && v.back() == 3u); }
            at->autoBox()->setChecked(false); at->setAntennaForDial(7.1e6, "ANT2");                                       // the main screen's selector
            { const auto v = ant_writes(); step("the selector switches at once, even with auto off, and remembers it for the band", v.size() == w0 + 7 && v.back() == 2u && at->antennaFor(7.074e6) == "ANT2"); }
            at->autoBox()->setChecked(true);
            for (auto v : ant_writes()) step("only wire values 1..4 were ever written", v >= 1u && v <= 4u);
            step("stop the radio after the antenna checks", rp->stopRadio()); step("status line forgets the antenna", !rx->status().contains("RX ANT"));
            const size_t w1 = ant_writes().size(); rx->setDialHz(14.074e6); step("no antenna writes after stop", ant_writes().size() == w1);
            at->bandBox(i40)->setCurrentIndex(0); at->bandBox(i20)->setCurrentIndex(0); rx->setDialHz(7.1e6);
        }
        {   // the Receiver's Source card: 48 / 96 / 192 buttons show the running stream, Stop radio is only usable while the radio runs
            auto btn = [&](const char* t) { for (auto* b : rx->findChildren<QPushButton*>()) if (b->text() == t) return b; return static_cast<QPushButton*>(nullptr); };
            QPushButton *b48 = btn("48"), *b96 = btn("96"), *b192 = btn("192"), *stopb = btn("Stop radio");
            step("rate buttons and Stop radio exist on the Receiver tab", b48 && b96 && b192 && stopb && b48->isCheckable() && !btn("Live..."));
            rx->syncRateButtons(192000); step("192 shows checked, the others not", b192->isChecked() && !b96->isChecked() && !b48->isChecked());
            rx->syncRateButtons(96000); step("switching shows only 96", b96->isChecked() && !b192->isChecked() && !b48->isChecked());
            rx->syncRateButtons(0); step("no stream: none checked", !b48->isChecked() && !b96->isChecked() && !b192->isChecked() && rx->liveRate() == 0);
            rp->disconnectRadio(); step("Stop radio unusable while the radio is not running", !stopb->isEnabled());
            const size_t n0 = ops().size(); rp->allowWritesBox()->setChecked(false); step("start by the rate path (simulated, confirmed)", rp->startAtRate(192000, true) && rp->session().state() == fireside::RadioSession::State::Started && ops().size() == n0 + 5);
            step("Stop radio usable once the radio runs", stopb->isEnabled());
            const size_t n1 = ops().size(); b96->click(); QApplication::processEvents();
            step("a rate button on a running radio writes nothing (only the stream restarts; none exists when simulated)", ops().size() == n1 && rp->session().state() == fireside::RadioSession::State::Started && rx->liveRate() == 0 && !b96->isChecked());
            stopb->click(); QApplication::processEvents();
            step("Stop radio button stops the radio", rp->session().state() != fireside::RadioSession::State::Started && !stopb->isEnabled());
        }
        {   // Start receiving (one click): from disconnected, from connected read-only, and when already started. The one question is skipped here (confirmed = true).
            int started_signals = 0; QObject::connect(rp, &RadioPanel::radioStarted, [&] { ++started_signals; });
            const std::vector<uint32_t> startup{1219, 1247, 1320, 1278, 1347}; auto tail5 = [&] { const auto o = ops(); return o.size() >= 5 && std::vector<uint32_t>(o.end() - 5, o.end()) == startup; };
            rp->disconnectRadio(); rp->allowWritesBox()->setChecked(false); size_t n0 = ops().size();
            step("quick start from disconnected", rp->quickStart(true) && rp->session().state() == fireside::RadioSession::State::Started);
            step("quick start allowed writes for the session and ran exactly the reviewed start-up", rp->allowWritesBox()->isChecked() && ops().size() == n0 + 5 && tail5());
            step("quick start announced the Receiver tab", started_signals == 1);
            step("quick start with a simulated radio opens no real live source", !rx->closeLive());
            n0 = ops().size(); step("quick start when already started writes nothing", rp->quickStart(true) && ops().size() == n0 && started_signals == 2);
            step("stop after quick start", rp->stopRadio()); rp->disconnectRadio(); rp->allowWritesBox()->setChecked(false);
            step("connect read-only again", rp->connectRadio() && !rp->session().allow_write()); n0 = ops().size();
            step("quick start from a read-only connection reconnects with writes", rp->quickStart(true) && rp->session().allow_write() && ops().size() == n0 + 5 && tail5() && started_signals == 3);
            step("stop at the end of the quick start checks", rp->stopRadio());
        }
        const bool ok = failed.empty();
        std::printf("%s%s%s: ops=%zu dial %.6f -> %.6f MHz status='%s'\n", ok ? "radio smoke ok" : "radio smoke FAILED at '", ok ? "" : failed.c_str(), ok ? "" : "'", v.size(), before / 1e6, rx->dialHz() / 1e6, rp->statusText().toStdString().c_str());
        return ok ? 0 : 1;
    }
    if (smokeTh) {                                                           // real worker thread + real timers: the GUI thread must stay responsive
        w.showReceiver(); w.show(); auto* r = w.receiver(); r->loadDemo();
        { auto* mb = r->mirrorBox(); bool mok = !mb->isChecked();                                // the demo/files start with Mirror off
          r->setSourceIsLive(true); mok = mok && mb->isChecked();                                    // the live radio comes up mirrored (user-confirmed on air)
          mb->setChecked(false); r->setSourceIsLive(false); mok = mok && !mb->isChecked();           // files keep their own setting
          r->setSourceIsLive(true); mok = mok && !mb->isChecked();                                   // ...and the live radio remembers that the user turned it off
          mb->setChecked(true); r->setSourceIsLive(false); mok = mok && !mb->isChecked(); r->setSourceIsLive(true); mok = mok && mb->isChecked();
          r->setSourceIsLive(false); if (!mok) { std::puts("receiver smoke FAILED: Mirror I/Q is not remembered per source kind"); return 1; } }
        int ticks = 0; QTimer heartbeat; heartbeat.setInterval(10); QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; }); heartbeat.start();
        QTimer::singleShot(1500, &app, &QApplication::quit); app.exec();
        const bool flowing = r->waterfallRows() > 15 && double(r->audioSamples()) > 0.5 * 48000 && r->playing();
        const bool responsive = ticks > 60;                                  // 1.5 s of 10 ms ticks = 150 ideal; the DSP is not on this thread
        auto* sp = r->findChild<SpectrumWidget*>(); const double before = r->dialHz();
        const int left = 44, pw = sp->width() - left - 6; const int x = left + fireside::display::hz_to_x(2000.0, 48000.0, pw);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(x, 40), QPointF(x, 40), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(sp, &press);
        const bool clicked = std::fabs(r->dialHz() - (before + 2000.0)) < 100.0;       // within the pixel resolution; snapped to 10 Hz
        r->setPlaying(false);
        std::printf("%s: rows=%zu audio=%zu ticks=%d dial %.6f -> %.6f MHz\n", flowing && responsive && clicked ? "threaded smoke ok" : "threaded smoke FAILED", r->waterfallRows(), r->audioSamples(), ticks, before / 1e6, r->dialHz() / 1e6);
        return flowing && responsive && clicked ? 0 : 1;
    }
    if (smokeRx) {
        std::string shot; for (int i = 2; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--screenshot")) shot = argv[i + 1];
        w.showReceiver(); w.show(); auto* r = w.receiver(); r->loadDemo();
        const int steps = shot.empty() ? 40 : 120;                                              // 4 s of demo signal (12 s when taking a screenshot so the waterfall is full), no wall-clock dependence
        for (int i = 0; i < steps; ++i) { r->advance(4800); app.processEvents(); }
        int cardMin = 1 << 30, cardMax = 0; std::set<QString> texts;                               // the signal card must keep its size whatever the numbers say (it used to follow the text and jitter)
        { QWidget* card = r->findChild<SMeter*>()->parentWidget(); auto labels = card->findChildren<QLabel*>("muted"); QLabel* txt = labels.isEmpty() ? nullptr : labels.first();
          if (txt) for (const char* t : {"no signal", "S3   -112 dBm (uncal)   -102.0 dBFS   peak line -95.5 dBFS", "S9+20   -53 dBm (uncal)   -43.0 dBFS   peak line -40.2 dBFS", "S9+60   -999 dBm (uncal)   -999.9 dBFS   peak line -999.9 dBFS", "S1   -9 dBm (uncal)   -1.0 dBFS   peak line -0.5 dBFS"}) {
              txt->setText(t); card->layout()->invalidate(); card->layout()->activate(); if (r->layout()) { r->layout()->invalidate(); r->layout()->activate(); }   // no event loop in between: the panel's own refresh timer would overwrite the text
              cardMin = std::min(cardMin, card->width()); cardMax = std::max(cardMax, card->width()); texts.insert(QString::fromLatin1(t)); } }
        const bool cardStable = cardMax == cardMin && cardMax > 0 && texts.size() == 5;
        const QImage img = r->grab().toImage();
        QSet<QRgb> colours; for (int y = 0; y < img.height(); y += 3) for (int x = 0; x < img.width(); x += 3) colours.insert(img.pixel(x, y));
        const bool peakOk = std::fabs(r->peakOffsetHz() - 1000.0) < 60.0 || std::fabs(r->peakOffsetHz() + 5000.0) < 60.0;
        const bool ok = r->waterfallRows() > 50 && peakOk && r->signalDbfs() > -40.0 && r->signalDbfs() < 0.0 && r->audioSamples() == 4800u * size_t(steps) && colours.size() > 40 && cardStable;
        if (!shot.empty()) {
            img.save(QString::fromStdString(shot));
            QImage sheet(1400, 4 * 190, QImage::Format_RGB32); sheet.fill(QColor("#0e1117")); QPainter pp(&sheet); pp.setPen(Qt::white);        // the four palettes on the same data
            for (int i = 0; i < fireside::display::kPaletteCount; ++i) {
                r->setPalette(i); const QImage g = r->waterfall()->grab().toImage().scaled(1400, 170, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                pp.drawImage(0, i * 190 + 20, g); pp.drawText(8, i * 190 + 14, QString::fromLatin1(fireside::display::palette_name(static_cast<fireside::display::Palette>(i))));
            }
            pp.end(); r->setPalette(0); sheet.save(QString::fromStdString(shot).replace(".png", "_palettes.png"));
        }
        // interaction paths: settings change rebuilds the chain and keeps producing audio; bad file is reported; a real WAV loads and plays
        const size_t before = r->audioSamples(); r->setModeIndex(1); r->setAgcIndex(0); r->mirrorBox()->setChecked(true); r->advance(4800);
        r->nrBox()->setChecked(true); r->anfBox()->setChecked(true); r->notchBox()->setValue(1500); r->squelchBox()->setValue(-90); r->advance(4800);
        const bool rebuilt = r->audioSamples() == before + 9600;                                 // both settings changes kept the audio flowing
        {   // wheel over the spectrum and waterfall tunes by the step (x10 with Shift); palettes recolour the history
            const double d0 = r->dialHz(); auto wheel = [&](QWidget* wdg, int notches, Qt::KeyboardModifiers mod) { QWheelEvent e(QPointF(100, 40), QPointF(100, 40), QPoint(), QPoint(0, 120 * notches), Qt::NoButton, mod, Qt::NoScrollPhase, false); QApplication::sendEvent(wdg, &e); };
            // direction (user preference 2026-11-18): wheel TOWARDS you (negative delta) = up in frequency, away = down
            wheel(r->spectrum(), -2, Qt::NoModifier); const bool w1 = std::fabs(r->dialHz() - (d0 + 2 * r->stepHz())) < 1;
            wheel(r->waterfall(), +1, Qt::NoModifier); const bool w2 = std::fabs(r->dialHz() - (d0 + 1 * r->stepHz())) < 1;
            wheel(r->spectrum(), -1, Qt::ShiftModifier); const bool w3 = std::fabs(r->dialHz() - (d0 + 11 * r->stepHz())) < 1;
            // Ctrl+wheel zooms spectrum and waterfall together about the mouse, without retuning; fully out again restores the whole span
            const double dz = r->dialHz(); wheel(r->spectrum(), +3, Qt::ControlModifier);
            const bool z1 = r->zoomFactor() > 1.9 && r->zoomFactor() < 2.0 && r->spectrum()->viewLo() == r->spectrum()->viewLo() && std::fabs(r->dialHz() - dz) < 1;   // 1/0.8^3 = 1.95
            wheel(r->waterfall(), +40, Qt::ControlModifier); const bool z2 = r->zoomFactor() <= 128.01 && r->spectrum()->spanHz() * (r->spectrum()->viewHi() - r->spectrum()->viewLo()) >= 1499;   // capped at 1.5 kHz
            r->setView(0.5, 0.75); bool zc = false; {                                               // zoomed: a click 1/2 across the waterfall is 5/8 of the full span = dial + span/8
                auto* wf = r->waterfall(); const double span = r->spectrum()->spanHz(); const int pw = wf->width() - 44 - 6; const int x = 44 + pw / 2;
                QMouseEvent e(QEvent::MouseButtonPress, QPointF(x, 20), QPointF(x, 20), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(wf, &e);
                zc = std::fabs(r->dialHz() - (dz + span / 8)) <= r->stepHz() + 1; r->setDialHz(dz); }
            wheel(r->spectrum(), -60, Qt::ControlModifier); const bool z3 = r->zoomFactor() == 1.0 && r->spectrum()->viewLo() == 0.0;
            auto zbtn = [&](const char* t) { for (auto* b : r->findChildren<QPushButton*>()) if (b->text() == t) return b; return static_cast<QPushButton*>(nullptr); };
            bool zb = zbtn("+") && zbtn("-") && zbtn("Full"); if (zb) { zbtn("+")->click(); const double f1 = r->zoomFactor(); zbtn("+")->click(); const double f2 = r->zoomFactor(); zbtn("-")->click(); const double f3 = r->zoomFactor();
                zb = f1 > 1.2 && f2 > f1 && f3 < f2 && f3 > 1.0; zbtn("Full")->click(); zb = zb && r->zoomFactor() == 1.0; }                       // the Zoom buttons (same path as the Ctrl+= / Ctrl+- / Ctrl+0 keys)
            if (!zb) { std::printf("receiver smoke FAILED: zoom buttons\n"); return 1; }
            { const int m0 = r->modeIndex(); r->setModeIndex(2); const bool amShown = r->amDetectorBox() && !r->amDetectorBox()->isHidden() && r->amDetectorBox()->count() == 4;
              r->amDetectorBox()->setCurrentIndex(1); r->setModeIndex(0); const bool amHidden = r->amDetectorBox()->isHidden(); r->amDetectorBox()->setCurrentIndex(0); r->setModeIndex(m0);
              if (!(amShown && amHidden)) { std::printf("receiver smoke FAILED: AM detector box shown=%d hidden-in-USB=%d\n", amShown, amHidden); return 1; } }
            if (!(z1 && z2 && z3 && zc)) { std::printf("receiver smoke FAILED: zoom in=%d cap=%d out=%d zoomed-click=%d (zoom %.2f)\n", z1, z2, z3, zc, r->zoomFactor()); return 1; }
            r->setDialHz(d0 + 11 * r->stepHz());
            const QRgb before = r->waterfall()->image().pixel(300, 2); r->setPalette(3); const QRgb gray = r->waterfall()->image().pixel(300, 2);
            const bool pal = before != gray && qRed(gray) == qGreen(gray) && qGreen(gray) == qBlue(gray);                          // gray palette: equal channels, and the old rows changed colour
            r->setPalette(0); r->setDialHz(d0);
            bool clickTune = false; {   // a left click on the waterfall (or the spectrum) retunes to that frequency, snapped to the step: here 3/4 of the way across = dial + span/4
                auto* wf = r->waterfall(); const double span = r->spectrum()->spanHz(); const int pw = wf->width() - 44 - 6; const int x = 44 + (3 * pw) / 4;
                QMouseEvent e(QEvent::MouseButtonPress, QPointF(x, 20), QPointF(x, 20), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(wf, &e);
                clickTune = std::fabs(r->dialHz() - (d0 + span / 4)) <= r->stepHz() + 1; r->setDialHz(d0); }
            if (!(w1 && w2 && w3 && pal && clickTune)) { std::printf("receiver smoke FAILED: wheel=%d,%d,%d palette=%d click=%d\n", w1, w2, w3, pal, clickTune); return 1; }
        }
        const bool badRejected = !r->loadFile("/nonexistent/none.wav") && r->status().contains("Cannot open");
        const std::string wav = "/tmp/fireside_smoke_" + std::to_string(::getpid()) + ".wav"; std::vector<float> il;
        for (auto& v : fireside::dsp::synth_tones(96000, 96000, {{12000, 0.3}}, 0.001)) { il.push_back(v.real()); il.push_back(v.imag()); }
        fireside::dsp::write_wav_f32(wav, 96000, 2, il);
        bool fileOk = r->loadFile(QString::fromStdString(wav)) && (r->advance(9600), r->audioSamples() == 4800u) && r->waterfallRows() > 50;   // 9600 @ 96 kHz -> 4800 audio samples
        std::remove(wav.c_str());
        {   // a recording at an odd sample rate (62.5 kHz) is resampled to 48 kHz automatically
            std::vector<float> odd; for (auto& v : fireside::dsp::synth_tones(62500, 62500, {{3000, 0.3}}, 0.001)) { odd.push_back(v.real()); odd.push_back(v.imag()); }
            const std::string w2 = "/tmp/fireside_smoke62_" + std::to_string(::getpid()) + ".wav"; fireside::dsp::write_wav_f32(w2, 62500, 2, odd);
            const bool oddOk = r->loadFile(QString::fromStdString(w2)) && (r->advance(4800), r->audioSamples() == 4800u) && QString::fromStdString(r->engine().status().source).contains("resampled to 48000 Hz");
            std::remove(w2.c_str()); fileOk = fileOk && oddOk; if (!oddOk) std::printf("(62.5 kHz recording failed: %s)\n", r->status().toStdString().c_str());
        }
        {   // tuning inside a recording: a tone 12 kHz above the centre moves to 7 kHz when the dial goes up 5 kHz (a radio would move its DDS; a file needs the receiver to do it); with Mirror I/Q the signs flip
            std::vector<float> t; for (auto& v : fireside::dsp::synth_tones(48000, 48000, {{12000, 0.3}}, 0.001)) { t.push_back(v.real()); t.push_back(v.imag()); }
            const std::string w3 = "/tmp/fireside_smoketune_" + std::to_string(::getpid()) + ".wav"; fireside::dsp::write_wav_f32(w3, 48000, 2, t);
            bool ok = true; for (int mir = 0; mir < 2; ++mir) {
                r->mirrorBox()->setChecked(mir == 1); ok = r->loadFile(QString::fromStdString(w3)) && ok; const double d0 = r->dialHz(); for (int i = 0; i < 8; ++i) r->advance(4800); const double p0 = r->peakOffsetHz();
                r->setDialHz(d0 + 5000); for (int i = 0; i < 8; ++i) r->advance(4800); const double p1 = r->peakOffsetHz(); r->setDialHz(d0);
                const double s = mir ? -1.0 : 1.0; const bool good = std::fabs(p0 - s * 12000) < 150 && std::fabs(p1 - s * 7000) < 150; ok = ok && good; if (!good) std::printf("(tuning in a recording failed, mirror=%d: peak %.0f -> %.0f Hz, wanted %.0f -> %.0f)\n", mir, p0, p1, s * 12000, s * 7000);
            }
            std::remove(w3.c_str()); r->mirrorBox()->setChecked(false); fileOk = fileOk && ok;
        }
        bool liveOk = true;                                                                  // real ALSA capture path against the silent "null" PCM (skipped if the host has no null device)
        if (r->loadLive("null", 48000)) { for (int i = 0; i < 15; ++i) r->advance(4800); liveOk = r->status().contains("STREAM DEAD") && r->audioSamples() > 0; r->setPlaying(false); }
        else std::printf("(live null capture unavailable: %s)\n", r->status().toStdString().c_str());
        {   // recording: the replay buffer fills from a live stream, Grab saves I/Q + audio + notes, the I/Q file replays with the notes applied, Rec saves with a pre-roll
            QTemporaryDir td; auto* rc = r->recorder(); bool recOk = rc != nullptr && td.isValid(); std::string why;
            if (recOk) {
                rc->setFolder(td.path()); rc->setOptions(30, 10, 2, true, true);
                recOk = r->loadLive("null", 48000); if (!recOk) why = "no null capture";
                if (recOk) { for (int i = 0; i < 40; ++i) r->advance(4800); recOk = rc->available() && rc->iq().buffered_seconds() > 3.5 && rc->audio().buffered_seconds() > 3.5; if (!recOk) why = "replay buffers did not fill"; }
                if (recOk) { rc->grab(); for (int i = 0; i < 100 && (rc->iq().status().saving || rc->audio().status().saving); ++i) { QApplication::processEvents(); QThread::msleep(10); }
                    const QStringList wavs = QDir(td.path()).entryList({"*_iq.wav"}), auds = QDir(td.path()).entryList({"*_audio.wav"}), notes = QDir(td.path()).entryList({"*_iq.txt"});
                    recOk = wavs.size() == 1 && auds.size() == 1 && notes.size() == 1 && QFileInfo(td.path() + "/" + wavs[0]).size() > 44 + 8 * 40000; if (!recOk) why = "grab files";
                    if (recOk) { const auto n = ReceiverPanel::readSidecar(td.path() + "/" + wavs[0]); recOk = n.value("kind") == "iq" && n.value("rate_hz") == "48000" && n.contains("mirror") && n.contains("dial_hz") && n.value("raw_radio_iq") == "1"; if (!recOk) why = "sidecar"; }
                    if (recOk) { const bool m0 = r->mirrorOn(); { QSignalBlocker b(r->mirrorBox()); r->mirrorBox()->setChecked(!m0); }                          // a file opened later takes the notes' mirror, not the pill's
                        const auto n = ReceiverPanel::readSidecar(td.path() + "/" + wavs[0]); recOk = r->loadFile(td.path() + "/" + wavs[0]) && r->mirrorOn() == (n.value("mirror") == "1"); if (!recOk) why = "replay applies notes"; r->setPlaying(false); }
                }
                if (recOk) { r->loadLive("null", 48000); for (int i = 0; i < 20; ++i) r->advance(4800); rc->toggleRecord(); recOk = rc->recording(); for (int i = 0; i < 20; ++i) r->advance(4800); const QString txt = rc->recordingText(); rc->toggleRecord();
                    recOk = recOk && !rc->recording() && txt.startsWith("REC "); if (!recOk) why = "start/stop"; }
                if (recOk) { QThread::msleep(50); const QStringList all = QDir(td.path()).entryList({"*.wav"}); recOk = all.size() >= 4; if (!recOk) why = "recording files"; }
                if (recOk) {   // the notes carry the times to the millisecond, and a recording's seconds equal what is in the file (pre-roll + what was pushed)
                    const QStringList ns = QDir(td.path()).entryList({"*_iq.txt"}, QDir::Files, QDir::Time); int checked = 0;
                    for (const QString& n : ns) {
                        const QString wav = td.path() + "/" + n.left(n.size() - 4) + ".wav"; const auto m = ReceiverPanel::readSidecar(wav);
                        const QDateTime end = QDateTime::fromString(m.value("utc"), Qt::ISODateWithMs), start = QDateTime::fromString(m.value("start_utc"), Qt::ISODateWithMs); const double secs = m.value("seconds").toDouble();
                        const qint64 bytes = QFileInfo(wav).size() - 44; const bool ms = m.value("utc").contains('.') && m.value("start_utc").contains('.');
                        const bool timesOk = end.isValid() && start.isValid() && ms && std::abs(start.msecsTo(end) - std::llround(secs * 1000.0)) <= 1;
                        const bool sizeOk = std::abs(double(bytes) - secs * 48000.0 * 8.0) <= 500.0;                                    // 2 channels x 4 bytes per frame
                        if (!timesOk || !sizeOk) { recOk = false; why = std::string("notes times/seconds in ") + n.toStdString(); break; } ++checked;
                    }
                    if (recOk && checked < 2) { recOk = false; why = "expected the grab's and the recording's notes"; }
                }   // grab: 2, recording: 2 (I/Q + audio)
                if (recOk) { r->loadDemo(); recOk = !rc->available() && !r->findChild<QPushButton*>() == false; for (auto* b : r->findChildren<QPushButton*>()) if (b->text().startsWith("Grab")) recOk = recOk && !b->isEnabled(); if (!recOk) why = "buttons without a live stream"; }
            } else why = "no controller";
            if (!recOk) { std::printf("receiver smoke FAILED: recording (%s)\n", why.c_str()); return 1; }
        }
        {   // manual gain: only meaningful (and enabled) while the AGC is Off; the options carry it
            QSlider* g = nullptr; for (auto* sl : r->findChildren<QSlider*>()) if (sl->toolTip().startsWith("Fixed gain")) g = sl;
            bool gainOk = g != nullptr; if (gainOk) { r->setAgcIndex(2); gainOk = !g->isEnabled(); r->setAgcIndex(0); gainOk = gainOk && g->isEnabled() && g->value() == 60 && r->options().dc.manual_gain_db == 60.0 && r->options().dc.agc == fireside::dsp::AgcMode::Off; g->setValue(45); gainOk = gainOk && r->options().dc.manual_gain_db == 45.0; g->setValue(60); r->setAgcIndex(2); }
            if (!gainOk) { std::printf("receiver smoke FAILED: manual gain slider\n"); return 1; }
        }
        const bool directRefused = !r->loadLiveDirect(44100) && r->status().contains("Cannot start the direct stream");   // a bad rate is refused before any FireWire traffic
        if (!directRefused) std::printf("FAIL: direct stream with a bad rate was not refused cleanly: %s\n", r->status().toStdString().c_str());
        fileOk = fileOk && liveOk && directRefused;
        {   // audio output selector: PC sound by default; the radio needs a live card AND an explicit channel pair; the "null" PCM stands in for the radio
            r->loadDemo();                                                                                                    // a non-live source: the radio option is not available
            bool outOk = r->audioOutputTarget() == 0 && !r->setAudioOutput(1, 1, true) && r->audioOutputTarget() == 0;           // no live capture open: refused
            if (r->loadLive("null", 48000)) {
                outOk = outOk && !r->setAudioOutput(1, 0, true) && r->audioOutputTarget() == 0;                                  // no pair chosen: refused
                outOk = outOk && r->setAudioOutput(1, 2, true) && r->audioOutputTarget() == 1;                                   // ch 2/3
                for (int i = 0; i < 6; ++i) r->advance(4800);
                outOk = outOk && r->radioFramesWritten() > 0;
                outOk = outOk && r->setAudioOutput(0, 0, true) && r->audioOutputTarget() == 0;
                r->setAudioOutput(1, 1, true); r->setPlaying(false); r->loadLive("null", 48000); outOk = outOk && r->audioOutputTarget() == 0;   // reopening forgets the radio output
                r->setPlaying(false);
            } else std::printf("(null playback check skipped)\n");
            if (!outOk) { std::printf("receiver smoke FAILED: audio output selector\n"); return 1; }
        }
        if (!(rebuilt && badRejected && fileOk)) { std::printf("receiver smoke FAILED: rebuilt=%d badRejected=%d fileOk=%d\n", rebuilt, badRejected, fileOk); return 1; }
        std::printf("%s: rows=%zu peak=%.0f Hz signal=%.1f dBFS audio=%zu colours=%d signal card width %d..%d (%zu texts)\n", ok ? "receiver smoke ok" : "receiver smoke FAILED",
                    r->waterfallRows(), r->peakOffsetHz(), r->signalDbfs(), r->audioSamples(), int(colours.size()), cardMin, cardMax, texts.size());
        return ok ? 0 : 1;
    }
    // normal run: fireside [--simulate] [--demo] [--screen DP-1]
    QString screenName; bool demo = false;
    for (int i = 1; i < argc; ++i) { if (!std::strcmp(argv[i], "--demo")) demo = true; else if (!std::strcmp(argv[i], "--screen") && i + 1 < argc) screenName = argv[++i]; }
    if (demo) { w.receiver()->setMuted(true); w.receiver()->loadDemo(); }          // the demo starts muted: nothing plays until you un-mute
    QScreen* target = nullptr; for (QScreen* sc : QGuiApplication::screens()) if (sc->name() == screenName) target = sc;
    if (!screenName.isEmpty() && !target) { std::fprintf(stderr, "no screen named '%s'; available:", screenName.toStdString().c_str()); for (QScreen* sc : QGuiApplication::screens()) std::fprintf(stderr, " %s", sc->name().toStdString().c_str()); std::fprintf(stderr, "\n"); }
    if (target) {                                                                  // size to the screen (but never below what the layout needs), centre on it
        const QRect g = target->availableGeometry(); QSize want(std::min(1700, g.width() - 40), std::min(1000, g.height() - 80));
        want = want.expandedTo(w.minimumSizeHint()); w.resize(want);
        w.move(g.center() - QPoint(want.width() / 2, want.height() / 2));
    }
    w.show();
    bool opening = demo; for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--open")) opening = true;
    if (!opening) QTimer::singleShot(600, &w, [&w] { w.radio()->autoStart(); });              // start receiving by itself (Radio tab box; never with --simulate, --demo or --open)
    { QString open; int dec = 0; for (int i = 1; i + 1 < argc; ++i) { if (!std::strcmp(argv[i], "--open")) open = QString::fromUtf8(argv[i + 1]); if (!std::strcmp(argv[i], "--decoder")) dec = std::atoi(argv[i + 1]); }   // --open file.wav [--decoder N]: start with a recording playing (N: 1 CW, 2 PSK31, 3 RTTY, 4 FT8, 5 FT4)
      if (!open.isEmpty()) QTimer::singleShot(300, &w, [&w, open, dec] { auto* r = w.receiver(); if (r->loadFile(open)) { r->setPlaying(true); if (dec > 0) r->setDecoderIndex(dec); } }); }
    // Ctrl+C, a closed terminal, logout or `kill`: close the window as if by hand, so the radio is shut down and the stream registers are put back
    // (a killed process cannot do that, and the next start was refused, 2026-11-19). The handler only writes a byte; Qt does the rest.
    static int sig_fd[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sig_fd) == 0) {
        auto* sn = new QSocketNotifier(sig_fd[1], QSocketNotifier::Read, &app);
        QObject::connect(sn, &QSocketNotifier::activated, &app, [&w, sn] { char c; (void)!::read(sig_fd[1], &c, 1); sn->setEnabled(false); appLog("app", "signal received: closing cleanly"); w.close(); QApplication::quit(); });
        struct sigaction sa{}; sa.sa_handler = [](int) { const char c = 1; (void)!::write(sig_fd[0], &c, 1); }; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART;
        for (int sig : {SIGINT, SIGTERM, SIGHUP}) ::sigaction(sig, &sa, nullptr);
    }
    return app.exec();
}
