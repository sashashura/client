/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"
#include <QtTest>
#include <syncengine.h>

using namespace OCC;

bool itemSuccessful(const ItemCompletedSpy &spy, const QString &path, const SyncInstructions instr)
{
    auto item = spy.findItem(path);
    return item->_status == SyncFileItem::Success && item->_instruction == instr;
}

bool itemConflict(const ItemCompletedSpy &spy, const QString &path)
{
    auto item = spy.findItem(path);
    return item->_status == SyncFileItem::Conflict && item->_instruction == CSYNC_INSTRUCTION_CONFLICT;
}

bool itemSuccessfulMove(const ItemCompletedSpy &spy, const QString &path)
{
    return itemSuccessful(spy, path, CSYNC_INSTRUCTION_RENAME);
}

QStringList findConflicts(const FileInfo &dir)
{
    QStringList conflicts;
    for (const auto &item : dir.children) {
        if (item.name.contains("(conflicted copy")) {
            conflicts.append(item.path());
        }
    }
    return conflicts;
}

bool expectAndWipeConflict(FileModifier &local, FileInfo state, const QString path)
{
    PathComponents pathComponents(path);
    auto base = state.find(pathComponents.parentDirComponents());
    if (!base)
        return false;
    for (const auto &item : qAsConst(base->children)) {
        if (item.name.startsWith(pathComponents.fileName()) && item.name.contains("(conflicted copy")) {
            local.remove(item.path());
            return true;
        }
    }
    return false;
}

class TestSyncMove : public QObject
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

    void testRemoteChangeInMovedFolder()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        // issue #5192
        FakeFolder fakeFolder(FileInfo { QString(), { FileInfo { QStringLiteral("folder"), { FileInfo { QStringLiteral("folderA"), { { QStringLiteral("file.txt"), 400 } } }, QStringLiteral("folderB") } } } }, vfsMode, filesAreDehydrated);

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // Edit a file in a moved directory.
        fakeFolder.remoteModifier().setContents("folder/folderA/file.txt", FileModifier::DefaultFileSize, 'a');
        fakeFolder.remoteModifier().rename("folder/folderA", "folder/folderB/folderA");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        auto oldState = fakeFolder.currentLocalState();
        QVERIFY(oldState.find("folder/folderB/folderA/file.txt"));
        QVERIFY(!oldState.find("folder/folderA/file.txt"));

        // This sync should not remove the file
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentLocalState(), oldState);
    }

    void testSelectiveSyncMovedFolder()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        // issue #5224
        FakeFolder fakeFolder(FileInfo { QString(), { FileInfo { QStringLiteral("parentFolder"), { FileInfo { QStringLiteral("subFolderA"), { { QStringLiteral("fileA.txt"), 400 } } }, FileInfo { QStringLiteral("subFolderB"), { { QStringLiteral("fileB.txt"), 400 } } } } } } }, vfsMode, filesAreDehydrated);

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        auto expectedServerState = fakeFolder.currentRemoteState();

        // Remove subFolderA with selectiveSync:
        fakeFolder.syncEngine().journal()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList,
            { "parentFolder/subFolderA/" });
        fakeFolder.syncEngine().journal()->schedulePathForRemoteDiscovery(QByteArrayLiteral("parentFolder/subFolderA/"));

        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        {
            // Nothing changed on the server
            QCOMPARE(fakeFolder.currentRemoteState(), expectedServerState);
            // The local state should not have subFolderA
            auto remoteState = fakeFolder.currentRemoteState();
            remoteState.remove("parentFolder/subFolderA");
            QCOMPARE(fakeFolder.currentLocalState(), remoteState);
        }

        // Rename parentFolder on the server
        fakeFolder.remoteModifier().rename("parentFolder", "parentFolderRenamed");
        expectedServerState = fakeFolder.currentRemoteState();
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        {
            QCOMPARE(fakeFolder.currentRemoteState(), expectedServerState);
            auto remoteState = fakeFolder.currentRemoteState();
            // The subFolderA should still be there on the server.
            QVERIFY(remoteState.find("parentFolderRenamed/subFolderA/fileA.txt"));
            // But not on the client because of the selective sync
            remoteState.remove("parentFolderRenamed/subFolderA");
            QCOMPARE(fakeFolder.currentLocalState(), remoteState);
        }

        // Rename it again, locally this time.
        fakeFolder.localModifier().rename("parentFolderRenamed", "parentThirdName");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        {
            auto remoteState = fakeFolder.currentRemoteState();
            // The subFolderA should still be there on the server.
            QVERIFY(remoteState.find("parentThirdName/subFolderA/fileA.txt"));
            // But not on the client because of the selective sync
            remoteState.remove("parentThirdName/subFolderA");
            QCOMPARE(fakeFolder.currentLocalState(), remoteState);

            expectedServerState = fakeFolder.currentRemoteState();
            ItemCompletedSpy completeSpy(fakeFolder);
            QVERIFY(fakeFolder.applyLocalModificationsAndSync()); // This sync should do nothing
            QCOMPARE(completeSpy.count(), 0);

            QCOMPARE(fakeFolder.currentRemoteState(), expectedServerState);
            QCOMPARE(fakeFolder.currentLocalState(), remoteState);
        }
    }

    void testLocalMoveDetection()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);
        fakeFolder.account()->setCapabilities(TestUtils::testCapabilities(CheckSums::Algorithm::ADLER32));

        OperationCounter counter(fakeFolder);

        // For directly editing the remote checksum
        FileInfo &remoteInfo = fakeFolder.remoteModifier();
        // Simple move causing a remote rename
        fakeFolder.localModifier().rename("A/a1", "A/a1m");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), remoteInfo);
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(remoteInfo));
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nPUT, 0);
        QCOMPARE(counter.nMOVE, 1);
        QCOMPARE(counter.nDELETE, 0);
        counter.reset();

        // Move-and-change, mtime+size, causing a upload and delete
        QVERIFY(fakeFolder.currentLocalState().find("A/a2")->isDehydratedPlaceholder == filesAreDehydrated); // no-one touched it, so the hydration state should be the same as the initial state
        auto mt = fakeFolder.currentLocalState().find("A/a2")->lastModified();
        QVERIFY(mt.toSecsSinceEpoch() + 1 < QDateTime::currentDateTime().toSecsSinceEpoch());
        fakeFolder.localModifier().rename("A/a2", "A/a2m");
        fakeFolder.localModifier().setContents("A/a2m", fakeFolder.remoteModifier().contentSize + 1, 'x');
        fakeFolder.localModifier().setModTime("A/a2m", mt.addSecs(1));
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a2m")->isDehydratedPlaceholder); // We overwrote all data in the file, so whatever the state was before, it is no longer dehydrated
        QCOMPARE(fakeFolder.currentLocalState(), remoteInfo);
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(remoteInfo));
        QCOMPARE(counter.nGET, filesAreDehydrated ? 1 : 0); // on winvfs, with a dehydrated file, the os will try to hydrate the file before we write to it. When the file is hydrated, it doesn't need to be fetched.
        QCOMPARE(counter.nMOVE, 0); // we cannot detect moves (and we didn't implement it yet in winvfs), so ...
        QCOMPARE(counter.nDELETE, 1); // ... the file just disappears, and ...
        QCOMPARE(counter.nPUT, 1); // ... another file (with just 1 byte difference) appears somewhere else. Coincidence.
        counter.reset();
        // Move-and-change, mtime+content only
        fakeFolder.localModifier().rename("B/b1", "B/b1m");
        fakeFolder.localModifier().setContents("B/b1m", FileModifier::DefaultFileSize, 'C');
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), remoteInfo);
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(remoteInfo));
        QCOMPARE(counter.nPUT, 1);
        QCOMPARE(counter.nDELETE, 1);
        counter.reset();

        // Move-and-change, size+content only
        auto mtime = fakeFolder.remoteModifier().find("B/b2")->lastModified();
        fakeFolder.localModifier().rename("B/b2", "B/b2m");
        fakeFolder.localModifier().appendByte("B/b2m");
        fakeFolder.localModifier().setModTime("B/b2m", mtime);
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), remoteInfo);
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(remoteInfo));
        if (vfsMode == Vfs::Off) {
            QCOMPARE(counter.nGET, 0); // b2m is detected as a *new* file, so we don't need to fetch the contents
            QCOMPARE(counter.nMOVE, 0); // content differs, so not a move
        } else {
            // with winvfs, we don't implement the CF_CALLBACK_TYPE_NOTIFY_RENAME callback, so:
            QCOMPARE(counter.nGET, 1); // callback to get the metadata/contents of b2m
            QCOMPARE(counter.nMOVE, 0); // no callback, contents differ, so not a move
        }
        QCOMPARE(counter.nPUT, 1); // upload b2m
        QCOMPARE(counter.nDELETE, 1); // delete b2
        counter.reset();

        // WinVFS handles this just fine.
        if (vfsMode == Vfs::Off) {
            // Move-and-change, content only -- c1 has no checksum, so we fail to detect this!
            // NOTE: This is an expected failure.
            mtime = fakeFolder.remoteModifier().find("C/c1")->lastModified();
            auto size = fakeFolder.currentRemoteState().find("C/c1")->contentSize;
            fakeFolder.localModifier().rename("C/c1", "C/c1m");
            fakeFolder.localModifier().setContents("C/c1m", size, 'C');
            fakeFolder.localModifier().setModTime("C/c1m", mtime);
            QVERIFY(fakeFolder.applyLocalModificationsAndSync());
            QCOMPARE(counter.nPUT, 0);
            QCOMPARE(counter.nDELETE, 0);
            QVERIFY(!(fakeFolder.currentLocalState() == remoteInfo));
            counter.reset();
        }

        // cleanup, and upload a file that will have a checksum in the db
        if (vfsMode == Vfs::Off) {
            // rename happened in the previous test
            fakeFolder.localModifier().remove("C/c1m");
        } else {
            // no rename happened, remove the "original"
            fakeFolder.localModifier().remove("C/c1");
        }
        fakeFolder.localModifier().insert("C/c3", 13, 'E'); // 13, because c1 (and c2) have a size of 24 bytes
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), remoteInfo);
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(remoteInfo));
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nMOVE, 0);
        QCOMPARE(counter.nPUT, 1);
        QCOMPARE(counter.nDELETE, 1);
        counter.reset();

        // Move-and-change, content only, this time while having a checksum
        mtime = fakeFolder.remoteModifier().find("C/c3")->lastModified();
        fakeFolder.localModifier().rename("C/c3", "C/c3m");
        fakeFolder.localModifier().setContents("C/c3m", FileModifier::DefaultFileSize, 'C');
        fakeFolder.localModifier().setModTime("C/c3m", mtime);
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nMOVE, 0);
        QCOMPARE(counter.nPUT, 1);
        QCOMPARE(counter.nDELETE, 1);
        QCOMPARE(fakeFolder.currentLocalState(), remoteInfo);
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(remoteInfo));
        counter.reset();
    }

    void testDuplicateFileId_data()
    {
        QTest::addColumn<QString>("prefix");

        // There have been bugs related to how the original
        // folder and the folder with the duplicate tree are
        // ordered. Test both cases here.
        QTest::newRow("first ordering") << "O"; // "O" > "A"
        QTest::newRow("second ordering") << "0"; // "0" < "A"
    }

    // If the same folder is shared in two different ways with the same
    // user, the target user will see duplicate file ids. We need to make
    // sure the move detection and sync still do the right thing in that
    // case.
    void testDuplicateFileId()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);
        QFETCH(QString, prefix);

        if (filesAreDehydrated) {
            QSKIP("This test expects to be able to modify local files on disk, which does not work with dehydrated files.");
        }

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);
        auto &remote = fakeFolder.remoteModifier();

        remote.mkdir("A/W");
        remote.insert("A/W/w1");
        remote.mkdir("A/Q");

        // Duplicate every entry in A under O/A
        remote.mkdir(prefix);
        remote.children[prefix].addChild(remote.children["A"]);

        // This already checks that the rename detection doesn't get
        // horribly confused if we add new files that have the same
        // fileid as existing ones
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        OperationCounter counter(fakeFolder);

        // Try a remote file move
        remote.rename("A/a1", "A/W/a1m");
        remote.rename(prefix + "/A/a1", prefix + "/A/W/a1m");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(counter.nGET, 0);

        // And a remote directory move
        remote.rename("A/W", "A/Q/W");
        remote.rename(prefix + "/A/W", prefix + "/A/Q/W");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(counter.nGET, 0);

        // Partial file removal (in practice, A/a2 may be moved to O/a2, but we don't care)
        remote.rename(prefix + "/A/a2", prefix + "/a2");
        remote.remove("A/a2");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(counter.nGET, 0);

        // Local change plus remote move at the same time
        fakeFolder.localModifier().appendByte(prefix + "/a2");
        remote.rename(prefix + "/a2", prefix + "/a3");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(counter.nGET, 1);
        counter.reset();

        // remove localy, and remote move at the same time
        fakeFolder.localModifier().remove("A/Q/W/a1m");
        remote.rename("A/Q/W/a1m", "A/Q/W/a1p");
        remote.rename(prefix + "/A/Q/W/a1m", prefix + "/A/Q/W/a1p");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(counter.nGET, 1);
        counter.reset();
    }

    void testMovePropagation()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);
        auto &local = fakeFolder.localModifier();
        auto &remote = fakeFolder.remoteModifier();

        OperationCounter counter(fakeFolder);

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        counter.reset();

        // Move
        {
            local.rename("A/a1", "A/a1m");
            remote.rename("B/b1", "B/b1m");
            ItemCompletedSpy completeSpy(fakeFolder);
            QVERIFY(fakeFolder.applyLocalModificationsAndSync());
            QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
            QCOMPARE(counter.nGET, 0);
            QCOMPARE(counter.nPUT, 0);
            QCOMPARE(counter.nMOVE, 1);
            QCOMPARE(counter.nDELETE, 0);
            QVERIFY(itemSuccessfulMove(completeSpy, "A/a1m"));
            QVERIFY(itemSuccessfulMove(completeSpy, "B/b1m"));
            QCOMPARE(completeSpy.findItem("A/a1m")->_file, QStringLiteral("A/a1"));
            QCOMPARE(completeSpy.findItem("A/a1m")->_renameTarget, QStringLiteral("A/a1m"));
            QCOMPARE(completeSpy.findItem("B/b1m")->_file, QStringLiteral("B/b1"));
            QCOMPARE(completeSpy.findItem("B/b1m")->_renameTarget, QStringLiteral("B/b1m"));
            counter.reset();
        }

        // Touch+Move on same side
        local.rename("A/a2", "A/a2m");
        local.setContents("A/a2m", FileModifier::DefaultFileSize, 'A');
        remote.rename("B/b2", "B/b2m");
        remote.setContents("B/b2m", FileModifier::DefaultFileSize, 'A');
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
        QCOMPARE(counter.nGET, 1);
        QCOMPARE(counter.nPUT, 1);
        QCOMPARE(counter.nMOVE, 0);
        QCOMPARE(counter.nDELETE, 1);
        QCOMPARE(remote.find("A/a2m")->contentChar, 'A');
        QCOMPARE(remote.find("B/b2m")->contentChar, 'A');
        counter.reset();

        // Touch+Move on opposite sides
        local.rename("A/a1m", "A/a1m2");
        remote.setContents("A/a1m", FileModifier::DefaultFileSize, 'B');
        remote.rename("B/b1m", "B/b1m2");
        local.setContents("B/b1m", FileModifier::DefaultFileSize, 'B');
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
        if (vfsMode == Vfs::Off) {
            QCOMPARE(counter.nGET, 2);
            QCOMPARE(counter.nPUT, 2);
            QCOMPARE(counter.nMOVE, 0);
            QCOMPARE(counter.nDELETE, 0);
        } else {
            QCOMPARE(counter.nGET, 0);
            QCOMPARE(counter.nPUT, 1); // the setContents for the "new" file b1m
            QCOMPARE(counter.nMOVE, 1); // the rename of a1m to a1m2
            QCOMPARE(counter.nDELETE, 0);
        }

        if (vfsMode != Vfs::Off) {
            QSKIP("Behaviour for any VFS is different at this point compared to no-VFS");
        }

        // All these files existing afterwards is debatable. Should we propagate
        // the rename in one direction and grab the new contents in the other?
        // Currently there's no propagation job that would do that, and this does
        // at least not lose data.
        QVERIFY(remote.find("A/a1m")->contentChar == 'B');
        QVERIFY(remote.find("B/b1m")->contentChar == 'B');
        QVERIFY(remote.find("A/a1m2")->contentChar == 'W');
        QVERIFY(remote.find("B/b1m2")->contentChar == 'W');
        QCOMPARE(remote.find("A/a1m")->contentChar, 'B');
        QCOMPARE(remote.find("B/b1m")->contentChar, 'B');
        QCOMPARE(remote.find("A/a1m2")->contentChar, 'W');
        QCOMPARE(remote.find("B/b1m2")->contentChar, 'W');
        counter.reset();

        // Touch+create on one side, move on the other
        {
            local.appendByte("A/a1m");
            local.insert("A/a1mt");
            remote.rename("A/a1m", "A/a1mt");
            remote.appendByte("B/b1m");
            remote.insert("B/b1mt");
            local.rename("B/b1m", "B/b1mt");
            ItemCompletedSpy completeSpy(fakeFolder);
            QVERIFY(fakeFolder.applyLocalModificationsAndSync());
            // First check the counters:
            QCOMPARE(counter.nGET, 3);
            QCOMPARE(counter.nPUT, 1);
            QCOMPARE(counter.nMOVE, 0);
            QCOMPARE(counter.nDELETE, 0);
            // Ok, now we can remove the conflicting files. This needs disk access, so it might trigger server interaction. (Hence checking the counters before we do this.)
            QVERIFY(expectAndWipeConflict(local, fakeFolder.currentLocalState(), "A/a1mt"));
            QVERIFY(expectAndWipeConflict(local, fakeFolder.currentLocalState(), "B/b1mt"));
            QVERIFY(fakeFolder.applyLocalModificationsAndSync());
            // Now we can compare the clean-up states:
            QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
            QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
            QVERIFY(itemSuccessful(completeSpy, "A/a1m", CSYNC_INSTRUCTION_NEW));
            QVERIFY(itemSuccessful(completeSpy, "B/b1m", CSYNC_INSTRUCTION_NEW));
            QVERIFY(itemConflict(completeSpy, "A/a1mt"));
            QVERIFY(itemConflict(completeSpy, "B/b1mt"));
            counter.reset();
        }

        // Create new on one side, move to new on the other
        {
            local.insert("A/a1N", 13);
            remote.rename("A/a1mt", "A/a1N");
            remote.insert("B/b1N", 13);
            local.rename("B/b1mt", "B/b1N");
            ItemCompletedSpy completeSpy(fakeFolder);
            QVERIFY(fakeFolder.applyLocalModificationsAndSync());
            // First check the counters:
            QCOMPARE(counter.nGET, 2);
            QCOMPARE(counter.nPUT, 0);
            QCOMPARE(counter.nMOVE, 0);
            QCOMPARE(counter.nDELETE, 1);
            // Ok, now we can remove the conflicting files. This needs disk access, so it might trigger server interaction. (Hence checking the counters before we do this.)
            QVERIFY(expectAndWipeConflict(local, fakeFolder.currentLocalState(), "A/a1N"));
            QVERIFY(expectAndWipeConflict(local, fakeFolder.currentLocalState(), "B/b1N"));
            QVERIFY(fakeFolder.applyLocalModificationsAndSync());
            // Now we can compare the clean-up states:
            QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
            QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
            QVERIFY(itemSuccessful(completeSpy, "A/a1mt", CSYNC_INSTRUCTION_REMOVE));
            QVERIFY(itemSuccessful(completeSpy, "B/b1mt", CSYNC_INSTRUCTION_REMOVE));
            QVERIFY(itemConflict(completeSpy, "A/a1N"));
            QVERIFY(itemConflict(completeSpy, "B/b1N"));
            counter.reset();
        }

        // Local move, remote move
        local.rename("C/c1", "C/c1mL");
        remote.rename("C/c1", "C/c1mR");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        // end up with both files
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
        QCOMPARE(counter.nGET, 1);
        QCOMPARE(counter.nPUT, 1);
        QCOMPARE(counter.nMOVE, 0);
        QCOMPARE(counter.nDELETE, 0);

        // Rename/rename conflict on a folder
        counter.reset();
        remote.rename("C", "CMR");
        local.rename("C", "CML");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        // End up with both folders
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
        QCOMPARE(counter.nGET, 3); // 3 files in C
        QCOMPARE(counter.nPUT, 3);
        QCOMPARE(counter.nMOVE, 0);
        QCOMPARE(counter.nDELETE, 0);
        counter.reset();

        // Folder move
        {
            local.rename("A", "AM");
            remote.rename("B", "BM");
            ItemCompletedSpy completeSpy(fakeFolder);
            QVERIFY(fakeFolder.applyLocalModificationsAndSync());
            QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
            QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
            QCOMPARE(counter.nGET, 0);
            QCOMPARE(counter.nPUT, 0);
            QCOMPARE(counter.nMOVE, 1);
            QCOMPARE(counter.nDELETE, 0);
            QVERIFY(itemSuccessfulMove(completeSpy, "AM"));
            QVERIFY(itemSuccessfulMove(completeSpy, "BM"));
            QCOMPARE(completeSpy.findItem("AM")->_file, QStringLiteral("A"));
            QCOMPARE(completeSpy.findItem("AM")->_renameTarget, QStringLiteral("AM"));
            QCOMPARE(completeSpy.findItem("BM")->_file, QStringLiteral("B"));
            QCOMPARE(completeSpy.findItem("BM")->_renameTarget, QStringLiteral("BM"));
            counter.reset();
        }

        // Folder move with contents touched on the same side
        {
            local.setContents("AM/a2m", FileModifier::DefaultFileSize, 'C');
            // We must change the modtime for it is likely that it did not change between sync.
            // (Previous version of the client (<=2.5) would not need this because it was always doing
            // checksum comparison for all renames. But newer version no longer does it if the file is
            // renamed because the parent folder is renamed)
            local.setModTime("AM/a2m", QDateTime::currentDateTimeUtc().addDays(3));
            local.rename("AM", "A2");
            remote.setContents("BM/b2m", FileModifier::DefaultFileSize, 'C');
            remote.rename("BM", "B2");
            ItemCompletedSpy completeSpy(fakeFolder);
            QVERIFY(fakeFolder.applyLocalModificationsAndSync());
            QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
            QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
            QCOMPARE(counter.nGET, 1);
            QCOMPARE(counter.nPUT, 1);
            QCOMPARE(counter.nMOVE, 1);
            QCOMPARE(counter.nDELETE, 0);
            QCOMPARE(remote.find("A2/a2m")->contentChar, 'C');
            QCOMPARE(remote.find("B2/b2m")->contentChar, 'C');
            QVERIFY(itemSuccessfulMove(completeSpy, "A2"));
            QVERIFY(itemSuccessfulMove(completeSpy, "B2"));
            counter.reset();
        }

        // Folder rename with contents touched on the other tree
        remote.setContents("A2/a2m", FileModifier::DefaultFileSize, 'D');
        // setContents alone may not produce updated mtime if the test is fast
        // and since we don't use checksums here, that matters.
        remote.appendByte("A2/a2m");
        local.rename("A2", "A3");
        local.setContents("B2/b2m", FileModifier::DefaultFileSize, 'D');
        local.appendByte("B2/b2m");
        remote.rename("B2", "B3");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
        QCOMPARE(counter.nGET, 1);
        QCOMPARE(counter.nPUT, 1);
        QCOMPARE(counter.nMOVE, 1);
        QCOMPARE(counter.nDELETE, 0);
        QCOMPARE(remote.find("A3/a2m")->contentChar, 'D');
        QCOMPARE(remote.find("B3/b2m")->contentChar, 'D');
        counter.reset();

        // Folder rename with contents touched on both ends
        remote.setContents("A3/a2m", FileModifier::DefaultFileSize, 'R');
        remote.appendByte("A3/a2m");
        local.setContents("A3/a2m", FileModifier::DefaultFileSize, 'L');
        local.appendByte("A3/a2m");
        local.appendByte("A3/a2m");
        local.rename("A3", "A4");
        remote.setContents("B3/b2m", FileModifier::DefaultFileSize, 'R');
        remote.appendByte("B3/b2m");
        local.setContents("B3/b2m", FileModifier::DefaultFileSize, 'L');
        local.appendByte("B3/b2m");
        local.appendByte("B3/b2m");
        remote.rename("B3", "B4");
        QThread::sleep(1); // This test is timing-sensitive. No idea why, it's probably the modtime on the client side.
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        qDebug() << counter;
        QCOMPARE(counter.nGET, 2);
        QCOMPARE(counter.nPUT, 0);
        QCOMPARE(counter.nMOVE, 1);
        QCOMPARE(counter.nDELETE, 0);
        auto currentLocal = fakeFolder.currentLocalState();
        auto conflicts = findConflicts(currentLocal.children["A4"]);
        QCOMPARE(conflicts.size(), 1);
        for (const auto &c : qAsConst(conflicts)) {
            QCOMPARE(currentLocal.find(c)->contentChar, 'L');
            local.remove(c);
        }
        conflicts = findConflicts(currentLocal.children["B4"]);
        QCOMPARE(conflicts.size(), 1);
        for (const auto &c : qAsConst(conflicts)) {
            QCOMPARE(currentLocal.find(c)->contentChar, 'L');
            local.remove(c);
        }
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
        QCOMPARE(remote.find("A4/a2m")->contentChar, 'R');
        QCOMPARE(remote.find("B4/b2m")->contentChar, 'R');
        counter.reset();

        // Rename a folder and rename the contents at the same time
        local.rename("A4/a2m", "A4/a2m2");
        local.rename("A4", "A5");
        remote.rename("B4/b2m", "B4/b2m2");
        remote.rename("B4", "B5");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nPUT, 0);
        QCOMPARE(counter.nMOVE, 2);
        QCOMPARE(counter.nDELETE, 0);
    }

    // These renames can be troublesome on windows
    void testRenameCaseOnly()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);
        auto &local = fakeFolder.localModifier();
        auto &remote = fakeFolder.remoteModifier();

        OperationCounter counter(fakeFolder);

        local.rename("A/a1", "A/A1");
        remote.rename("A/a2", "A/A2");

        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), remote);
        QCOMPARE(printDbData(fakeFolder.dbState()), printDbData(fakeFolder.currentRemoteState()));
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nPUT, 0);
        QCOMPARE(counter.nMOVE, 1);
        QCOMPARE(counter.nDELETE, 0);
    }

    // Check interaction of moves with file type changes
    void testMoveAndTypeChange()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);
        auto &local = fakeFolder.localModifier();
        auto &remote = fakeFolder.remoteModifier();

        // Touch on one side, rename and mkdir on the other
        {
            local.appendByte("A/a1");
            remote.rename("A/a1", "A/a1mq");
            remote.mkdir("A/a1");
            remote.appendByte("B/b1");
            local.rename("B/b1", "B/b1mq");
            local.mkdir("B/b1");
            ItemCompletedSpy completeSpy(fakeFolder);
            QVERIFY(fakeFolder.applyLocalModificationsAndSync());
            // BUG: This doesn't behave right
            //QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        }
    }

    // https://github.com/owncloud/client/issues/6629#issuecomment-402450691
    // When a file is moved and the server mtime was not in sync, the local mtime should be kept
    void testMoveAndMTimeChange()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);
        OperationCounter counter(fakeFolder);

        // Changing the mtime on the server (without invalidating the etag)
        fakeFolder.remoteModifier().find("A/a1")->setLastModified(QDateTime::currentDateTime().addSecs(-50000));
        fakeFolder.remoteModifier().find("A/a2")->setLastModified(QDateTime::currentDateTime().addSecs(-40000));

        // Move a few files
        fakeFolder.remoteModifier().rename("A/a1", "A/a1_server_renamed");
        fakeFolder.localModifier().rename("A/a2", "A/a2_local_renamed");

        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nPUT, 0);
        QCOMPARE(counter.nMOVE, 1);
        QCOMPARE(counter.nDELETE, 0);

        // Another sync should do nothing
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nPUT, 0);
        QCOMPARE(counter.nMOVE, 1);
        QCOMPARE(counter.nDELETE, 0);

        // Check that everything other than the mtime is still equal:
        QVERIFY(fakeFolder.currentLocalState().equals(fakeFolder.currentRemoteState(), FileInfo::IgnoreLastModified));
    }

    // Test for https://github.com/owncloud/client/issues/6694
    void testInvertFolderHierarchy()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);
        fakeFolder.remoteModifier().mkdir("A/Empty");
        fakeFolder.remoteModifier().mkdir("A/Empty/Foo");
        fakeFolder.remoteModifier().mkdir("C/AllEmpty");
        fakeFolder.remoteModifier().mkdir("C/AllEmpty/Bar");
        fakeFolder.remoteModifier().insert("A/Empty/f1");
        fakeFolder.remoteModifier().insert("A/Empty/Foo/f2");
        fakeFolder.remoteModifier().mkdir("C/AllEmpty/f3");
        fakeFolder.remoteModifier().mkdir("C/AllEmpty/Bar/f4");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        OperationCounter counter(fakeFolder);

        // "Empty" is after "A", alphabetically
        fakeFolder.localModifier().rename("A/Empty", "Empty");
        fakeFolder.localModifier().rename("A", "Empty/A");

        // "AllEmpty" is before "C", alphabetically
        fakeFolder.localModifier().rename("C/AllEmpty", "AllEmpty");
        fakeFolder.localModifier().rename("C", "AllEmpty/C");

        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(counter.nDELETE, 0);
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nPUT, 0);

        // Now, the revert, but "crossed"
        fakeFolder.localModifier().rename("Empty/A", "A");
        fakeFolder.localModifier().rename("AllEmpty/C", "C");
        fakeFolder.localModifier().rename("Empty", "C/Empty");
        fakeFolder.localModifier().rename("AllEmpty", "A/AllEmpty");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(counter.nDELETE, 0);
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nPUT, 0);

        // Reverse on remote
        fakeFolder.remoteModifier().rename("A/AllEmpty", "AllEmpty");
        fakeFolder.remoteModifier().rename("C/Empty", "Empty");
        fakeFolder.remoteModifier().rename("C", "AllEmpty/C");
        fakeFolder.remoteModifier().rename("A", "Empty/A");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(counter.nDELETE, 0);
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nPUT, 0);
    }

    void testDeepHierarchy_data()
    {
        QTest::addColumn<bool>("local");
        QTest::newRow("remote") << false;
        QTest::newRow("local") << true;
    }

    void testDeepHierarchy()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);
        QFETCH(bool, local);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);
        FileModifier &modifier = local ? fakeFolder.localModifier() : fakeFolder.remoteModifier();

        modifier.mkdir("FolA");
        modifier.mkdir("FolA/FolB");
        modifier.mkdir("FolA/FolB/FolC");
        modifier.mkdir("FolA/FolB/FolC/FolD");
        modifier.mkdir("FolA/FolB/FolC/FolD/FolE");
        modifier.insert("FolA/FileA.txt");
        modifier.insert("FolA/FolB/FileB.txt");
        modifier.insert("FolA/FolB/FolC/FileC.txt");
        modifier.insert("FolA/FolB/FolC/FolD/FileD.txt");
        modifier.insert("FolA/FolB/FolC/FolD/FolE/FileE.txt");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        OperationCounter counter(fakeFolder);

        modifier.insert("FolA/FileA2.txt");
        modifier.insert("FolA/FolB/FileB2.txt");
        modifier.insert("FolA/FolB/FolC/FileC2.txt");
        modifier.insert("FolA/FolB/FolC/FolD/FileD2.txt");
        modifier.insert("FolA/FolB/FolC/FolD/FolE/FileE2.txt");
        modifier.rename("FolA", "FolA_Renamed");
        modifier.rename("FolA_Renamed/FolB", "FolB_Renamed");
        modifier.rename("FolB_Renamed/FolC", "FolA");
        modifier.rename("FolA/FolD", "FolA/FolD_Renamed");
        modifier.mkdir("FolB_Renamed/New");
        modifier.rename("FolA/FolD_Renamed/FolE", "FolB_Renamed/New/FolE");
        auto expected = local ? fakeFolder.currentLocalState() : fakeFolder.currentRemoteState();
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(counter.nDELETE, local ? 1 : 0); // FolC was is renamed to an existing name, so it is not considered as renamed
        // There was 5 inserts
        QCOMPARE(counter.nGET, local || filesAreDehydrated ? 0 : 5);
        QCOMPARE(counter.nPUT, local ? 5 : 0);
    }

    void renameOnBothSides()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);
        OperationCounter counter(fakeFolder);

        // Test that renaming a file within a directory that was renamed on the other side actually do a rename.

        // 1) move the folder alphabeticaly before
        fakeFolder.remoteModifier().rename("A/a1", "A/a1m");
        fakeFolder.localModifier().rename("A", "_A");
        fakeFolder.localModifier().rename("B/b1", "B/b1m");
        fakeFolder.remoteModifier().rename("B", "_B");

        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentRemoteState(), fakeFolder.currentRemoteState());
        QVERIFY(fakeFolder.currentRemoteState().find("_A/a1m"));
        QVERIFY(fakeFolder.currentRemoteState().find("_B/b1m"));
        QCOMPARE(counter.nDELETE, 0);
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nPUT, 0);
        QCOMPARE(counter.nMOVE, 2);
        counter.reset();

        // 2) move alphabetically after
        fakeFolder.remoteModifier().rename("_A/a2", "_A/a2m");
        fakeFolder.localModifier().rename("_B/b2", "_B/b2m");
        fakeFolder.localModifier().rename("_A", "S/A");
        fakeFolder.remoteModifier().rename("_B", "S/B");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentRemoteState(), fakeFolder.currentRemoteState());
        QVERIFY(fakeFolder.currentRemoteState().find("S/A/a2m"));
        QVERIFY(fakeFolder.currentRemoteState().find("S/B/b2m"));
        QCOMPARE(counter.nDELETE, 0);
        QCOMPARE(counter.nGET, 0);
        QCOMPARE(counter.nPUT, 0);
        QCOMPARE(counter.nMOVE, 2);
    }

    void moveFileToDifferentFolderOnBothSides()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder(FileInfo::A12_B12_C12_S12(), vfsMode, filesAreDehydrated);
        OperationCounter counter(fakeFolder);

        QCOMPARE(fakeFolder.currentLocalState().find("B/b1")->isDehydratedPlaceholder, filesAreDehydrated);
        QCOMPARE(fakeFolder.currentLocalState().find("B/b2")->isDehydratedPlaceholder, filesAreDehydrated);

        // Test that moving a file within to different folder on both side does the right thing.

        fakeFolder.remoteModifier().rename("B/b1", "A/b1");
        fakeFolder.localModifier().rename("B/b1", "C/b1");

        fakeFolder.localModifier().rename("B/b2", "A/b2");
        fakeFolder.remoteModifier().rename("B/b2", "C/b2");

        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentRemoteState(), fakeFolder.currentRemoteState());
        // The easy checks: the server always has the data, so it can successfully move the files:
        QVERIFY(fakeFolder.currentRemoteState().find("A/b1"));
        QVERIFY(fakeFolder.currentRemoteState().find("C/b2"));
        // Either the client has hydrated files, in which case it will upload the data to the target locations;
        // or the files were dehydrated, so it has to remove the files. (No data-loss in the latter case: the
        // files were dehydrated, so there was no data anyway.)
        QVERIFY(fakeFolder.currentRemoteState().find("C/b1") || filesAreDehydrated);
        QVERIFY(fakeFolder.currentRemoteState().find("A/b2") || filesAreDehydrated);

        QCOMPARE(counter.nMOVE, 0); // Unfortunately, we can't really make a move in this case
        QCOMPARE(counter.nGET, filesAreDehydrated ? 0 : 2);
        QCOMPARE(counter.nPUT, filesAreDehydrated ? 0 : 2);
        QCOMPARE(counter.nDELETE, 0);
        counter.reset();
    }

    // Test that deletes don't run before renames
    void testRenameParallelism()
    {
        QFETCH_GLOBAL(Vfs::Mode, vfsMode);
        QFETCH_GLOBAL(bool, filesAreDehydrated);

        FakeFolder fakeFolder({ FileInfo {} }, vfsMode, filesAreDehydrated);
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/file");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        {
            auto localState = fakeFolder.currentLocalState();
            FileInfo *localFile = localState.find({ "A/file" });
            QVERIFY(localFile != nullptr); // check if the file exists
            QCOMPARE(vfsMode == Vfs::Off || localFile->isDehydratedPlaceholder, vfsMode == Vfs::Off || filesAreDehydrated);

            auto remoteState = fakeFolder.currentRemoteState();
            FileInfo *remoteFile = remoteState.find({ "A/file" });
            QVERIFY(remoteFile != nullptr);
            QCOMPARE(localFile->lastModified(), remoteFile->lastModified());

            QCOMPARE(localState, remoteState);
        }

        fakeFolder.localModifier().mkdir("B");
        fakeFolder.localModifier().rename("A/file", "B/file");
        fakeFolder.localModifier().remove("A");
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        {
            auto localState = fakeFolder.currentLocalState();
            QVERIFY(localState.find("A/file") == nullptr); // check if the file is gone
            QVERIFY(localState.find("A") == nullptr); // check if the directory is gone
            FileInfo *localFile = localState.find({ "B/file" });
            QVERIFY(localFile != nullptr); // check if the file exists
            QCOMPARE(vfsMode == Vfs::Off || localFile->isDehydratedPlaceholder, vfsMode == Vfs::Off || filesAreDehydrated);

            auto remoteState = fakeFolder.currentRemoteState();
            FileInfo *remoteFile = remoteState.find({ "B/file" });
            QVERIFY(remoteFile != nullptr);
            QCOMPARE(localFile->lastModified(), remoteFile->lastModified());

            QCOMPARE(localState, remoteState);
        }
    }

    void testMovedWithError_data()
    {
        QTest::addColumn<Vfs::Mode>("vfsMode");

        QTest::newRow("Vfs::Off") << Vfs::Off;
        QTest::newRow("Vfs::WithSuffix") << Vfs::WithSuffix;
#ifdef Q_OS_WIN32
        if (isVfsPluginAvailable(Vfs::WindowsCfApi))
        {
            QTest::newRow("Vfs::WindowsCfApi") << Vfs::WindowsCfApi;
        } else {
            QWARN("Skipping Vfs::WindowsCfApi");
        }
#endif
    }

    void testMovedWithError()
    {
        QFETCH(Vfs::Mode, vfsMode);
        const auto getName = [vfsMode] (const QString &s)
        {
            if (vfsMode == Vfs::WithSuffix)
            {
                return QStringLiteral("%1" APPLICATION_DOTVIRTUALFILE_SUFFIX).arg(s);
            }
            return s;
        };
        const QString src = "folder/folderA/file.txt";
        const QString dest = "folder/folderB/file.txt";
        FakeFolder fakeFolder{ FileInfo{ QString(), { FileInfo{ QStringLiteral("folder"), { FileInfo{ QStringLiteral("folderA"), { { QStringLiteral("file.txt"), 400 } } }, QStringLiteral("folderB") } } } } };
        auto syncOpts = fakeFolder.syncEngine().syncOptions();
        syncOpts._parallelNetworkJobs = 0;
        fakeFolder.syncEngine().setSyncOptions(syncOpts);

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        if (vfsMode != Vfs::Off)
        {
            auto vfs = QSharedPointer<Vfs>(createVfsFromPlugin(vfsMode).release());
            QVERIFY(vfs);
            fakeFolder.switchToVfs(vfs);
            fakeFolder.syncJournal().internalPinStates().setForPath("", PinState::OnlineOnly);

            // make files virtual
            QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        }


        fakeFolder.serverErrorPaths().append(src, 403);
        fakeFolder.localModifier().rename(getName(src), getName(dest));
        QVERIFY(fakeFolder.currentRemoteState().find(src));
        QVERIFY(!fakeFolder.currentRemoteState().find(dest));

        // sync1 file gets detected as error, instruction is still NEW_FILE
        QVERIFY(!fakeFolder.applyLocalModificationsAndSync());

        // sync2 file is in error state, checkErrorBlacklisting sets instruction to IGNORED
        QVERIFY(!fakeFolder.applyLocalModificationsAndSync());

        if (vfsMode != Vfs::Off)
        {
            fakeFolder.syncJournal().internalPinStates().setForPath("", PinState::AlwaysLocal);
            QVERIFY(!fakeFolder.applyLocalModificationsAndSync());
        }

        QVERIFY(!fakeFolder.currentLocalState().find(src));
        QVERIFY(fakeFolder.currentLocalState().find(getName(dest)));
        if (vfsMode == Vfs::WithSuffix)
        {
            // the placeholder was not restored as it is still in error state
            QVERIFY(!fakeFolder.currentLocalState().find(dest));
        }
        QVERIFY(fakeFolder.currentRemoteState().find(src));
        QVERIFY(!fakeFolder.currentRemoteState().find(dest));
    }

};

QTEST_GUILESS_MAIN(TestSyncMove)
#include "testsyncmove.moc"
