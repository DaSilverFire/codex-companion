#include "codex/accounts/CodexAccountRouter.h"

#include <utility>

namespace companion {
namespace {

CompanionError routerError(
    QString code,
    QString message,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

} // namespace

CodexAccountRouter::CodexAccountRouter(
    QProcessEnvironment
        baseEnvironment,
    CodexAccountProfileStore&
        profileStore,
    const CodexAccountRuntime&
        profileRuntime,
    CodexThreadAccountBindingStore&
        bindingStore)
    : baseEnvironment_(
          std::move(baseEnvironment)),
      profileStore_(&profileStore),
      profileRuntime_(
          &profileRuntime),
      bindingStore_(&bindingStore)
{
}

CodexAccountRoute
CodexAccountRouter::routeNewWork()
    const
{
    return route(
        profileStore_
            ->selectedProfile());
}

CodexAccountRoute
CodexAccountRouter::routeThread(
    QStringView threadId) const
{
    const auto profileId =
        bindingStore_->profileIdFor(
            threadId);
    return route(
        profileId.has_value()
            ? profileStore_->profile(
                  *profileId)
            : std::nullopt);
}

CodexAccountRoute
CodexAccountRouter::routeProfile(
    const QUuid& profileId) const
{
    return route(
        profileStore_->profile(
            profileId));
}

Result<void>
CodexAccountRouter::bindNewThread(
    QString threadId,
    std::optional<QUuid> profileId)
{
    if (!profileId.has_value()) {
        return Result<void>::success();
    }
    if (!profileStore_->profile(
             *profileId)
             .has_value()) {
        return Result<void>::failure(
            routerError(
                QStringLiteral(
                    "codex.account_profile_missing"),
                QStringLiteral(
                    "The Codex account profile for this thread does not exist."),
                {
                    {
                        QStringLiteral(
                            "profileId"),
                        codexAccountProfileIdString(
                            *profileId),
                    },
                }));
    }
    return bindingStore_->bind(
        std::move(threadId),
        *profileId);
}

Result<bool>
CodexAccountRouter::removeProfile(
    const QUuid& profileId)
{
    if (profileId.isNull()) {
        return Result<bool>::failure(
            routerError(
                QStringLiteral(
                    "codex.account_profile_id_invalid"),
                QStringLiteral(
                    "The Codex account profile ID is empty.")));
    }
    if (bindingStore_->hasBindingsTo(
            profileId)) {
        return Result<bool>::failure(
            routerError(
                QStringLiteral(
                    "codex.account_profile_in_use"),
                QStringLiteral(
                    "This Codex account profile still owns existing tasks."),
                {
                    {
                        QStringLiteral(
                            "profileId"),
                        codexAccountProfileIdString(
                            profileId),
                    },
                }));
    }
    return profileStore_->remove(
        profileId);
}

CodexAccountRoute
CodexAccountRouter::route(
    std::optional<
        CodexAccountProfile> profile)
    const
{
    if (!profile.has_value()) {
        return {
            std::nullopt,
            baseEnvironment_,
        };
    }
    const auto environment =
        profileRuntime_->forProfile(
            baseEnvironment_,
            *profile);
    if (!environment.hasValue()) {
        return {
            std::nullopt,
            baseEnvironment_,
        };
    }
    return {
        profile->id,
        environment.value(),
    };
}

} // namespace companion
