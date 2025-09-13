#pragma once
#include <QtCore/qglobal.h>
#ifdef Q_OS_MACOS
#include <QImage>
#include <QString>

// Synchronous, fast first-frame fetch using AVFoundation.
// Returns a QImage; null image if failed.
class MacVideoThumbnailer {
public:
    static QImage firstFrame(const QString& localFilePath);
};
#endif
