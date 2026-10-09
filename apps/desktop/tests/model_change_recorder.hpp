#ifndef LAPIS_DESKTOP_TESTS_MODEL_CHANGE_RECORDER_HPP
#define LAPIS_DESKTOP_TESTS_MODEL_CHANGE_RECORDER_HPP

#include <QObject>

struct ModelChangeRecorder final : QObject {
    Q_OBJECT

  public:
    int changes{};

  public Q_SLOTS:
    void changed() { ++changes; }
};

#endif
