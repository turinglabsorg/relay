#pragma once
#include <QObject>
#include <QThreadPool>
#include <memory>
#include <vector>

struct _GCancellable;

// Passwords live only in the desktop's Secret Service, never in QSettings.
class Credentials : public QObject {
    Q_OBJECT
public:
    enum Operation { Lookup, Store, Clear };
    explicit Credentials(QObject *parent = nullptr);
    ~Credentials() override;
    void request(Operation operation, QString host, int port, QString user, QByteArray password = {});
signals:
    void completed(int operation, QString host, int port, QString user, QString password, bool found, bool success);
private:
    QThreadPool m_pool;
    std::vector<std::weak_ptr<_GCancellable>> m_requests;
};
