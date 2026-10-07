#include "updater.h"
#include "platform.h"
#include "unixupdate.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QVersionNumber>
#include <QUuid>
#include <memory>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <climits>
#endif
namespace
{
bool processAlive(qint64 pid)
{
    if (pid <= 0)
        return true; // Invalid or unknown ownership must never authorize deletion.
#ifdef Q_OS_WIN
    if (pid > MAXDWORD)
        return true;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process)
        return GetLastError() != ERROR_INVALID_PARAMETER;
    const bool alive = WaitForSingleObject(process, 0) != WAIT_OBJECT_0;
    CloseHandle(process);
    return alive;
#else
    if (pid > INT_MAX)
        return true;
    return ::kill(pid_t(pid), 0) == 0 || errno != ESRCH;
#endif
}
bool writeOwners(const QString &archive, const QList<qint64> &pids, bool handoffPending = false)
{
    QJsonArray owners;
    for (auto pid : pids)
        owners.append(pid);
    QSaveFile file(archive + ".owner.json");
    const auto data = QJsonDocument(QJsonObject{{"pids", owners}, {"handoff_pending", handoffPending}}).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}
void removeDownload(const QString &archive)
{
    QFile::remove(archive);
    QFile::remove(archive + ".owner.json");
}
void cleanAbandonedDownloads(const QString &folder)
{
    QDir downloads(folder);
    if (!downloads.exists())
        return;
    QLockFile lock(downloads.filePath("cleanup.lock"));
    if (!lock.tryLock(0))
        return;
    for (const auto &name : downloads.entryList({"update-*.owner.json"}, QDir::Files))
    {
        if (!QRegularExpression("^update-[0-9a-f]{32}\\.(?:exe|zip|tar\\.gz)\\.owner\\.json$").match(name).hasMatch())
            continue;
        QFile file(downloads.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QJsonParseError error;
        const auto manifest = QJsonDocument::fromJson(file.readAll(), &error).object();
        const auto owners = manifest.value("pids").toArray();
        if (error.error || owners.isEmpty() || manifest.value("handoff_pending").toBool())
            continue;
        bool active = false;
        for (auto owner : owners)
            active |= !owner.isDouble() || processAlive(owner.toInteger());
        file.close();
        if (!active)
            removeDownload(downloads.filePath(name.chopped(QString(".owner.json").size())));
    }
}
}
static const QString releaseApi = "https://api.github.com/repos/SixFawn253/Athanor/releases/latest";
static bool trustedAsset(const QUrl &url)
{
    return url.scheme() == "https" && url.host() == "github.com" && url.userInfo().isEmpty() &&
           url.path().startsWith("/SixFawn253/Athanor/releases/download/");
}
static QNetworkRequest request(const QUrl &url)
{
    QNetworkRequest r(url);
    r.setRawHeader("User-Agent", "Athanor/" + QCoreApplication::applicationVersion().toUtf8());
    r.setRawHeader("Accept", "application/vnd.github+json");
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    r.setTransferTimeout(60000);
    return r;
}
Updater::Updater(QObject *owner) : QObject(owner), controller(owner)
{
    auto folder = Platform::settingsDirectory();
    preferences = folder + "/updates.ini";
    QDir().mkpath(folder);
    cleanAbandonedDownloads(folder + "/updates");
    autoCheck = QSettings(preferences, QSettings::IniFormat).value("automatic", true).toBool();
    message = "Athanor " + QCoreApplication::applicationVersion();
    QFile updateError(folder + "/update-error.txt");
    if (updateError.open(QIODevice::ReadOnly))
    {
        message = QString::fromUtf8(updateError.readAll());
        updateError.close();
        updateError.remove();
    }
    timer.setInterval(6 * 60 * 60 * 1000);
    connect(&timer, &QTimer::timeout, this, [this] {
        if (autoCheck)
            check();
    });
    if (!qEnvironmentVariableIsSet("ATHANOR_TEST"))
    {
        timer.start();
        if (autoCheck)
            QTimer::singleShot(2500, this, &Updater::check);
    }
}
bool Updater::canInstall() const
{
#ifdef Q_OS_WIN
    return !pending && !asset.isEmpty() && !qEnvironmentVariable("ATHANOR_LAUNCHER_PATH").isEmpty();
#else
    return !pending && !asset.isEmpty() && UnixUpdate::canInstall();
#endif
}
void Updater::setAutomatic(bool value)
{
    autoCheck = value;
    QSettings(preferences, QSettings::IniFormat).setValue("automatic", value);
    emit changed();
}
void Updater::check()
{
    if (pending)
        return;
    pending = true;
    percent = 0;
    message = "Checking GitHub…";
    emit changed();
    QUrl endpoint(releaseApi);
    if (qEnvironmentVariableIsSet("ATHANOR_TEST") && qEnvironmentVariableIsSet("ATHANOR_TEST_UPDATE_API"))
        endpoint = QUrl(qEnvironmentVariable("ATHANOR_TEST_UPDATE_API"));
    auto reply = network.get(request(endpoint));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        pending = false;
        asset = {};
        version.clear();
        auto bytes = reply->readAll();
        auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError)
            message = code == 403 || code == 429 ? "GitHub’s request limit was reached. Try again later."
                                                 : "Couldn’t check for updates. Try again when connected.";
        else
        {
            QJsonParseError error;
            auto release = QJsonDocument::fromJson(bytes, &error).object();
            QString tag = release.value("tag_name").toString();
            QRegularExpression pattern("^(?:alpha-|v)?(\\d+\\.\\d+(?:\\.\\d+)?)$");
            auto match = pattern.match(tag);
            if (error.error || !match.hasMatch() || release.value("draft").toBool() ||
                release.value("prerelease").toBool())
                message = "GitHub returned an unsupported release.";
            else if (QVersionNumber::fromString(match.captured(1)) <=
                     QVersionNumber::fromString(QCoreApplication::applicationVersion()))
            {
                asset = {};
                version.clear();
                message = "Athanor is up to date.";
            }
            else
            {
                asset = {};
                version.clear();
                QString name = Platform::releaseAsset(match.captured(1));
                for (auto value : release.value("assets").toArray())
                {
                    auto a = value.toObject();
                    if (a.value("name").toString() == name && a.value("state").toString() == "uploaded" &&
                        a.value("size").toInteger() > 0 && a.value("size").toInteger() < 2000000000 &&
                        QRegularExpression("^sha256:[0-9a-fA-F]{64}$").match(a.value("digest").toString()).hasMatch() &&
                        trustedAsset(QUrl(a.value("browser_download_url").toString())))
                    {
                        asset = a;
                        version = release.value("name").toString(tag);
                        break;
                    }
                }
                message =
                    asset.isEmpty() ? "This release has no verified " + Platform::name() + " download for this architecture yet." : version + " is available.";
                if (!asset.isEmpty() && !canInstall())
                    message += " Use a writable portable installation to install updates.";
            }
        }
        reply->deleteLater();
        emit changed();
    });
}
void Updater::install()
{
    if (!canInstall())
        return;
    if (controller->property("busy").toBool() || controller->property("previewBusy").toBool())
    {
        message = "Finish the current conversion or preview before updating.";
        emit changed();
        return;
    }
    QString folder = QFileInfo(preferences).absolutePath() + "/updates";
    if (!QDir().mkpath(folder))
    {
        message = "The update folder is not writable.";
        emit changed();
        return;
    }
    const auto suffix = Platform::releaseAsset("0").endsWith(".tar.gz") ? ".tar.gz"
                        : Platform::releaseAsset("0").endsWith(".zip") ? ".zip" : ".exe";
    auto file = std::make_shared<QFile>(folder + "/update-" + QUuid::createUuid().toString(QUuid::Id128) + suffix);
    QLockFile lock(folder + "/cleanup.lock");
    if (!lock.tryLock(1000) || !writeOwners(file->fileName(), {QCoreApplication::applicationPid()}) ||
        !file->open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        removeDownload(file->fileName());
        message = "Cannot save the update. Check free disk space.";
        emit changed();
        return;
    }
    lock.unlock();
    pending = true;
    percent = 0;
    message = "Downloading " + version + "…";
    emit changed();
    QUrl download(asset.value("browser_download_url").toString());
    if (qEnvironmentVariableIsSet("ATHANOR_TEST") && qEnvironmentVariableIsSet("ATHANOR_TEST_UPDATE_DOWNLOAD"))
        download = QUrl(qEnvironmentVariable("ATHANOR_TEST_UPDATE_DOWNLOAD"));
    auto reply = network.get(request(download));
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto count = std::make_shared<qint64>(0);
    qint64 expected = asset.value("size").toInteger();
    auto consume = [reply, file, hash, count, expected] {
        auto bytes = reply->readAll();
        *count += bytes.size();
        if (*count > expected || file->write(bytes) != bytes.size())
        {
            reply->abort();
            return;
        }
        hash->addData(bytes);
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::downloadProgress, this, [this, expected](qint64 done, qint64) {
        percent = int(qBound(qint64(0), done * 100 / expected, qint64(100)));
        emit changed();
    });
    QString digest = asset.value("digest").toString().mid(7).toLower();
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, hash, count, expected, digest, consume] {
        consume();
        bool flushed = file->flush();
        file->close();
        pending = false;
        bool valid = reply->error() == QNetworkReply::NoError && flushed && *count == expected &&
                     hash->result().toHex() == digest.toLatin1();
        reply->deleteLater();
        if (!valid)
        {
            removeDownload(file->fileName());
            message = "The update could not be verified. Your current app is unchanged.";
            emit changed();
            return;
        }
        if (controller->property("busy").toBool() || controller->property("previewBusy").toBool())
        {
            removeDownload(file->fileName());
            message = "Finish the current job, then download the update again.";
            emit changed();
            return;
        }
        qint64 helperPid = 0;
        // If recording the helper later fails, retain this conservative marker
        // even after the parent exits; a pending transfer is never garbage.
        if (!writeOwners(file->fileName(), {QCoreApplication::applicationPid()}, true))
        {
            removeDownload(file->fileName());
            message = "Could not prepare the update helper. Try again later.";
            emit changed();
            return;
        }
#ifdef Q_OS_WIN
        QString launcher = qEnvironmentVariable("ATHANOR_LAUNCHER_PATH");
        QStringList args{"--apply-update", launcher, QString::number(QCoreApplication::applicationPid()),
                         qEnvironmentVariable("ATHANOR_LAUNCHER_PID", "0"), digest};
        if (!QProcess::startDetached(file->fileName(), args, QFileInfo(launcher).absolutePath(), &helperPid))
        {
            removeDownload(file->fileName());
            message = "Couldn’t start the update. Your current app is unchanged.";
            emit changed();
            return;
        }
#else
        QStringList args{"--apply-update", file->fileName(), Platform::installRoot(),
                         QString::number(QCoreApplication::applicationPid()), digest};
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), args, QString(), &helperPid))
        {
            removeDownload(file->fileName());
            message = "Could not start the update helper.";
            emit changed();
            return;
        }
#endif
        // The parent stays alive until the helper is recorded, so another
        // instance always sees at least one live owner during the handoff.
        if (!writeOwners(file->fileName(), {QCoreApplication::applicationPid(), helperPid}))
        {
            // Retain the parent's lease. The helper will time out waiting for
            // this instance to close and leave the current installation intact.
            message = "Could not transfer the update download to its helper. Try again later.";
            emit changed();
            return;
        }
        message = "Restarting Athanor…";
        emit changed();
        QCoreApplication::quit();
    });
}
