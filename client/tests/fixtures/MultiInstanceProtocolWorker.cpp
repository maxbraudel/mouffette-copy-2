#include "MultiInstanceProtocolWorker.h"

#include "backend/runtime/ApplicationInstanceManager.h"
#include "backend/runtime/RuntimeStorageBootstrap.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/managers/network/ConnectionManager.h"
#include "backend/config/AppConfig.h"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <memory>

namespace {
void report(QJsonObject event)
{
    QTextStream output(stdout);
    output << QJsonDocument(event).toJson(QJsonDocument::Compact) << '\n';
    output.flush();
}
}

int runMultiInstanceProtocolWorker(QApplication& app, const QStringList& arguments)
{
    if (arguments.size() != 4) return 10;
    // Each real process waits on the same parent-controlled launch barrier.
    QTextStream input(stdin);
    if (input.readLine() != QLatin1String("go")) return 11;
    ApplicationInstanceManager instances(QStringLiteral("v7-protocol-worker"), true,
                                         arguments.at(2));
    QString error;
    if (instances.start(&error) != ApplicationInstanceManager::StartResult::Started) {
        report({{"event", "failure"}, {"error", error}});
        return 12;
    }
    const auto profile = instances.profile();
    RuntimeProfile::configure(profile);
    const auto bootstrap = RuntimeStorageBootstrap(profile).run();
    if (!bootstrap.succeeded()) {
        report({{"event", "failure"}, {"error", bootstrap.cause}});
        return 13;
    }
    int result = 0;
    {
        WebSocketClient client(RuntimeProfile::identityLocation(),
                               profile.useNativeIdentityVault, nullptr, {}, profile.ordinal);
        ConnectionManager connection(&client);
        const auto advertise = [&] {
            client.registerClient(QStringLiteral("shared-computer"), QStringLiteral("test"), {}, 50);
        };
        QObject::connect(&client, &WebSocketClient::connected, &client, advertise);
        QObject::connect(&client, &WebSocketClient::localDeviceSnapshotRequested, &client, advertise);
        QObject::connect(&connection, &ConnectionManager::stateChanged, &client,
                         [&](ConnectionManager::State) {
            report({{"event", "state"}, {"state", connection.getConnectionStatus()}});
        });
        QObject::connect(&client, &WebSocketClient::clientListReceived, &client,
                         [](const QList<ClientInfo>& peers) {
            QJsonArray entries;
            for (const auto& peer : peers) {
                auto entry = peer.toJson();
                entry.insert("displayName", peer.getInstanceDisplayName());
                entries.append(entry);
            }
            report({{"event", "clients"}, {"clients", entries}});
        });
        const auto sessionEvent = [](QJsonObject event) {
            event.insert("event", "session");
            report(event);
        };
        QObject::connect(&client, &WebSocketClient::remoteSessionOpened, &client, sessionEvent);
        QObject::connect(&client, &WebSocketClient::remoteSessionLeaseStateChanged, &client, sessionEvent);
        QObject::connect(&client, &WebSocketClient::remoteSessionClosed, &client, sessionEvent);
        QObject::connect(&client, &WebSocketClient::fatalError, &client, [](const QString& reason) {
            report({{"event", "failure"}, {"error", reason}});
        });

        bool disableAcknowledged = false;
        QSet<QString> closing;
        const auto finishDrain = [&] {
            if (connection.disconnectInProgress() && disableAcknowledged && closing.isEmpty())
                connection.completeDisconnect(connection.transitionId());
        };
        QObject::connect(&connection, &ConnectionManager::disconnectRequested, &client,
                         [&](quint64 cycle) {
            disableAcknowledged = false;
            closing.clear();
            for (const auto& binding : client.remoteSessionCoordinator()->all())
                if (binding.phase != QLatin1String("Closed")) closing.insert(binding.remoteSessionId);
            if (!client.beginEndpointDisable()) connection.completeDisconnect(cycle);
            QTimer::singleShot(AppConfig::instance().controlledDisconnectDrainTimeoutMs(), &client,
                               [&, cycle] { connection.completeDisconnect(cycle); });
        });
        QObject::connect(&client, &WebSocketClient::endpointDisableAcknowledged, &client,
                         [&](const QString&, quint64) { disableAcknowledged = true; finishDrain(); });
        QObject::connect(&client, &WebSocketClient::remoteSessionLogicallyClosed, &client,
                         [&](const QJsonObject& event) {
            closing.remove(event.value("remoteSessionId").toString());
            finishDrain();
        });
        QObject::connect(&client, &WebSocketClient::remoteSessionTerminating, &client,
                         [&](const QJsonObject& event) {
            if (event.value("targetEndpointId").toString() == client.endpointId())
                client.acknowledgeRemoteSessionTeardown(event.value("remoteSessionId").toString(),
                    event.value("teardownId").toString(), true, true, true, 0, QString(), 0);
        });

        report({{"event", "identity"}, {"ordinal", profile.ordinal},
                {"installationId", client.installationId()}, {"endpointId", client.endpointId()},
                {"runtimeId", client.runtimeId()}, {"root", profile.rootPath},
                {"identityRoot", RuntimeProfile::identityLocation()}});
        // Blocking pipe reads run outside Qt's network event loop on every OS.
        auto reader = std::unique_ptr<QThread>(QThread::create([&] {
            while (!input.atEnd()) {
                const auto message = QJsonDocument::fromJson(input.readLine().toUtf8()).object();
                QMetaObject::invokeMethod(&client, [&, message] {
                    const QString action = message.value("action").toString();
                    bool ok = true;
                    QJsonObject result;
                    if (action == QLatin1String("open"))
                        ok = client.openRemoteSession(message.value("target").toString());
                    else if (action == QLatin1String("disable")) connection.setConnectionEnabled(false);
                    else if (action == QLatin1String("enable")) connection.setConnectionEnabled(true);
                    else if (action == QLatin1String("quit")) app.quit();
                    else if (action == QLatin1String("inspect")) {
                        result.insert("ready", client.canIssueSessionCommands(message.value("session").toString()));
                    }
                    else ok = false;
                    result.insert("event", "result");
                    result.insert("id", message.value("id"));
                    result.insert("ok", ok);
                    report(result);
                }, Qt::QueuedConnection);
            }
            QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
        }));
        reader->start();
        connection.connectToServer(arguments.at(3));
        result = app.exec();
        connection.disconnect();
        // The parent closes stdin for a graceful exit; failure cleanup kills
        // only this child. Never detach a thread that still refers to the peer.
        reader->wait();
    }
    QThreadPool::globalInstance()->waitForDone();
    return result;
}
