#pragma once

#include "core/Result.h"
#include "ui/pet/PetDefinition.h"

#include <QSet>
#include <QString>
#include <QStringView>
#include <QVector>

#include <optional>

namespace companion {

struct PetCatalogRoots final {
    QString companion;
    QString native;
    QString bundled;
};

struct PetCatalogDiagnostic final {
    CompanionError error;
    QString sourceDirectory;
};

class PetCatalog final {
public:
    explicit PetCatalog(PetCatalogRoots roots);

    static PetCatalogRoots liveRoots();

    Result<void> reload();
    const QVector<PetDefinition>& pets() const noexcept;
    const QVector<PetCatalogDiagnostic>& diagnostics() const noexcept;
    QString resolveSelection(QStringView selectedPetId) const;
    std::optional<PetDefinition> find(
        QStringView petId) const;

private:
    void loadRoot(
        const QString& rootPath,
        PetSourceKind source,
        QVector<PetDefinition>& loaded,
        QVector<PetCatalogDiagnostic>& diagnostics,
        QSet<QString>& seenIds) const;
    static QString canonicalSelectionId(
        QStringView petId);

    PetCatalogRoots roots_;
    QVector<PetDefinition> pets_;
    QVector<PetCatalogDiagnostic> diagnostics_;
};

} // namespace companion
