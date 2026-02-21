#ifndef IOVERLAYPROJECTION_H
#define IOVERLAYPROJECTION_H

#include <QString>

class QPushButton;

class IOverlayProjection {
public:
    virtual ~IOverlayProjection() = default;

    virtual QPushButton* getUploadButton() const = 0;
    virtual bool isRemoteSceneLaunched() const = 0;
    virtual QString overlayDisabledButtonStyle() const = 0;
};

#endif // IOVERLAYPROJECTION_H
