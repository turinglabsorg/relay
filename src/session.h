#pragma once

#include <QImage>
#include <QCursor>
#include <QMutex>
#include <QObject>
#include <QQuickItem>
#include <QSettings>
#include <QSet>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <deque>
#include <set>
#include "credentials.h"

struct InputEvent {
    enum Type { Pointer, Key } type = Pointer;
    int x = 0, y = 0, mask = 0;
    quint32 key = 0;
    bool down = false;
    qint64 queuedAt = 0;
};

class VncWorker : public QThread {
    Q_OBJECT
public:
    VncWorker(QString host, int port, QString user, QString password, bool lowBandwidth, QObject *parent);
    ~VncWorker() override;
    void stop();
    void enqueue(InputEvent event);
    QImage latestFrame();
signals:
    void authenticated();
    void frameAvailable();
    void cursorAvailable(QImage image, QPoint hotspot);
    void ended(QString reason);
protected:
    void run() override;
private:
    friend struct VncHooks;
    QString m_host;
    int m_port;
    bool m_lowBandwidth;
    QByteArray m_user, m_password;
    QImage m_buffer, m_latest;
    QMutex m_queueMutex, m_frameMutex;
    QMutex m_socketMutex;
    int m_socket = -1;
    void trackSocket(int socket);
    std::deque<InputEvent> m_queue;
    std::atomic<bool> m_stop{false};
    bool m_notificationPending = false;
    bool m_dirty = false;
};

class Session : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(QString host READ host NOTIFY preferencesChanged)
    Q_PROPERTY(QString username READ username NOTIFY preferencesChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY preferencesChanged)
    Q_PROPERTY(int port READ port NOTIFY preferencesChanged)
    Q_PROPERTY(bool dark READ dark NOTIFY preferencesChanged)
    Q_PROPERTY(bool macShortcuts READ macShortcuts NOTIFY preferencesChanged)
    Q_PROPERTY(bool lowBandwidth READ lowBandwidth NOTIFY preferencesChanged)
    Q_PROPERTY(bool online READ online NOTIFY onlineChanged)
    Q_PROPERTY(QString resolution READ resolution NOTIFY resolutionChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
public:
    explicit Session(bool probeEnabled = true, QObject *parent = nullptr);
    ~Session() override;
    QString state() const { return m_state; }
    QString message() const { return m_message; }
    QString host() const;
    QString username() const;
    QString displayName() const;
    int port() const;
    bool dark() const;
    bool macShortcuts() const;
    bool lowBandwidth() const;
    bool online() const { return m_online; }
    QString resolution() const { return m_resolution; }
    QString notice() const { return m_notice; }
    Q_INVOKABLE void connectRemote(QString user, QString password, bool remember = false);
    Q_INVOKABLE bool remembersPassword(QString user) const;
    Q_INVOKABLE void loadPassword(QString user);
    Q_INVOKABLE void forgetPassword(QString user);
    Q_INVOKABLE void setDisplayName(QString name);
    void connectEndpoint(QString host, int port, QString user, QString password);
    Q_INVOKABLE void disconnectRemote();
    Q_INVOKABLE void savePreferences(QString host, int port, QString user, bool dark);
    Q_INVOKABLE void setDark(bool enabled);
    Q_INVOKABLE void setMacShortcuts(bool enabled);
    Q_INVOKABLE void setLowBandwidth(bool enabled);
    Q_INVOKABLE void probe();
    Q_INVOKABLE void saveScreenshot();
    void pointer(int x, int y, int mask);
    void key(quint32 key, bool down);
signals:
    void stateChanged();
    void preferencesChanged();
    void onlineChanged();
    void resolutionChanged();
    void noticeChanged();
    void frameReady(QImage image);
    void cursorReady(QImage image, QPoint hotspot);
    void passwordLoaded(QString user, QString password);
    void credentialError(QString message);
private:
    void setState(QString state, QString message = {});
    VncWorker *m_worker = nullptr;
    QSettings m_settings;
    QString m_state = "idle", m_message, m_resolution, m_notice;
    bool m_online = false;
    bool m_probeEnabled = true;
    QImage m_lastFrame;
    QTcpSocket m_probe;
    QTimer m_probeTimeout, m_probeInterval, m_noticeTimeout;
    Credentials m_credentials;
    QSet<QString> m_storingCredentials;
    QByteArray m_pendingPassword;
    QString m_pendingUser;
    bool m_rememberOnAuthentication = false;
    void showNotice(QString message);
};

quint32 remoteKey(int qtKey, const QString &text);
QRectF fittedRect(QSize image, QSizeF viewport);

class RemoteView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(Session* session READ session WRITE setSession NOTIFY sessionChanged)
public:
    explicit RemoteView(QQuickItem *parent = nullptr);
    Session *session() const { return m_session; }
    void setSession(Session *session);
    Q_INVOKABLE void releaseInput();
signals:
    void sessionChanged();
protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
private:
    bool sendPointer(QPointF position, Qt::MouseButtons buttons, int wheel = 0);
    Session *m_session = nullptr;
    QImage m_image;
    quint64 m_frameVersion = 0;
    QHash<int, quint32> m_pressedKeys;
    QPoint m_lastPointer;
};
