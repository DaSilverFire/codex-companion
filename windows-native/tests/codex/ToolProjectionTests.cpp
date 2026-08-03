#include "codex/state/ToolProjection.h"

#include <QJsonObject>
#include <QtTest>

using namespace companion;

class ToolProjectionTests final : public QObject {
    Q_OBJECT

private slots:
    void nestedTerminalCommandKeepsOnlyTheCommand()
    {
        const ToolProjection projection = ToolProjector::project(
            QStringLiteral("exec"),
            QString::fromUtf8(
                R"(const result = await tools.exec_command({cmd:"swift test",workdir:"C:\\project"});)"));

        QCOMPARE(projection.title, QStringLiteral("Tested the app"));
        QCOMPARE(projection.detail.value(), QStringLiteral("swift test"));
        QVERIFY(!projection.omitsWrapper);
    }

    void terminalPayloadsUseOperationLevelTitles()
    {
        const ToolProjection read = ToolProjector::project(
            QStringLiteral("exec_command"),
            QStringLiteral(
                R"({"cmd":"sed -n '1,120p' Sources/App.swift"})"));
        const ToolProjection search = ToolProjector::project(
            QStringLiteral("exec"),
            QString::fromUtf8(
                R"(const result = await tools.exec_command({cmd:"rg -n 'TaskTimeline' Sources"});)"));
        const ToolProjection test = ToolProjector::project(
            QStringLiteral("exec_command"),
            QStringLiteral(
                R"({"cmd":"swift test --filter Timeline"})"));
        const ToolProjection build = ToolProjector::project(
            QStringLiteral("exec_command"),
            QStringLiteral(
                R"({"cmd":"cmake --build --preset windows-msvc-debug"})"));
        const ToolProjection mixed = ToolProjector::project(
            QStringLiteral("exec_command"),
            QStringLiteral(
                R"({"cmd":"rg -n Task Sources && ctest --preset windows-msvc-debug"})"));

        QCOMPARE(read.title, QStringLiteral("Read files"));
        QCOMPARE(read.detail.value(), QStringLiteral("Sources/App.swift"));
        QCOMPARE(search.title, QStringLiteral("Searched files"));
        QCOMPARE(
            search.detail.value(),
            QStringLiteral("rg -n 'TaskTimeline' Sources"));
        QCOMPARE(test.title, QStringLiteral("Tested the app"));
        QCOMPARE(
            test.detail.value(),
            QStringLiteral("swift test --filter Timeline"));
        QCOMPARE(build.title, QStringLiteral("Built the app"));
        QCOMPARE(mixed.title, QStringLiteral("Ran a command"));
    }

    void shellBackedReadsExposeFilesInsteadOfShellSyntax()
    {
        const ToolProjection quoted = ToolProjector::project(
            QStringLiteral("exec_command"),
            QStringLiteral(
                R"({"cmd":"sed -n '1,120p' 'Sources/Task Detail.swift'"})"));
        const ToolProjection multiple = ToolProjector::project(
            QStringLiteral("exec_command"),
            QStringLiteral(
                R"({"cmd":"cat Sources/App.swift Sources/Timeline.swift"})"));
        const ToolProjection optionValue = ToolProjector::project(
            QStringLiteral("exec_command"),
            QStringLiteral(
                R"({"cmd":"tail -n 80 C:\\Temp\\companion.log"})"));

        QCOMPARE(
            quoted.detail.value(),
            QStringLiteral("Sources/Task Detail.swift"));
        QCOMPARE(
            multiple.detail.value(),
            QStringLiteral("Sources/App.swift\nSources/Timeline.swift"));
        QCOMPARE(
            optionValue.detail.value(),
            QStringLiteral("C:\\Temp\\companion.log"));
    }

    void patchActivityExposesOnlyAffectedPaths()
    {
        const ToolProjection projection = ToolProjector::project(
            QStringLiteral("apply_patch"),
            QString::fromUtf8(
                "*** Begin Patch\n"
                "*** Update File: Sources/App.cpp\n"
                "@@\n"
                "-const bool secret = true;\n"
                "+const bool secret = false;\n"
                "*** Add File: Sources/Timeline.cpp\n"
                "+struct PrivateImplementation {};\n"
                "*** End Patch\n"));

        QCOMPARE(projection.title, QStringLiteral("Edited files"));
        QCOMPARE(
            projection.detail.value(),
            QStringLiteral("Sources/App.cpp\nSources/Timeline.cpp"));
        QVERIFY(!projection.detail->contains(QStringLiteral("secret")));
        QVERIFY(!projection.detail->contains(
            QStringLiteral("PrivateImplementation")));
    }

    void patchCompletionRecoversOnlyAffectedPaths()
    {
        const auto fromChanges =
            ToolProjector::editedFilePathsFromChanges(QJsonObject{
                {QStringLiteral("C:\\project\\Sources\\Timeline.cpp"),
                 QJsonObject{
                     {QStringLiteral("type"), QStringLiteral("add")},
                     {QStringLiteral("content"),
                      QStringLiteral("private implementation")},
                 }},
                {QStringLiteral("C:\\project\\Sources\\App.cpp"),
                 QJsonObject{
                     {QStringLiteral("type"), QStringLiteral("update")},
                     {QStringLiteral("content"),
                      QStringLiteral("secret value")},
                 }},
            });
        const auto fromOutput =
            ToolProjector::editedFilePathsFromToolOutput(
                QString::fromUtf8(
                    "Success. Updated the following files:\n"
                    "M Sources/App.cpp\n"
                    "A Sources/Timeline View.cpp\n"
                    "D Tests/LegacyTests.cpp\n"));

        QCOMPARE(
            fromChanges.value(),
            QStringLiteral(
                "C:\\project\\Sources\\App.cpp\n"
                "C:\\project\\Sources\\Timeline.cpp"));
        QCOMPARE(
            fromOutput.value(),
            QStringLiteral(
                "Sources/App.cpp\n"
                "Sources/Timeline View.cpp\n"
                "Tests/LegacyTests.cpp"));
        QVERIFY(!ToolProjector::editedFilePathsFromToolOutput(
            QStringLiteral("M Sources/App.cpp")).has_value());
    }

    void wrappersAndToolInventoryHideImplementationSource()
    {
        const ToolProjection wrapper = ToolProjector::project(
            QStringLiteral("exec"),
            QString::fromUtf8(
                R"(const result = await tools.mcp__node_repl__js({title:"Verify timeline",code:`await sky.click({x:10,y:20})`});)"));
        const ToolProjection inventory = ToolProjector::project(
            QStringLiteral("exec"),
            QString::fromUtf8(
                "const matches = ALL_TOOLS.filter(tool => "
                "tool.name.includes('thread')); text(matches);"));
        const ToolProjection directInventory = ToolProjector::project(
            QStringLiteral("mcp__node_repl__js"),
            QStringLiteral(
                R"({"code":"const hits = ALL_TOOLS.filter(tool => tool.name.includes('thread')); text(hits);"})"));
        const ToolProjection unknownWrapper = ToolProjector::project(
            QStringLiteral("exec"),
            QString::fromUtf8(
                R"(const result = await tools.future_tool({secretImplementation:"raw wrapper code"});)"));
        const ToolProjection workspaceDependencies =
            ToolProjector::project(
                QStringLiteral("load_workspace_dependencies"));

        QCOMPARE(wrapper.title, QStringLiteral("Inspected an app"));
        QCOMPARE(wrapper.detail.value(), QStringLiteral("Verify timeline"));
        QVERIFY(wrapper.omitsWrapper);
        QCOMPARE(inventory.title, QStringLiteral("Loaded tools"));
        QVERIFY(!inventory.detail.has_value());
        QCOMPARE(directInventory.title, QStringLiteral("Loaded tools"));
        QVERIFY(!directInventory.detail.has_value());
        QCOMPARE(
            unknownWrapper.title,
            QStringLiteral("Used a tool"));
        QVERIFY(!unknownWrapper.detail.has_value());
        QCOMPARE(
            workspaceDependencies.title,
            QStringLiteral("Loaded tools"));
        QVERIFY(!workspaceDependencies.detail.has_value());
    }

    void directAppInspectionKeepsOnlyProductLevelDetail()
    {
        const ToolProjection titled = ToolProjector::project(
            QStringLiteral("js"),
            QString::fromUtf8(
                R"(const result = await tools.mcp__node_repl__js({title:"Verify text entry",code:`await sky.click({x:10,y:20})`});)"));
        const ToolProjection untitled = ToolProjector::project(
            QStringLiteral("computer_use"),
            QStringLiteral(
                R"({"app":"Codex Companion","action":"inspect","x":10,"y":20})"));

        QCOMPARE(titled.title, QStringLiteral("Inspected an app"));
        QCOMPARE(titled.detail.value(), QStringLiteral("Verify text entry"));
        QVERIFY(!titled.detail->contains(QStringLiteral("await sky")));
        QCOMPARE(untitled.title, QStringLiteral("Inspected an app"));
        QCOMPARE(untitled.detail.value(), QStringLiteral("Codex Companion"));
        QVERIFY(!untitled.detail->contains(QStringLiteral("\"x\"")));
    }

    void integrationAndUnknownToolsKeepOnlySafeFields()
    {
        const ToolProjection integration = ToolProjector::project(
            QStringLiteral("fetch_record"),
            QStringLiteral(
                R"({"title":"Current task","secretImplementation":"hidden"})"),
            QStringLiteral("example-connector"));
        const ToolProjection withPath = ToolProjector::project(
            QStringLiteral("future_tool"),
            QStringLiteral(
                R"({"path":"C:\\Temp\\result.json","secretImplementation":"raw"})"));
        const ToolProjection opaque = ToolProjector::project(
            QStringLiteral("future_tool"),
            QStringLiteral(
                R"({"secretImplementation":"raw"})"));

        QCOMPARE(
            integration.title,
            QStringLiteral("Used an integration"));
        QCOMPARE(
            integration.detail.value(),
            QStringLiteral("Example Connector - Current task"));
        QVERIFY(!integration.detail->contains(
            QStringLiteral("secretImplementation")));
        QCOMPARE(withPath.title, QStringLiteral("Used a tool"));
        QCOMPARE(
            withPath.detail.value(),
            QStringLiteral("C:\\Temp\\result.json"));
        QVERIFY(!opaque.detail.has_value());
    }

    void firstPartyActionsUseStableVocabulary()
    {
        const ToolProjection progress = ToolProjector::project(
            QStringLiteral("exec"),
            QString::fromUtf8(
                R"(const result = await tools.update_plan({explanation:"Timeline rows are semantic",plan:[]});)"));
        const ToolProjection generated = ToolProjector::project(
            QStringLiteral("image_gen__imagegen"),
            QStringLiteral(R"({"prompt":"Shadow waving"})"));
        const ToolProjection web = ToolProjector::project(
            QStringLiteral("web__run"),
            QStringLiteral(R"({"q":"Qt timeline pagination"})"));

        QCOMPARE(progress.title, QStringLiteral("Updated progress"));
        QCOMPARE(
            progress.detail.value(),
            QStringLiteral("Timeline rows are semantic"));
        QCOMPARE(generated.title, QStringLiteral("Generated an image"));
        QCOMPARE(web.title, QStringLiteral("Searched the web"));
    }

    void callLifecycleWaitsForOutput()
    {
        QCOMPARE(
            ToolProjector::callStatus(QStringLiteral("completed")),
            TimelineStatus::InProgress);
        QCOMPARE(
            ToolProjector::callStatus(QStringLiteral("failed")),
            TimelineStatus::Failed);
        QCOMPARE(
            ToolProjector::resolvedStatus(
                TimelineStatus::InProgress, {}),
            TimelineStatus::InProgress);
        QCOMPARE(
            ToolProjector::resolvedStatus(
                TimelineStatus::InProgress,
                {TimelineStatus::Completed}),
            TimelineStatus::Completed);
        QCOMPARE(
            ToolProjector::resolvedStatus(
                TimelineStatus::InProgress,
                {TimelineStatus::Failed}),
            TimelineStatus::Failed);
    }
};

QTEST_GUILESS_MAIN(ToolProjectionTests)
#include "ToolProjectionTests.moc"
