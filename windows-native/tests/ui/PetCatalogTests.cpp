#include "ui/pet/PetCatalog.h"
#include "ui/pet/PetDefinition.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <utility>

namespace {

bool writePet(
    const QString& root,
    const QString& directoryName,
    const QString& id,
    const QString& displayName,
    int columns,
    int rows,
    bool currentShadowContract = false)
{
    const QString directory =
        QDir(root).filePath(directoryName);
    if (!QDir().mkpath(directory)) {
        return false;
    }

    QImage sheet(
        columns * 4,
        rows * 4,
        QImage::Format_ARGB32_Premultiplied);
    sheet.fill(Qt::transparent);
    sheet.setPixelColor(1, 1, QColor(255, 214, 32));
    if (!sheet.save(
            QDir(directory).filePath(
                QStringLiteral("spritesheet.png")))) {
        return false;
    }

    QJsonObject animationFrameCounts{
        {QStringLiteral("idle"), columns},
        {QStringLiteral("running"), columns},
    };
    if (currentShadowContract) {
        const QStringList states{
            QStringLiteral("idle"),
            QStringLiteral("running-right"),
            QStringLiteral("running-left"),
            QStringLiteral("waving"),
            QStringLiteral("jumping"),
            QStringLiteral("failed"),
            QStringLiteral("waiting"),
            QStringLiteral("running"),
            QStringLiteral("review"),
            QStringLiteral("goal-complete"),
            QStringLiteral("thinking"),
            QStringLiteral("talking"),
        };
        for (const QString& state : states) {
            animationFrameCounts.insert(
                state,
                columns);
        }
    }

    const QJsonObject manifest{
        {QStringLiteral("id"), id},
        {QStringLiteral("displayName"), displayName},
        {QStringLiteral("description"),
         QStringLiteral("Test pet")},
        {QStringLiteral("spritesheetPath"),
         QStringLiteral("spritesheet.png")},
        {QStringLiteral("spriteColumns"), columns},
        {QStringLiteral("spriteRows"), rows},
        {QStringLiteral("animationFrameCounts"),
         animationFrameCounts},
    };
    QFile file(
        QDir(directory).filePath(
            QStringLiteral("pet.json")));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(
               QJsonDocument(manifest).toJson(
                   QJsonDocument::Compact))
        > 0;
}

bool addMobilePresenceMetadata(
    const QString& root,
    const QString& directoryName,
    QJsonObject mobilePresence,
    bool createDirectory = true)
{
    const QString packageDirectory =
        QDir(root).filePath(directoryName);
    if (createDirectory
        && !QDir().mkpath(
            QDir(packageDirectory).filePath(
                QStringLiteral("mobile-presence")))) {
        return false;
    }

    QFile file(
        QDir(packageDirectory).filePath(
            QStringLiteral("pet.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError parseError;
    QJsonDocument document =
        QJsonDocument::fromJson(
            file.readAll(),
            &parseError);
    file.close();
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return false;
    }

    QJsonObject manifest = document.object();
    manifest.insert(
        QStringLiteral("mobilePresence"),
        std::move(mobilePresence));
    if (!file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate)) {
        return false;
    }
    return file.write(
               QJsonDocument(manifest).toJson(
                   QJsonDocument::Compact))
        > 0;
}

int cycleDurationMilliseconds(
    const companion::PetAnimationSequence& sequence)
{
    int duration = 0;
    for (const companion::PetFrame& frame :
         sequence.frames) {
        duration += frame.durationMilliseconds;
    }
    return duration;
}

} // namespace

class PetCatalogTests final : public QObject {
    Q_OBJECT

private slots:
    void customRootsTakePrecedenceAndBundledPetsSortLast()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString companionRoot =
            temporary.filePath(
                QStringLiteral("companion"));
        const QString nativeRoot =
            temporary.filePath(
                QStringLiteral("native"));
        const QString bundledRoot =
            temporary.filePath(
                QStringLiteral("bundled"));

        QVERIFY(writePet(
            companionRoot,
            QStringLiteral("z-copy"),
            QStringLiteral("shared"),
            QStringLiteral("Bravo"),
            8,
            9));
        QVERIFY(writePet(
            nativeRoot,
            QStringLiteral("a-pet"),
            QStringLiteral("alpha"),
            QStringLiteral("Alpha"),
            8,
            9));
        QVERIFY(writePet(
            nativeRoot,
            QStringLiteral("duplicate"),
            QStringLiteral("shared"),
            QStringLiteral("Wrong source"),
            16,
            12));
        QVERIFY(writePet(
            bundledRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Shadow"),
            16,
            12,
            true));

        companion::PetCatalog catalog({
            companionRoot,
            nativeRoot,
            bundledRoot,
        });
        const auto reloaded = catalog.reload();

        QVERIFY(reloaded.hasValue());
        QCOMPARE(catalog.pets().size(), 3);
        QCOMPARE(
            catalog.pets().at(0).id,
            QStringLiteral("alpha"));
        QCOMPARE(
            catalog.pets().at(1).id,
            QStringLiteral("shared"));
        QCOMPARE(
            catalog.pets().at(2).id,
            QStringLiteral("shadow-16"));
        QCOMPARE(
            catalog.pets().at(1).sourceDirectory,
            QDir(companionRoot).filePath(
                QStringLiteral("z-copy")));
        QCOMPARE(
            catalog.pets().at(1).sourceTitle(),
            QStringLiteral("Custom"));
        QCOMPARE(
            catalog.pets().at(2).sourceTitle(),
            QStringLiteral("Built-in"));
        QVERIFY(catalog.diagnostics().isEmpty());
    }

    void customDuplicateRemainsAuthoritativeOverBundledPet()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString companionRoot =
            temporary.filePath(
                QStringLiteral("companion"));
        const QString bundledRoot =
            temporary.filePath(
                QStringLiteral("bundled"));

        QVERIFY(writePet(
            companionRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Legacy Shadow"),
            8,
            11));
        QVERIFY(writePet(
            bundledRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Shadow"),
            16,
            12,
            true));

        companion::PetCatalog catalog({
            companionRoot,
            temporary.filePath(
                QStringLiteral("native")),
            bundledRoot,
        });
        const auto reloaded = catalog.reload();

        QVERIFY(reloaded.hasValue());
        QCOMPARE(catalog.pets().size(), 1);
        const auto& shadow =
            catalog.pets().constFirst();
        QCOMPARE(
            shadow.id,
            QStringLiteral("shadow-16"));
        QCOMPARE(
            shadow.source,
            companion::PetSourceKind::CompanionCustom);
        QCOMPARE(shadow.spriteColumns, 8);
        QCOMPARE(shadow.spriteRows, 11);
        QCOMPARE(
            shadow.sourceDirectory,
            QDir(companionRoot).filePath(
                QStringLiteral("shadow-16")));
    }

    void invalidPackagesRemainDiagnosticsWithoutHidingValidPets()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString companionRoot =
            temporary.filePath(
                QStringLiteral("companion"));
        const QString outsideSheet =
            temporary.filePath(
                QStringLiteral("outside.png"));
        QImage(8, 8, QImage::Format_ARGB32)
            .save(outsideSheet);

        QVERIFY(writePet(
            companionRoot,
            QStringLiteral("valid"),
            QStringLiteral("valid"),
            QStringLiteral("Valid"),
            8,
            9));
        const QString brokenDirectory =
            QDir(companionRoot).filePath(
                QStringLiteral("broken"));
        QVERIFY(QDir().mkpath(brokenDirectory));
        QFile brokenManifest(
            QDir(brokenDirectory).filePath(
                QStringLiteral("pet.json")));
        QVERIFY(brokenManifest.open(
            QIODevice::WriteOnly));
        const QJsonObject broken{
            {QStringLiteral("id"),
             QStringLiteral("broken")},
            {QStringLiteral("displayName"),
             QStringLiteral("Broken")},
            {QStringLiteral("spritesheetPath"),
             QStringLiteral("../../outside.png")},
            {QStringLiteral("spriteColumns"), 8},
            {QStringLiteral("spriteRows"), 9},
        };
        QVERIFY(
            brokenManifest.write(
                QJsonDocument(broken).toJson(
                    QJsonDocument::Compact))
            > 0);
        brokenManifest.close();

        companion::PetCatalog catalog({
            companionRoot,
            temporary.filePath(
                QStringLiteral("native")),
            temporary.filePath(
                QStringLiteral("bundled")),
        });
        const auto reloaded = catalog.reload();

        QVERIFY(reloaded.hasValue());
        QCOMPARE(catalog.pets().size(), 1);
        QCOMPARE(
            catalog.pets().constFirst().id,
            QStringLiteral("valid"));
        QCOMPARE(catalog.diagnostics().size(), 1);
        QCOMPARE(
            catalog.diagnostics()
                .constFirst()
                .error.code,
            QStringLiteral("pet.unsafe-path"));
        QCOMPARE(
            catalog.diagnostics()
                .constFirst()
                .sourceDirectory,
            brokenDirectory);
    }

    void selectionAliasesPreserveExistingSettingsAndUseFirstAvailablePet()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString bundledRoot =
            temporary.filePath(
                QStringLiteral("bundled"));
        QVERIFY(writePet(
            bundledRoot,
            QStringLiteral("other"),
            QStringLiteral("other"),
            QStringLiteral("Other"),
            8,
            9));
        QVERIFY(writePet(
            bundledRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Shadow"),
            16,
            12));

        companion::PetCatalog catalog({
            temporary.filePath(
                QStringLiteral("companion")),
            temporary.filePath(
                QStringLiteral("native")),
            bundledRoot,
        });
        QVERIFY(catalog.reload().hasValue());

        QCOMPARE(
            catalog.resolveSelection(
                QStringLiteral(
                    "custom:shadow-16")),
            QStringLiteral("shadow-16"));
        QCOMPARE(
            catalog.resolveSelection(
                QStringLiteral(
                    "built-in:shadow-16")),
            QStringLiteral("shadow-16"));
        QCOMPARE(
            catalog.resolveSelection(
                QStringLiteral("missing")),
            QStringLiteral("other"));
        QVERIFY(
            catalog.find(
                QStringLiteral(
                    "custom:shadow-16"))
                .has_value());
    }

    void emptyCatalogHasNoSelection()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        companion::PetCatalog catalog({
            temporary.filePath(
                QStringLiteral("companion")),
            temporary.filePath(
                QStringLiteral("native")),
            temporary.filePath(
                QStringLiteral("bundled")),
        });

        QVERIFY(catalog.reload().hasValue());
        QVERIFY(catalog.pets().isEmpty());
        QVERIFY(
            catalog.resolveSelection(
                QStringLiteral("missing"))
                .isEmpty());
        QVERIFY(
            !catalog.find(
                 QStringLiteral("missing"))
                 .has_value());
    }

    void legacyNativeShadowSelectionResolvesToCurrentShadow()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString companionRoot =
            temporary.filePath(
                QStringLiteral("companion"));
        const QString bundledRoot =
            temporary.filePath(
                QStringLiteral("bundled"));
        QVERIFY(writePet(
            companionRoot,
            QStringLiteral("shadow-native-v2"),
            QStringLiteral("shadow-native-v2"),
            QStringLiteral("Legacy Shadow"),
            8,
            11,
            true));
        QVERIFY(writePet(
            bundledRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Shadow"),
            16,
            12,
            true));

        companion::PetCatalog catalog({
            companionRoot,
            temporary.filePath(
                QStringLiteral("native")),
            bundledRoot,
        });
        QVERIFY(catalog.reload().hasValue());

        QCOMPARE(
            catalog.resolveSelection(
                QStringLiteral(
                    "custom:shadow-native-v2")),
            QStringLiteral("shadow-16"));
        const auto migrated =
            catalog.find(
                QStringLiteral(
                    "custom:shadow-native-v2"));
        QVERIFY(migrated.has_value());
        QCOMPARE(
            migrated->id,
            QStringLiteral("shadow-16"));
    }

    void loadsOptionalMobilePresenceWithoutChangingDesktopIdentity()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString bundledRoot =
            temporary.filePath(
                QStringLiteral("bundled"));
        QVERIFY(writePet(
            bundledRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Shadow"),
            16,
            12,
            true));
        QVERIFY(addMobilePresenceMetadata(
            bundledRoot,
            QStringLiteral("shadow-16"),
            {
                {
                    QStringLiteral("directory"),
                    QStringLiteral("mobile-presence"),
                },
                {
                    QStringLiteral("packageID"),
                    QStringLiteral(
                        "shadow-16-mobile-presence-v10"),
                },
                {
                    QStringLiteral("contentHash"),
                    QStringLiteral(
                        "69b1fbb5730390bf24190d53a312b504834b94db51ec2397d5a1984f4ad40e9f"),
                },
            }));

        const auto loaded =
            companion::PetDefinition::load(
                QDir(bundledRoot).filePath(
                    QStringLiteral(
                        "shadow-16/pet.json")),
                companion::PetSourceKind::BuiltIn);

        QVERIFY(loaded.hasValue());
        const auto& pet = loaded.value();
        QCOMPARE(
            pet.id,
            QStringLiteral("shadow-16"));
        QCOMPARE(pet.spriteColumns, 16);
        QCOMPARE(pet.spriteRows, 12);
        QVERIFY(pet.mobilePresence.has_value());
        QCOMPARE(
            pet.mobilePresence->directory,
            QStringLiteral("mobile-presence"));
        QCOMPARE(
            pet.mobilePresence->packageId,
            QStringLiteral(
                "shadow-16-mobile-presence-v10"));
        QCOMPARE(
            pet.mobilePresence->contentHash,
            QStringLiteral(
                "69b1fbb5730390bf24190d53a312b504834b94db51ec2397d5a1984f4ad40e9f"));
        QVERIFY(
            !pet.mobilePresenceDiagnostic.has_value());
    }

    void invalidMobilePresenceIsNonfatalAndReportedByCatalog()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString bundledRoot =
            temporary.filePath(
                QStringLiteral("bundled"));
        QVERIFY(writePet(
            bundledRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Shadow"),
            16,
            12,
            true));
        QVERIFY(addMobilePresenceMetadata(
            bundledRoot,
            QStringLiteral("shadow-16"),
            {
                {
                    QStringLiteral("directory"),
                    QStringLiteral("mobile-presence"),
                },
                {
                    QStringLiteral("packageID"),
                    QStringLiteral(
                        "shadow-16-mobile-presence-v10"),
                },
                {
                    QStringLiteral("contentHash"),
                    QStringLiteral("not-a-sha256"),
                },
            }));

        companion::PetCatalog catalog({
            temporary.filePath(
                QStringLiteral("companion")),
            temporary.filePath(
                QStringLiteral("native")),
            bundledRoot,
        });

        QVERIFY(catalog.reload().hasValue());
        QCOMPARE(catalog.pets().size(), 1);
        const auto& pet = catalog.pets().constFirst();
        QCOMPARE(
            pet.id,
            QStringLiteral("shadow-16"));
        QVERIFY(!pet.mobilePresence.has_value());
        QVERIFY(
            pet.mobilePresenceDiagnostic.has_value());
        QCOMPARE(catalog.diagnostics().size(), 1);
        QCOMPARE(
            catalog.diagnostics()
                .constFirst()
                .error.code,
            QStringLiteral(
                "pet.mobile-presence-invalid"));
    }

    void shortShadowContinuousRowsPreserveMacCycleTiming()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString customRoot =
            temporary.filePath(
                QStringLiteral("custom"));
        QVERIFY(writePet(
            customRoot,
            QStringLiteral("shadow-native-v2"),
            QStringLiteral("shadow-native-v2"),
            QStringLiteral("Legacy Shadow"),
            8,
            12,
            true));
        QVERIFY(writePet(
            customRoot,
            QStringLiteral("compact"),
            QStringLiteral("compact"),
            QStringLiteral("Compact"),
            8,
            12,
            true));

        const auto shadowLoaded =
            companion::PetDefinition::load(
                QDir(customRoot).filePath(
                    QStringLiteral(
                        "shadow-native-v2/pet.json")),
                companion::PetSourceKind::Custom);
        const auto compactLoaded =
            companion::PetDefinition::load(
                QDir(customRoot).filePath(
                    QStringLiteral(
                        "compact/pet.json")),
                companion::PetSourceKind::Custom);
        QVERIFY(shadowLoaded.hasValue());
        QVERIFY(compactLoaded.hasValue());

        struct TimingCase final {
            companion::PetAnimation animation;
            int firstFrameMilliseconds;
            int finalFrameMilliseconds;
            int cycleMilliseconds;
        };
        const std::array cases{
            TimingCase{
                companion::PetAnimation::Idle,
                709,
                240,
                5203,
            },
            TimingCase{
                companion::PetAnimation::RunningRight,
                367,
                80,
                2649,
            },
            TimingCase{
                companion::PetAnimation::RunningLeft,
                367,
                80,
                2649,
            },
            TimingCase{
                companion::PetAnimation::Running,
                367,
                80,
                2649,
            },
            TimingCase{
                companion::PetAnimation::Thinking,
                489,
                180,
                3603,
            },
            TimingCase{
                companion::PetAnimation::Talking,
                246,
                80,
                1802,
            },
        };

        for (const TimingCase& timing : cases) {
            const auto sequence =
                shadowLoaded.value()
                    .animationSequence(
                        timing.animation,
                        1.0);
            QCOMPARE(sequence.frames.size(), 8);
            QCOMPARE(sequence.loopStartIndex, 0);
            QCOMPARE(
                sequence.frames
                    .constFirst()
                    .durationMilliseconds,
                timing.firstFrameMilliseconds);
            QCOMPARE(
                sequence.frames
                    .constLast()
                    .durationMilliseconds,
                timing.finalFrameMilliseconds);
            QCOMPARE(
                cycleDurationMilliseconds(sequence),
                timing.cycleMilliseconds);
        }

        const auto shadowWave =
            shadowLoaded.value()
                .animationSequence(
                    companion::PetAnimation::Waving,
                    1.0);
        QCOMPARE(
            shadowWave.frames
                .constFirst()
                .durationMilliseconds,
            120);
        QCOMPARE(
            shadowWave.frames
                .at(7)
                .durationMilliseconds,
            200);

        const auto compactIdle =
            compactLoaded.value()
                .animationSequence(
                    companion::PetAnimation::Idle,
                    1.0);
        QCOMPARE(
            compactIdle.frames
                .constFirst()
                .durationMilliseconds,
            160);
        QCOMPARE(
            compactIdle.frames
                .constLast()
                .durationMilliseconds,
            240);
    }

    void loadsSixteenByTenMacCompanionManifest()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString customRoot =
            temporary.filePath(
                QStringLiteral("custom"));
        QVERIFY(writePet(
            customRoot,
            QStringLiteral("mac-companion"),
            QStringLiteral("mac-companion"),
            QStringLiteral("Mac Companion"),
            16,
            10));

        companion::PetCatalog catalog({
            customRoot,
            temporary.filePath(
                QStringLiteral("native")),
            temporary.filePath(
                QStringLiteral("bundled")),
        });

        QVERIFY(catalog.reload().hasValue());
        QCOMPARE(catalog.pets().size(), 1);
        const auto& pet = catalog.pets().constFirst();
        QCOMPARE(
            pet.id,
            QStringLiteral("mac-companion"));
        QCOMPARE(pet.spriteColumns, 16);
        QCOMPARE(pet.spriteRows, 10);
        QCOMPARE(pet.sourceFrameSize, QSize(4, 4));
        QCOMPARE(
            pet.frameCount(
                companion::PetAnimation::Idle),
            16);
        QCOMPARE(
            pet.resolvedAnimation(
                companion::PetAnimation::Thinking),
            companion::PetAnimation::Running);
        QCOMPARE(
            pet.resolvedAnimation(
                companion::PetAnimation::Talking),
            companion::PetAnimation::Review);

        const auto sequence =
            pet.animationSequence(
                companion::PetAnimation::Review,
                1.0);
        QVERIFY(!sequence.frames.isEmpty());
        QCOMPARE(sequence.frames.constFirst().row, 8);
        QVERIFY(sequence.loopStartIndex > 0);
        QCOMPARE(
            sequence.frames
                .at(sequence.loopStartIndex)
                .row,
            0);
        for (const auto& frame : sequence.frames) {
            QVERIFY(frame.row >= 0);
            QVERIFY(frame.row < 10);
            QVERIFY(frame.column >= 0);
            QVERIFY(frame.column < 16);
        }
    }

    void definitionUsesManifestGeometryAndMacStateFallbacks()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString root =
            temporary.filePath(
                QStringLiteral("pets"));
        QVERIFY(writePet(
            root,
            QStringLiteral("compact"),
            QStringLiteral("compact"),
            QStringLiteral("Compact"),
            8,
            9));
        const QString manifestPath =
            QDir(root)
                .filePath(
                    QStringLiteral(
                        "compact/pet.json"));

        const auto loaded =
            companion::PetDefinition::load(
                manifestPath,
                companion::PetSourceKind::Custom);

        QVERIFY(loaded.hasValue());
        const auto& pet = loaded.value();
        QCOMPARE(pet.spriteColumns, 8);
        QCOMPARE(pet.spriteRows, 9);
        QCOMPARE(pet.sourceFrameSize, QSize(4, 4));
        QCOMPARE(
            pet.resolvedAnimation(
                companion::PetAnimation::Thinking),
            companion::PetAnimation::Running);
        QCOMPARE(
            pet.resolvedAnimation(
                companion::PetAnimation::Talking),
            companion::PetAnimation::Review);
        QCOMPARE(
            pet.frameCount(
                companion::PetAnimation::Idle),
            8);

        const auto sequence =
            pet.animationSequence(
                companion::PetAnimation::
                    GoalComplete,
                1.0);
        QVERIFY(!sequence.frames.isEmpty());
        QVERIFY(sequence.loopStartIndex > 0);
        QCOMPARE(
            sequence.frames
                .at(sequence.loopStartIndex)
                .row,
            0);
    }
};

QTEST_GUILESS_MAIN(PetCatalogTests)
#include "PetCatalogTests.moc"
