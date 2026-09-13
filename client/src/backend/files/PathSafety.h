#ifndef PATHSAFETY_H
#define PATHSAFETY_H

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace PathSafety {

inline Qt::CaseSensitivity fileSystemCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

inline QString normalizedAbsolutePath(const QString& path)
{
    if (path.isEmpty()) {
        return {};
    }
    return QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

inline bool samePath(const QString& first, const QString& second)
{
    return normalizedAbsolutePath(first).compare(
               normalizedAbsolutePath(second), fileSystemCaseSensitivity()) == 0;
}

// Lexically proves that candidate is below directory on the current platform.
// Both paths are made absolute and normalized first; the component boundary is
// always '/', which is Qt's canonical path separator even on Windows.
inline bool isDescendant(const QString& candidatePath,
                         const QString& directoryPath)
{
    const QString candidate = normalizedAbsolutePath(candidatePath);
    QString directory = normalizedAbsolutePath(directoryPath);
    if (candidate.isEmpty() || directory.isEmpty()
        || candidate.compare(directory, fileSystemCaseSensitivity()) == 0) {
        return false;
    }
    if (!directory.endsWith(QLatin1Char('/'))) {
        directory += QLatin1Char('/');
    }
    return candidate.startsWith(directory, fileSystemCaseSensitivity());
}

} // namespace PathSafety

#endif // PATHSAFETY_H
