#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>

#include <memory>
#include <mutex>

namespace companion {

class CodexRuntimeOperationState;

namespace detail {
struct CodexRuntimeOperationTestAccess;
}

class CodexRuntimeOperationRegistry final
    : public std::enable_shared_from_this<
          CodexRuntimeOperationRegistry> {
public:
    static std::shared_ptr<
        CodexRuntimeOperationRegistry> create();

    quint64 registerOperation(
        const std::shared_ptr<
            CodexRuntimeOperationState>& operation,
        QString cancellationKey = {});
    bool requestOperationStop(
        const QString& cancellationKey) noexcept;
    void requestRuntimeStop() noexcept;
    qsizetype activeOperationCount() const noexcept;

    CodexRuntimeOperationRegistry(
        const CodexRuntimeOperationRegistry&) = delete;
    CodexRuntimeOperationRegistry& operator=(
        const CodexRuntimeOperationRegistry&) = delete;

private:
    CodexRuntimeOperationRegistry() = default;

    void removeOperation(
        quint64 operationId,
        const CodexRuntimeOperationState*
            operation) noexcept;

    friend class CodexRuntimeOperationState;
    friend struct detail::
        CodexRuntimeOperationTestAccess;

    struct RegisteredOperation final {
        std::weak_ptr<
            CodexRuntimeOperationState>
            operation;
        QString cancellationKey;
    };

    mutable std::mutex mutex_;
    QHash<
        quint64,
        RegisteredOperation>
        operations_;
    quint64 nextOperationId_ = 1;
};

} // namespace companion
