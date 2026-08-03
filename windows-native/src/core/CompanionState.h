#pragma once

#include "codex/runtime/TaskListModel.h"

#include <QObject>

#include <memory>

class QThread;

namespace companion {

class CodexRuntime;

namespace detail {
struct CompanionStateAccessState;
}

class CompanionState final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        companion::TaskListModel* tasks
        READ tasks
        CONSTANT)

public:
    explicit CompanionState(QObject* parent = nullptr);
    ~CompanionState() override;

    TaskListModel* tasks() noexcept;

private:
    static bool tryGetOwnerThread(
        const std::shared_ptr<
            detail::CompanionStateAccessState>&
            accessState,
        QThread*& ownerThread) noexcept;

    friend class CodexRuntime;

    TaskListModel tasks_;
    std::shared_ptr<
        detail::CompanionStateAccessState>
        accessState_;
};

} // namespace companion
