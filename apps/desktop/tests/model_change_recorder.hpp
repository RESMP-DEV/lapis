#pragma once

#include <QObject>

struct ModelChangeRecorder final : QObject {
    Q_OBJECT

  public:
    int changes{};

  public Q_SLOTS:
    void changed() { ++changes; }
};
