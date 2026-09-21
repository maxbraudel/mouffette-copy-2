#pragma once

#include "backend/config/AppConfig.h"

#include <QAuthenticator>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QUrl>
#include <QVariant>
#include <QWebSocket>

// Apply before opening each disposable control, upload or screen socket.
// This policy never changes SSL configuration or logs proxy credentials.
inline void configureNetworkProxy(QWebSocket* socket, const QUrl& destination)
{
    if (!socket) return;
    Q_UNUSED(destination);
    const auto& config = AppConfig::instance();
    const QString type = config.proxyType();
    if (type == QLatin1String("system")) {
        // Let Qt use its normal platform discovery path. Do not add a blocking
        // systemProxyForQuery call on every video-channel reconnection.
        if (!QNetworkProxyFactory::usesSystemConfiguration()) {
            QNetworkProxyFactory::setUseSystemConfiguration(true);
        }
        socket->setProxy(QNetworkProxy::DefaultProxy);
    } else if (type == QLatin1String("none")) {
        socket->setProxy(QNetworkProxy::NoProxy);
    } else {
        socket->setProxy(QNetworkProxy(type == QLatin1String("socks5")
                ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
            config.proxyHost(), quint16(config.proxyPort()), config.proxyUser(), config.proxyPassword()));
    }

    // A socket can be reopened. Install exactly one callback and read the
    // current configuration when authentication is requested, not at creation.
    constexpr auto installedProperty = "_mouffetteProxyAuthenticationInstalled";
    if (socket->property(installedProperty).toBool()) return;
    socket->setProperty(installedProperty, true);
    QObject::connect(socket, &QWebSocket::proxyAuthenticationRequired, socket,
        [socket](const QNetworkProxy& proxy, QAuthenticator* authenticator) {
            if (!authenticator) return;
            const auto& current = AppConfig::instance();
            if (current.proxyType() != QLatin1String("http")
                && current.proxyType() != QLatin1String("socks5")) return;
            const auto expectedType = current.proxyType() == QLatin1String("socks5")
                ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy;
            if (proxy.type() != expectedType || socket->proxy().type() != expectedType
                || proxy.hostName().compare(current.proxyHost(), Qt::CaseInsensitive) != 0
                || proxy.port() != current.proxyPort()
                || socket->proxy().hostName().compare(current.proxyHost(), Qt::CaseInsensitive) != 0
                || socket->proxy().port() != current.proxyPort()
                || current.proxyUser().isEmpty()) return;
            authenticator->setUser(current.proxyUser());
            authenticator->setPassword(current.proxyPassword());
        }, Qt::DirectConnection);
}
