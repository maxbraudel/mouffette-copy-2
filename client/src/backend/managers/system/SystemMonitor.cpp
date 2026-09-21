#include "backend/managers/system/SystemMonitor.h"
#include "backend/managers/system/ScreenCoordinateMapping.h"
#include "backend/platform/LocalScreenTopology.h"
#include "backend/config/AppConfig.h"
#include "backend/domain/models/ClientInfo.h"
#include <QTimer>
#include <QGuiApplication>
#include <QCursor>
#include <QScreen>
#include <QHostInfo>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

SystemMonitor::SystemMonitor(QObject* parent, ScreenProvider screenProvider,
                             VolumeBackendFactory volumeBackendFactory)
    : QObject(parent), m_screenProvider(std::move(screenProvider)),
      m_volumeBackendFactory(std::move(volumeBackendFactory))
{
    m_screenChangeTimer = new QTimer(this);
    m_screenChangeTimer->setSingleShot(true);
    m_screenChangeTimer->setInterval(
        AppConfig::instance().screenChangeDebounceMs());
    connect(m_screenChangeTimer, &QTimer::timeout, this, [this]() {
        QList<ScreenInfo> screens;
        if (captureScreenInfo(&screens)) emit screenConfigurationChanged(screens);
    });

    if (QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        for (QScreen* screen : app->screens()) {
            watchScreen(screen);
        }
        connect(app, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
            watchScreen(screen);
            scheduleScreenConfigurationChanged();
        });
        connect(app, &QGuiApplication::screenRemoved, this, [this](QScreen*) {
            scheduleScreenConfigurationChanged();
        });
        connect(app, &QGuiApplication::primaryScreenChanged, this, [this](QScreen*) {
            scheduleScreenConfigurationChanged();
        });
    }
}

void SystemMonitor::watchScreen(QScreen* screen) {
    if (!screen) return;
    const auto schedule = [this]() { scheduleScreenConfigurationChanged(); };
    connect(screen, &QScreen::geometryChanged, this, schedule);
    connect(screen, &QScreen::availableGeometryChanged, this, schedule);
    connect(screen, &QScreen::logicalDotsPerInchChanged, this, schedule);
    connect(screen, &QScreen::physicalDotsPerInchChanged, this, schedule);
    connect(screen, &QScreen::virtualGeometryChanged, this, schedule);
}

void SystemMonitor::scheduleScreenConfigurationChanged() {
    m_topologyReady = false;
    emit screenTopologyInvalidated();
    if (m_screenChangeTimer) {
        m_screenChangeTimer->start();
    }
}

SystemMonitor::~SystemMonitor() {
    stopVolumeMonitoring();
}

int SystemMonitor::getSystemVolumePercent() {
    return m_cachedSystemVolume;
}

void SystemMonitor::startVolumeMonitoring() {
    if (m_volumeMonitoring) return;
    m_volumeMonitoring = true;
    if (!m_volumeBackend) {
        auto publish = [this](int percent) {
            if (!m_volumeMonitoring) return;
            const int value = percent >= 0 && percent <= 100 ? percent : -1;
            if (value == m_cachedSystemVolume) return;
            m_cachedSystemVolume = value;
            emit volumeChanged(value);
        };
        m_volumeBackend = m_volumeBackendFactory
            ? m_volumeBackendFactory(std::move(publish))
            : createSystemVolumeMonitorBackend(std::move(publish));
    }
    if (m_volumeBackend) m_volumeBackend->start();
}

void SystemMonitor::stopVolumeMonitoring() {
    m_volumeMonitoring = false;
    if (m_volumeBackend) m_volumeBackend->stop();
    m_cachedSystemVolume = -1;
}

QList<ScreenInfo> SystemMonitor::getLocalScreenInfo() const {
    return m_screens;
}

bool SystemMonitor::captureScreenInfo(QList<ScreenInfo>* screens)
{
    bool valid = false;
    const auto topology = m_screenProvider
        ? m_screenProvider(&valid) : LocalScreenTopology::screens(false, &valid);
    if (!valid) {
        m_topologyReady = false;
        return false;
    }
    QList<ScreenInfo> result;
    for (qsizetype index = 0; index < topology.size(); ++index) {
        const auto& entry = topology[index];
        const QRect g = entry.advertisedGeometry;
        const QRect work = entry.advertisedAvailableGeometry.intersected(g);
        ScreenInfo screen(static_cast<int>(index), g.width(), g.height(), g.x(), g.y(), entry.primary);
        if (!work.isEmpty()) {
            const auto zone = [&](const QString& type, int x, int y, int w, int h) {
                if (w > 0 && h > 0) screen.uiZones.append({type, x, y, w, h});
            };
#ifdef Q_OS_WIN
            const QString top = QStringLiteral("taskbar"), other = top;
#elif defined(Q_OS_MACOS)
            const QString top = QStringLiteral("menu_bar"), other = QStringLiteral("dock");
#else
            const QString top = QStringLiteral("taskbar"), other = top;
#endif
            zone(top, 0, 0, g.width(), work.top() - g.top());
            zone(other, 0, work.bottom() - g.top() + 1, g.width(), g.bottom() - work.bottom());
            zone(other, 0, 0, work.left() - g.left(), g.height());
            zone(other, work.right() - g.left() + 1, 0, g.right() - work.right(), g.height());
        }
        result.append(screen);
    }
    // The same immutable enumeration supplies both publication and cursor ids.
    // Never enumerate monitors on the 16-ms mouse sampling path.
    m_topology = topology;
    m_screens = result;
    m_topologyReady = !(m_screenChangeTimer && m_screenChangeTimer->isActive());
    if (screens) *screens = result;
    return true;
}

bool SystemMonitor::getLocalCursorPosition(int* screenId, QPointF* screenPosition) const
{
    if (!screenId || !screenPosition || !m_topologyReady) return false;
#ifdef Q_OS_WIN
    if (!m_topology.isEmpty() && m_topology.first().nativeWindowsCoordinates) {
        POINT physicalPosition{};
        if (!GetPhysicalCursorPos(&physicalPosition)) return false;
        const QPoint position(physicalPosition.x, physicalPosition.y);
        for (qsizetype i = 0; i < m_topology.size(); ++i) {
            const QRect bounds = m_topology[i].advertisedGeometry;
            if (!bounds.contains(position)) continue;
            *screenId = static_cast<int>(i);
            *screenPosition = position - bounds.topLeft();
            return true;
        }
        return false;
    }
#endif
    const QPoint position = QCursor::pos();
    for (qsizetype i = 0; i < m_topology.size(); ++i) {
        const auto& entry = m_topology[i];
        if (!entry.geometry.contains(position)) continue;
        const qreal scale = entry.geometry.width() > 0
            ? qreal(entry.advertisedGeometry.width()) / entry.geometry.width() : 1.0;
        *screenId = static_cast<int>(i);
        *screenPosition = ScreenCoordinateMapping::screenLocalPosition(position, entry.geometry, scale);
        return true;
    }
    return false;
}

QString SystemMonitor::getMachineName() const {
    QString hostName = QHostInfo::localHostName();
    if (hostName.isEmpty()) {
        hostName = "Unknown Machine";
    }
    return hostName;
}

QString SystemMonitor::getPlatformName() const {
#ifdef Q_OS_MACOS
    return "macOS";
#elif defined(Q_OS_WIN)
    return "Windows";
#elif defined(Q_OS_LINUX)
    return "Linux";
#else
    return "Unknown";
#endif
}
