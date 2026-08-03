#pragma once

#include "codex/chat/PortableToolHttpTransport.h"
#include "core/Result.h"

#include <QString>
#include <QStringView>
#include <QUrl>
#include <QVector>

#include <memory>
#include <stop_token>

namespace companion {

struct PortableWebReference final {
    QString title;
    QString excerpt;
    QUrl sourceUrl;
};

struct PortableWebLookupResult final {
    QString query;
    QVector<PortableWebReference> references;

    QString toolSummary() const;
};

class PortableWebLookupService final {
public:
    explicit PortableWebLookupService(
        std::shared_ptr<
            PortableToolHttpTransport>
            transport =
                createDefaultPortableToolHttpTransport(),
        QUrl endpoint =
            QUrl(QStringLiteral(
                "https://en.wikipedia.org/w/api.php")));

    Result<PortableWebLookupResult> lookup(
        QStringView query,
        int maximumResults = 4,
        std::stop_token stopToken = {})
        const;

private:
    Result<PortableToolHttpRequest>
    makeRequest(
        QStringView query,
        int maximumResults) const;
    static Result<
        PortableWebLookupResult>
    decode(
        const PortableToolHttpResponse&
            response,
        QStringView query);

    std::shared_ptr<
        PortableToolHttpTransport>
        transport_;
    QUrl endpoint_;
};

} // namespace companion
