// Switching agents in the visual view, against made-up conversations: a
// conversation not yet read shows its end at once (the rows of a whole
// reading's end, wherever the cut falls), and its history comes in after;
// conversations read stay kept within a budget, least recently shown going
// first; kept ones follow their files without being read again; and each
// keeps where its view was. A view check times switches offscreen.
#include "transcript.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QThreadPool>
#include <cstdint>
#include <cstdio>

using lapis::desktop::TranscriptModel;
using namespace lapis::desktop::transcript;

namespace {
int failures = 0;
void require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

QString fixture(const char* name) {
    return QDir(QStringLiteral(LAPIS_VISUAL_FIXTURES)).filePath(QString::fromUtf8(name));
}

// A made-up Claude Code conversation of `turns` turns, its records named
// `tag`-<n>: each turn a prompt, a thought, a command and its output, an
// edit and a reply, timed. Every third turn starts a background command
// that a later turn's notification ends, a /compact runs between two turns,
// and `pad` characters of output make the file as long as a test needs.
// Positional contract; callers and tests pin the order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QByteArray claude_conversation(const char* tag, int turns, int pad = 0) {
    QByteArray out;
    int record = 0;
    const auto id = [&] { return QStringLiteral("%1-%2").arg(tag).arg(++record).toUtf8(); };
    const auto at = [](int turn, int second) {
        return QStringLiteral("2026-11-07T%1:%2:%3.000Z")
            .arg(10 + turn / 60, 2, 10, QLatin1Char('0'))
            .arg(turn % 60, 2, 10, QLatin1Char('0'))
            .arg(second, 2, 10, QLatin1Char('0'))
            .toUtf8();
    };
    const QByteArray filler(pad, 'x');
    for (int turn = 0; turn < turns; ++turn) {
        const auto n = QByteArray::number(turn);
        out += R"({"type":"user","uuid":")" + id() + R"(","timestamp":")" + at(turn, 0) +
               R"(","message":{"content":"Step )" + n + R"( please"}})" + '\n';
        out +=
            R"({"type":"assistant","uuid":")" + id() + R"(","timestamp":")" + at(turn, 1) +
            R"(","message":{"model":"claude-opus-5-5","content":[{"type":"thinking","thinking":"Plan )" +
            n + R"("}]}})" + '\n';
        out += R"({"type":"assistant","uuid":")" + id() + R"(","timestamp":")" + at(turn, 2) +
               R"(","message":{"content":[{"type":"tool_use","id":"run)" + n +
               R"(","name":"Bash","input":{"command":"make step)" + n + R"("}}]}})" + '\n';
        out += R"({"type":"user","uuid":")" + id() + R"(","timestamp":")" + at(turn, 3) +
               R"(","message":{"content":[{"type":"tool_result","tool_use_id":"run)" + n +
               R"(","content":"built )" + n + filler + R"("}]}})" + '\n';
        if (turn % 3 == 1) {
            out +=
                R"({"type":"assistant","uuid":")" + id() + R"(","timestamp":")" + at(turn, 4) +
                R"(","message":{"content":[{"type":"tool_use","id":"bg)" + n +
                R"(","name":"Bash","input":{"command":"sleep 60","run_in_background":true}}]}})" +
                '\n';
            out += R"({"type":"user","uuid":")" + id() + R"(","timestamp":")" + at(turn, 5) +
                   R"(","message":{"content":[{"type":"tool_result","tool_use_id":"bg)" + n +
                   R"(","content":"Command running in background with ID: task)" + n +
                   R"("}]},"toolUseResult":{"backgroundTaskId":"task)" + n + R"("}})" + '\n';
        }
        if (turn % 3 == 2) {
            const auto ended = QByteArray::number(turn - 1);
            out +=
                R"({"type":"user","uuid":")" + id() + R"(","timestamp":")" + at(turn, 6) +
                R"(","message":{"content":"<task-notification><task-id>task)" + ended +
                R"(</task-id><status>completed</status><summary>Done</summary></task-notification>"}})" +
                '\n';
        }
        out +=
            R"({"type":"assistant","uuid":")" + id() + R"(","timestamp":")" + at(turn, 7) +
            R"(","message":{"content":[{"type":"tool_use","id":"edit)" + n +
            R"(","name":"Edit","input":{"file_path":"/w/a.cpp","old_string":"a","new_string":"b"}}]}})" +
            '\n';
        out += R"({"type":"user","uuid":")" + id() + R"(","timestamp":")" + at(turn, 8) +
               R"(","message":{"content":[{"type":"tool_result","tool_use_id":"edit)" + n +
               R"(","content":"ok"}]}})" + '\n';
        out += R"({"type":"assistant","uuid":")" + id() + R"(","timestamp":")" + at(turn, 9) +
               R"(","message":{"content":[{"type":"text","text":"Step )" + n +
               R"( is **done**."}]}})" + '\n';
        out +=
            R"({"type":"system","subtype":"turn_duration","durationMs":9000})" + QByteArray("\n");
        if (turn == turns / 2)
            out +=
                R"({"type":"user","uuid":")" + id() +
                R"(","message":{"content":"<command-name>/compact</command-name><command-args></command-args>"}})" +
                '\n';
    }
    return out;
}

QString write(const QTemporaryDir& directory, const QString& name, const QByteArray& bytes) {
    QFile file(directory.filePath(name));
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "fixture written");
    return file.fileName();
}

void append(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::Append) && file.write(bytes) == bytes.size(), "fixture grows");
}

// Whether the rows a tail shows are the last rows of the whole reading.
bool tail_matches(const Read& whole, const Layout& whole_layout, const Read& tail) {
    auto layout = lay_out(tail.reader->items(), {});
    if (tail.tail)
        trim_to_turns(layout);
    if (layout.rows.size() > whole_layout.rows.size())
        return false;
    const auto skip = whole_layout.rows.size() - layout.rows.size();
    for (size_t i = 0; i < layout.rows.size(); ++i)
        if (!same_content(layout, tail.reader->items(), layout.rows[i], whole_layout,
                          whole.reader->items(), whole_layout.rows[skip + i]))
            return false;
    return true;
}

// The end read at every cut through the file (inside records, between
// them, mid-turn) shows what a whole reading's end shows.
// Cohesive parser/test flow; branching stays local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void tails_match(const char* name, const QString& path, Format format) {
    const auto whole = read_file(path, format);
    const auto whole_layout = lay_out(whole.reader->items(), {});
    const auto size = QFileInfo(path).size();
    int cuts = 0, mismatched = 0, trimmed = 0;
    qint64 first_bad = -1;
    // Every third byte, and each record's first byte and the one after it.
    std::vector<qint64> starts;
    for (qint64 start = 1; start < size; start += 3)
        starts.push_back(start);
    QFile file(path);
    const auto bytes = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    for (auto at = bytes.indexOf('\n') + 1; at > 0 && at < size; at = bytes.indexOf('\n', at) + 1)
        starts.insert(starts.end(), {at, at + 1});
    for (const qint64 start : starts) {
        const auto tail = read_file(path, format, size - start);
        ++cuts;
        if (!tail.tail)
            continue;
        auto untrimmed = lay_out(tail.reader->items(), {});
        // Holding fewer than two turns' starts, a tail is read further back
        // (read_end); that one is checked instead.
        if (untrimmed.turns.size() < 2)
            continue;
        auto trim = untrimmed;
        trim_to_turns(trim);
        if (trim.rows.size() < untrimmed.rows.size())
            ++trimmed;
        if (!tail_matches(whole, whole_layout, tail)) {
            ++mismatched;
            if (first_bad < 0)
                first_bad = start;
        }
    }
    if (mismatched > 0) {
        std::fprintf(stderr, "  %s: %d of %d cuts differ, first at byte %lld\n", name, mismatched,
                     cuts, static_cast<long long>(first_bad));
        const auto tail = read_file(path, format, size - first_bad);
        auto layout = lay_out(tail.reader->items(), {});
        trim_to_turns(layout);
        const auto row_text = [](const Layout& l, const std::vector<Item>& items, const Row& r) {
            if (r.item < 0) {
                const auto& p =
                    l.turns[static_cast<size_t>(r.turn)].parts[static_cast<size_t>(r.part)];
                return QStringLiteral("work steps=%1 dur=%2 sum=%3")
                    .arg(p.steps)
                    .arg(p.duration_ms)
                    .arg(p.summary);
            }
            const auto& i = items[static_cast<size_t>(r.item)];
            return QStringLiteral("%1 %2 | %3 | ans=%4 at=%5 dur=%6 task=%7 rec=%8")
                .arg(kind_name(i.kind), i.title, i.body.left(30))
                .arg(i.answered)
                .arg(i.at_ms)
                .arg(i.duration_ms)
                .arg(i.task)
                .arg(i.record);
        };
        const auto skip = whole_layout.rows.size() - layout.rows.size();
        for (size_t i = 0; i < layout.rows.size(); ++i)
            std::fprintf(stderr, "    T %s\n    W %s\n",
                         qPrintable(row_text(layout, tail.reader->items(), layout.rows[i])),
                         qPrintable(row_text(whole_layout, whole.reader->items(),
                                             whole_layout.rows[skip + i])));
    }
    require(mismatched == 0, "a tail shows the end of the whole reading at every cut");
    require(trimmed > 0, "some tails start mid-turn and drop the cut turn");
    const auto grown = read_end(path, format, 64, size);
    require(grown.tail && lay_out(grown.reader->items(), {}).turns.size() >= 2 &&
                tail_matches(whole, whole_layout, grown),
            "a tail holding less than two turns is read further back");
    // A cut exactly where a record starts reads that record; one byte into
    // it, the record is skipped, never read from its middle.
    const auto record_at = bytes.lastIndexOf('\n', bytes.lastIndexOf('\n', size - 2) - 1) + 1;
    const auto at = read_file(path, format, size - record_at);
    const auto into = read_file(path, format, size - record_at - 1);
    // What reading from the first line at `from` the reader can start at
    // makes (Codex starts at a turn's start), or from `from` when none can.
    const auto alone = [&](qsizetype from) {
        auto reader = make_reader(format);
        const auto lines = bytes.mid(from).split('\n');
        const bool any = std::ranges::any_of(
            lines, [&](const QByteArray& line) { return reader->starts_reading(line + '\n'); });
        bool started = !any;
        for (const auto& line : lines) {
            started = started || reader->starts_reading(line + '\n');
            if (started && !line.isEmpty())
                (void)reader->add(line);
        }
        return reader->items().size();
    };
    require(at.tail && at.offset == size && at.reader->items().size() == alone(record_at) &&
                into.reader->items().size() == alone(bytes.indexOf('\n', record_at) + 1),
            "a tail starts at the first whole record");
}

void tails_for_each_cli() {
    QTemporaryDir directory;
    tails_match("claude",
                write(directory, QStringLiteral("c.jsonl"), claude_conversation("c", 12, 40)),
                Format::claude);
    tails_match(
        "codex",
        fixture("codex/rollout-2026-11-10T10-00-00-0199a000-0000-7000-8000-00000000c0de.jsonl"),
        Format::codex);
    tails_match("antigravity", fixture("antigravity/transcript_full.jsonl"), Format::antigravity);
}

void wait_for(const std::function<bool()>& done, int ms = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThreadPool::globalInstance()->waitForDone(5);
    }
}

// Each row's record, to tell whose rows the model shows.
QStringList records(const TranscriptModel& model) {
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row)
        out << model.data(model.index(row), TranscriptModel::RecordRole).toString();
    return out;
}

// A conversation longer than the tail shows its end in the call that shows
// it, never the last conversation's rows, and its history replaces the
// tail once read, the rows at the end staying the same.
void cold_conversation_shows_its_end() {
    QTemporaryDir directory;
    const auto small =
        write(directory, QStringLiteral("small.jsonl"), claude_conversation("small", 2));
    const auto big =
        write(directory, QStringLiteral("big.jsonl"), claude_conversation("big", 60, 40000));
    require(QFileInfo(big).size() > 2 * TranscriptModel::tail_bytes, "the long one is long");
    TranscriptModel model;
    model.setPath(small);
    require(!model.loading() && model.rowCount() > 0, "a short conversation is read at once");
    int reloaded = 0;
    QObject::connect(&model, &TranscriptModel::reloaded, [&reloaded] { ++reloaded; });
    model.setPath(big);
    const auto shown = records(model);
    require(model.loading() && model.tailShown() && model.rowCount() > 0,
            "a long conversation shows its end at once while the rest is read");
    require(std::ranges::all_of(
                shown, [](const QString& r) { return r.isEmpty() || r.startsWith("big-"); }),
            "only the new conversation's rows show");
    wait_for([&] { return !model.loading(); });
    require(!model.loading() && !model.tailShown() && reloaded == 1, "the history came in");
    const auto whole = records(model);
    require(whole.size() > shown.size() && whole.mid(whole.size() - shown.size()) == shown,
            "the end shown first is the end of the whole thread");
    const auto all = read_file(big, Format::claude);
    require(model.rowCount() == static_cast<int>(lay_out(all.reader->items(), {}).rows.size()) &&
                model.items().size() == all.reader->items().size(),
            "all of it is read");
}

// Conversations read stay kept until the budget (count or memory) is
// full, the least recently shown going first and the shown one never.
void kept_within_budget() {
    QTemporaryDir directory;
    QStringList paths;
    for (const char* name : {"a", "b", "c", "d"})
        paths << write(directory, QString::fromLatin1(name) + ".jsonl",
                       claude_conversation(name, 3));
    TranscriptModel model;
    model.setBudget(qint64{1'073'741'824}, 2);
    for (const auto& path : paths)
        model.setPath(path);
    require(model.keptCount() == 2 && !model.kept(paths[0]) && model.kept(paths[1]) &&
                model.kept(paths[2]),
            "the least recently shown goes first");
    model.setPath(paths[1]);
    require(!model.loading() && records(model).front().startsWith("b-"),
            "a kept conversation shows at once");
    require(model.kept(paths[3]) && model.kept(paths[2]) && !model.kept(paths[1]),
            "the shown one is not counted as kept");
    model.setBudget(1, 8);
    require(model.keptCount() == 0 && model.rowCount() > 0,
            "a smaller budget lets kept ones go, never the shown one");
    // Read ahead only into room the budget has.
    model.setBudget(qint64{1'073'741'824}, 1);
    model.warm({paths[0], paths[2]});
    wait_for([&] { return model.kept(paths[0]); });
    QCoreApplication::processEvents();
    require(model.kept(paths[0]) && !model.kept(paths[2]), "reading ahead stops at the budget");
    model.setPath(paths[2]);
    model.setPath(paths[3]);
    require(model.kept(paths[2]) && !model.kept(paths[0]),
            "a conversation read ahead goes before one the person showed");
}

// A model destroyed while its threads read: the shown conversation's
// history (startLoad, on the global pool), a read-ahead and a kept one's
// catch-up (its own low-priority thread), and readings already queued back
// to it but not taken. Each is dropped, never handed to the gone model; the
// sanitizer builds check that no thread touches it. Files are long enough
// that the reads are still running when the model goes.
void destroyed_while_reading() {
    QTemporaryDir directory;
    const auto big =
        write(directory, QStringLiteral("big.jsonl"), claude_conversation("big", 60, 40000));
    const auto ahead =
        write(directory, QStringLiteral("ahead.jsonl"), claude_conversation("ahead", 60, 40000));
    const auto grown = claude_conversation("grown", 60, 40000);
    require(QFileInfo(big).size() > 2 * TranscriptModel::tail_bytes, "the shown one is long");
    enum class Case : std::uint8_t { read_ahead, catch_up, queued };
    int loading = 0, reading = 0, queued = 0;
    constexpr int rounds = 36;
    for (int round = 0; round < rounds; ++round) {
        const auto which = static_cast<Case>(round % 3);
        const auto kept =
            write(directory, QStringLiteral("kept.jsonl"), claude_conversation("kept", 2));
        auto* model = new TranscriptModel;
        model->setBudget(qint64{1} * 1024 * 1024 * 1024, 8);
        if (which == Case::catch_up)
            model->setPath(kept); // read at once, then kept
        model->setPath(big);      // its end now, the rest on the global pool
        loading += model->loading() ? 1 : 0;
        if (which == Case::read_ahead) {
            model->warm({ahead});
            reading += model->readingAhead() ? 1 : 0;
        } else if (which == Case::catch_up) {
            append(kept, grown);
            model->followKept();
            reading += model->readingAhead() && model->kept(kept) ? 1 : 0;
        } else {
            // Let the reading finish and queue itself back, then destroy
            // the model before the queue is taken.
            QThreadPool::globalInstance()->waitForDone();
            queued += model->loading() ? 1 : 0;
        }
        delete model;
        // Whatever was queued back is delivered now, to nothing.
        QCoreApplication::processEvents();
    }
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
    require(loading == rounds, "each model was loading the shown conversation when destroyed");
    require(reading == 2 * rounds / 3,
            "each read-ahead or catch-up was running when its model was destroyed");
    require(queued == rounds / 3, "each queued reading was not yet taken when destroyed");
    // A model made afterwards still gets its readings.
    TranscriptModel after;
    after.setPath(big);
    wait_for([&] { return !after.loading(); });
    require(!after.loading() && !after.tailShown(), "a later model's reading arrives");
}

// A kept conversation follows its file: what its CLI wrote while another
// was shown is read on (only the new lines; a result for an earlier call
// still finds the call), in the background or when it is shown again.
void kept_ones_follow_their_files() {
    QTemporaryDir directory;
    const auto a = write(directory, QStringLiteral("a.jsonl"), claude_conversation("a", 3));
    const auto b = write(directory, QStringLiteral("b.jsonl"), claude_conversation("b", 1));
    const QByteArray call =
        R"({"type":"user","uuid":"a-new-1","message":{"content":"One more"}})"
        "\n"
        R"({"type":"assistant","uuid":"a-new-2","message":{"content":[{"type":"tool_use","id":"late","name":"Bash","input":{"command":"ls"}}]}})"
        "\n";
    const QByteArray result =
        R"({"type":"user","uuid":"a-new-3","message":{"content":[{"type":"tool_result","tool_use_id":"late","content":"files"}]}})"
        "\n";
    TranscriptModel model;
    model.setPath(a);
    const auto before = model.items().size();
    model.setPath(b);
    append(a, call);
    model.followKept();
    wait_for([&] { return !model.readingAhead(); });
    append(a, result);
    model.setPath(a);
    require(!model.loading() && model.items().size() == before + 2,
            "what was written while away is shown at once");
    require(model.items().back().answered && model.items().back().result == "files",
            "a result for a call read in the background attaches to it");
}

// Where the view was, and the work opened, come back with the conversation.
void view_comes_back() {
    QTemporaryDir directory;
    const auto a = write(directory, QStringLiteral("a.jsonl"), claude_conversation("a", 6));
    const auto b = write(directory, QStringLiteral("b.jsonl"), claude_conversation("b", 2));
    TranscriptModel model;
    model.setPath(a);
    int work = -1;
    for (int row = 0; row < model.rowCount() && work < 0; ++row)
        if (model.data(model.index(row), TranscriptModel::KindRole).toString() == "work")
            work = row;
    require(work >= 0, "a turn folds to a work row");
    model.toggleWork(work);
    QObject::connect(&model, &TranscriptModel::leaving,
                     [&model] { model.keepView(4, 12.5, false); });
    model.setPath(b);
    require(model.view().value("following").toBool(), "another conversation opens at its end");
    QObject::disconnect(&model, &TranscriptModel::leaving, nullptr, nullptr);
    model.setPath(a);
    const auto view = model.view();
    require(!view.value("following").toBool() && view.value("row").toInt() == 4 &&
                view.value("offset").toDouble() == 12.5,
            "the place left comes back");
    require(model.data(model.index(work), TranscriptModel::ExpandedRole).toBool(),
            "opened work stays open");
}

// The view itself, offscreen: each switch shows the new conversation at
// its end (a cold one's end first) without the list making rows it does
// not show, and a place scrolled to comes back. Bounds are loose: software
// rendering on a shared machine.
void view_switches() {
    QTemporaryDir directory;
    const auto big =
        write(directory, QStringLiteral("big.jsonl"), claude_conversation("big", 60, 40000));
    const auto a = write(directory, QStringLiteral("a.jsonl"), claude_conversation("a", 30));
    const auto b = write(directory, QStringLiteral("b.jsonl"), claude_conversation("b", 30));
    TranscriptModel model;
    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(800, 900);
    view.setSource(QUrl::fromLocalFile(QStringLiteral(LAPIS_TRANSCRIPT_QML)));
    require(view.status() == QQuickView::Ready && view.rootObject() != nullptr,
            "the visual view loads");
    if (view.rootObject() == nullptr)
        return;
    view.rootObject()->setProperty("model", QVariant::fromValue(static_cast<QObject*>(&model)));
    auto* list = view.rootObject()->property("list").value<QObject*>();
    view.show();
    const auto settle = [] {
        for (int i = 0; i < 6; ++i)
            QCoreApplication::processEvents();
    };
    const auto at_end = [&] { return list->property("atYEnd").toBool(); };
    double slowest = 0;
    for (const auto& path : {a, b, big, a, b, big}) {
        QElapsedTimer timer;
        timer.start();
        model.setPath(path);
        settle();
        (void)view.grabWindow();
        slowest = std::max(slowest, static_cast<double>(timer.elapsed()));
        require(model.rowCount() > 0 && list->property("count").toInt() == model.rowCount(),
                "the list shows the conversation's rows");
        require(at_end(), "a conversation opens at its end");
    }
    require(slowest < 3000, "each switch draws its first frame in time");
    // A cold one's history comes in under the end it showed.
    model.setPath(a);
    settle();
    TranscriptModel cold;
    view.rootObject()->setProperty("model", QVariant::fromValue(static_cast<QObject*>(&cold)));
    cold.setPath(big);
    require(cold.tailShown() && at_end(), "a cold conversation shows its end first");
    wait_for([&] { return !cold.loading(); });
    settle();
    require(!cold.tailShown() && at_end() && list->property("count").toInt() == cold.rowCount(),
            "its history comes in above, the view staying at the end");
    view.rootObject()->setProperty("model", QVariant::fromValue(static_cast<QObject*>(&model)));
    settle();
    // A place scrolled to in one conversation comes back after another.
    model.setPath(a);
    settle();
    view.rootObject()->setProperty("following", false);
    QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 3),
                              Q_ARG(int, 0 /* ListView.Beginning */));
    list->setProperty("contentY", list->property("contentY").toReal() + 7);
    settle();
    // The row at the top and how far into it the view is (content
    // coordinates move as rows are made, so contentY alone does not say).
    const auto place = [&] {
        const auto y = list->property("contentY").toReal();
        int row = -1;
        QMetaObject::invokeMethod(list, "indexAt", Q_RETURN_ARG(int, row),
                                  Q_ARG(qreal, list->property("width").toReal() / 2),
                                  Q_ARG(qreal, y + 1));
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item),
                                  Q_ARG(int, row));
        return std::pair{row, item != nullptr ? y - item->y() : -1.0};
    };
    const auto left = place();
    model.setPath(b);
    settle();
    require(view.rootObject()->property("following").toBool() && at_end(),
            "another conversation opens at its end");
    model.setPath(a);
    settle();
    const auto back = place();
    if (back.first != left.first || std::abs(back.second - left.second) >= 1)
        std::fprintf(stderr,
                     "  left at row %d +%.1f, back at row %d +%.1f (view %s, following %d)\n",
                     left.first, left.second, back.first, back.second,
                     qPrintable(QVariant(model.view()).toString() +
                                QString::number(model.view().value("row").toInt())),
                     view.rootObject()->property("following").toBool());
    require(!view.rootObject()->property("following").toBool() && back.first == left.first &&
                std::abs(back.second - left.second) < 1,
            "the place scrolled to comes back");
}
} // namespace

int run(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    // The view writes a chart style under ~/.lapis; keep it in a throwaway home.
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    tails_for_each_cli();
    cold_conversation_shows_its_end();
    kept_within_budget();
    kept_ones_follow_their_files();
    destroyed_while_reading();
    view_comes_back();
    view_switches();
    QThreadPool::globalInstance()->waitForDone();
    if (failures == 0)
        std::puts("transcript_switch_test: PASS");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (...) {
        std::fprintf(stderr, "transcript_switch_test: FAIL unexpected exception\n");
        return 1;
    }
}
