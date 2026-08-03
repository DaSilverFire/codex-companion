#include "codex/state/SessionIndexReader.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace companion {

Result<QHash<QString, QString>> SessionIndexReader::readNames(
    const QString& sessionIndexPath)
{
    if (!QFileInfo::exists(sessionIndexPath)) {
        return Result<QHash<QString, QString>>::success({});
    }

    QFile file(sessionIndexPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<QHash<QString, QString>>::failure({
            QStringLiteral("codex.session_index_read_failed"),
            QStringLiteral("Could not read the Codex session index."),
            false,
            {{QStringLiteral("path"), sessionIndexPath}},
        });
    }

    QHash<QString, QString> names;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }

        QJsonParseError error;
        const QJsonDocument document =
            QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError ||
            !document.isObject()) {
            continue;
        }

        const QJsonObject object = document.object();
        const QJsonValue idValue = object.value(QStringLiteral("id"));
        const QJsonValue nameValue =
            object.value(QStringLiteral("thread_name"));
        if (!idValue.isString() || !nameValue.isString()) {
            continue;
        }

        const QString id = idValue.toString().trimmed();
        const QString name = nameValue.toString().trimmed();
        if (!id.isEmpty() && !name.isEmpty()) {
            names.insert(id, name);
        }
    }

    return Result<QHash<QString, QString>>::success(std::move(names));
}

} // namespace companion
