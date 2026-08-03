#pragma once

#include "codex/models/BridgeModels.h"
#include "core/CompanionError.h"
#include "core/Result.h"

#include <QHash>
#include <QReadWriteLock>
#include <QString>
#include <QVector>

#include <optional>

namespace companion {

struct MobilePresencePetPackageSource final {
    QString petId;
    QString displayName;
    QString packageDirectory;
    QString packageId;
    QString contentHash;

    friend bool operator==(
        const MobilePresencePetPackageSource&,
        const MobilePresencePetPackageSource&) = default;
};

struct MobilePresencePetCatalogSnapshot final {
    std::optional<QString> selectedDesktopPetId;
    QVector<MobilePresencePetPackageSource> packages;

    friend bool operator==(
        const MobilePresencePetCatalogSnapshot&,
        const MobilePresencePetCatalogSnapshot&) = default;
};

struct MobilePresencePetCatalogPresentation final {
    std::optional<QString> selectedDesktopPetId;
    QVector<BridgePresencePetCatalogEntry> catalog;

    friend bool operator==(
        const MobilePresencePetCatalogPresentation&,
        const MobilePresencePetCatalogPresentation&) = default;
};

class MobilePresencePetCatalogService final {
public:
    static constexpr qint64 kMaximumChunkLength =
        196608;
    static constexpr qint64 kMaximumPackageBytes =
        8 * 1024 * 1024;

    QVector<CompanionError> replaceSnapshot(
        MobilePresencePetCatalogSnapshot snapshot);
    MobilePresencePetCatalogPresentation
    presentation() const;
    Result<BridgePresencePetManifest> manifest(
        QString packageId,
        QString contentHash) const;
    Result<BridgePresencePetChunk> chunk(
        QString packageId,
        QString contentHash,
        QString fileName,
        qint64 offset,
        qint64 requestedLength) const;

private:
    struct IndexedPackage final {
        BridgePresencePetManifest manifest;
        QString packageDirectory;
        QHash<QString, QString> files;
        qint64 byteCount = 0;
    };

    static Result<IndexedPackage> validatePackage(
        const MobilePresencePetPackageSource& source);
    Result<IndexedPackage> indexedPackage(
        const QString& packageId,
        const QString& contentHash) const;

    mutable QReadWriteLock lock_;
    std::optional<QString> selectedDesktopPetId_;
    QHash<QString, IndexedPackage> packagesById_;
    QVector<BridgePresencePetCatalogEntry> catalog_;
};

} // namespace companion
