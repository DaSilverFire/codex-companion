#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QHash>
#include <QUrl>

#include <memory>
#include <stop_token>

namespace companion {

struct PortableToolHttpRequest final {
    QUrl endpoint;
    QHash<QByteArray, QByteArray> headers;
    int timeoutMilliseconds = 15000;
    int maximumResponseBytes = 1024 * 1024;
};

struct PortableToolHttpResponse final {
    int statusCode = 0;
    QByteArray body;
};

class PortableToolHttpTransport {
public:
    virtual ~PortableToolHttpTransport() = default;

    virtual Result<PortableToolHttpResponse> get(
        const PortableToolHttpRequest& request,
        std::stop_token stopToken = {}) = 0;
};

std::shared_ptr<PortableToolHttpTransport>
createDefaultPortableToolHttpTransport();

} // namespace companion
