#include "backend/platform/windows/WindowsVideoThumbnailer.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <QDir>
#include <QFile>
#include <QDebug>
#include <QTransform>
#include <algorithm>

#ifndef MFSTARTUP_LITE
#define MFSTARTUP_LITE 0x1
#endif
using Microsoft::WRL::ComPtr;

namespace {

class ComInitializer {
public:
    ComInitializer() : m_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInitializer() {
        if (SUCCEEDED(m_hr)) {
            CoUninitialize();
        }
    }
    bool initialized() const {
        return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE;
    }
private:
    HRESULT m_hr;
};

class MediaFoundationGuard {
public:
    static MediaFoundationGuard& instance() {
        static MediaFoundationGuard guard;
        return guard;
    }

    bool ok() const { return m_ok; }

private:
    MediaFoundationGuard() {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            if (hr == MF_E_ALREADY_INITIALIZED) {
                m_ok = true;
            } else {
                m_ok = false;
            }
        } else {
            m_ok = true;
        }
    }

    ~MediaFoundationGuard() = default;

    bool m_ok = false;
};

QSize displaySizeForMediaType(IMFMediaType* mediaType) {
    if (!mediaType) {
        return {};
    }

    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &width, &height))
        || width == 0 || height == 0) {
        return {};
    }

    UINT32 parNumerator = 1;
    UINT32 parDenominator = 1;
    if (FAILED(MFGetAttributeRatio(mediaType, MF_MT_PIXEL_ASPECT_RATIO,
                                   &parNumerator, &parDenominator))
        || parNumerator == 0 || parDenominator == 0) {
        parNumerator = 1;
        parDenominator = 1;
    }
    int displayWidth = qMax(1, qRound(static_cast<double>(width)
                                     * parNumerator / parDenominator));
    int displayHeight = static_cast<int>(height);

    UINT32 rotation = MFVideoRotationFormat_0;
    mediaType->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation);
    if (rotation == MFVideoRotationFormat_90
        || rotation == MFVideoRotationFormat_270) {
        std::swap(displayWidth, displayHeight);
    }
    return QSize(displayWidth, displayHeight);
}

QImage decodeExactFirstFrame(const QString& path) {
    if (!MediaFoundationGuard::instance().ok()) {
        return {};
    }

    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 2))) {
        return {};
    }
    // Let Source Reader insert the colour converter needed for a CPU-readable
    // RGB frame. This is a one-shot import operation, not the playback path.
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(path.utf16()),
                                           attributes.Get(), &reader))) {
        return {};
    }
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    UINT32 nativeRotation = MFVideoRotationFormat_0;
    UINT32 nativeWidth = 0;
    UINT32 nativeHeight = 0;
    UINT32 parNumerator = 1;
    UINT32 parDenominator = 1;
    ComPtr<IMFMediaType> nativeType;
    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                              &nativeType))) {
        nativeType->GetUINT32(MF_MT_VIDEO_ROTATION, &nativeRotation);
        MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE,
                           &nativeWidth, &nativeHeight);
        if (FAILED(MFGetAttributeRatio(nativeType.Get(), MF_MT_PIXEL_ASPECT_RATIO,
                                       &parNumerator, &parDenominator))
            || parNumerator == 0 || parDenominator == 0) {
            parNumerator = 1;
            parDenominator = 1;
        }
    }

    ComPtr<IMFMediaType> requestedType;
    if (FAILED(MFCreateMediaType(&requestedType))
        || FAILED(requestedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
        || FAILED(requestedType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32))
        || FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                              nullptr, requestedType.Get()))) {
        return {};
    }

    // A fresh Source Reader starts at presentation time zero. Read only until
    // the first actual video sample; unlike Shell thumbnails this cannot choose
    // a later representative frame.
    for (int attempt = 0; attempt < 64; ++attempt) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp100ns = 0;
        ComPtr<IMFSample> sample;
        const HRESULT readResult = reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags,
            &timestamp100ns, &sample);
        if (FAILED(readResult)
            || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            return {};
        }
        if (!sample) {
            continue;
        }

        ComPtr<IMFMediaType> outputType;
        if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               &outputType))) {
            return {};
        }
        UINT32 width = 0;
        UINT32 height = 0;
        if (FAILED(MFGetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE,
                                      &width, &height))
            || width == 0 || height == 0) {
            return {};
        }

        LONG sourceStride = 0;
        UINT32 strideValue = 0;
        if (SUCCEEDED(outputType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideValue))) {
            sourceStride = static_cast<LONG>(strideValue);
        } else if (FAILED(MFGetStrideForBitmapInfoHeader(
                       MFVideoFormat_RGB32.Data1, width, &sourceStride))) {
            sourceStride = static_cast<LONG>(width * 4);
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            return {};
        }
        BYTE* bytes = nullptr;
        DWORD maximumLength = 0;
        DWORD currentLength = 0;
        if (FAILED(buffer->Lock(&bytes, &maximumLength, &currentLength)) || !bytes) {
            return {};
        }

        QImage image(static_cast<int>(width), static_cast<int>(height),
                     QImage::Format_RGB32);
        HRESULT copyResult = E_FAIL;
        if (!image.isNull()) {
            copyResult = MFCopyImage(image.bits(), image.bytesPerLine(), bytes,
                                     sourceStride, width * 4, height);
        }
        buffer->Unlock();
        if (SUCCEEDED(copyResult)) {
            // Media Foundation may expose the decoder's macroblock-aligned
            // buffer (for example 1920x1088 for visible 1920x1080 video).
            // Never leak those padding rows into the canvas geometry/poster.
            if (nativeWidth > 0 && nativeHeight > 0
                && (nativeWidth < width || nativeHeight < height)) {
                image = image.copy(0, 0,
                                   qMin<int>(nativeWidth, image.width()),
                                   qMin<int>(nativeHeight, image.height()));
            }
            if (parNumerator != parDenominator && parDenominator > 0) {
                const int displayWidth = qMax(1, qRound(image.width()
                    * static_cast<double>(parNumerator) / parDenominator));
                image = image.scaled(displayWidth, image.height(),
                                     Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
            }
            if (nativeRotation == MFVideoRotationFormat_90
                || nativeRotation == MFVideoRotationFormat_180
                || nativeRotation == MFVideoRotationFormat_270) {
                QTransform transform;
                transform.rotate(static_cast<qreal>(nativeRotation));
                image = image.transformed(transform, Qt::SmoothTransformation);
            }
            return image;
        }
        return {};
    }
    return {};
}

} // namespace

QSize WindowsVideoThumbnailer::videoDimensions(const QString& localFilePath) {
    if (localFilePath.isEmpty()) {
        return QSize();
    }

    ComInitializer comGuard;
    if (!comGuard.initialized()) {
        return QSize();
    }

    if (!QFile::exists(localFilePath)) {
        return QSize();
    }

    const QString path = QDir::toNativeSeparators(localFilePath);

    if (!MediaFoundationGuard::instance().ok()) {
        return QSize();
    }

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, &reader))) {
        return QSize();
    }

    ComPtr<IMFMediaType> mediaType;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mediaType))) {
        return QSize();
    }

    return displaySizeForMediaType(mediaType.Get());
}

QImage WindowsVideoThumbnailer::firstFrame(const QString& localFilePath) {
    if (localFilePath.isEmpty()) {
        return QImage();
    }

    ComInitializer comGuard;

    if (!comGuard.initialized()) {
        return QImage();
    }

    if (!QFile::exists(localFilePath)) {
        return QImage();
    }

    const QString path = QDir::toNativeSeparators(localFilePath);

    return decodeExactFirstFrame(path);
}

#endif
