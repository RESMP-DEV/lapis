#ifndef LAPIS_DESKTOP_BIRTH_ROUTE_HPP
#define LAPIS_DESKTOP_BIRTH_ROUTE_HPP

#include "live_connection.hpp"

#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>

namespace lapis::desktop {

struct BirthTicket {
    QByteArray session_id;
    QByteArray session_epoch;
};

struct BirthRequest {
    QString endpoint;
    QString fingerprint;
    session::LaunchSpec launch;
    QStringList service_arguments;
    QString log;
    QHash<QString, QString> environment;
};

class BirthRoute {
  public:
    virtual ~BirthRoute() = default;
    BirthRoute(const BirthRoute&) = delete;
    BirthRoute& operator=(const BirthRoute&) = delete;

    [[nodiscard]] virtual BirthTicket birth(const BirthRequest& request) = 0;

  protected:
    BirthRoute() = default;
};

class DirectBirthRoute final : public BirthRoute {
  public:
    explicit DirectBirthRoute(ServiceLauncher launcher = {});

    [[nodiscard]] BirthTicket birth(const BirthRequest& request) override;

  private:
    ServiceLauncher launcher_;
};

class SupervisorBirthRoute final : public BirthRoute {
  public:
    explicit SupervisorBirthRoute(QString control_endpoint, int timeout_ms = 5000);

    [[nodiscard]] BirthTicket birth(const BirthRequest& request) override;

  private:
    QString control_endpoint_;
    int timeout_ms_;
};

} // namespace lapis::desktop

#endif
