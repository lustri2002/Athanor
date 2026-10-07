#include "unixupdate.h"
#include "platform.h"
#include "scratch.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QUuid>
#include <stdexcept>
#include <climits>
#ifndef Q_OS_WIN
#include <cerrno>
#include <signal.h>
#endif

namespace
{
constexpr auto readyFileVariable = "ATHANOR_UPDATE_READY_FILE";
constexpr auto readyTokenVariable = "ATHANOR_UPDATE_READY_TOKEN";
#ifndef Q_OS_WIN
bool processRunning(qint64 pid)
{
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
}
bool stopProcess(qint64 pid)
{
    if (!pid || !processRunning(pid))
        return true;
    for (const auto signal : {SIGTERM, SIGKILL})
    {
        ::kill(pid_t(pid), signal);
        QElapsedTimer timer;
        timer.start();
        while (processRunning(pid) && timer.elapsed() < 2000)
            QThread::msleep(25);
        if (!processRunning(pid))
            return true;
    }
    return false;
}
#endif
}
bool UnixUpdate::startupPending()
{
    return qEnvironmentVariableIsSet(readyFileVariable) || qEnvironmentVariableIsSet(readyTokenVariable);
}
bool UnixUpdate::acknowledgeStartup()
{
    if (!startupPending())
        return true;
    const auto path = qEnvironmentVariable(readyFileVariable);
    const auto token = qEnvironmentVariable(readyTokenVariable);
    qunsetenv(readyFileVariable);
    qunsetenv(readyTokenVariable);
    const QFileInfo file(path), parent(file.absolutePath());
    if (!QRegularExpression("^[0-9a-f]{32}$").match(token).hasMatch() ||
        file.fileName() != "startup-ready.txt" || file.isSymLink() || !parent.isDir() || parent.isSymLink() ||
        !parent.fileName().startsWith(".athanor-update-"))
        return false;
    QSaveFile ready(path);
    return ready.open(QIODevice::WriteOnly) && ready.write(token.toUtf8()) == token.size() && ready.commit();
}
bool UnixUpdate::canInstall()
{
    const auto root = Platform::installRoot();
    return !root.isEmpty() && !QFileInfo(root).isSymLink() && QFileInfo(QFileInfo(root).absolutePath()).isWritable();
}
int UnixUpdate::apply(const QStringList &args)
{
#ifdef Q_OS_WIN
    Q_UNUSED(args);
    return 1;
#else
    QString backup, target, relativeExecutable;
    qint64 parentPid = 0, restartedPid = 0;
    bool replaced = false;
    auto fail = [](const QString &message) { throw std::runtime_error(message.toUtf8().constData()); };
    try
    {
        // The helper is the currently installed binary, not an executable from
        // the unverified download. It must only update its own installation.
        if (args.size() != 6 || !canInstall())
            fail("Invalid or unwritable portable installation.");
        target = QFileInfo(Platform::installRoot()).canonicalFilePath();
        if (target.isEmpty() || target != QFileInfo(args[3]).canonicalFilePath())
            fail("Update target does not match this application.");
        QFile archive(args[2]);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!archive.open(QIODevice::ReadOnly) || !hash.addData(&archive) ||
            hash.result().toHex() != args[5].toLatin1())
            fail("The downloaded update could not be verified.");
        archive.close();
        bool validPid = false;
        const auto pid = args[4].toLongLong(&validPid);
        if (!validPid || pid <= 0 || pid > INT_MAX || pid == QCoreApplication::applicationPid())
            fail("Invalid update process.");
        parentPid = pid;
        Scratch staging(QFileInfo(target).absolutePath() + "/.athanor-update-XXXXXX");
        if (!staging.isValid())
            fail("Cannot create update workspace.");
#ifdef Q_OS_MACOS
        const QString bundle = "Athanor.app";
        relativeExecutable = "Contents/MacOS/Athanor";
        const QString listingTool = "/usr/bin/unzip";
        const QStringList listingArgs{"-Z", "-1", archive.fileName()};
        const QString extractTool = "/usr/bin/ditto";
        const QStringList extractArgs{"-x", "-k", archive.fileName(), staging.path()};
#else
        const QString bundle = "Athanor";
        relativeExecutable = "usr/bin/Athanor";
        const QString listingTool = "/bin/tar";
        const QStringList listingArgs{"-tzf", archive.fileName()};
        const QString extractTool = "/bin/tar";
        const QStringList extractArgs{"-xzf", archive.fileName(), "-C", staging.path(), "--no-same-owner", "--no-same-permissions"};
#endif
        auto run = [&](const QString &program, const QStringList &arguments, bool checkGui = false) {
            QProcess process;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.remove(readyFileVariable);
            environment.remove(readyTokenVariable);
            if (checkGui)
            {
                environment.insert("ATHANOR_TEST", "1");
                environment.insert("ATHANOR_SETTINGS_DIR", staging.path() + "/preflight-settings");
            }
            process.setProcessEnvironment(environment);
            process.start(program, arguments);
            if (!process.waitForStarted() || !process.waitForFinished(60000) || process.exitCode() ||
                process.exitStatus() != QProcess::NormalExit)
                fail("Could not unpack or start the update: " + QString::fromUtf8(process.readAllStandardError()));
            return process.readAllStandardOutput();
        };
        const auto listing = run(listingTool, listingArgs);
        if (listing.isEmpty())
            fail("The update archive is empty.");
        for (auto name : QString::fromUtf8(listing).split('\n', Qt::SkipEmptyParts))
        {
            while (name.startsWith("./"))
                name.remove(0, 2);
            if (name.contains('\\') || name.split('/').contains("..") ||
                (name != bundle && !name.startsWith(bundle + '/')))
                fail("The update archive contains an unexpected path.");
        }
        run(extractTool, extractArgs);
        const auto replacement = staging.path() + '/' + bundle;
        const auto executable = replacement + '/' + relativeExecutable;
        QDirIterator entries(replacement, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
        while (entries.hasNext())
        {
            const auto path = entries.next();
            const auto canonical = QFileInfo(path).canonicalFilePath();
            if (canonical.isEmpty() || !canonical.startsWith(replacement + '/'))
                fail("The update contains an external or broken link.");
        }
        if (!QFileInfo(executable).isExecutable())
            fail("The update is missing its application executable.");
        // Construct the GUI before touching the installation. CLI startup alone
        // does not load the QML imports or controls used by the application.
        run(executable, {"--qml-check"}, true);
        QElapsedTimer timer;
        timer.start();
        while (processRunning(pid))
        {
            if (timer.elapsed() >= 120000)
                fail("Close all Athanor windows and try the update again.");
            QThread::msleep(50);
        }
        backup = target + ".athanor-backup-" + QFileInfo(staging.path()).fileName();
        if (QFileInfo::exists(backup) || !QDir().rename(target, backup))
            fail("Cannot save an application backup.");
        if (!QDir().rename(replacement, target))
            fail("Cannot publish the new application.");
        replaced = true;
        const auto readyPath = staging.path() + "/startup-ready.txt";
        const auto readyToken = QUuid::createUuid().toString(QUuid::Id128);
        QProcess restart;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(readyFileVariable, readyPath);
        environment.insert(readyTokenVariable, readyToken);
        restart.setProcessEnvironment(environment);
        restart.setProgram(target + '/' + relativeExecutable);
        restart.setArguments(qEnvironmentVariableIsSet("ATHANOR_TEST") ? QStringList{"--qml-check"} : QStringList{});
        restart.setWorkingDirectory(QFileInfo(target).absolutePath());
        if (!restart.startDetached(&restartedPid))
            fail("Could not restart Athanor.");
        int readyTimeout = 20000;
        if (qEnvironmentVariableIsSet("ATHANOR_TEST"))
        {
            bool valid = false;
            const auto timeout = qEnvironmentVariableIntValue("ATHANOR_TEST_UPDATE_READY_TIMEOUT_MS", &valid);
            if (valid)
                readyTimeout = qBound(250, timeout, readyTimeout);
        }
        timer.restart();
        while (true)
        {
            QFile ready(readyPath);
            if (ready.open(QIODevice::ReadOnly) && ready.readAll() == readyToken.toUtf8())
                break;
            if (!processRunning(restartedPid))
                fail("The updated app closed before its first frame. Your previous app will be restored.");
            if (timer.elapsed() >= readyTimeout)
                fail("The updated app did not confirm startup. Your previous app will be restored.");
            QThread::msleep(25);
        }
        QDir(backup).removeRecursively();
        if (QFile::remove(archive.fileName()))
            QFile::remove(archive.fileName() + ".owner.json");
        return 0;
    }
    catch (const std::exception &e)
    {
        QString message = QString::fromUtf8(e.what());
        bool restored = !replaced && QFileInfo::exists(target);
        if (!backup.isEmpty() && QFileInfo::exists(backup))
        {
            if (!stopProcess(restartedPid))
                message += " Could not stop the updated app; the previous app remains at " + backup + '.';
            else if ((replaced && !QDir(target).removeRecursively()) || !QDir().rename(backup, target))
                message += " Could not restore the previous app; its backup remains at " + backup + '.';
            else
                restored = true;
        }
        QDir().mkpath(Platform::settingsDirectory());
        QSaveFile error(Platform::settingsDirectory() + "/update-error.txt");
        if (error.open(QIODevice::WriteOnly))
        {
            error.write(message.toUtf8());
            error.commit();
        }
        if (restored && parentPid && !processRunning(parentPid) && !relativeExecutable.isEmpty())
        {
            QProcess restart;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.remove(readyFileVariable);
            environment.remove(readyTokenVariable);
            restart.setProcessEnvironment(environment);
            restart.setProgram(target + '/' + relativeExecutable);
            restart.setArguments(qEnvironmentVariableIsSet("ATHANOR_TEST") ? QStringList{"--qml-check"} : QStringList{});
            restart.setWorkingDirectory(QFileInfo(target).absolutePath());
            if (!restart.startDetached())
                message += " Could not restart the restored app; open Athanor again.";
        }
        Platform::print(message.toUtf8() + '\n');
        return 1;
    }
#endif
}
