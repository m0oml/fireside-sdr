#include "QrzClient.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
using namespace fireside;

namespace {
constexpr int kQueueMax = 60;
bool no_settings() { return qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS"); }
int64_t now_utc() { return QDateTime::currentSecsSinceEpoch(); }
}  // namespace

QrzClient::QrzClient(QObject* parent) : QObject(parent), net_(new QNetworkAccessManager(this)), gap_(new QTimer(this)) {
    gap_->setSingleShot(true); gap_->setInterval(1000);                    // at most one request a second
    connect(gap_, &QTimer::timeout, this, [this] { busy_ = false; pump(); });
    if (!no_settings()) { setCachePath(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/fireside/qrz-cache.tsv"); loadCredentials(); }
    setStatus(configured() ? "QRZ: not logged in yet" : "QRZ: not configured");
}
QString QrzClient::envPath() { return QDir::homePath() + "/.config/fireside/qrz.env"; }
void QrzClient::loadCredentials() {
    if (no_settings()) return;
    QFile f(envPath()); if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const auto m = qrz::parse_env(f.readAll().toStdString());
    auto get = [&](const char* k) { auto it = m.find(k); return it == m.end() ? QString() : QString::fromStdString(it->second); };
    user_ = get("QRZ_USER"); password_ = get("QRZ_PASSWORD");
}
bool QrzClient::saveCredentials() const {
    if (no_settings()) return false;
    QDir().mkpath(QFileInfo(envPath()).absolutePath()); QFile f(envPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);    // before the secret goes in
    f.write(("QRZ_USER=" + user_ + "\nQRZ_PASSWORD=" + password_ + "\n").toUtf8()); return true;
}
void QrzClient::setCredentials(const QString& user, const QString& password) {
    if (user.trimmed() == user_ && password == password_) return;
    user_ = user.trimmed(); password_ = password; key_.clear(); login_failed_ = false;
    setStatus(configured() ? "QRZ: not logged in yet" : "QRZ: not configured");
}
void QrzClient::setCachePath(const QString& path) {
    cache_path_ = path; cache_.clear();
    QFile f(path); if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const int64_t now = now_utc();
    while (!f.atEnd()) { QByteArray l = f.readLine(); while (l.endsWith('\n') || l.endsWith('\r')) l.chop(1); const auto i = qrz::parse_cache_line(l.toStdString()); /* not trimmed(): empty last fields end in a tab */ if (i && qrz::fresh(*i, now)) cache_.insert(QString::fromStdString(i->call), *i); }
}
void QrzClient::store(const qrz::Info& i) {
    cache_.insert(QString::fromStdString(i.call), i);
    if (cache_path_.isEmpty()) return;
    QDir().mkpath(QFileInfo(cache_path_).absolutePath()); QFile f(cache_path_);
    if (f.open(QIODevice::Append | QIODevice::Text)) f.write((QString::fromStdString(qrz::cache_line(i)) + "\n").toUtf8());   // newest line wins on reload
}
const qrz::Info* QrzClient::cached(const QString& call) const {
    const QString b = QString::fromStdString(qrz::base_call(call.toStdString())); auto it = cache_.constFind(b);
    return it != cache_.constEnd() && qrz::fresh(*it, now_utc()) ? &*it : nullptr;
}
void QrzClient::wantCached(const QString& decoded) {
    const QString b = QString::fromStdString(qrz::base_call(decoded.toStdString())); if (b.isEmpty()) return;
    if (const auto* i = cached(b)) emit infoReady(b, *i);
}
void QrzClient::want(const QString& decoded, bool urgent) {
    const QString b = QString::fromStdString(qrz::base_call(decoded.toStdString())); if (b.isEmpty()) return;
    if (const auto* i = cached(b)) { emit infoReady(b, *i); return; }
    if (!configured() || login_failed_ || (!urgent && !every_)) return;
    const int at = queue_.indexOf(b);
    if (urgent) { if (at >= 0) queue_.removeAt(at); queue_.prepend(b); }
    else if (at < 0) { queue_.append(b); while (queue_.size() > kQueueMax) queue_.removeLast(); }
    pump();
}
void QrzClient::testLogin() { key_.clear(); login_failed_ = false; if (!configured()) { setStatus("QRZ: enter the username and password"); return; } if (!busy_) login(false); }
void QrzClient::setStatus(const QString& s) { status_ = s; emit statusChanged(s); }
QString QrzClient::countText() const { return qrz_count_ >= 0 ? QString(", %1 lookups in 24 h on QRZ's count").arg(qrz_count_) : QString(); }

void QrzClient::pump() {
    if (busy_ || queue_.isEmpty() || !configured() || login_failed_) return;
    if (key_.isEmpty()) { login(true); return; }
    lookup(queue_.takeFirst());
}
void QrzClient::login(bool then_pump) {
    busy_ = true;
    QNetworkRequest rq(QUrl(base_ + QString::fromStdString(qrz::login_query(user_.toStdString(), password_.toStdString()))));
    rq.setTransferTimeout(15000);
    auto* r = net_->get(rq);
    connect(r, &QNetworkReply::finished, this, [this, r, then_pump] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) { setStatus("QRZ: cannot reach the server (" + r->errorString() + ")"); QTimer::singleShot(10000, this, [this] { busy_ = false; pump(); }); return; }   // try again in 10 s
        const auto p = qrz::parse_reply(r->readAll().toStdString());
        if (p.key.empty()) { login_failed_ = true; queue_.clear(); busy_ = false; setStatus("QRZ login refused: " + QString::fromStdString(p.error.empty() ? "no session key in the reply" : p.error)); return; }
        key_ = QString::fromStdString(p.key); qrz_count_ = p.count;
        const QString sub = QString::fromStdString(p.sub_exp);
        setStatus(sub == "non-subscriber" ? "QRZ: logged in as " + user_ + ", but NOT a subscriber (names and towns only for some)" + countText()
                                          : "QRZ: logged in as " + user_ + (sub.isEmpty() ? QString() : ", subscription to " + sub) + countText());
        if (then_pump) { busy_ = false; pump(); } else gap_->start();
    });
}
void QrzClient::lookup(const QString& call) {
    busy_ = true; ++lookups_;
    QNetworkRequest rq(QUrl(base_ + QString::fromStdString(qrz::lookup_query(key_.toStdString(), call.toStdString())))); rq.setTransferTimeout(15000);
    auto* r = net_->get(rq);
    connect(r, &QNetworkReply::finished, this, [this, r, call] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) { setStatus("QRZ: lookup failed (" + r->errorString() + ")"); queue_.prepend(call); QTimer::singleShot(10000, this, [this] { busy_ = false; pump(); }); return; }
        const auto p = qrz::parse_reply(r->readAll().toStdString()); if (p.count >= 0) qrz_count_ = p.count;
        if (p.session_expired()) { key_.clear(); queue_.prepend(call); busy_ = false; pump(); return; }   // log in again and retry this call
        if (p.info) { auto i = *p.info; i.fetched_utc = now_utc(); i.call = call.toStdString(); store(i); emit infoReady(call, i); }
        else if (p.not_found()) { qrz::Info i; i.call = call.toStdString(); i.not_found = true; i.fetched_utc = now_utc(); store(i); emit infoReady(call, i); }
        else if (!p.error.empty()) setStatus("QRZ: " + QString::fromStdString(p.error));
        if (p.info || p.not_found()) setStatus("QRZ: logged in as " + user_ + QString(", %1 looked up this session").arg(lookups_) + countText());
        gap_->start();
    });
}
