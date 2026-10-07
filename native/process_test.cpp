#include "process_test.h"
#include "childprocess.h"
#include "controller.h"
#include "conversion.h"
#include "platform.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc.h>
#endif
#endif

namespace
{
bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}
bool writeJson(const QString &path, const QJsonObject &object)
{
    return write(path, QJsonDocument(object).toJson(QJsonDocument::Compact));
}
QJsonObject readJson(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
bool waitUntil(const std::function<bool()> &condition, int timeout = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }
    return condition();
}
bool alive(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process)
        return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    if (::kill(pid_t(pid), 0) == -1)
        return errno == EPERM;
    // An orphan can remain a zombie until its new parent reaps it.
#ifdef Q_OS_MACOS
    proc_bsdinfo info{};
    if (proc_pidinfo(int(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info))
        return info.pbi_status != SZOMB;
#elif defined(Q_OS_LINUX)
    QFile stat(QString("/proc/%1/stat").arg(pid));
    if (stat.open(QIODevice::ReadOnly))
    {
        const auto bytes = stat.readAll();
        const auto end = bytes.lastIndexOf(')');
        if (end >= 0 && bytes.mid(end + 2, 1) == "Z")
            return false;
    }
#endif
    return true;
#endif
}
QString helperSpec(const QString &root, const QString &mode)
{
    const auto path = root + "/helper.json";
    if (!QDir().mkpath(root) ||
        !writeJson(path, {{"mode", mode}, {"ready", root + "/ready.json"}}))
        return {};
    return path;
}
struct Counts
{
    bool finished = false;
    int success = -1, failed = -1, cancelled = -1;
};
Counts batch(Controller &controller, bool cancel)
{
    Counts counts;
    QEventLoop loop;
    QObject::connect(&controller, &Controller::batchFinished, &loop, [&](int success, int failed, int cancelled) {
        counts = {true, success, failed, cancelled};
        loop.quit();
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(10000);
    controller.start();
    if (cancel)
        controller.cancel();
    if (!counts.finished)
        loop.exec();
    if (!counts.finished)
    {
        controller.cancel();
        waitUntil([&] { return !controller.busy(); });
    }
    return counts;
}
}

int runProcessTestHelper(const QString &specPath)
{
    if (!qEnvironmentVariableIsSet("ATHANOR_TEST"))
        return 1;
    const auto spec = readJson(specPath);
    const auto mode = spec.value("mode").toString();
    const auto ready = spec.value("ready").toString();
    if (ready.isEmpty())
        return 1;
    if (mode == "parent")
    {
        const auto childSpec = specPath + ".child";
        if (!writeJson(childSpec, {{"mode", "wait"}, {"ready", ready + ".child"}}))
            return 1;
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--process-test-helper", childSpec});
        if (!child.waitForStarted() || !waitUntil([&] { return QFileInfo::exists(ready + ".child"); }))
            return 1;
        if (!writeJson(ready, {{"pid", QCoreApplication::applicationPid()}, {"child", child.processId()}}))
            return 1;
        return child.waitForFinished(15000) ? 0 : 1;
    }
    if (mode != "wait")
        return 1;
    QJsonObject info{{"pid", QCoreApplication::applicationPid()}};
#ifndef Q_OS_WIN
    info["group"] = qint64(getpgrp());
#endif
    if (!writeJson(ready, info))
        return 1;
    QThread::msleep(15000);
    return 0;
}

int runProcessTest()
{
    qputenv("ATHANOR_TEST", "1");
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const auto root = temporary.path();
    qputenv("ATHANOR_SETTINGS_DIR", (root + "/settings").toUtf8());
    QStringList errors;
    auto check = [&](bool ok, const QString &message) {
        if (!ok)
            errors << message;
    };
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::red);

    // Real conversion workers: cancellation must beat publication and deletion.
    ChildProcess worker;
    Platform::setupProcess(&worker, true);
    for (int run = 0; run < 3; ++run)
    {
        const auto folder = root + QString("/immediate-%1").arg(run);
        const auto source = folder + "/source.png", scratch = folder + "/scratch", job = folder + "/job.json";
        check(QDir().mkpath(scratch) && image.save(source), "Cannot prepare cancellation fixture");
        Options options;
        options.image = "webp";
        options.deleteOriginal = true;
        check(writeJson(job, {{"source", source}, {"scratch", scratch}, {"options", options.json()}}),
              "Cannot write cancellation job");
        worker.start(QCoreApplication::applicationFilePath(), {"--worker", job});
        worker.kill();
        check(worker.waitForFinished(5000), "Immediately cancelled worker did not finish");
        check(worker.exitCode() != 0, "Immediately cancelled worker completed conversion");
        check(QFileInfo::exists(source), "Immediately cancelled worker deleted the original");
        check(QDir(folder).entryList({"*_athanor-compressed*"}, QDir::AllEntries).isEmpty(),
              "Immediately cancelled worker published output");
        worker.kill(); // A stopped object must never signal a stale PID.
    }

    // One root with a live descendant exercises group teardown and destruction.
    for (bool destroy : {false, true})
    {
        const auto folder = root + (destroy ? "/tree-destructor" : "/tree-kill");
        const auto spec = helperSpec(folder, "parent");
        qint64 childPid = 0;
        {
            ChildProcess tree;
            Platform::setupProcess(&tree, true);
            tree.start(QCoreApplication::applicationFilePath(), {"--process-test-helper", spec});
            check(tree.waitForStarted(5000), "Process tree did not start");
            check(waitUntil([&] { return QFileInfo::exists(folder + "/ready.json"); }),
                  "Process tree descendant did not start");
            childPid = readJson(folder + "/ready.json").value("child").toInteger();
            check(alive(childPid), "Process tree descendant was not alive before cancellation");
            if (!destroy)
            {
                tree.kill();
                check(tree.waitForFinished(5000), "Cancelled process tree did not finish");
            }
        }
        check(waitUntil([&] { return !alive(childPid); }), "Process tree descendant survived cancellation");
    }

    // Reuse must clear the previous group modifier and tolerate a failed start.
    ChildProcess reused;
    reused.start(root + "/missing-program", {});
    check(!reused.waitForStarted(1000), "Missing program unexpectedly started");
    reused.kill();
    for (bool group : {true, false})
    {
        const auto folder = root + (group ? "/reuse-group" : "/reuse-plain");
        const auto spec = helperSpec(folder, "wait");
        Platform::setupProcess(&reused, group);
        reused.start(QCoreApplication::applicationFilePath(), {"--process-test-helper", spec});
        check(reused.waitForStarted(5000), "Reused process did not start");
        check(waitUntil([&] { return QFileInfo::exists(folder + "/ready.json"); }), "Reused helper did not become ready");
#ifndef Q_OS_WIN
        const auto info = readJson(folder + "/ready.json");
        check(info.value("group").toInteger() == (group ? reused.processId() : qint64(getpgrp())),
              "Reused process retained the wrong session modifier");
#endif
        reused.kill();
        check(reused.waitForFinished(5000), "Reused process did not finish after cancellation");
    }

    const auto a = root + "/a.png", b = root + "/b.png";
    check(image.save(a) && image.save(b), "Cannot prepare PDF queue fixtures");
    for (bool cancel : {false, true})
    {
        Controller controller;
        controller.setOption("image", "pdf");
        controller.setOption("batch_workers", 1);
        controller.setOption("delete", true);
        if (!cancel)
        {
            const auto blocked = root + "/blocked";
            check(write(blocked, "file"), "Cannot prepare blocked destination");
            controller.setOption("output", blocked + "/unavailable");
        }
        controller.addPaths({a, b});
        check(waitUntil([&] { return controller.count() == 2; }), "PDF queue did not load both images");
        const auto counts = batch(controller, cancel);
        check(counts.finished && !controller.busy(), "PDF batch did not finish");
        check(counts.success == 0 && counts.failed == (cancel ? 0 : 2) && counts.cancelled == (cancel ? 2 : 0),
              "PDF batch counted group members incorrectly");
        for (const auto &row : controller.queue()->items)
            check(row.status == (cancel ? "Cancelled" : "Error"), "PDF group left a row in an incorrect state");
        check(QFileInfo::exists(a) && QFileInfo::exists(b), "Failed or cancelled PDF batch deleted an original");
    }

    QJsonArray list;
    for (const auto &error : errors)
        list.append(error);
    Conversion::writeLine({{"ok", errors.isEmpty()}, {"errors", list}});
    return errors.isEmpty() ? 0 : 1;
}
