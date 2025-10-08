#pragma once
#include <QtCore/qglobal.h>
#ifdef Q_OS_WIN
#include <QImage>
#include <QString>

class WindowsVideoThumbnailer {
public:
    static QImage firstFrame(const QString& localFilePath);
};
#endif
