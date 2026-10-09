#include <QApplication>
#include <QMainWindow>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QPushButton>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QInputDialog>
#include <QFileDialog>
#include <QStyle>
#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QProcess>
#include <QTimer>
#include <QSettings>
#include <QStandardPaths>
#include <QSaveFile>
#include <QTextStream>
#include <QDir>
#include <QTimeZone>
#include <QTemporaryDir>
#include <QtSql>
#include <cmath>
#include <stdexcept>
#include <time.h>
#include <cstdio>

struct Stamp {
    qint64 utc, mono, boot;
};

static qint64 ns(clockid_t id) {
    timespec t{};
    if (clock_gettime(id, &t) != 0) throw std::runtime_error("clock_gettime failed");
    return qint64(t.tv_sec) * 1000000000 + t.tv_nsec;
}

static Stamp stamp() {
    const auto utc = ns(CLOCK_REALTIME);
    return {utc, ns(CLOCK_MONOTONIC), ns(CLOCK_BOOTTIME)};
}

static double offset(qint64 expectedMs, qint64 actualNs) {
    return double(expectedMs * 1000000 - actualNs) / 1e9;
}

static bool interrupted(Stamp start, Stamp end) {
    return std::abs((end.utc - start.utc) - (end.mono - start.mono)) > 50000000
        || std::abs((end.boot - start.boot) - (end.mono - start.mono)) > 50000000;
}

struct Reference {
    bool valid = false;
    QString source, reason = "Verifica del riferimento in corso…";
    double bound = 0;
    qint64 fetched = 0;
};

static Reference parseReference(const QByteArray &csv, qint64 fetched) {
    Reference r;
    r.fetched = fetched;
    const auto fields = QString::fromUtf8(csv).trimmed().split(',');
    r.reason = "Riferimento Chrony non disponibile o non sincronizzato";
    if (fields.size() != 14 || fields[13] != "Normal" || fields[0] == "7F7F0101") return r;
    bool good = true;
    auto number = [&](int i) {
        bool ok;
        double v = fields[i].toDouble(&ok);
        good &= ok && std::isfinite(v);
        return v;
    };
    double stratum = number(2), correction = number(4), delay = number(10), dispersion = number(11);
    if (!good || stratum < 1 || stratum > 15 || delay < 0 || dispersion < 0) return r;
    r.valid = true;
    r.source = fields[1];
    r.bound = std::abs(correction) + delay / 2 + dispersion;
    r.reason.clear();
    return r;
}

static void sqlCheck(bool ok, const QSqlError &error) {
    if (!ok) throw std::runtime_error(error.text().toStdString());
}

class Plot : public QWidget {
public:
    QVector<QPointF> points;
    explicit Plot(QWidget *parent = nullptr) : QWidget(parent) { setMinimumHeight(160); }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF area = rect().adjusted(62, 15, -24, -32);
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(area.bottomLeft(), area.bottomRight());
        p.drawLine(area.topLeft(), area.bottomLeft());
        p.setPen(palette().color(QPalette::Text));
        if (points.isEmpty()) { p.drawText(area, Qt::AlignCenter, "Lo storico apparirà qui"); return; }
        double lo = points[0].y(), hi = lo;
        for (auto v : points) { lo = std::min(lo, v.y()); hi = std::max(hi, v.y()); }
        double margin = std::max(0.1, (hi - lo) * 0.15);
        lo -= margin; hi += margin;
        p.drawText(QRectF(0, area.top() - 8, 56, 20), Qt::AlignRight, QString::number(hi, 'f', 1));
        p.drawText(QRectF(0, area.bottom() - 12, 56, 20), Qt::AlignRight, QString::number(lo, 'f', 1));
        p.drawText(QRectF(area.left(), area.bottom() + 8, area.width(), 20), Qt::AlignCenter,
                   "Scarto in secondi · tempo →");
        double x0 = points.front().x(), span = std::max(1.0, points.back().x() - x0);
        QPainterPath line;
        p.setPen(QPen(palette().color(QPalette::Highlight), 2));
        for (int i = 0; i < points.size(); ++i) {
            QPointF pos(area.left() + (points[i].x() - x0) / span * area.width(),
                        area.bottom() - (points[i].y() - lo) / (hi - lo) * area.height());
            if (i == 0) line.moveTo(pos); else line.lineTo(pos);
            p.drawEllipse(pos, 3, 3);
        }
        p.drawPath(line);
    }
};

class Window : public QMainWindow {
    QSqlDatabase db;
    QComboBox *watches;
    QDateTimeEdit *expected;
    QPushButton *arm, *capture, *discard;
    QLabel *clock, *referenceLabel, *result, *rate;
    QTableWidget *history;
    Plot *plot;
    QProcess chrony;
    Reference reference;
    Stamp started{};
    bool armed = false, pending = false;
    Stamp captured{};
    Reference capturedReference;
    qint64 target = 0, watchId = 0;
    int series = 0;
    QString themePath;
    QByteArray themeBytes;

    void error(const QString &message) { QMessageBox::critical(this, "Watchrate", message); }
    void query(QSqlQuery &q) { bool ok = q.exec(); sqlCheck(ok, q.lastError()); }
    void refreshReference() {
        if (chrony.state() != QProcess::NotRunning) return;
        chrony.start("chronyc", {"-c", "tracking"});
        QTimer::singleShot(4000, &chrony, [this] {
            if (chrony.state() != QProcess::NotRunning) chrony.kill();
        });
    }
    bool referenceReady() const {
        return reference.valid && ns(CLOCK_BOOTTIME) - reference.fetched < 15000000000LL;
    }
    void setArmed(bool value) {
        armed = value;
        expected->setEnabled(pending && !value);
        watches->setEnabled(!value && !pending);
        arm->setText(pending ? "Salva misura" : value ? "Annulla" : "Prepara misura");
        capture->setEnabled(value);
        discard->setEnabled(value || pending);
    }
    void prepare() {
        if (pending) { save(); return; }
        if (armed) { setArmed(false); result->setText("Misura annullata"); return; }
        if (!watchId) { error("Aggiungi prima un orologio."); return; }
        if (!referenceReady()) { error("Attendi un riferimento Chrony sincronizzato e aggiornato."); return; }
        started = stamp();
        setArmed(true);
        capture->setFocus();
        result->setText("Premi Spazio e memorizza l’ora segnata dal tuo orologio allo stop");
    }
    void record(Stamp event) {
        if (!armed) return;
        setArmed(false);
        if (interrupted(started, event)) { error("Misura scartata: salto dell’ora o sospensione durante l’attesa."); return; }
        if (!referenceReady()) { error("Misura scartata: riferimento non più disponibile."); return; }
        captured = event;
        capturedReference = reference;
        pending = true;
        setArmed(false);
        expected->setDateTime(QDateTime::fromMSecsSinceEpoch(event.utc / 1000000000 * 1000));
        result->setText("Riferimento congelato. Inserisci l’ora del tuo orologio allo stop, poi salva.");
        expected->setFocus();
    }
    void save() {
        if (!pending) return;
        target = expected->dateTime().toMSecsSinceEpoch();
        if (!expected->dateTime().isValid()) { error("Ora non valida."); return; }
        double delta = offset(target, captured.utc);
        if (std::abs(delta) > 43200) { error("Scarto oltre 12 ore: controlla data e ora segnata allo stop."); return; }
        QSqlQuery q(db);
        q.prepare("INSERT INTO samples(watch,series,utc_ns,expected_ms,offset,ntp_bound,source) VALUES(?,?,?,?,?,?,?)");
        q.addBindValue(watchId); q.addBindValue(series); q.addBindValue(captured.utc); q.addBindValue(target);
        q.addBindValue(delta); q.addBindValue(capturedReference.bound); q.addBindValue(capturedReference.source);
        try { query(q); reloadHistory(); }
        catch (const std::exception &e) { error(QString::fromUtf8(e.what())); return; }
        pending = false;
        setArmed(false);
        result->setText(QString("%1 s · %2").arg(delta, 0, 'f', 3)
                           .arg(delta >= 0 ? "in anticipo" : "in ritardo"));
    }
    void cancel() {
        pending = false;
        setArmed(false);
        result->setText("Misura annullata");
    }
    void reloadWatches() {
        QSignalBlocker block(watches);
        watches->clear();
        QSqlQuery q("SELECT id,name,kind FROM watches ORDER BY name", db);
        sqlCheck(!q.lastError().isValid(), q.lastError());
        while (q.next()) watches->addItem(q.value(1).toString() + " · " + q.value(2).toString(), q.value(0));
        selectWatch();
    }
    void selectWatch() {
        setArmed(false);
        watchId = watches->currentData().toLongLong();
        QSqlQuery q(db);
        q.prepare("SELECT series FROM watches WHERE id=?"); q.addBindValue(watchId); query(q);
        series = q.next() ? q.value(0).toInt() : 0;
        reloadHistory();
        result->setText("Prepara la misura, premi stop e inserisci l’ora del tuo orologio");
    }
    void reloadHistory() {
        QSqlQuery q(db);
        q.prepare("SELECT utc_ns,offset,ntp_bound,series FROM samples WHERE watch=? ORDER BY utc_ns");
        q.addBindValue(watchId); query(q);
        history->setRowCount(0); plot->points.clear();
        while (q.next()) {
            int row = history->rowCount(); history->insertRow(row);
            qint64 t = q.value(0).toLongLong(); double delta = q.value(1).toDouble();
            QStringList cells{QDateTime::fromMSecsSinceEpoch(t / 1000000).toString("dd/MM/yyyy HH:mm:ss"),
                QString::number(delta, 'f', 3), QString::number(q.value(2).toDouble() * 1000, 'f', 1),
                QString::number(q.value(3).toInt() + 1)};
            for (int c = 0; c < cells.size(); ++c) history->setItem(row, c, new QTableWidgetItem(cells[c]));
            if (q.value(3).toInt() == series) plot->points.push_back({double(t) / 1e9, delta});
        }
        plot->update();
        const auto &pts = plot->points;
        if (pts.size() < 2 || pts.back().x() - pts.front().x() < 3600) {
            rate->setText("Marcia: servono almeno due misure nella stessa serie, distanziate di un’ora");
        } else {
            double days = (pts.back().x() - pts.front().x()) / 86400;
            double daily = (pts.back().y() - pts.front().y()) / days;
            rate->setText(QString("Marcia media: %1 s/giorno · intervallo %2 giorni%3")
                .arg(daily, 0, 'f', 3).arg(days, 0, 'f', 2)
                .arg(days < 1 ? " · intervallo breve, errore manuale amplificato" : ""));
        }
        history->scrollToBottom();
    }
    void addWatch() {
        if (armed || pending) return;
        bool ok;
        QString name = QInputDialog::getText(this, "Nuovo orologio", "Nome o modello", QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        QString kind = QInputDialog::getItem(this, "Movimento", "Tipo", {"Meccanico", "Quarzo"}, 0, false, &ok);
        if (!ok) return;
        QSqlQuery q(db); q.prepare("INSERT INTO watches(name,kind) VALUES(?,?)");
        q.addBindValue(name); q.addBindValue(kind);
        if (!q.exec()) { error(q.lastError().text()); return; }
        qint64 id = q.lastInsertId().toLongLong();
        reloadWatches(); watches->setCurrentIndex(watches->findData(id));
    }
    void newSeries() {
        if (armed || pending || !watchId) return;
        if (QMessageBox::question(this, "Nuova serie", "Hai regolato l’orologio? Le nuove misure avranno una marcia separata; lo storico resta conservato.") != QMessageBox::Yes) return;
        QSqlQuery q(db); q.prepare("UPDATE watches SET series=series+1 WHERE id=?"); q.addBindValue(watchId);
        if (!q.exec()) { error(q.lastError().text()); return; }
        selectWatch();
    }
    void exportCsv() {
        if (armed || pending || !watchId) return;
        QString path = QFileDialog::getSaveFileName(this, "Esporta storico", "watchrate.csv", "CSV (*.csv)");
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { error(file.errorString()); return; }
        QTextStream out(&file);
        out << "utc,watch_utc,offset_seconds,ntp_bound_seconds,source,series\n";
        QSqlQuery q(db);
        q.prepare("SELECT utc_ns,expected_ms,offset,ntp_bound,source,series FROM samples WHERE watch=? ORDER BY utc_ns");
        q.addBindValue(watchId);
        if (!q.exec()) { error(q.lastError().text()); return; }
        while (q.next()) {
            QString source = q.value(4).toString(); source.replace('"', "\"\"");
            out << QDateTime::fromMSecsSinceEpoch(q.value(0).toLongLong() / 1000000, QTimeZone::UTC).toString(Qt::ISODateWithMs) << ','
                << QDateTime::fromMSecsSinceEpoch(q.value(1).toLongLong(), QTimeZone::UTC).toString(Qt::ISODateWithMs) << ','
                << QString::number(q.value(2).toDouble(), 'f', 9) << ','
                << QString::number(q.value(3).toDouble(), 'f', 9) << ",\"" << source << "\"," << q.value(5).toInt() + 1 << '\n';
        }
        out.flush();
        if (!file.commit()) error(file.errorString());
    }
    void loadTheme() {
        QString path = QFile::exists(themePath) ? themePath
            : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/kdeglobals";
        QFile file(path);
        QByteArray bytes;
        if (file.open(QIODevice::ReadOnly)) bytes = file.readAll();
        if (bytes == themeBytes) return;
        themeBytes = bytes;
        QPalette p = QApplication::style()->standardPalette();
        if (!bytes.isEmpty()) {
            QSettings settings(path, QSettings::IniFormat);
            if (path == themePath) settings.beginGroup("Palette");
            const QPair<const char *, QPalette::ColorRole> roles[] = {
                {"Window", QPalette::Window}, {"WindowText", QPalette::WindowText}, {"Base", QPalette::Base},
                {"AlternateBase", QPalette::AlternateBase}, {"Text", QPalette::Text}, {"Button", QPalette::Button},
                {"ButtonText", QPalette::ButtonText}, {"Highlight", QPalette::Highlight},
                {"HighlightedText", QPalette::HighlightedText}, {"Link", QPalette::Link}};
            for (auto [name, role] : roles) {
                const QMap<QString, QString> kdeKeys{
                    {"Window", "Colors:Window/BackgroundNormal"}, {"WindowText", "Colors:Window/ForegroundNormal"},
                    {"Base", "Colors:View/BackgroundNormal"}, {"AlternateBase", "Colors:View/BackgroundAlternate"},
                    {"Text", "Colors:View/ForegroundNormal"}, {"Button", "Colors:Button/BackgroundNormal"},
                    {"ButtonText", "Colors:Button/ForegroundNormal"}, {"Highlight", "Colors:Selection/BackgroundNormal"},
                    {"HighlightedText", "Colors:Selection/ForegroundNormal"}, {"Link", "Colors:View/ForegroundLink"}};
                QVariant entry = settings.value(path == themePath ? QString(name) : kdeKeys.value(name));
                QString value = entry.metaType().id() == QMetaType::QStringList
                    ? entry.toStringList().join(',') : entry.toString();
                auto rgb = value.split(',');
                QColor color = rgb.size() == 3 ? QColor(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt()) : QColor(value);
                if (color.isValid()) p.setColor(role, color);
            }
        }
        QApplication::setPalette(p);
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if ((armed || pending) && event->type() == QEvent::KeyPress) {
            auto key = dynamic_cast<QKeyEvent *>(event);
            if (!key) return false;
            if (armed && key->key() == Qt::Key_Space) {
                if (!key->isAutoRepeat()) { const Stamp click = stamp(); record(click); }
                return true;
            }
            if (key->key() == Qt::Key_Escape) { cancel(); return true; }
        }
        if (armed && object == capture && event->type() == QEvent::MouseButtonPress) {
            auto mouse = dynamic_cast<QMouseEvent *>(event);
            if (mouse && mouse->button() == Qt::LeftButton) {
                const Stamp click = stamp(); record(click); return true;
            }
        }
        if (armed && event->type() == QEvent::ApplicationDeactivate) {
            setArmed(false); result->setText("Misura annullata: applicazione fuori fuoco");
        }
        return QMainWindow::eventFilter(object, event);
    }
public:
    void exerciseCapture() {
        QSqlQuery q(db);
        bool ok = q.exec("INSERT INTO watches(name,kind) VALUES('Test','Quarzo')"); sqlCheck(ok, q.lastError());
        reloadWatches();
        reference = parseReference("C0A80101,192.168.1.1,3,0,0,0,0,0,0,0,0.04,0.004,64,Normal", ns(CLOCK_BOOTTIME));
        prepare();
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ", true);
        eventFilter(capture, &repeat);
        if (!armed) throw std::runtime_error("auto-repeat captured a sample");
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        eventFilter(capture, &key);
        eventFilter(capture, &key);
        if (!pending || armed) throw std::runtime_error("capture did not freeze reference");
        const qint64 frozenUtc = captured.utc;
        expected->setDateTime(QDateTime::fromMSecsSinceEpoch(frozenUtc / 1000000 + 2500));
        const qint64 entered = expected->dateTime().toMSecsSinceEpoch();
        save();
        q.exec("SELECT count(*) FROM samples"); q.next();
        if (q.value(0).toInt() != 1) throw std::runtime_error("single-click capture duplicated");
        q.exec("SELECT utc_ns,offset FROM samples"); q.next();
        if (q.value(0).toLongLong() != frozenUtc
            || std::abs(q.value(1).toDouble() - offset(entered, frozenUtc)) > 1e-9)
            throw std::runtime_error("save used current time instead of frozen reference");
        prepare();
        QEvent focus(QEvent::ApplicationDeactivate); eventFilter(this, &focus);
        if (armed) throw std::runtime_error("focus loss did not disarm");
        prepare(); reference.valid = false;
        if (referenceReady()) throw std::runtime_error("invalid reference accepted");
        setArmed(false);
        q.exec("DELETE FROM samples");
        q.prepare("INSERT INTO samples(watch,series,utc_ns,expected_ms,offset,ntp_bound,source) VALUES(?,0,?,0,?,0.02,'test')");
        for (int i = 0; i < 2; ++i) {
            q.bindValue(0, watchId); q.bindValue(1, qint64(i + 1) * 86400000000000LL);
            q.bindValue(2, i * 2.0); query(q);
        }
        reloadHistory();
        if (!rate->text().contains("2.000 s/giorno")) throw std::runtime_error("daily rate");
        QTemporaryDir theme;
        themePath = theme.filePath("theme.ini");
        QSettings colors(themePath, QSettings::IniFormat);
        colors.setValue("Palette/Window", QStringList{"18", "52", "86"}); colors.sync(); loadTheme();
        if (QApplication::palette().color(QPalette::Window) != QColor("#123456"))
            throw std::runtime_error("theme reload");
    }
    explicit Window(const QString &database) {
        db = QSqlDatabase::addDatabase("QSQLITE"); db.setDatabaseName(database);
        bool opened = db.open(); sqlCheck(opened, db.lastError());
        for (const auto &statement : {
            "PRAGMA foreign_keys=ON",
            "CREATE TABLE IF NOT EXISTS watches(id INTEGER PRIMARY KEY,name TEXT NOT NULL,kind TEXT NOT NULL,series INTEGER NOT NULL DEFAULT 0)",
            "CREATE TABLE IF NOT EXISTS samples(id INTEGER PRIMARY KEY,watch INTEGER NOT NULL REFERENCES watches(id),series INTEGER NOT NULL,utc_ns INTEGER NOT NULL,expected_ms INTEGER NOT NULL,offset REAL NOT NULL,ntp_bound REAL NOT NULL,source TEXT NOT NULL)"}) {
            QSqlQuery q(db); bool ok = q.exec(statement); sqlCheck(ok, q.lastError());
        }
        setWindowTitle("Watchrate"); setWindowIcon(QIcon::fromTheme("chronometer")); resize(880, 780);
        auto central = new QWidget; setCentralWidget(central);
        auto layout = new QVBoxLayout(central); layout->setContentsMargins(24, 24, 24, 24); layout->setSpacing(14);
        auto top = new QHBoxLayout;
        watches = new QComboBox; top->addWidget(watches, 1);
        auto add = new QPushButton("Aggiungi orologio"); top->addWidget(add);
        auto reset = new QPushButton("Ho regolato l’ora"); top->addWidget(reset);
        auto exportButton = new QPushButton("Esporta CSV"); top->addWidget(exportButton); layout->addLayout(top);
        clock = new QLabel; QFont clockFont = font(); clockFont.setPointSize(36); clockFont.setStyleHint(QFont::Monospace);
        clock->setFont(clockFont); clock->setAlignment(Qt::AlignCenter); layout->addWidget(clock);
        referenceLabel = new QLabel; referenceLabel->setAlignment(Qt::AlignCenter); referenceLabel->setWordWrap(true); layout->addWidget(referenceLabel);
        auto form = new QFormLayout;
        expected = new QDateTimeEdit(QDateTime::currentDateTime()); expected->setDisplayFormat("dd/MM/yyyy HH:mm:ss"); expected->setCalendarPopup(true);
        form->addRow("Ora locale del tuo orologio allo stop", expected); layout->addLayout(form);
        auto controls = new QHBoxLayout;
        arm = new QPushButton("Prepara misura"); capture = new QPushButton("Stop · Spazio"); capture->setEnabled(false);
        discard = new QPushButton("Scarta"); discard->setEnabled(false);
        capture->setMinimumHeight(56); controls->addWidget(arm); controls->addWidget(capture, 1); controls->addWidget(discard); layout->addLayout(controls);
        result = new QLabel; result->setAlignment(Qt::AlignCenter); result->setWordWrap(true); layout->addWidget(result);
        auto disclaimer = new QLabel("+ anticipo · − ritardo. I millisecondi sono risoluzione, non precisione del gesto.\n"
            "Il riferimento è l’ora di sistema sincronizzata via NTP; latenza di input e reazione non sono compensate.");
        disclaimer->setAlignment(Qt::AlignCenter); disclaimer->setWordWrap(true); layout->addWidget(disclaimer);
        rate = new QLabel; rate->setWordWrap(true); layout->addWidget(rate);
        plot = new Plot; layout->addWidget(plot);
        history = new QTableWidget(0, 4); history->setHorizontalHeaderLabels({"Misura · ora locale", "Scarto · s", "Limite NTP · ms", "Serie"});
        history->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); history->setEditTriggers(QAbstractItemView::NoEditTriggers);
        history->setAlternatingRowColors(true); layout->addWidget(history, 1);
        connect(add, &QPushButton::clicked, this, [this] { addWatch(); });
        connect(reset, &QPushButton::clicked, this, [this] { newSeries(); });
        connect(exportButton, &QPushButton::clicked, this, [this] { exportCsv(); });
        connect(watches, &QComboBox::currentIndexChanged, this, [this] { selectWatch(); });
        connect(arm, &QPushButton::clicked, this, [this] { prepare(); });
        connect(discard, &QPushButton::clicked, this, [this] { cancel(); });
        connect(capture, &QPushButton::clicked, this, [this] { record(stamp()); });
        connect(&chrony, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            reference = code == 0 && status == QProcess::NormalExit
                ? parseReference(chrony.readAllStandardOutput(), ns(CLOCK_BOOTTIME)) : Reference{};
            if (!reference.valid) reference.reason = "Chrony non disponibile o non sincronizzato: acquisizione disabilitata";
        });
        connect(&chrony, &QProcess::errorOccurred, this, [this] {
            reference = {}; reference.reason = "Impossibile interrogare chronyc: acquisizione disabilitata";
        });
        themePath = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/watchrate/theme.ini";
        auto tick = new QTimer(this);
        connect(tick, &QTimer::timeout, this, [this] {
            clock->setText((pending ? QDateTime::fromMSecsSinceEpoch(captured.utc / 1000000)
                                   : QDateTime::currentDateTime()).toString("HH:mm:ss.zzz"));
            const auto &shown = pending ? capturedReference : reference;
            referenceLabel->setText(pending || referenceReady()
                ? QString("%1 · %2 · limite stimato ±%3 ms (solo riferimento)")
                    .arg(pending ? "Riferimento allo stop" : "NTP sincronizzato")
                    .arg(shown.source).arg(shown.bound * 1000, 0, 'f', 1)
                : reference.valid ? "Stato NTP scaduto: attendo una nuova verifica" : reference.reason);
        }); tick->start(33);
        auto poll = new QTimer(this); connect(poll, &QTimer::timeout, this, [this] { refreshReference(); loadTheme(); }); poll->start(5000);
        qApp->installEventFilter(this); reloadWatches(); refreshReference(); loadTheme();
    }
    ~Window() override { if (chrony.state() != QProcess::NotRunning) { chrony.kill(); chrony.waitForFinished(1000); } }
};

static int selfTest() {
    auto check = [](bool ok, const char *name) { if (!ok) throw std::runtime_error(name); };
    check(offset(10000, 9750000000LL) == 0.25, "fast sign");
    check(offset(10000, 10250000000LL) == -0.25, "slow sign");
    check(offset(86400000, 86399000000000LL) == 1.0, "midnight");
    Stamp a{1000000000, 1000000000, 1000000000};
    check(!interrupted(a, {2000000000, 2000000000, 2000000000}), "normal interval");
    check(interrupted(a, {3000000000, 2000000000, 2000000000}), "clock step");
    check(interrupted(a, {3000000000, 2000000000, 3000000000}), "suspend");
    auto r = parseReference("C0A80101,192.168.1.1,3,1791531186,-0.001,0,0,0,0,0,0.04,0.004,64,Normal", 1);
    check(r.valid && std::abs(r.bound - 0.025) < 1e-10, "reference bound");
    check(!parseReference("bad", 1).valid, "malformed reference");
    check(!parseReference("7F7F0101,127.127.1.1,3,0,0,0,0,0,0,0,0,0,64,Normal", 1).valid, "local reference");
    check(!parseReference("C0A80101,x,0,0,nan,0,0,0,0,0,0,0,64,Normal", 1).valid, "invalid reference");
    Window window(":memory:");
    window.exerciseCapture();
    std::puts("PASS: offset signs, midnight, step/suspend detection, Chrony parsing, SQLite persistence, single-key capture, repeat/focus handling, daily rate, theme reload and UI construction");
    return 0;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("watchrate"); QCoreApplication::setOrganizationName("ooonea");
    QGuiApplication::setDesktopFileName("dev.ooonea.Watchrate");
    try {
        if (app.arguments().contains("--self-test")) return selfTest();
        QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (!QDir().mkpath(path)) throw std::runtime_error("Cannot create application data directory");
        Window window(path + "/watchrate.sqlite"); window.show();
        return app.exec();
    } catch (const std::exception &e) { qCritical("Watchrate: %s", e.what()); return 1; }
}
