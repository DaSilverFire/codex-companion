#include "codex/runtime/CodexRuntimeOperationRegistry.h"

#include "codex/runtime/CodexRuntimeOperationState.h"

#include <QVector>

namespace companion {

std::shared_ptr<CodexRuntimeOperationRegistry>
CodexRuntimeOperationRegistry::create()
{
    return std::shared_ptr<
        CodexRuntimeOperationRegistry>(
        new CodexRuntimeOperationRegistry());
}

quint64 CodexRuntimeOperationRegistry::
registerOperation(
    const std::shared_ptr<
        CodexRuntimeOperationState>& operation,
    QString cancellationKey)
{
    if (operation == nullptr) {
        return 0;
    }
    const std::weak_ptr<
        CodexRuntimeOperationRegistry> owner =
        weak_from_this();
    if (owner.expired()) {
        return 0;
    }

    const std::scoped_lock lock(mutex_);
    const quint64 operationId =
        nextOperationId_;
    quint64 nextOperationId =
        operationId + 1;
    if (nextOperationId == 0) {
        nextOperationId = 1;
    }
    if (!operation->bindRegistry(
            owner,
            operationId)) {
        return 0;
    }
    operations_.insert(
        operationId,
        RegisteredOperation{
            operation,
            cancellationKey.trimmed(),
        });
    nextOperationId_ = nextOperationId;
    return operationId;
}

bool CodexRuntimeOperationRegistry::
requestOperationStop(
    const QString& cancellationKey) noexcept
{
    const QString normalizedKey =
        cancellationKey.trimmed();
    if (normalizedKey.isEmpty()) {
        return false;
    }

    QVector<
        std::shared_ptr<
            CodexRuntimeOperationState>>
        operations;
    try {
        {
            const std::scoped_lock lock(mutex_);
            for (auto operation =
                     operations_.begin();
                 operation
                 != operations_.end();) {
                const auto active =
                    operation->operation.lock();
                if (active == nullptr) {
                    operation =
                        operations_.erase(
                            operation);
                    continue;
                }
                if (operation->cancellationKey
                    == normalizedKey) {
                    operations.append(active);
                }
                ++operation;
            }
        }
        for (const auto& operation : operations) {
            operation->requestRuntimeStop();
        }
        return !operations.isEmpty();
    } catch (...) {
        return false;
    }
}

void CodexRuntimeOperationRegistry::
requestRuntimeStop() noexcept
{
    QVector<
        std::shared_ptr<
            CodexRuntimeOperationState>>
        operations;
    try {
        {
            const std::scoped_lock lock(mutex_);
            operations.reserve(
                operations_.size());
            for (auto operation =
                     operations_.begin();
                 operation
                 != operations_.end();) {
                if (const auto active =
                        operation->operation.lock()) {
                    operations.append(
                        std::move(active));
                    ++operation;
                } else {
                    operation =
                        operations_.erase(
                            operation);
                }
            }
        }
        for (const auto& operation : operations) {
            operation->requestRuntimeStop();
        }
    } catch (...) {
    }
}

qsizetype CodexRuntimeOperationRegistry::
activeOperationCount() const noexcept
{
    try {
        const std::scoped_lock lock(mutex_);
        qsizetype count = 0;
        for (auto operation =
                 operations_.cbegin();
             operation
             != operations_.cend();
             ++operation) {
            if (!operation->operation.expired()) {
                ++count;
            }
        }
        return count;
    } catch (...) {
        return 0;
    }
}

void CodexRuntimeOperationRegistry::
removeOperation(
    quint64 operationId,
    const CodexRuntimeOperationState*
        operation) noexcept
{
    if (operationId == 0 || operation == nullptr) {
        return;
    }
    try {
        const std::scoped_lock lock(mutex_);
        const auto registered =
            operations_.find(operationId);
        if (registered == operations_.end()) {
            return;
        }
        const auto active =
            registered->operation.lock();
        if (active == nullptr
            || active.get() == operation) {
            operations_.erase(registered);
        }
    } catch (...) {
    }
}

} // namespace companion
