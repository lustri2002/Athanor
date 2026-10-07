#include "platform.h"
#include "desktopintegration.h"
#include "conversion.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QJsonArray>
#include <QMap>
#include <QTemporaryDir>

int runPlatformTest()
{
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    QStringList errors;
    auto check = [&](bool ok, const QString &message) { if (!ok) errors << message; };
    auto write = [](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const auto root = QFileInfo(temporary.path()).canonicalFilePath();
    qputenv("ATHANOR_SETTINGS_DIR", (root + "/settings").toUtf8());
    check(Platform::settingsDirectory() == root + "/settings", "Settings override lost");
    check(QFileInfo::exists(Platform::assetPath("check.svg")), "Embedded icons missing");
    check(write(root + "/source", "new") && write(root + "/destination", "old"), "Cannot create publication fixtures");
    check(!Platform::publish(root + "/source", root + "/destination"), "Publication overwrote an existing file");
    check(QFileInfo::exists(root + "/source"), "Failed publication consumed source");
    check(Platform::publish(root + "/source", root + "/new-file"), "File publication failed");
    check(!QFileInfo::exists(root + "/source"), "Publication left staged source");
    QDir().mkpath(root + "/pages");
    write(root + "/pages/page-0001.jpg", "page");
    check(Platform::publish(root + "/pages", root + "/export"), "Directory publication failed");
    check(QFileInfo::exists(root + "/export/page-0001.jpg"), "Published PDF pages missing");
    QDir().mkpath(root + "/other-pages");
    check(!Platform::publish(root + "/other-pages", root + "/export"), "Directory collision overwrote results");
    const auto asset = Platform::releaseAsset("9.9");
    check(asset.contains(Platform::name()) && asset.startsWith("Athanor-Alpha-9.9-"), "Wrong platform release name");
#ifndef Q_OS_WIN
    qputenv("ATHANOR_INTEGRATION_DIR", (root + "/actions").toUtf8());
    QString error;
    check(DesktopIntegration::configure(true, "/bin/echo", &error), "Action installation failed: " + error);
    struct ActionSnapshot
    {
        QByteArray bytes;
        QDateTime modified;
    };
    QMap<QString, ActionSnapshot> installed;
    auto read = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    QDirIterator actions(root + "/actions", QDir::Files, QDirIterator::Subdirectories);
    int count = 0;
    while (actions.hasNext())
    {
        const auto path = actions.next();
        count++;
#ifdef Q_OS_MACOS
        QProcess lint;
        lint.start("/usr/bin/plutil", {"-lint", path});
        check(lint.waitForFinished() && lint.exitCode() == 0, "Invalid Finder workflow: " + path);
#else
        if (!path.endsWith(".desktop"))
        {
            QProcess lint;
            lint.start("/bin/bash", {"-n", path});
            check(lint.waitForFinished() && lint.exitCode() == 0, "Invalid file manager script: " + path);
        }
#endif
        QFile file(path);
        check(file.open(QIODevice::ReadOnly), "Cannot inspect installed action");
        const auto bytes = file.readAll();
        // A fixed old timestamp detects replacement even on coarse filesystems.
        check(file.setFileTime(QDateTime::fromMSecsSinceEpoch(946684800000LL), QFileDevice::FileModificationTime),
              "Cannot set action timestamp");
        file.close();
        installed.insert(path, {bytes, QFileInfo(path).lastModified()});
    }
    check(count >= 21, "Missing file manager conversion actions");
    check(DesktopIntegration::configure(true, "/bin/echo", &error), "Repeated action configuration failed: " + error);
    for (auto action = installed.cbegin(); action != installed.cend(); ++action)
    {
        check(read(action.key()) == action->bytes, "Repeated configuration changed action contents");
        check(QFileInfo(action.key()).lastModified() == action->modified,
              "Repeated configuration rewrote an unchanged action");
    }
#ifdef Q_OS_MACOS
    const auto file = root + "/foto à ' test.png";
    write(file, "test");
    QProcess automator;
    automator.start("/usr/bin/automator", {"-i", file, root + "/actions/Athanor - Images - Convert to PNG.workflow"});
    const bool done = automator.waitForFinished(30000);
    const auto stdoutBytes = automator.readAllStandardOutput();
    const auto stderrBytes = automator.readAllStandardError();
    check(done && automator.exitCode() == 0 && QString::fromUtf8(stdoutBytes).normalized(QString::NormalizationForm_C).contains(file.normalized(QString::NormalizationForm_C)),
          "Finder workflow did not pass selected path intact: " + QString::fromUtf8(stdoutBytes + stderrBytes));
    const auto gifWorkflow = root + "/actions/Athanor - GIF - Convert to WebM.workflow";
    QFile gifInfo(gifWorkflow + "/Contents/Info.plist");
    check(gifInfo.open(QIODevice::ReadOnly) && gifInfo.readAll().contains("com.compuserve.gif"),
          "Finder video targets are not registered for the GIF image UTI");
    const auto gif = root + "/animation.gif";
    write(gif, "test");
    automator.start("/usr/bin/automator", {"-i", gif, gifWorkflow});
    check(automator.waitForFinished(30000) && automator.exitCode() == 0 &&
          QString::fromUtf8(automator.readAllStandardOutput()).contains("--target webm -- " + gif),
          "Finder GIF action did not select the existing video conversion target");
#endif
    check(DesktopIntegration::configure(true, "/bin/true", &error), "Action executable update failed: " + error);
    int updated = 0;
    for (auto action = installed.cbegin(); action != installed.cend(); ++action)
    {
        const auto current = read(action.key());
        if (action->bytes.contains("/bin/echo"))
        {
            updated++;
            check(current.contains("/bin/true") && !current.contains("/bin/echo"),
                  "Action retained the previous executable");
            check(QFileInfo(action.key()).lastModified() != action->modified,
                  "Changed action retained its previous timestamp");
        }
        else
            check(current == action->bytes && QFileInfo(action.key()).lastModified() == action->modified,
                  "Executable update rewrote an unrelated action file");
    }
    check(updated > 0, "Executable update did not change any action");
#ifdef Q_OS_LINUX
    const auto launcher = root + "/actions/athanor/actions/Images-avif";
    const auto launcherBytes = read(launcher);
    const auto launcherTime = QFileInfo(launcher).lastModified();
    check(QFile::setPermissions(launcher, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::WriteOther),
          "Cannot change action permissions");
    check(DesktopIntegration::configure(true, "/bin/true", &error), "Action permission repair failed: " + error);
    const auto permissionMask = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    check((QFile::permissions(launcher) & permissionMask) == (QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
          "Action permissions were not repaired");
    check(read(launcher) == launcherBytes && QFileInfo(launcher).lastModified() == launcherTime,
          "Permission repair rewrote action contents");
#endif
    check(DesktopIntegration::configure(false, "/bin/true", &error), "Action removal failed: " + error);
    QDirIterator remaining(root + "/actions", QDir::Files, QDirIterator::Subdirectories);
    check(!remaining.hasNext(), "Disabling integration left generated actions");

    check(DesktopIntegration::configure(true, "/bin/echo", &error), "Cannot reinstall actions for ownership test");
    if (!installed.isEmpty())
    {
        const auto protectedPath = installed.firstKey();
        const QByteArray custom = "User-defined action\n";
        check(write(protectedPath, custom), "Cannot prepare user-owned action");
        check(!DesktopIntegration::configure(true, "/bin/true", &error), "Installation accepted a user-owned action");
        check(read(protectedPath) == custom, "Installation replaced a user-owned action");
        check(!DesktopIntegration::configure(false, "/bin/true", &error), "Removal accepted a user-owned action");
        check(read(protectedPath) == custom, "Removal deleted a user-owned action");
        QDirIterator preserved(root + "/actions", QDir::Files, QDirIterator::Subdirectories);
        int preservedCount = 0;
        while (preserved.hasNext())
        {
            check(preserved.next() == protectedPath, "Removal preserved an unrelated generated action");
            preservedCount++;
        }
        check(preservedCount == 1, "Removal did not preserve only the user-owned action");

        check(QFile::remove(protectedPath), "Cannot prepare symlink protection test");
        const auto target = root + "/external-action";
        const QByteArray targetBytes = "Generated by Athanor\nExternal action\n";
        check(write(target, targetBytes) && QFile::link(target, protectedPath), "Cannot prepare redirected action");
        check(!DesktopIntegration::configure(true, "/bin/echo", &error), "Installation accepted a redirected action");
        check(read(target) == targetBytes && QFileInfo(protectedPath).isSymLink(), "Installation changed a redirected action");
        check(!DesktopIntegration::configure(false, "/bin/echo", &error), "Removal accepted a redirected action");
        check(read(target) == targetBytes && QFileInfo(protectedPath).isSymLink(), "Removal changed a redirected action");
    }
#endif
    QJsonArray list;
    for (const auto &error : errors)
        list.append(error);
    Conversion::writeLine({{"ok", errors.isEmpty()}, {"errors", list}});
    return errors.isEmpty() ? 0 : 1;
}
