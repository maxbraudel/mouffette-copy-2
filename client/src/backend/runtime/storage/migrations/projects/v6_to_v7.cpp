#include "v6_to_v7.h"
#include "backend/runtime/storage/StorageIO.h"

#include <QJsonArray>
#include <QUuid>
#include <cmath>

namespace RuntimeStorage::Migrations {
namespace {
bool integer(const QJsonValue& value, double low, double high) {
    const double n = value.toDouble(-1);
    return value.isDouble() && std::isfinite(n) && n >= low && n <= high && n == std::floor(n);
}
Operation invalid() { return {Failure::InvalidData, QStringLiteral("Invalid version 6 project timeline")}; }
}

Operation projectsV6ToV7(const QString& root, const QString& path)
{
    QJsonObject document;
    const auto inspected = readVersionedJson(root, path, 6, 64 * 1024 * 1024, &document);
    if (inspected.state == State::IoError) return {Failure::IoError, inspected.reason};
    if (inspected.state != State::Current || !document.value("projects").isArray()) return invalid();
    auto projects = document.value("projects").toArray();
    for (qsizetype p = 0; p < projects.size(); ++p) {
        if (!projects[p].isObject()) return invalid();
        auto project = projects[p].toObject();
        if (!project.value("canvasState").isObject()) return invalid();
        auto scene = project.value("canvasState").toObject();
        // A project which has never been opened has no canvas yet.
        if (scene.isEmpty()) continue;
        const auto settings = scene.value("timeline").toObject();
        if (scene.value("renderSchemaVersion").toInt(-1) != 4
            || !integer(settings.value("maxDurationMs"), 1, 604800000)
            || !integer(settings.value("slotsPerSecond"), 1, 240)
            || !scene.value("media").isArray()) return invalid();
        const qint64 maximum = settings.value("maxDurationMs").toInteger()
            * settings.value("slotsPerSecond").toInteger() / 1000;
        if (maximum < 1 || !integer(settings.value("stopSlot"), -1, maximum)) return invalid();
        auto media = scene.value("media").toArray();
        for (qsizetype m = 0; m < media.size(); ++m) {
            auto element = media[m].toObject();
            const QString type = element.value("type").toString();
            auto track = element.value("timeline").toObject();
            if ((type != "video" && type != "image" && type != "text")
                || !track.value("clips").isArray() || !track.value("keyframes").isArray()
                || !track.value("clipsInitialized").isBool()) return invalid();
            if (type != "video") {
                if (!track.value("clips").toArray().isEmpty()) return invalid();
                track.insert("clips", QJsonArray{QJsonObject{
                    {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"startSlot", 0}, {"durationSlots", maximum}, {"sourceStartSlot", QJsonValue::Null}}});
                track.insert("clipsInitialized", true);
                element.insert("timeline", track);
                media[m] = element;
            }
        }
        scene.insert("media", media);
        scene.insert("renderSchemaVersion", 5);
        project.insert("canvasState", scene);
        projects[p] = project;
    }
    document.insert("projects", projects);
    document.insert("schemaVersion", 7);
    return writeJson(root, path, document);
}
}
