#pragma once
#include "conversion.h"
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

namespace InputSelection
{
inline bool applyTarget(Options &options, const QString &target)
{
    if (target == "auto")
        return true;
    if (QStringList{"avif", "webp", "heic", "heif", "jpg", "png", "ico"}.contains(target))
        options.image = target;
    else if (QStringList{"webm", "mkv", "av1", "mp4", "gif"}.contains(target))
        options.video = target;
    else if (QStringList{"opus", "mp3", "wav"}.contains(target))
        options.audio = options.video = target;
    else if (target == "images-pdf")
        options.image = "pdf";
    else if (target == "pdf-jpg" || target == "pdf")
        options.pdfOutput = target == "pdf-jpg" ? "jpg" : "pdf";
    else
        return false;
    return true;
}

inline QStringList paths(const QStringList &inputs, bool recursive, bool filterExplicit = true)
{
    QStringList result;
    QSet<QString> seen;
    auto append = [&](const QString &path, bool filter) {
        QFileInfo info(path);
        if (filter && (!info.isFile() || Conversion::kind(path).isEmpty()))
            return;
        const auto absolute = info.absoluteFilePath();
        auto key = QDir::cleanPath(absolute);
#ifdef Q_OS_WIN
        key = key.toLower();
#endif
        if (!seen.contains(key))
        {
            seen.insert(key);
            result.append(absolute);
        }
    };
    for (const auto &input : inputs)
        if (QFileInfo(input).isDir())
        {
            QDirIterator iterator(input, QDir::Files | QDir::NoDotAndDotDot,
                                  recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
            while (iterator.hasNext())
                append(iterator.next(), true);
        }
        else
            append(input, filterExplicit);
    return result;
}
}
