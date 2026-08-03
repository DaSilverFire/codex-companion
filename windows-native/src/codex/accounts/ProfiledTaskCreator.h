#pragma once

#include "codex/accounts/CodexAccountRouter.h"
#include "codex/appserver/TaskCreator.h"

#include <functional>

namespace companion {

using ProfiledTaskCreateCommand =
    std::function<Result<QString>(
        const CreateTaskRequest&,
        const CodexAccountRoute&)>;

class ProfiledTaskCreator final {
public:
    ProfiledTaskCreator(
        const CodexEnvironment& environment,
        CodexAccountRouter& router);
    ProfiledTaskCreator(
        CodexAccountRouter& router,
        ProfiledTaskCreateCommand command);

    Result<QString> create(
        const CreateTaskRequest& request) const;

private:
    CodexAccountRouter* router_ = nullptr;
    ProfiledTaskCreateCommand command_;
};

} // namespace companion
