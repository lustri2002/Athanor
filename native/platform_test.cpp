#include "platform.h"
#include "desktopintegration.h"
#include "conversion.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QJsonArray>
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
    }
    check(count >= 21, "Missing file manager conversion actions");
#ifdef Q_OS_MACOS
    const auto file = root + "/foto à ' test.png";
    write(file, "test");
    QProcess automator;
    automator.start("/usr/bin/automator", {"-i", file, root + "/actions/Athanor - Images - Convert to PNG.workflow"});
    check(automator.waitForFinished(30000) && automator.exitCode() == 0 &&
          automator.readAllStandardOutput().contains(file.toUtf8()), "Finder workflow did not pass selected path intact: " + QString::fromUtf8(automator.readAllStandardError()));
#endif
    check(DesktopIntegration::configure(false, "/bin/echo", &error), "Action removal failed: " + error);
    QDirIterator remaining(root + "/actions", QDir::Files, QDirIterator::Subdirectories);
    check(!remaining.hasNext(), "Disabling integration left generated actions");
#endif
    QJsonArray list;
    for (const auto &error : errors)
        list.append(error);
    Conversion::writeLine({{"ok", errors.isEmpty()}, {"errors", list}});
    return errors.isEmpty() ? 0 : 1;
}
