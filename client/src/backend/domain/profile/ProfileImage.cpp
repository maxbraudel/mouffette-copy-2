#include "backend/domain/profile/ProfileImage.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>

namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
}

bool ProfileImage::normalizeUsername(const QString& input, QString* normalized, QString* error)
{
    // Validate before trimming: control characters must not silently disappear.
    const auto points = input.toUcs4();
    for (char32_t point : points) {
        if (QChar::category(point) == QChar::Other_Control
            || QChar::category(point) == QChar::Other_Surrogate) {
            return fail(error, QStringLiteral("Username cannot contain control characters."));
        }
    }
    const QString value = input.trimmed();
    if (value.toUcs4().size() > 64)
        return fail(error, QStringLiteral("Username must contain at most 64 characters."));
    if (normalized) *normalized = value;
    return true;
}

bool ProfileImage::importFile(const QUrl& file, QByteArray* jpeg, QString* error)
{
    if (!jpeg || !file.isLocalFile())
        return fail(error, QStringLiteral("Choose a local PNG, JPEG or WebP image."));
    const QFileInfo info(file.toLocalFile());
    const QString suffix = info.suffix().toLower();
    if (suffix != QLatin1String("png") && suffix != QLatin1String("jpg")
        && suffix != QLatin1String("jpeg") && suffix != QLatin1String("webp"))
        return fail(error, QStringLiteral("Supported formats are PNG, JPG/JPEG and WebP."));
    QFile source(info.absoluteFilePath());
    if (!source.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Cannot read this image: %1").arg(source.errorString()));
    if (source.size() > MaximumImportBytes)
        return fail(error, QStringLiteral("The image must be no larger than 20 MiB."));
    QByteArray bytes = source.read(MaximumImportBytes + 1);
    if (source.error() != QFileDevice::NoError || bytes.size() > MaximumImportBytes)
        return fail(error, QStringLiteral("Cannot read this image, or it exceeds 20 MiB."));
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const QByteArray format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg" && format != "webp")
        return fail(error, QStringLiteral("The file is not a valid PNG, JPEG or WebP image."));
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > MaximumImportPixels)
        return fail(error, QStringLiteral("The image must contain no more than 64 megapixels."));
    reader.setAutoTransform(true);
    const QImage decoded = reader.read(); // First frame only, including animated WebP/PNG.
    if (decoded.isNull())
        return fail(error, QStringLiteral("Cannot decode this image: %1").arg(reader.errorString()));
    const int side = qMin(decoded.width(), decoded.height());
    const QImage square = decoded.copy((decoded.width() - side) / 2,
                                      (decoded.height() - side) / 2, side, side)
        .scaled(Side, Side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage flattened(Side, Side, QImage::Format_RGB32);
    flattened.fill(Qt::white);
    {
        QPainter painter(&flattened);
        painter.drawImage(0, 0, square);
    }
    QByteArray result;
    QBuffer output(&result);
    output.open(QIODevice::WriteOnly);
    if (!flattened.save(&output, "JPEG", 90) || result.size() > MaximumJpegBytes)
        return fail(error, QStringLiteral("Cannot create the 250 × 250 JPEG profile picture."));
    *jpeg = result;
    return true;
}

bool ProfileImage::decodeJpeg(const QByteArray& jpeg, QImage* image)
{
    if (jpeg.isEmpty() || jpeg.size() > MaximumJpegBytes) return false;
    QBuffer buffer;
    buffer.setData(jpeg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    if (reader.format().toLower() != "jpeg" || reader.size() != QSize(Side, Side)) return false;
    const QImage decoded = reader.read();
    if (decoded.isNull() || decoded.size() != QSize(Side, Side)) return false;
    if (image) *image = decoded;
    return true;
}

QString ProfileImage::hash(const QByteArray& jpeg)
{
    return jpeg.isEmpty() ? QString()
        : QString::fromLatin1(QCryptographicHash::hash(jpeg, QCryptographicHash::Sha256).toHex());
}
