// Offscreen render of the Swamp view against REAL swamp_core instances (fake loam_core + fake
// Storage underneath). Two peers publish real models; screenshots of every tab.
//   scripts/qml-harness/render.sh
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlContext>
#include <QQuickView>
#include <QQuickItem>
#include <QTimer>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QElapsedTimer>
#include <QJSValue>
#include <filesystem>
#include <cstdio>
#include "swamp_core_impl.h"
#include "logos_sdk.h"
void SwampCoreImpl::stateChanged(const std::string&) {}
using json = nlohmann::json;
static int g_errors = 0;
static void handler(QtMsgType t, const QMessageLogContext&, const QString& m) {
    if ((t == QtWarningMsg || t == QtCriticalMsg) && (m.contains("is not a type") || m.contains("non-existent property") || m.contains("TypeError") || m.contains("ReferenceError"))) g_errors++;
    if (!m.contains("QSettings") && !m.contains("application identifiers")) fprintf(stderr, "[%s] %s\n", t == QtWarningMsg ? "W" : "I", m.toUtf8().constData());
}
class RealLogos : public QObject {
    Q_OBJECT
public:
    SwampCoreImpl* core = nullptr;
    std::string dispatch(const QString& method, const QVariantList& a) {
        auto s = [&](int i) { return i < a.size() ? a[i].toString().toStdString() : std::string(); };
        const std::string m = method.toStdString();
        if (m == "snapshot") return core->snapshot();
        if (m == "resync") return core->resync();
        if (m == "listModels") return core->listModels(s(0));
        if (m == "getModel") return core->getModel(s(0));
        if (m == "cacheImage") return core->cacheImage(s(0), s(1));
        if (m == "publish") return core->publish(s(0));
        if (m == "retract") return core->retract(s(0), s(1));
        if (m == "download") return core->download(s(0), s(1));
        if (m == "comment") return core->comment(s(0), s(1));
        if (m == "postMake") return core->postMake(s(0), s(1));
        if (m == "like") return core->like(s(0), s(1));
        if (m == "setProfile") return core->setProfile(s(0));
        return "{\"error\":\"Invalid response\"}";
    }
    Q_INVOKABLE void callModuleAsync(const QString& mod, const QString& method, const QVariantList& args, const QJSValue& cb, int) {
        QString r = mod == "swamp_core" ? QString::fromStdString(dispatch(method, args)) : QString("{\"error\":\"unknown module\"}");
        QJSValue c = cb;
        QTimer::singleShot(0, this, [c, r]() mutable { if (c.isCallable()) c.call({QJSValue(r)}); });
    }
    Q_INVOKABLE void onModuleEvent(const QString&, const QString&) {}
signals:
    void moduleEventReceived(const QString& mod, const QString& ev, const QString& data);
};
#include "harness.moc"

// Like Basecamp 0.3's plugin sandbox: a view may load files only from its own directory (and Qt's
// QML imports). Anything else is blocked and counted, so a view that reads the core's data dir by
// path fails here too instead of only in Basecamp.
struct Sandbox : QQmlAbstractUrlInterceptor {
    QStringList roots;
    int blocked = 0;
    QUrl intercept(const QUrl& url, DataType type) override {
        if (getenv("SANDBOX_TRACE")) fprintf(stderr, "INTERCEPT %d %s\n", (int)type, url.toString().left(100).toUtf8().constData());
        if (type != UrlString) return url;   // module/qmldir probes are not the view's file access
        if (url.scheme() == "data") { blocked++; fprintf(stderr, "SANDBOX blocked a data: URL\n"); return QUrl(); }
        if (!url.isLocalFile()) return url;   // module/qmldir probes are not the view's file access
        QString p = url.toLocalFile();
        for (const auto& r : roots) if (p.startsWith(r)) return url;
        blocked++;
        fprintf(stderr, "SANDBOX blocked %s\n", p.toUtf8().constData());
        return QUrl();
    }
};
struct Peer { FakeLoamNode bus; FakeStoreNode store; SwampCoreImpl core; };
static Peer* spawn(const std::string& root, const std::string& name) {
    setenv("SWAMP_CORE_DATA", (root + "/" + name + "/data").c_str(), 1);
    setenv("SWAMP_DOWNLOADS", (root + "/" + name + "/Downloads").c_str(), 1);
    Peer* p = new Peer(); p->bus.name = p->store.name = name;
    p->core.modules().loam_core.node = &p->bus; p->core.modules().storage_module.node = &p->store;
    FakeLoamBus::get().nodes.push_back(&p->bus); FakeStoreNet::get().nodes.push_back(&p->store);
    p->core.fakeStart();
    return p;
}
static void pump(int ms) { QElapsedTimer t; t.start(); while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10); }
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); qputenv("QT_QUICK_BACKEND", "software");
    setenv("SWAMP_TICK_MS", "100", 1);
    QGuiApplication app(argc, argv);
    qInstallMessageHandler(handler);
    std::string qml = argv[1], out = argv[2], repo = argv[3], root = out + "/data";
    std::filesystem::remove_all(root);
    Peer* me = spawn(root, "me"); Peer* friend_ = spawn(root, "friend");
    pump(1500);
    friend_->core.setProfile(json{{"name", "Lizard Lab"}}.dump());
    me->core.setProfile(json{{"name", "Swamp Thing"}, {"bio", "I print ducks"}}.dump());
    auto pub = [&](Peer* p, const char* id, const char* title, const char* summary, json tags, const char* lic) {
        return json::parse(p->core.publish(json{{"title", title}, {"summary", summary}, {"description", "Printed at 0.2 mm, PLA, no supports.\nScale it as you like."}, {"licence", lic}, {"tags", tags}, {"files", {{{"path", repo + "/bench/data/raw/" + id + ".stl"}}}}}.dump()));
    };
    json a = pub(friend_, "74890", "Geometric bracelet", "faceted, prints flat", {"fashion", "bracelet"}, "CC-BY-4.0");
    pub(friend_, "80353", "Desk lamp shade", "a lamp head", {"home", "lighting"}, "CC-BY-SA-4.0");
    pub(friend_, "278455", "Balloon", "party decoration", {"toy", "party"}, "CC0-1.0");
    pub(me, "168080", "Dashboard wall clock", "clock face with widgets", {"home", "clock"}, "CC-BY-NC-4.0");
    pump(2500);
    std::string mid = a["modelId"];
    me->core.comment(mid, "Printed this in silk PLA, looks great");
    me->core.like(mid, "true");
    pump(1500);

    QQuickView view; view.setResizeMode(QQuickView::SizeRootObjectToView); view.resize(1280, 900);
    Sandbox sandbox;
    sandbox.roots << QFileInfo(QString::fromStdString(qml)).absolutePath() + "/" << QLibraryInfo::path(QLibraryInfo::QmlImportsPath) + "/";
    // the engine's import path includes the harness's own dir, which also holds the test data
    for (const auto& ip : view.engine()->importPathList()) if (ip != QCoreApplication::applicationDirPath() && !ip.startsWith("qrc:")) sandbox.roots << ip + "/";
    view.engine()->addUrlInterceptor(&sandbox);
    RealLogos logos; logos.core = &me->core;
    view.engine()->rootContext()->setContextProperty("logos", &logos);
    view.setSource(QUrl::fromLocalFile(QString::fromStdString(qml)));
    if (view.status() == QQuickView::Error) { for (auto& e : view.errors()) fprintf(stderr, "LOAD ERROR %s\n", e.toString().toUtf8().constData()); return 1; }
    view.show(); pump(2500);
    QQuickItem* r = view.rootObject();
    auto shot = [&](const char* n) { pump(1500); view.grabWindow().save(QString::fromStdString(out + "/swamp-" + n + ".png")); fprintf(stderr, "SHOT %s\n", n); };
    shot("browse");
    QMetaObject::invokeMethod(r, "openModel", Q_ARG(QVariant, QString::fromStdString(mid)));
    shot("model");
    r->setProperty("openId", ""); r->setProperty("tab", "publish"); shot("publish");
    r->setProperty("tab", "me"); shot("me");
    fprintf(stderr, "QML_ERRORS=%d SANDBOX_BLOCKED=%d\n", g_errors, sandbox.blocked);
    g_errors += sandbox.blocked;
    return g_errors ? 1 : 0;
}
