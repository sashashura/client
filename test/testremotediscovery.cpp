/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QtTest>
#include "testutils/syncenginetestutils.h"
#include <syncengine.h>
#include <localdiscoverytracker.h>

using namespace std::chrono_literals;
using namespace OCC;

struct FakeBrokenXmlPropfindReply : FakePropfindReply {
    FakeBrokenXmlPropfindReply(FileInfo &remoteRootFileInfo, QNetworkAccessManager::Operation op,
                               const QNetworkRequest &request, QObject *parent)
        : FakePropfindReply(remoteRootFileInfo, op, request, parent) {
        QVERIFY(payload.size() > 50);
        // turncate the XML
        payload.chop(20);
    }
};

struct MissingPermissionsPropfindReply : FakePropfindReply {
    MissingPermissionsPropfindReply(FileInfo &remoteRootFileInfo, QNetworkAccessManager::Operation op,
                               const QNetworkRequest &request, QObject *parent)
        : FakePropfindReply(remoteRootFileInfo, op, request, parent) {
        // If the propfind contains a single file without permissions, this is a server error
        const char toRemove[] = "<oc:permissions>RDNVCKW</oc:permissions>";
        auto pos = payload.indexOf(toRemove, payload.size()/2);
        QVERIFY(pos > 0);
        payload.remove(pos, sizeof(toRemove) - 1);
    }
};

enum ErrorKind : int {
    // Lower code are corresponding to HTML error code
    InvalidXML = 1000,
    Timeout,
};

Q_DECLARE_METATYPE(ErrorCategory)

class TestRemoteDiscovery : public QObject
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

    void testRemoteDiscoveryError_data()
    {
        qRegisterMetaType<ErrorCategory>();
        QTest::addColumn<int>("errorKind");
        QTest::addColumn<QString>("expectedErrorString");
        QTest::addColumn<bool>("syncSucceeds");

        QString itemErrorMessage = QStringLiteral("Internal Server Fake Error");

        QTest::newRow("400") << 400 << itemErrorMessage << false;
        QTest::newRow("401") << 401 << itemErrorMessage << false;
        QTest::newRow("403") << 403 << itemErrorMessage << true;
        QTest::newRow("404") << 404 << itemErrorMessage << true;
        QTest::newRow("500") << 500 << itemErrorMessage << true;
        QTest::newRow("503") << 503 << itemErrorMessage << true;
        // 200 should be an error since propfind should return 207
        QTest::newRow("200") << 200 << itemErrorMessage << false;
        QTest::newRow("InvalidXML") << +InvalidXML << "Unknown error" << false;
        QTest::newRow("Timeout") << +Timeout << "Operation canceled" << false;
    }


    // Check what happens when there is an error.
    void testRemoteDiscoveryError()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        QFETCH(int, errorKind);
        QFETCH(QString, expectedErrorString);
        QFETCH(bool, syncSucceeds);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);

        // Do Some change as well
        fakeFolder.localModifier().insert(QStringLiteral("A/z1"));
        fakeFolder.localModifier().insert(QStringLiteral("B/z1"));
        fakeFolder.localModifier().insert(QStringLiteral("C/z1"));
        fakeFolder.remoteModifier().insert(QStringLiteral("A/z2"));
        fakeFolder.remoteModifier().insert(QStringLiteral("B/z2"));
        fakeFolder.remoteModifier().insert(QStringLiteral("C/z2"));
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());

        auto oldLocalState = fakeFolder.currentLocalState();
        auto oldRemoteState = fakeFolder.currentRemoteState();

        QString errorFolder = fakeFolder.account()->davPath() + QStringLiteral("B");
        QString fatalErrorPrefix = QStringLiteral("Server replied with an error while reading directory 'B' : ");
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *)
                -> QNetworkReply *{
            if (req.attribute(QNetworkRequest::CustomVerbAttribute) == "PROPFIND" && req.url().path().endsWith(errorFolder)) {
                if (errorKind == InvalidXML) {
                    return new FakeBrokenXmlPropfindReply(fakeFolder.remoteModifier(), op, req, this);
                } else if (errorKind == Timeout) {
                    return new FakeHangingReply(op, req, this);
                } else if (errorKind < 1000) {
                    return new FakeErrorReply(op, req, this, errorKind);
                }
            }
            return nullptr;
        });

        // So the test that test timeout finishes fast
        QScopedValueRollback<std::chrono::seconds> setHttpTimeout(AbstractNetworkJob::httpTimeout, errorKind == Timeout ? 1s : 10000s);

        ItemCompletedSpy completeSpy(fakeFolder);
        QSignalSpy errorSpy(&fakeFolder.syncEngine(), &SyncEngine::syncError);
        QCOMPARE(fakeFolder.applyLocalModificationsAndSync(), syncSucceeds);

        // The folder B should not have been sync'ed (and in particular not removed)
        QCOMPARE(oldLocalState.children["B"], fakeFolder.currentLocalState().children["B"]);
        QCOMPARE(oldRemoteState.children["B"], fakeFolder.currentRemoteState().children["B"]);
        if (!syncSucceeds) {
            QCOMPARE(errorSpy.size(), 1);
            QCOMPARE(errorSpy[0][0].toString(), QString(fatalErrorPrefix + expectedErrorString));
        } else {
            QCOMPARE(completeSpy.findItem("B")->_instruction, CSYNC_INSTRUCTION_IGNORE);
            QVERIFY(completeSpy.findItem("B")->_errorString.contains(expectedErrorString));

            // The other folder should have been sync'ed as the sync just ignored the faulty dir
            QCOMPARE(fakeFolder.currentRemoteState().children["A"], fakeFolder.currentLocalState().children["A"]);
            QCOMPARE(fakeFolder.currentRemoteState().children["C"], fakeFolder.currentLocalState().children["C"]);
            QCOMPARE(completeSpy.findItem("A/z1")->_instruction, CSYNC_INSTRUCTION_NEW);
        }

        //
        // Check the same discovery error on the sync root
        //
        errorFolder = fakeFolder.account()->davPath();
        fatalErrorPrefix = QLatin1String("Server replied with an error while reading directory '' : ");
        errorSpy.clear();
        QVERIFY(!fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(errorSpy.size(), 1);
        QCOMPARE(errorSpy[0][0].toString(), QString(fatalErrorPrefix + expectedErrorString));
    }

    void testMissingData()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder(FileInfo(), vfsMode, filesAreDehydrated);
        fakeFolder.remoteModifier().insert(QStringLiteral("good"));
        fakeFolder.remoteModifier().insert(QStringLiteral("noetag"));
        fakeFolder.remoteModifier().find("noetag")->etag.clear();
        fakeFolder.remoteModifier().insert(QStringLiteral("nofileid"));
        fakeFolder.remoteModifier().find("nofileid")->fileId.clear();
        fakeFolder.remoteModifier().mkdir(QStringLiteral("nopermissions"));
        fakeFolder.remoteModifier().insert(QStringLiteral("nopermissions/A"));

        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *)
                                         -> QNetworkReply * {
            if (req.attribute(QNetworkRequest::CustomVerbAttribute) == "PROPFIND" && req.url().path().endsWith(QLatin1String("nopermissions")))
                return new MissingPermissionsPropfindReply(fakeFolder.remoteModifier(), op, req, this);
            return nullptr;
        });

        ItemCompletedSpy completeSpy(fakeFolder);
        QVERIFY(!fakeFolder.applyLocalModificationsAndSync());

        QCOMPARE(completeSpy.findItem("good")->_instruction, CSYNC_INSTRUCTION_NEW);
        QCOMPARE(completeSpy.findItem("noetag")->_instruction, CSYNC_INSTRUCTION_ERROR);
        QCOMPARE(completeSpy.findItem("nofileid")->_instruction, CSYNC_INSTRUCTION_ERROR);
        QCOMPARE(completeSpy.findItem("nopermissions")->_instruction, CSYNC_INSTRUCTION_NEW);
        QCOMPARE(completeSpy.findItem("nopermissions/A")->_instruction, CSYNC_INSTRUCTION_ERROR);
        QVERIFY(completeSpy.findItem("noetag")->_errorString.contains("etag"));
        QVERIFY(completeSpy.findItem("nofileid")->_errorString.contains("file id"));
        QVERIFY(completeSpy.findItem("nopermissions/A")->_errorString.contains("permissions"));
    }
};

QTEST_GUILESS_MAIN(TestRemoteDiscovery)
#include "testremotediscovery.moc"
