#include <QtTest/QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include "app/WindowStateStore.h"

class WindowStateStoreTest : public QObject {
    Q_OBJECT

private slots:
    void savesAndLoadsGeometryAndState() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("airplay-window-state.dat");
        const WindowStateSnapshot snapshot{
            QByteArray("geometry-bytes"),
            QByteArray("state-bytes")
        };

        WindowStateStore store(path);
        QVERIFY(store.save(snapshot));

        const std::optional<WindowStateSnapshot> loaded = store.load();
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->geometry, snapshot.geometry);
        QCOMPARE(loaded->state, snapshot.state);
    }

    void missingFileReturnsNoSnapshot() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        WindowStateStore store(dir.filePath("missing.json"));

        QVERIFY(!store.load().has_value());
    }

    void malformedFileReturnsNoSnapshot() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("airplay-window-state.dat");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("too short") > 0);
        file.close();

        WindowStateStore store(path);

        QVERIFY(!store.load().has_value());
    }

    void storesBinaryPayloadInsteadOfJson() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("airplay-window-state.dat");
        const WindowStateSnapshot snapshot{
            QByteArray("geometry-bytes"),
            QByteArray("state-bytes")
        };

        WindowStateStore store(path);
        QVERIFY(store.save(snapshot));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();

        QVERIFY(QJsonDocument::fromJson(bytes).isNull());
        QVERIFY(bytes.contains("geometry-bytes"));
        QVERIFY(bytes.contains("state-bytes"));
    }
};

QTEST_MAIN(WindowStateStoreTest)
#include "WindowStateStoreTest.moc"
