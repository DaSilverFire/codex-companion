#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

namespace companion {

class CredentialStore {
public:
    virtual ~CredentialStore() = default;

    virtual Result<QByteArray> read(
        const QString& service) const = 0;
    virtual Result<void> write(
        const QString& service,
        QByteArrayView secret) = 0;
    virtual Result<void> remove(
        const QString& service) = 0;
    virtual bool contains(
        const QString& service) const = 0;
};

} // namespace companion
