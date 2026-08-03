#pragma once

#include "core/CredentialStore.h"

#include <QString>

#include <functional>

namespace companion {

using CredentialAclApplier =
    std::function<Result<void>(
        const QString& path,
        bool directory)>;

class DpapiCredentialStore final
    : public CredentialStore {
public:
    DpapiCredentialStore();
    explicit DpapiCredentialStore(
        QString rootDirectory);
    DpapiCredentialStore(
        QString rootDirectory,
        CredentialAclApplier aclApplier);

    static QString defaultRootDirectory();
    static unsigned long protectionFlags() noexcept;

    Result<QByteArray> read(
        const QString& service) const override;
    Result<void> write(
        const QString& service,
        QByteArrayView secret) override;
    Result<void> remove(
        const QString& service) override;
    bool contains(
        const QString& service) const override;

private:
    Result<QString> pathForService(
        const QString& service) const;
    Result<void> ensureRootDirectory() const;

    QString rootDirectory_;
    CredentialAclApplier aclApplier_;
};

} // namespace companion
