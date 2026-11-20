#pragma once
// QRZ.com XML callbook lookups (the user's subscription), straight from QRZ, not through Wavelog.
//   * credentials: entered on the Log tab, kept in ~/.config/fireside/qrz.env (mode 600, outside the repo, never in QSettings)
//   * cache: ~/.cache/fireside/qrz-cache.tsv (found 30 days, not found 3 days): repeats cost nothing
//   * one request per second at most; stations calling me jump the queue; the queue is capped (oldest low-priority calls dropped)
//   * QRZ's own 24-hour lookup count is shown, so the use is visible
// Read-only: it never writes to QRZ. E-mail / street address are never parsed or kept (fireside/qrz.h).
#include <QHash>
#include <QObject>
#include <QStringList>
#include "fireside/qrz.h"
class QNetworkAccessManager; class QTimer;

class QrzClient : public QObject {
    Q_OBJECT
public:
    explicit QrzClient(QObject* parent = nullptr);
    void setCredentials(const QString& user, const QString& password);   // forgets the session; does not save
    QString user() const { return user_; }
    bool configured() const { return !user_.isEmpty() && !password_.isEmpty(); }
    void setBaseUrl(const QString& url) { base_ = url; }                   // tests: a fake server
    void setCachePath(const QString& path);                                // tests; default ~/.cache/fireside/qrz-cache.tsv (loaded here)
    void setEveryDecode(bool on) { every_ = on; }                          // also look up every decoded station (low priority), not just callers / the entry
    bool everyDecode() const { return every_; }
    // Wants this station's record. Cached and fresh: infoReady at once (synchronously). Else queued: urgent = calling me / typed in the entry;
    // not urgent = calling CQ (only with "Also look up stations calling CQ").
    void want(const QString& decodedCall, bool urgent);
    void wantCached(const QString& decodedCall);                           // any other decode: shown only if already cached (no network)
    const fireside::qrz::Info* cached(const QString& call) const;              // fresh record for the base call, or nullptr
    void testLogin();                                                      // logs in now (the Log tab's button); answers through statusChanged
    QString status() const { return status_; }
    int lookups() const { return lookups_; }                               // network lookups this session
    int qrzCount() const { return qrz_count_; }                            // QRZ's own 24-hour count (-1 unknown)
    bool loggedIn() const { return !key_.isEmpty(); }
    int queued() const { return int(queue_.size()); }
    bool saveCredentials() const;                                          // writes qrz.env (0600); false on failure or FIRESIDE_NO_SETTINGS
    static QString envPath();
    void loadCredentials();                                                // from qrz.env (not with FIRESIDE_NO_SETTINGS)
signals:
    void infoReady(const QString& call, const fireside::qrz::Info& info);     // base call
    void statusChanged(const QString& text);
private:
    QNetworkAccessManager* net_; QTimer* gap_; QString base_ = fireside::qrz::kBaseUrl, user_, password_, key_, status_, cache_path_;
    QHash<QString, fireside::qrz::Info> cache_; QStringList queue_; bool busy_ = false, every_ = true, login_failed_ = false; int lookups_ = 0, qrz_count_ = -1;
    void pump(); void login(bool then_pump); void lookup(const QString& call); void setStatus(const QString& s); void store(const fireside::qrz::Info& i);
    QString countText() const;
};
