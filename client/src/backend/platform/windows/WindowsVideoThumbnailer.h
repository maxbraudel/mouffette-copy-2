#pragma once
#include <QtCore/qglobal.h>
#ifdef Q_OS_WIN
#include <QImage>
#include <QString>
#include <QSize>

class WindowsVideoThumbnailer {
public:
    static QSize videoDimensions(const QString& localFilePath);
    static QImage firstFrame(const QString& localFilePath);
};
#endif
