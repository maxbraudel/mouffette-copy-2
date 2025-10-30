#include "platform/windows/WindowsVideoThumbnailer.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <QDir>
#include <QFile>
#include <QDebug>

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

QImage convertBitmapSourceToImage(IWICBitmapSource* source) {
    if (!source) return QImage();

    UINT width = 0, height = 0;
    if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0) {
        return QImage();
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return QImage();
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) {
        return QImage();
    }

    if (FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom))) {
        return QImage();
    }

    const UINT stride = width * 4;
    const UINT bufferSize = stride * height;
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return QImage();
    }

    if (FAILED(converter->CopyPixels(nullptr, stride, bufferSize, image.bits()))) {
        return QImage();
    }

    return image;
}

ComPtr<IWICBitmapSource> captureFirstFrameWithWmp(const QString& path) {
    ComPtr<IWICBitmapSource> bitmap;
    ComPtr<IWICImagingFactory> wicFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory)))) {
        return nullptr;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory->CreateDecoderFromFilename(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) {
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return nullptr;
    }

    // Some video containers expose the first frame via the decoder directly.
    return frame;
}

ComPtr<IWICBitmapSource> captureFirstFrameWithShell(const QString& path) {
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, IID_PPV_ARGS(&item)))) {
        return nullptr;
    }

    ComPtr<IShellItemImageFactory> imageFactory;
    if (FAILED(item.As(&imageFactory))) {
        return nullptr;
    }

    SIZE size = { 640, 360 };
    HBITMAP hBitmap = nullptr;
    if (FAILED(imageFactory->GetImage(size, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY, &hBitmap))) {
        return nullptr;
    }

    ComPtr<IWICImagingFactory> wicFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory)))) {
        DeleteObject(hBitmap);
        return nullptr;
    }

    ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wicFactory->CreateBitmapFromHBITMAP(hBitmap, nullptr, WICBitmapUseAlpha, &wicBitmap))) {
        DeleteObject(hBitmap);
        return nullptr;
    }

    DeleteObject(hBitmap);
    return wicBitmap;
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

    UINT32 width = 0, height = 0;
    MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    
    if (width > 0 && height > 0) {
        return QSize(width, height);
    }
    
    return QSize();
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

    if (ComPtr<IWICBitmapSource> wmpFrame = captureFirstFrameWithWmp(path)) {
        QImage img = convertBitmapSourceToImage(wmpFrame.Get());
        if (!img.isNull()) {
            return img;
        }
    }

    if (ComPtr<IWICBitmapSource> shellThumb = captureFirstFrameWithShell(path)) {
        QImage img = convertBitmapSourceToImage(shellThumb.Get());
        if (!img.isNull()) {
            return img;
        }
    }

    return QImage();
}

#endif
