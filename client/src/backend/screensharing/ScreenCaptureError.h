#pragma once

#include <QMetaType>

// Preserve the distinction through capture, publisher status and the relay.
// A denied OS permission must never be reported as an unexplained codec error.
enum class ScreenCaptureError { PermissionDenied, CaptureFailed, EncodingFailed };
Q_DECLARE_METATYPE(ScreenCaptureError)
