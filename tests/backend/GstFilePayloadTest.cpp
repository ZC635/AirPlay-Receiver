#include "backend/GstFileLocation.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

// QTemporaryDir reserves an exclusive directory; only that reservation is removed.
class OwnedFixtures {
    QString parent;
public:
    QString error;
    explicit OwnedFixtures(const QString &supplied)
        : parent(QDir::cleanPath(QFileInfo(supplied).absoluteFilePath())) {
        // The reserved basename has exactly 8 units (t- plus six random units).
        const int required = parent.size() + 1 + 8 + 1 + QStringLiteral("中文 空格").size() + 1 + 5;
        if (supplied.isEmpty() || required > 213) {
            error = QStringLiteral("Runtime parent cannot fit exact shortest target 213: parent units=%1, maximum=192; no fixture writes").arg(parent.size());
            return;
        }
        if (!QDir().mkpath(parent)) {
            error = QStringLiteral("Cannot create runtime parent: %1").arg(parent);
            return;
        }
        QTemporaryDir owned(parent + QStringLiteral("/t-XXXXXX"));
        owned.setAutoRemove(false);
        ownedPath = owned.path();
        if (!owned.isValid()) error = owned.errorString();
        else qInfo().noquote() << "owned_fixture=" + ownedPath;
    }
    QString ownedPath;
    QString path() const { return ownedPath; }
    bool cleanup() {
        if (ownedPath.isEmpty()) return true;
        const QFileInfo info(ownedPath);
        if (info.isSymLink() || info.absolutePath() != parent ||
            !info.fileName().startsWith(QStringLiteral("t-")) || info.fileName().size() != 8)
            return false;
        const bool removed = QDir(ownedPath).removeRecursively();
        if (removed) ownedPath.clear();
        return removed;
    }
    ~OwnedFixtures() { if (!cleanup()) qWarning("Owned fixture cleanup failed boundary validation or removal"); }
};

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <iostream>

// Elements retain their initial references until successfully added to the bin.
struct Pipeline {
    GstElement *p = gst_pipeline_new(nullptr);
    GstElement *src = nullptr, *sink = nullptr;
    bool shutdown() {
        if (!p) return true;
        const auto changed = gst_element_set_state(p, GST_STATE_NULL);
        GstState state = GST_STATE_VOID_PENDING;
        const auto stopped = gst_element_get_state(p, &state, nullptr, 5 * GST_SECOND);
        gst_object_unref(p); p = nullptr;
        return changed != GST_STATE_CHANGE_FAILURE && stopped != GST_STATE_CHANGE_FAILURE && state == GST_STATE_NULL;
    }
    ~Pipeline() {
        if (src) gst_object_unref(src);
        if (sink) gst_object_unref(sink);
        if (!shutdown()) std::cerr << "Pipeline teardown failed (5 seconds)\n";
    }
    bool create(const char *source, const char *destination) {
        src = gst_element_factory_make(source, nullptr);
        sink = gst_element_factory_make(destination, nullptr);
        if (!p || !src || !sink) { std::cerr << "Factory creation failed: " << source << " / " << destination << '\n'; return false; }
        return true;
    }
    bool link() {
        auto *source = src; auto *destination = sink;
        if (!gst_bin_add(GST_BIN(p), src)) return false;
        src = nullptr;
        if (!gst_bin_add(GST_BIN(p), sink)) return false;
        sink = nullptr;
        return gst_element_link(source, destination);
    }
};
static bool finish(Pipeline &pipeline) {
    GstBus *bus = gst_element_get_bus(pipeline.p);
    if (!bus) return false;
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
        GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    bool ok = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
    if (m && !ok) { GError *e=nullptr; gchar *d=nullptr;
        gst_message_parse_error(m,&e,&d); std::cerr << (e?e->message:"unknown error") << " " << (d?d:"") << '\n';
        g_clear_error(&e); g_free(d); }
    if (!m) std::cerr << "EOS timeout (5 seconds)\n";
    if (m) gst_message_unref(m);
    gst_object_unref(bus);
    return pipeline.shutdown() && ok;
}
static bool roundtrip(const QString &path, bool candidate) {
    const QByteArray location=(candidate?QString::fromUtf8(GstFileLocation::forIo(path)):path).toUtf8();
    const QByteArray bytes("fixed raw payload\0\1\2\377",22);
    Pipeline writer;
    if (!writer.create("appsrc", "filesink")) return false;
    GstElement *src = writer.src, *sink = writer.sink;
    g_object_set(sink,"location",location.constData(),nullptr);
    if (!writer.link()) return false;
    if (gst_element_set_state(writer.p,GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) { finish(writer); return false; }
    GstBuffer *b=gst_buffer_new_allocate(nullptr,bytes.size(),nullptr);
    if (!b) return false;
    if (gst_buffer_fill(b,0,bytes.constData(),bytes.size()) != gsize(bytes.size())) { gst_buffer_unref(b); return false; }
    // push_buffer takes ownership even when it returns a flow error.
    if(gst_app_src_push_buffer(GST_APP_SRC(src),b)!=GST_FLOW_OK) return false;
    if(gst_app_src_end_of_stream(GST_APP_SRC(src))!=GST_FLOW_OK || !finish(writer)) return false;
    Pipeline reader;
    if (!reader.create("filesrc", "appsink")) return false;
    src = reader.src; sink = reader.sink;
    g_object_set(src,"location",location.constData(),nullptr);
    g_object_set(sink,"sync",FALSE,nullptr);
    if (!reader.link()) return false;
    if (gst_element_set_state(reader.p,GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) { finish(reader); return false; }
    QByteArray actual;
    GstSample *s=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),5*GST_SECOND);
    if(s) { GstMapInfo map; GstBuffer *buffer=gst_sample_get_buffer(s);
        if(buffer && gst_buffer_map(buffer,&map,GST_MAP_READ)) {actual.append(reinterpret_cast<char*>(map.data),map.size); gst_buffer_unmap(buffer,&map);}
        gst_sample_unref(s); }
    bool ok=finish(reader) && actual==bytes;
    std::cout << (candidate?"candidate":"ordinary") << " units=" << path.size()
      << " max_component=" << [&]{int n=0; for(const auto &c:path.split('/')) n=qMax(n,int(c.size())); return n;}() << " payload=" << actual.size() << " equal=" << (actual==bytes) << " path=" << path.toUtf8().constData() << '\n';
    return ok;
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); gst_init(&argc,&argv);
    if(argc != 2) return 64;
    std::cout << gst_version_string() << " GLib " << glib_major_version << '.' << glib_minor_version << '.' << glib_micro_version << '\n';
#ifdef Q_OS_WIN
    for(const QString &invalid:{QString(),QStringLiteral("//./NUL"),QStringLiteral("//?/GLOBALROOT/device"),QString(QChar(0))}) {
        QString error; if(!GstFileLocation::forIo(invalid,&error).isEmpty() || error.isEmpty()) return 4;
    }
#endif
    // Test-only failure injection exercises partial ownership in both groups.
    if (qEnvironmentVariableIsSet("AIRPLAY_PAYLOAD_MISSING_FACTORY")) {
        for (const char *factory : {"appsrc", "filesrc"}) {
            Pipeline partial;
            if (partial.create(factory, "airplay-deliberately-missing-factory")) return 8;
        }
        return 0;
    }
    OwnedFixtures fixtures(QString::fromUtf8(argv[1]));
    if (!fixtures.error.isEmpty()) { std::cerr << fixtures.error.toUtf8().constData() << '\n'; return 2; }
    QString root=fixtures.path(); bool ok=true;
    if(!roundtrip(root+"/short.raw",true) || !roundtrip(root+"/short.raw",false) || !QFile::remove(root+"/short.raw")) return 5;
    for(const QString &flavor:{QStringLiteral("ascii"),QStringLiteral("中文 空格")}) {
        for(int length:{213,259,260,275,325}) {
            QString directory=root+"/"+flavor;
            while(directory.size()+70<length) directory+="/"+QString(40,'d');
            QString path=directory+"/"+QString(length-directory.size()-5,'x')+".raw";
            if(path.size()!=length || !QDir().mkpath(directory)) return 2;
            if(!roundtrip(path,true)) ok=false;
            bool ordinary=roundtrip(path,false);
            std::cout << "ordinary_result=" << ordinary << '\n';
            if(!QFile::remove(path)) return 3;
        }
    }
    if (!fixtures.cleanup()) return 3;
    return ok?0:1;
}
