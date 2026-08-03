#include "ui/pet/PetCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace {

QString cleanAbsolutePath(const QString& path)
{
    return QDir::cleanPath(
        QFileInfo(path).absoluteFilePath());
}

int sourceRank(
    companion::PetSourceKind source)
{
    return source
            == companion::PetSourceKind::Custom
        ? 0
        : 1;
}

} // namespace

namespace companion {

PetCatalog::PetCatalog(PetCatalogRoots roots)
    : roots_({
          cleanAbsolutePath(roots.companion),
          cleanAbsolutePath(roots.native),
          cleanAbsolutePath(roots.bundled),
      })
{
}

PetCatalogRoots PetCatalog::liveRoots()
{
    QString localData =
        qEnvironmentVariable("LOCALAPPDATA");
    if (localData.trimmed().isEmpty()) {
        localData =
            QStandardPaths::writableLocation(
                QStandardPaths::
                    GenericDataLocation);
    }
    if (localData.trimmed().isEmpty()) {
        localData = QDir::homePath();
    }

    return {
        cleanAbsolutePath(
            QDir(localData).filePath(
                QStringLiteral(
                    "Codex Companion/Pets"))),
        cleanAbsolutePath(
            QDir::home().filePath(
                QStringLiteral(".codex/pets"))),
        cleanAbsolutePath(
            QDir(
                QCoreApplication::
                    applicationDirPath())
                .filePath(
                    QStringLiteral(
                        "../resources/pets"))),
    };
}

Result<void> PetCatalog::reload()
{
    QVector<PetDefinition> loaded;
    QVector<PetCatalogDiagnostic>
        diagnostics;
    QSet<QString> seenIds;

    loadRoot(
        roots_.companion,
        PetSourceKind::Custom,
        loaded,
        diagnostics,
        seenIds);
    loadRoot(
        roots_.native,
        PetSourceKind::Custom,
        loaded,
        diagnostics,
        seenIds);

    QVector<PetDefinition> bundled;
    QSet<QString> seenBundledIds;
    loadRoot(
        roots_.bundled,
        PetSourceKind::BuiltIn,
        bundled,
        diagnostics,
        seenBundledIds);

    for (PetDefinition& candidate : bundled) {
        const auto existing =
            std::find_if(
                loaded.begin(),
                loaded.end(),
                [&candidate](
                    const PetDefinition& pet) {
                    return pet.id
                        == candidate.id;
                });
        if (existing == loaded.end()) {
            loaded.append(
                std::move(candidate));
        }
    }

    std::sort(
        loaded.begin(),
        loaded.end(),
        [](const PetDefinition& left,
           const PetDefinition& right) {
            const int leftRank =
                sourceRank(left.source);
            const int rightRank =
                sourceRank(right.source);
            if (leftRank != rightRank) {
                return leftRank < rightRank;
            }

            const int displayOrder =
                left.displayName.compare(
                    right.displayName,
                    Qt::CaseInsensitive);
            if (displayOrder != 0) {
                return displayOrder < 0;
            }

            const int idOrder =
                left.id.compare(
                    right.id,
                    Qt::CaseSensitive);
            if (idOrder != 0) {
                return idOrder < 0;
            }
            return left.sourceDirectory.compare(
                       right.sourceDirectory,
                       Qt::CaseInsensitive)
                < 0;
        });

    pets_ = std::move(loaded);
    diagnostics_ =
        std::move(diagnostics);
    return Result<void>::success();
}

const QVector<PetDefinition>&
PetCatalog::pets() const noexcept
{
    return pets_;
}

const QVector<PetCatalogDiagnostic>&
PetCatalog::diagnostics() const noexcept
{
    return diagnostics_;
}

QString PetCatalog::resolveSelection(
    QStringView selectedPetId) const
{
    const QString canonical =
        canonicalSelectionId(selectedPetId);
    if (!canonical.isEmpty()) {
        const auto existing =
            std::find_if(
                pets_.constBegin(),
                pets_.constEnd(),
                [&canonical](
                    const PetDefinition& pet) {
                    return pet.id == canonical;
                });
        if (existing != pets_.constEnd()) {
            return existing->id;
        }
    }

    return pets_.isEmpty()
        ? QString()
        : pets_.constFirst().id;
}

std::optional<PetDefinition>
PetCatalog::find(QStringView petId) const
{
    const QString canonical =
        canonicalSelectionId(petId);
    const auto found =
        std::find_if(
            pets_.constBegin(),
            pets_.constEnd(),
            [&canonical](
                const PetDefinition& pet) {
                return pet.id == canonical;
            });
    if (found == pets_.constEnd()) {
        return std::nullopt;
    }
    return *found;
}

void PetCatalog::loadRoot(
    const QString& rootPath,
    PetSourceKind source,
    QVector<PetDefinition>& loaded,
    QVector<PetCatalogDiagnostic>& diagnostics,
    QSet<QString>& seenIds) const
{
    const QFileInfo rootInformation(rootPath);
    if (!rootInformation.exists()
        || !rootInformation.isDir()) {
        return;
    }

    QFileInfoList directories =
        QDir(rootPath).entryInfoList(
            QDir::Dirs
                | QDir::NoDotAndDotDot
                | QDir::Hidden
                | QDir::System,
            QDir::NoSort);
    std::sort(
        directories.begin(),
        directories.end(),
        [](const QFileInfo& left,
           const QFileInfo& right) {
            return left.absoluteFilePath()
                       .compare(
                           right.absoluteFilePath(),
                           Qt::CaseInsensitive)
                < 0;
        });

    for (const QFileInfo& directory :
         directories) {
        if (directory.isHidden()
            || directory.fileName()
                   .startsWith(
                       QLatin1Char('.'))) {
            continue;
        }

        const QString sourceDirectory =
            cleanAbsolutePath(
                directory.absoluteFilePath());
        const auto pet =
            PetDefinition::load(
                QDir(sourceDirectory)
                    .filePath(
                        QStringLiteral(
                            "pet.json")),
                source);
        if (!pet.hasValue()) {
            diagnostics.append({
                pet.error(),
                sourceDirectory,
            });
            continue;
        }
        if (seenIds.contains(
                pet.value().id)) {
            continue;
        }
        seenIds.insert(pet.value().id);
        loaded.append(pet.value());
        if (pet.value()
                .mobilePresenceDiagnostic
                .has_value()) {
            diagnostics.append({
                *pet.value()
                     .mobilePresenceDiagnostic,
                sourceDirectory,
            });
        }
    }
}

QString PetCatalog::canonicalSelectionId(
    QStringView petId)
{
    QString result =
        petId.toString().trimmed();
    constexpr QStringView customPrefix =
        u"custom:";
    constexpr QStringView builtInPrefix =
        u"built-in:";
    if (result.startsWith(customPrefix)) {
        result.remove(
            0,
            customPrefix.size());
    } else if (
        result.startsWith(builtInPrefix)) {
        result.remove(
            0,
            builtInPrefix.size());
    }
    if (result
        == QStringLiteral(
            "shadow-native-v2")) {
        return QStringLiteral("shadow-16");
    }
    return result;
}

} // namespace companion
