#include "backend/GstSampleQueue.h"

#include <QtTest>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

void countFinalization(gpointer userData, GstMiniObject *)
{
    static_cast<std::atomic<int> *>(userData)->fetch_add(1, std::memory_order_relaxed);
}

GstSample *makeSample(int marker, std::atomic<int> *finalizations = nullptr)
{
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 4, nullptr);
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(marker);
    GstCaps *caps = gst_caps_new_simple("application/x-airplay-sample",
                                        "marker", G_TYPE_INT, marker,
                                        nullptr);
    GstSample *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);

    if (finalizations) {
        gst_mini_object_weak_ref(GST_MINI_OBJECT(sample), countFinalization, finalizations);
    }
    return sample;
}

int sampleMarker(GstSample *sample)
{
    int marker = -1;
    const GstStructure *structure = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
    if (!gst_structure_get_int(structure, "marker", &marker)) {
        return -1;
    }
    return marker;
}

} // namespace

class GstSampleQueueTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void rejectsNullAndNonPositiveCapacity();
    void retainsBorrowedSampleUntilOwnedPop();
    void preservesCapacityAndFifoOrder();
    void clearAndDestructorReleaseQueueReferences();
    void fullPushDoesNotRetainRejectedSample();
    void fullPushesDoNotWaitForSleepingConsumer();
    void concurrentPushPopAndClearReleaseEverySample();
};

void GstSampleQueueTest::initTestCase()
{
    gst_init(nullptr, nullptr);
}

void GstSampleQueueTest::rejectsNullAndNonPositiveCapacity()
{
    GstSampleQueue zero(0);
    GstSampleQueue negative(-3);
    GstSample *sample = makeSample(1);

    QVERIFY(!zero.tryPushBorrowed(sample));
    QVERIFY(!negative.tryPushBorrowed(sample));
    QVERIFY(!zero.tryPushBorrowed(nullptr));
    QVERIFY(!negative.tryPushBorrowed(nullptr));
    QCOMPARE(zero.size(), qsizetype(0));
    QCOMPARE(negative.size(), qsizetype(0));
    QVERIFY(zero.tryPopOwned() == nullptr);
    gst_sample_unref(sample);
}

void GstSampleQueueTest::retainsBorrowedSampleUntilOwnedPop()
{
    std::atomic<int> finalizations{0};
    GstSampleQueue queue(1);
    GstSample *borrowed = makeSample(17, &finalizations);

    QVERIFY(queue.tryPushBorrowed(borrowed));
    gst_sample_unref(borrowed);
    QCOMPARE(finalizations.load(), 0);

    GstSample *owned = queue.tryPopOwned();
    QVERIFY(owned != nullptr);
    QCOMPARE(sampleMarker(owned), 17);
    QCOMPARE(GST_BUFFER_PTS(gst_sample_get_buffer(owned)), GstClockTime(17));
    QCOMPARE(queue.size(), qsizetype(0));
    gst_sample_unref(owned);
    QCOMPARE(finalizations.load(), 1);
}

void GstSampleQueueTest::preservesCapacityAndFifoOrder()
{
    GstSampleQueue queue(2);
    GstSample *a = makeSample(1);
    GstSample *b = makeSample(2);
    GstSample *c = makeSample(3);

    QVERIFY(queue.tryPushBorrowed(a));
    QVERIFY(queue.tryPushBorrowed(b));
    QVERIFY(!queue.tryPushBorrowed(c));
    QCOMPARE(queue.size(), qsizetype(2));
    gst_sample_unref(a);
    gst_sample_unref(b);
    gst_sample_unref(c);

    GstSample *first = queue.tryPopOwned();
    GstSample *second = queue.tryPopOwned();
    QCOMPARE(sampleMarker(first), 1);
    QCOMPARE(sampleMarker(second), 2);
    gst_sample_unref(first);
    gst_sample_unref(second);
    QVERIFY(queue.tryPopOwned() == nullptr);
}

void GstSampleQueueTest::clearAndDestructorReleaseQueueReferences()
{
    std::atomic<int> clearFinalizations{0};
    GstSampleQueue queue(2);
    GstSample *a = makeSample(1, &clearFinalizations);
    GstSample *b = makeSample(2, &clearFinalizations);
    QVERIFY(queue.tryPushBorrowed(a));
    QVERIFY(queue.tryPushBorrowed(b));
    gst_sample_unref(a);
    gst_sample_unref(b);
    QCOMPARE(clearFinalizations.load(), 0);

    queue.clear();
    QCOMPARE(queue.size(), qsizetype(0));
    QCOMPARE(clearFinalizations.load(), 2);

    std::atomic<int> destructorFinalizations{0};
    {
        GstSampleQueue scopedQueue(1);
        GstSample *sample = makeSample(3, &destructorFinalizations);
        QVERIFY(scopedQueue.tryPushBorrowed(sample));
        gst_sample_unref(sample);
        QCOMPARE(destructorFinalizations.load(), 0);
    }
    QCOMPARE(destructorFinalizations.load(), 1);
}

void GstSampleQueueTest::fullPushDoesNotRetainRejectedSample()
{
    std::atomic<int> finalizations{0};
    GstSampleQueue queue(2);
    GstSample *a = makeSample(1, &finalizations);
    GstSample *b = makeSample(2, &finalizations);
    GstSample *c = makeSample(3, &finalizations);

    QVERIFY(queue.tryPushBorrowed(a));
    QVERIFY(queue.tryPushBorrowed(b));
    QVERIFY(!queue.tryPushBorrowed(c));
    gst_sample_unref(a);
    gst_sample_unref(b);
    gst_sample_unref(c);
    QCOMPARE(finalizations.load(), 1);

    queue.clear();
    QCOMPARE(finalizations.load(), 3);
}

void GstSampleQueueTest::fullPushesDoNotWaitForSleepingConsumer()
{
    GstSampleQueue queue(1);
    GstSample *initial = makeSample(1);
    GstSample *probe = makeSample(2);
    QVERIFY(queue.tryPushBorrowed(initial));
    gst_sample_unref(initial);

    std::atomic<bool> consumerHasPopped{false};
    std::thread consumer([&] {
        GstSample *owned = queue.tryPopOwned();
        consumerHasPopped.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        gst_sample_unref(owned);
    });

    QTRY_VERIFY_WITH_TIMEOUT(consumerHasPopped.load(std::memory_order_acquire), 250);
    QVERIFY(queue.tryPushBorrowed(probe));
    QElapsedTimer timer;
    timer.start();
    int rejected = 0;
    for (int index = 0; index < 10'000; ++index) {
        rejected += queue.tryPushBorrowed(probe) ? 0 : 1;
    }
    const qint64 elapsedMs = timer.elapsed();

    consumer.join();
    QCOMPARE(rejected, 10'000);
    QVERIFY2(elapsedMs < 250,
             qPrintable(QStringLiteral("10,000 full pushes took %1 ms").arg(elapsedMs)));
    queue.clear();
    gst_sample_unref(probe);
}

void GstSampleQueueTest::concurrentPushPopAndClearReleaseEverySample()
{
    constexpr int rounds = 8;
    constexpr int samplesPerRound = 1'000;

    for (int round = 0; round < rounds; ++round) {
        GstSampleQueue queue(32);
        std::atomic<int> finalizations{0};
        std::atomic<bool> producerDone{false};

        std::thread producer([&] {
            for (int index = 0; index < samplesPerRound; ++index) {
                GstSample *sample = makeSample(index, &finalizations);
                queue.tryPushBorrowed(sample);
                gst_sample_unref(sample);
                if ((index & 31) == 0) {
                    std::this_thread::yield();
                }
            }
            producerDone.store(true, std::memory_order_release);
        });

        std::thread consumer([&] {
            while (!producerDone.load(std::memory_order_acquire) || queue.size() != 0) {
                if (GstSample *sample = queue.tryPopOwned()) {
                    gst_sample_unref(sample);
                } else {
                    std::this_thread::yield();
                }
            }
        });

        std::thread clearer([&] {
            while (!producerDone.load(std::memory_order_acquire)) {
                queue.clear();
                std::this_thread::yield();
            }
            // No producer can push after producerDone. This final clear therefore
            // releases every item accepted after the previous clear linearization point.
            queue.clear();
        });

        producer.join();
        clearer.join();
        consumer.join();
        queue.clear();
        QTRY_COMPARE_WITH_TIMEOUT(finalizations.load(), samplesPerRound, 2'000);
    }
}

QTEST_APPLESS_MAIN(GstSampleQueueTest)

#include "GstSampleQueueTest.moc"
