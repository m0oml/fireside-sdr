#pragma once
// One line per event to ~/.local/share/fireside/app.log (UTC time stamp): radio / session / stream messages and receiver notes, so what
// happened can be read back afterwards. Never with FIRESIDE_NO_SETTINGS (tests).
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>
inline void appLog(const QString& what, const QString& line) {
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS") || line.trimmed().isEmpty()) return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/fireside"; QDir().mkpath(dir);
    QFile f(dir + "/app.log"); if (f.open(QIODevice::Append | QIODevice::Text)) f.write((QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss.zzz ") + what + ": " + line + "\n").toUtf8());
}
