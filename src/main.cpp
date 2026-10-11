#include "session.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QDir>
#include <QTimer>
#include <QQuickWindow>
#include <QTextStream>
#include <signal.h>

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("Relay");
    QCoreApplication::setOrganizationName("Relay");
    QCoreApplication::setApplicationVersion("0.2.0");
    app.setDesktopFileName("io.relay");
    app.setWindowIcon(QIcon(":/assets/relay.svg"));
    app.setFont(QFont("Inter", 11));
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<RemoteView>("Relay",1,0,"RemoteView");
    qmlRegisterUncreatableType<Session>("Relay",1,0,"Session","Provided by the application");
    const auto args = app.arguments();
    Session session(!args.contains("--self-test") && !args.contains("--test-connect") && !args.contains("--test-ui") && !args.contains("--test-credentials"));
    if (args.contains("--test-credentials")) {
        Credentials credentials;
        const QString secret = QString::fromUtf8("Synthetic Relay päss 🚀");
        int step = 0;
        QObject::connect(&credentials, &Credentials::completed, &app,
            [&](int, QString, int, QString, QString password, bool found, bool success) {
                if (!success) { QTextStream(stderr) << "Test keyring unavailable\n"; app.exit(7); return; }
                if ((step == 1 && (!found || password != secret)) || (step > 1 && step != 5 && found)) {
                    QTextStream(stderr) << "Credential lookup or isolation failed\n"; app.exit(8); return;
                }
                ++step;
                if (step == 1) credentials.request(Credentials::Lookup, "relay-test.invalid", 5900, "synthetic-user");
                else if (step == 2) credentials.request(Credentials::Lookup, "other-test.invalid", 5900, "synthetic-user");
                else if (step == 3) credentials.request(Credentials::Lookup, "relay-test.invalid", 5901, "synthetic-user");
                else if (step == 4) credentials.request(Credentials::Lookup, "relay-test.invalid", 5900, "other-user");
                else if (step == 5) credentials.request(Credentials::Clear, "relay-test.invalid", 5900, "synthetic-user");
                else if (step == 6) credentials.request(Credentials::Lookup, "relay-test.invalid", 5900, "synthetic-user");
                else if (step == 7) {
                    credentials.request(Credentials::Store, "relay-test.invalid", 5900, "synthetic-user", secret.toUtf8());
                    credentials.request(Credentials::Clear, "relay-test.invalid", 5900, "synthetic-user");
                } else if (step == 8) { /* The removal is already queued after the store. */ }
                else if (step == 9) credentials.request(Credentials::Lookup, "relay-test.invalid", 5900, "synthetic-user");
                else { QTextStream(stdout) << "PASS keyring store, Unicode lookup, host/port/user isolation and queued removal\n"; app.exit(0); }
            });
        credentials.request(Credentials::Store, "relay-test.invalid", 5900, "synthetic-user", secret.toUtf8());
        QTimer::singleShot(10000, &app, [] { QCoreApplication::exit(9); });
        return app.exec();
    }
    if (args.contains("--self-test")) {
        auto require = [](bool passed, const char *name) { if (!passed) { qFatal("Test failed: %s",name); } };
        require(remoteKey(Qt::Key_Meta, {}) == 0xffeb,"Mac Command mapping");
        require(remoteKey(Qt::Key_C, QString(QChar(3))) == 'c',"Control shortcut mapping");
        require(remoteKey(Qt::Key_E, "€") == (0x01000000u | 0x20ac),"Unicode keysym");
        require(fittedRect({1920,1080},{1000,1000}) == QRectF(0,218.75,1000,562.5),"Letterbox coordinates");
        require(fittedRect({},{1000,1000}).isEmpty(),"Empty framebuffer");
        QTextStream(stdout) << "PASS input mapping and viewport geometry\n";
        return 0;
    }
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, &app, [](const QList<QQmlError> &errors) {
        for (const auto &error : errors) QTextStream(stderr) << error.toString() << '\n';
    });
    engine.rootContext()->setContextProperty("session", &session);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl("qrc:/ui/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    const int uiIndex = args.indexOf("--test-ui");
    if (uiIndex >= 0 && uiIndex + 1 < args.size()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        const int sizeIndex = args.indexOf("--ui-size");
        if (sizeIndex >= 0 && sizeIndex+1 < args.size()) {
            const auto size = args[sizeIndex+1].split('x');
            if (size.size() != 2) return 2;
            window->resize(size[0].toInt(), size[1].toInt());
        }
        QTimer::singleShot(100, &app, [&,window] {
            const int dialogIndex = args.indexOf("--ui-dialog");
            if (dialogIndex >= 0 && dialogIndex+1 < args.size())
                QMetaObject::invokeMethod(window, "previewDialog", Q_ARG(QVariant, args[dialogIndex+1]));
            QTimer::singleShot(250, &app, [&,window] {
                for (const auto name : {"loginDialog", "preferencesDialog", "helpDialog"}) {
                    auto *dialog = window->findChild<QObject *>(name);
                    if (dialog && dialog->property("visible").toBool()
                        && (dialog->property("x").toDouble() < 15 || dialog->property("y").toDouble() < 15
                            || dialog->property("width").toDouble() > window->width()-31
                            || dialog->property("height").toDouble() > window->height()-31)) {
                        QTextStream(stderr) << "Dialog outside viewport: " << name << '\n'; app.exit(11); return;
                    }
                }
                const int selectedDialog = args.indexOf("--ui-dialog");
                if (selectedDialog >= 0 && selectedDialog+1 < args.size()) {
                    const auto actionName = args[selectedDialog+1] + "Action";
                    auto *action = window->findChild<QQuickItem *>(actionName);
                    if (!action) { app.exit(12); return; }
                    const QPointF position = action->mapToScene({0,0});
                    if (position.x() < 15 || position.y() < 15 || position.x()+action->width() > window->width()-15
                        || position.y()+action->height() > window->height()-15) {
                        QTextStream(stderr) << "Dialog action outside viewport\n"; app.exit(12); return;
                    }
                }
                const auto image = window->grabWindow();
                if (image.isNull() || !image.save(args[uiIndex+1])) { app.exit(10); return; }
                QTextStream(stdout) << "PASS UI capture " << window->width() << 'x' << window->height() << '\n';
                app.exit(0);
            });
        });
    }
    // Integration-test path; scripts isolate settings and use only synthetic credentials.
    const bool rememberTest = args.contains("--test-remember");
    const QString testSecret = QString::fromUtf8("Synthetic Relay päss 🚀");
    int testIndex = args.indexOf("--test-connect");
    if (testIndex >= 0 && testIndex+1 < args.size()) {
        const bool lowBandwidthTest = args.contains("--test-low-bandwidth");
        session.setLowBandwidth(lowBandwidthTest);
        const QColor expectedPixel(lowBandwidthTest ? "#1034ad" : "#1234ab");
        const bool cancelTest = args.contains("--test-cancel");
        auto endpoint=args[testIndex+1].split(':');
        if (endpoint.size()!=2) return 2;
        QObject::connect(&session, &Session::cursorReady, &app, [](const QImage &image, QPoint hotspot) {
            if (!image.isNull()) QTextStream(stdout) << "CURSOR " << image.width() << 'x' << image.height()
                << ' ' << hotspot.x() << ',' << hotspot.y() << ' ' << image.pixelColor(0,0).name()
                << ' ' << image.pixelColor(1,0).alpha() << '\n';
        });
        QObject::connect(&session, &Session::frameReady, &app, [&,expectedPixel](const QImage &image) {
            QTextStream(stdout) << "FRAME " << image.width() << 'x' << image.height() << ' ' << image.pixelColor(0,0).name() << '\n';
            session.pointer(2,1,1); session.pointer(2,1,0);
            session.key('a',true); session.key('a',false);
            if (image.size()==QSize(8,6) || image.size()==QSize(9,6) || image.size()==QSize(64,6)) {
                if (image.pixelColor(0,0)!=expectedPixel || image.pixelColor(image.width()-1,image.height()-1)!=expectedPixel) app.exit(3);
                else QTimer::singleShot(100, &app, [&,expectedPixel] {
                    auto checkRendering = [&engine,expectedPixel] {
                        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
                        auto *view = window->findChild<RemoteView *>();
                        const QImage rendered = window->grabWindow();
                        if (!view || rendered.isNull()) { QTextStream(stderr) << "Render capture unavailable\n"; return false; }
                        const qreal scale = rendered.width() / qreal(window->width());
                        const QPointF middle = view->mapToScene(QPointF(view->width()/2, view->height()/2));
                        const QPointF corner = view->mapToScene(QPointF(2,2));
                        const QColor centerColor = rendered.pixelColor((middle*scale).toPoint());
                        const QColor cornerColor = rendered.pixelColor((corner*scale).toPoint());
                        // Hardware captures may use a floating-point QColor format.
                        // Compare the displayed 8-bit RGB channels across backends.
                        const bool correct = centerColor.rgb() == expectedPixel.rgb()
                            && cornerColor.rgb() == QColor("#080d0d").rgb();
                        if (!correct) QTextStream(stderr) << "Render colors: " << centerColor.name()
                            << ' ' << cornerColor.name() << "; window " << window->width() << 'x'
                            << window->height() << "; view " << view->width() << 'x' << view->height() << '\n';
                        return correct;
                    };
                    if (!checkRendering()) { QTextStream(stderr) << "Rendering failed\n"; app.exit(6); return; }
                    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
                    window->resize(940,680);
                    QTimer::singleShot(100, &app, [&,checkRendering] {
                        if (!checkRendering()) { QTextStream(stderr) << "Rendering after resize failed\n"; app.exit(6); return; }
                        QTextStream(stdout) << "RENDER pixels and letterbox after window resize\n";
                        session.disconnectRemote();
                        if (rememberTest) {
                            auto *poll = new QTimer(&app);
                            QObject::connect(poll, &QTimer::timeout, &app, [&,poll] {
                                if (!session.remembersPassword("synthetic-user")) return;
                                poll->stop();
                                QObject::connect(&session, &Session::passwordLoaded, &app, [&](QString user, QString password) {
                                    if (user != "synthetic-user" || password != testSecret) { app.exit(8); return; }
                                    session.forgetPassword(user);
                                    auto *removal = new QTimer(&app);
                                    QObject::connect(removal, &QTimer::timeout, &app, [&,removal] {
                                        if (session.remembersPassword("synthetic-user")) return;
                                        removal->stop();
                                        QTextStream(stdout) << "PASS consent, successful-authentication save, reload and forget\n";
                                        app.exit(0);
                                    });
                                    removal->start(20);
                                });
                                session.loadPassword("synthetic-user");
                            });
                            poll->start(20);
                        } else QTimer::singleShot(300,&app,[] { QCoreApplication::exit(0); });
                    });
                });
            }
        });
        QObject::connect(&session,&Session::stateChanged,&app,[&,cancelTest] {
            if (session.state()=="error") { if (rememberTest && session.remembersPassword("synthetic-user")) { app.exit(8); return; } QTextStream(stderr)<<session.message()<<'\n'; app.exit(4); }
            if (cancelTest && session.state()=="loading") QTimer::singleShot(200,&session,&Session::disconnectRemote);
            if (cancelTest && session.state()=="idle") { QTextStream(stdout)<<"CANCELLED\n"; app.exit(0); }
        });
        QTimer::singleShot(0,&app,[&,endpoint] {
            if (rememberTest) {
                session.savePreferences(endpoint[0], endpoint[1].toInt(), "synthetic-user", true);
                session.connectRemote("synthetic-user", testSecret, true);
            } else session.connectEndpoint(endpoint[0],endpoint[1].toInt(),{},{});
        });
        QTimer::singleShot(10000,&app,[] { QCoreApplication::exit(5); });
    }
    return app.exec();
}
