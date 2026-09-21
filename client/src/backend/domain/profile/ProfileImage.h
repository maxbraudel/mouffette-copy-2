#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QUrl>

namespace ProfileImage {
inline constexpr qint64 MaximumImportBytes = 20 * 1024 * 1024;
inline constexpr qint64 MaximumImportPixels = 64 * 1000 * 1000;
inline constexpr int MaximumJpegBytes = 128 * 1024;
inline constexpr int Side = 250;

bool normalizeUsername(const QString& input, QString* normalized, QString* error = nullptr);
bool importFile(const QUrl& file, QByteArray* jpeg, QString* error = nullptr);
bool decodeJpeg(const QByteArray& jpeg, QImage* image = nullptr);
QString hash(const QByteArray& jpeg);
}
