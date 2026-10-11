#include <libsecret/secret.h>
#include "credentials.h"
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <algorithm>

namespace {
const SecretSchema schema = [] {
    SecretSchema value{};
    value.name = "io.relay.Credentials";
    value.flags = SECRET_SCHEMA_NONE;
    value.attributes[0] = {"host", SECRET_SCHEMA_ATTRIBUTE_STRING};
    value.attributes[1] = {"port", SECRET_SCHEMA_ATTRIBUTE_INTEGER};
    value.attributes[2] = {"username", SECRET_SCHEMA_ATTRIBUTE_STRING};
    return value;
}();
struct Result { QString password; bool found = false, success = false; };
}

Credentials::Credentials(QObject *parent) : QObject(parent) { m_pool.setMaxThreadCount(1); }
Credentials::~Credentials() {
    for (auto &request : m_requests) if (auto pending = request.lock()) g_cancellable_cancel(pending.get());
    m_pool.waitForDone();
}
void Credentials::request(Operation operation, QString host, int port, QString user, QByteArray password) {
    auto cancellation = std::shared_ptr<GCancellable>(g_cancellable_new(), [](GCancellable *value) { g_object_unref(value); });
    std::erase_if(m_requests, [](auto &request) { return request.expired(); });
    m_requests.push_back(cancellation);
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [=,this] {
        const auto result = watcher->result();
        emit completed(operation, host, port, user, result.password, result.found, result.success);
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, [=,password = std::move(password)]() mutable {
        GError *error = nullptr;
        Result result;
        const auto hostname = host.toUtf8(), username = user.toUtf8();
        if (operation == Lookup) {
            char *secret = secret_password_lookup_sync(&schema, cancellation.get(), &error,
                "host", hostname.constData(), "port", port, "username", username.constData(), nullptr);
            result.found = secret != nullptr;
            if (secret) { result.password = QString::fromUtf8(secret); secret_password_free(secret); }
            result.success = error == nullptr;
        } else if (operation == Store) {
            const auto label = QString("Relay · %1@%2:%3").arg(user, host).arg(port).toUtf8();
            result.success = secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT,
                label.constData(), password.constData(), cancellation.get(), &error,
                "host", hostname.constData(), "port", port, "username", username.constData(), nullptr);
        } else {
            secret_password_clear_sync(&schema, cancellation.get(), &error,
                "host", hostname.constData(), "port", port, "username", username.constData(), nullptr);
            result.success = error == nullptr; // Removing an absent entry is also successful.
        }
        password.fill('\0'); password.clear();
        if (error) g_error_free(error);
        return result;
    }));
}
