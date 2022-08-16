/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */
#include <syncengine.h>
#include <common/syncjournaldb.h>

#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"

#include <QtTest>

using namespace OCC;

class TestUploadReset : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase_data()
    {
        QTest::addColumn<Vfs::Mode>("vfsMode");
        QTest::addColumn<bool>("filesAreDehydrated");

        QTest::newRow("Vfs::Off") << Vfs::Off << false;

        if (isVfsPluginAvailable(Vfs::WindowsCfApi)) {
            QTest::newRow("Vfs::WindowsCfApi dehydrated") << Vfs::WindowsCfApi << true;

            // TODO: the hydrated version will fail due to an issue in the winvfs plugin, so leave it disabled for now.
            // QTest::newRow("Vfs::WindowsCfApi hydrated") << Vfs::WindowsCfApi << false;
        } else if (Utility::isWindows()) {
            QWARN("Skipping Vfs::WindowsCfApi");
        }
    }


    // Verify that the chunked transfer eventually gets reset with the new chunking
    void testFileUploadNg()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);

        auto httpErrorCodesThatResetFailingChunkedUploadsCapabilities = [](const QVariantList &codes) {
            auto cap = TestUtils::testCapabilities();
            auto dav = cap[QStringLiteral("dav")].toMap();
            dav.insert({ { "httpErrorCodesThatResetFailingChunkedUploads", codes } });
            cap[QStringLiteral("dav")] = dav;
            return cap;
        };
        fakeFolder.syncEngine().account()->setCapabilities(httpErrorCodesThatResetFailingChunkedUploadsCapabilities({ 500 }));

        const int size = 100 * 1000 * 1000; // 100 MB
        fakeFolder.localModifier().insert(QStringLiteral("A/a0"), size);
        QDateTime modTime = QDateTime::currentDateTime();
        fakeFolder.localModifier().setModTime(QStringLiteral("A/a0"), modTime);

        // Create a transfer id, so we can make the final MOVE fail
        SyncJournalDb::UploadInfo uploadInfo;
        uploadInfo._transferid = 1;
        uploadInfo._valid = true;
        uploadInfo._modtime = Utility::qDateTimeToTime_t(modTime);
        uploadInfo._size = size;
        fakeFolder.syncEngine().journal()->setUploadInfo(QStringLiteral("A/a0"), uploadInfo);

        fakeFolder.uploadState().mkdir(QStringLiteral("1"));
        fakeFolder.serverErrorPaths().append(QStringLiteral("1/.file"));

        QVERIFY(!fakeFolder.applyLocalModificationsAndSync());

        uploadInfo = fakeFolder.syncEngine().journal()->getUploadInfo(QStringLiteral("A/a0"));
        QCOMPARE(uploadInfo._errorCount, 1);
        QCOMPARE(uploadInfo._transferid, 1U);

        fakeFolder.syncEngine().journal()->wipeErrorBlacklist();
        QVERIFY(!fakeFolder.applyLocalModificationsAndSync());

        uploadInfo = fakeFolder.syncEngine().journal()->getUploadInfo(QStringLiteral("A/a0"));
        QCOMPARE(uploadInfo._errorCount, 2);
        QCOMPARE(uploadInfo._transferid, 1U);

        fakeFolder.syncEngine().journal()->wipeErrorBlacklist();
        QVERIFY(!fakeFolder.applyLocalModificationsAndSync());

        uploadInfo = fakeFolder.syncEngine().journal()->getUploadInfo(QStringLiteral("A/a0"));
        QCOMPARE(uploadInfo._errorCount, 3);
        QCOMPARE(uploadInfo._transferid, 1U);

        fakeFolder.syncEngine().journal()->wipeErrorBlacklist();
        QVERIFY(!fakeFolder.applyLocalModificationsAndSync());

        uploadInfo = fakeFolder.syncEngine().journal()->getUploadInfo(QStringLiteral("A/a0"));
        QCOMPARE(uploadInfo._errorCount, 0);
        QCOMPARE(uploadInfo._transferid, 0U);
        QVERIFY(!uploadInfo._valid);
    }
};

QTEST_GUILESS_MAIN(TestUploadReset)
#include "testuploadreset.moc"
