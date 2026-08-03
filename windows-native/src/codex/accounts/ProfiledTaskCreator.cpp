#include "codex/accounts/ProfiledTaskCreator.h"

#include <utility>

namespace companion {
namespace {

CompanionError profiledTaskError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

} // namespace

ProfiledTaskCreator::ProfiledTaskCreator(
    const CodexEnvironment& environment,
    CodexAccountRouter& router)
    : ProfiledTaskCreator(
          router,
          [environment](
              const CreateTaskRequest& request,
              const CodexAccountRoute& route) {
              TaskCreator creator(
                  environment,
                  route.environment);
              return creator.create(request);
          })
{
}

ProfiledTaskCreator::ProfiledTaskCreator(
    CodexAccountRouter& router,
    ProfiledTaskCreateCommand command)
    : router_(&router),
      command_(std::move(command))
{
}

Result<QString> ProfiledTaskCreator::create(
    const CreateTaskRequest& request) const
{
    if (router_ == nullptr || !command_) {
        return Result<QString>::failure(
            profiledTaskError(
                QStringLiteral(
                    "codex.account_task_creator_unavailable"),
                QStringLiteral(
                    "Codex account task creation is unavailable.")));
    }

    const CodexAccountRoute route =
        router_->routeNewWork();
    Result<QString> created =
        command_(request, route);
    if (!created.hasValue()) {
        return created;
    }

    const auto bound =
        router_->bindNewThread(
            created.value(),
            route.profileId);
    if (!bound.hasValue()) {
        CompanionError error =
            bound.error();
        error.context.insert(
            QStringLiteral("threadId"),
            created.value());
        return Result<QString>::failure(
            std::move(error));
    }
    return created;
}

} // namespace companion
