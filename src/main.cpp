#include <QApplication>
#include <QMainWindow>
#include <QComboBox>
#include <QListWidget>
#include <QToolButton>
#include <QMenu>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QFrame>
#include <QStackedWidget>
#include <QCloseEvent>
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
    QListWidget *watches;
    QPushButton *add, *edit, *remove, *up, *down, *removeSamples;
    QToolButton *sortButton, *options;
    QLabel *watchTitle, *watchKind, *phase, *latest;
    QWidget *reading;
    QStackedWidget *pages;
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
        arm->setText(pending ? "Salva misura" : "Nuova misura");
        arm->setVisible(!value);
        arm->setEnabled(watchId != 0);
        capture->setEnabled(value);
        capture->setVisible(value);
        discard->setVisible(value || pending);
        reading->setVisible(pending);
        phase->setText(pending ? "3 · Inserisci la lettura" : value ? "2 · Premi Stop" : "1 · Prepara la misura");
        bool idle = !value && !pending;
        add->setEnabled(idle); edit->setEnabled(idle && watchId); remove->setEnabled(idle && watchId);
        up->setEnabled(idle && watches->currentRow() > 0);
        down->setEnabled(idle && watches->currentRow() >= 0 && watches->currentRow() + 1 < watches->count());
        sortButton->setEnabled(idle && watches->count() > 1);
        options->setEnabled(idle && watchId);
        updateSampleAction();
    }
    void prepare() {
        if (pending) { save(); return; }
        if (armed) { setArmed(false); result->setText("Misura annullata"); return; }
        if (!watchId) { error("Aggiungi prima un orologio."); return; }
        if (!referenceReady()) { error("Attendi un riferimento Chrony sincronizzato e aggiornato."); return; }
        started = stamp();
        setArmed(true);
        capture->setFocus();
        result->setText("Al passaggio della lancetta premi Spazio. Ricorda l’ora segnata allo Stop.");
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
        result->setText("Il riferimento è congelato: puoi inserire la lettura con calma.");
        expected->setCurrentSection(QDateTimeEdit::SecondSection);
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
    void reloadWatches(qint64 selected = 0) {
        if (!selected) selected = watchId;
        QSignalBlocker block(watches);
        watches->clear();
        QSqlQuery q("SELECT id,name,kind FROM watches ORDER BY sort_order,name COLLATE NOCASE,id", db);
        sqlCheck(!q.lastError().isValid(), q.lastError());
        int row = 0;
        while (q.next()) {
            auto item = new QListWidgetItem(QIcon::fromTheme("chronometer"),
                q.value(1).toString() + "\n" + q.value(2).toString(), watches);
            item->setData(Qt::UserRole, q.value(0));
            item->setData(Qt::UserRole + 1, q.value(1));
            item->setData(Qt::UserRole + 2, q.value(2));
            item->setToolTip(q.value(1).toString() + " · " + q.value(2).toString());
            item->setSizeHint(QSize(180, 68));
            if (q.value(0).toLongLong() == selected) row = watches->count() - 1;
        }
        watches->setCurrentRow(watches->count() ? row : -1);
        selectWatch();
    }
    void selectWatch() {
        auto item = watches->currentItem();
        watchId = item ? item->data(Qt::UserRole).toLongLong() : 0;
        pages->setCurrentIndex(watchId ? 1 : 0);
        watchTitle->setText(item ? item->data(Qt::UserRole + 1).toString() : "La tua collezione");
        watchKind->setText(item ? item->data(Qt::UserRole + 2).toString() : "");
        setArmed(false);
        QSqlQuery q(db);
        q.prepare("SELECT series FROM watches WHERE id=?"); q.addBindValue(watchId); query(q);
        series = q.next() ? q.value(0).toInt() : 0;
        reloadHistory();
        result->setText("Premi Nuova misura, poi Spazio su un secondo preciso.");
    }
    void storeOrder() {
        if (armed || pending) return;
        if (!db.transaction()) { error(db.lastError().text()); return; }
        QSqlQuery q(db); q.prepare("UPDATE watches SET sort_order=? WHERE id=?");
        for (int i = 0; i < watches->count(); ++i) {
            q.bindValue(0, i); q.bindValue(1, watches->item(i)->data(Qt::UserRole));
            if (!q.exec()) { db.rollback(); error(q.lastError().text()); reloadWatches(); return; }
        }
        if (!db.commit()) { db.rollback(); error(db.lastError().text()); reloadWatches(); return; }
        setArmed(false);
    }
    void moveWatch(int delta) {
        if (armed || pending) return;
        int row = watches->currentRow(), destination = row + delta;
        if (row < 0 || destination < 0 || destination >= watches->count()) return;
        QSignalBlocker block(watches);
        auto item = watches->takeItem(row); watches->insertItem(destination, item); watches->setCurrentItem(item);
        storeOrder();
    }
    void alphabetical() {
        if (armed || pending) return;
        QSignalBlocker block(watches);
        watches->sortItems(Qt::AscendingOrder); storeOrder();
    }
    void reloadHistory() {
        QSqlQuery q(db);
        q.prepare("SELECT utc_ns,offset,ntp_bound,series,id FROM samples WHERE watch=? ORDER BY utc_ns,id");
        q.addBindValue(watchId); query(q);
        history->setRowCount(0); plot->points.clear();
        while (q.next()) {
            int row = history->rowCount(); history->insertRow(row);
            qint64 t = q.value(0).toLongLong(); double delta = q.value(1).toDouble();
            QStringList cells{QDateTime::fromMSecsSinceEpoch(t / 1000000).toString("dd/MM/yyyy HH:mm:ss"),
                QString::number(delta, 'f', 3), QString::number(q.value(2).toDouble() * 1000, 'f', 1),
                QString::number(q.value(3).toInt() + 1)};
            for (int c = 0; c < cells.size(); ++c) history->setItem(row, c, new QTableWidgetItem(cells[c]));
            history->item(row, 0)->setData(Qt::UserRole, q.value(4));
            if (q.value(3).toInt() == series) plot->points.push_back({double(t) / 1e9, delta});
        }
        if (history->rowCount()) {
            int row = history->rowCount() - 1;
            double delta = history->item(row, 1)->text().toDouble();
            latest->setText(QString("Ultimo scarto  %1%2 s").arg(delta >= 0 ? "+" : "").arg(delta, 0, 'f', 3));
        } else latest->setText("Nessuna misura salvata");
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
        updateSampleAction();
    }
    QList<qint64> selectedSamples() const {
        QList<qint64> ids;
        for (const auto &row : history->selectionModel()->selectedRows())
            ids.append(history->item(row.row(), 0)->data(Qt::UserRole).toLongLong());
        return ids;
    }
    void updateSampleAction() {
        const int count = selectedSamples().size();
        removeSamples->setEnabled(count && !armed && !pending);
        removeSamples->setText(count > 1 ? QString("Elimina %1 misure").arg(count) : "Elimina misura");
    }
    bool eraseSamples(const QList<qint64> &ids) {
        if (armed || pending || !watchId || ids.isEmpty()) return false;
        if (!db.transaction()) { error(db.lastError().text()); return false; }
        QSqlQuery q(db); q.prepare("DELETE FROM samples WHERE id=? AND watch=?");
        for (qint64 id : ids) {
            q.bindValue(0, id); q.bindValue(1, watchId);
            if (!q.exec() || q.numRowsAffected() != 1) {
                db.rollback();
                error(q.lastError().isValid() ? q.lastError().text() : "Lo storico è cambiato: nessuna misura cancellata.");
                reloadHistory(); return false;
            }
        }
        if (!db.commit()) { db.rollback(); error(db.lastError().text()); return false; }
        reloadHistory();
        result->setText(ids.size() == 1 ? "Misura eliminata" : QString("%1 misure eliminate").arg(ids.size()));
        return true;
    }
    void deleteSamples() {
        const auto ids = selectedSamples();
        if (armed || pending || ids.isEmpty()) return;
        QMessageBox confirmation(QMessageBox::Warning, "Elimina misure",
            QString("Eliminare %1 %2 di «%3»?\nL’eliminazione è permanente.")
                .arg(ids.size()).arg(ids.size() == 1 ? "misura selezionata" : "misure selezionate")
                .arg(watches->currentItem()->data(Qt::UserRole + 1).toString()),
            QMessageBox::NoButton, this);
        auto destroy = confirmation.addButton(ids.size() == 1 ? "Elimina misura" : "Elimina misure", QMessageBox::DestructiveRole);
        auto cancelButton = confirmation.addButton("Annulla", QMessageBox::RejectRole);
        confirmation.setDefaultButton(cancelButton); confirmation.setEscapeButton(cancelButton); confirmation.exec();
        if (confirmation.clickedButton() == destroy) eraseSamples(ids);
    }
    void watchDialog(bool editing) {
        if (armed || pending || (editing && !watchId)) return;
        QDialog dialog(this); dialog.setWindowTitle(editing ? "Modifica orologio" : "Aggiungi orologio");
        auto layout = new QVBoxLayout(&dialog);
        auto form = new QFormLayout;
        QLineEdit name;
        name.setPlaceholderText("Es. Seiko Presage");
        QComboBox kind; kind.addItems({"Meccanico", "Quarzo"});
        if (editing) {
            name.setText(watches->currentItem()->data(Qt::UserRole + 1).toString());
            kind.setCurrentText(watches->currentItem()->data(Qt::UserRole + 2).toString());
        }
        form->addRow("Nome / modello", &name); form->addRow("Movimento", &kind); layout->addLayout(form);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Save)->setText(editing ? "Salva" : "Aggiungi");
        buttons->button(QDialogButtonBox::Cancel)->setText("Annulla");
        layout->addWidget(buttons);
        auto validate = [&] { buttons->button(QDialogButtonBox::Save)->setEnabled(!name.text().trimmed().isEmpty()); };
        connect(&name, &QLineEdit::textChanged, &dialog, validate); validate();
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.resize(420, 170);
        if (dialog.exec() != QDialog::Accepted) return;
        QSqlQuery q(db);
        q.prepare(editing ? "UPDATE watches SET name=?,kind=? WHERE id=?"
            : "INSERT INTO watches(name,kind,sort_order) VALUES(?,?,(SELECT COALESCE(MAX(sort_order),-1)+1 FROM watches))");
        q.addBindValue(name.text().trimmed()); q.addBindValue(kind.currentText());
        if (editing) q.addBindValue(watchId);
        if (!q.exec()) { error(q.lastError().text()); return; }
        reloadWatches(editing ? watchId : q.lastInsertId().toLongLong());
    }
    bool eraseWatch(qint64 id) {
        if (armed || pending || !id) return false;
        if (!db.transaction()) return false;
        for (const char *statement : {"DELETE FROM samples WHERE watch=?", "DELETE FROM watches WHERE id=?"}) {
            QSqlQuery q(db); q.prepare(statement); q.addBindValue(id);
            if (!q.exec()) { db.rollback(); error(q.lastError().text()); return false; }
        }
        if (!db.commit()) { db.rollback(); error(db.lastError().text()); return false; }
        reloadWatches();
        return true;
    }
    void deleteWatch() {
        if (armed || pending || !watchId) return;
        QSqlQuery q(db); q.prepare("SELECT count(*) FROM samples WHERE watch=?"); q.addBindValue(watchId); query(q); q.next();
        QMessageBox confirmation(QMessageBox::Warning, "Elimina orologio",
            QString("Eliminare «%1» e le sue %2 misure?\nL’eliminazione è permanente.")
                .arg(watches->currentItem()->data(Qt::UserRole + 1).toString()).arg(q.value(0).toInt()),
            QMessageBox::NoButton, this);
        auto destroy = confirmation.addButton("Elimina orologio", QMessageBox::DestructiveRole);
        auto cancelButton = confirmation.addButton("Annulla", QMessageBox::RejectRole);
        confirmation.setDefaultButton(cancelButton); confirmation.setEscapeButton(cancelButton);
        confirmation.exec();
        if (confirmation.clickedButton() == destroy) eraseWatch(watchId);
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
        setPalette(p);
        setStyleSheet(styleSheet());
    }
protected:
    void closeEvent(QCloseEvent *event) override {
        if (pending) {
            QMessageBox prompt(QMessageBox::Question, "Misura non salvata",
                "La lettura allo Stop non è ancora salvata.", QMessageBox::NoButton, this);
            auto close = prompt.addButton("Scarta e chiudi", QMessageBox::DestructiveRole);
            auto stay = prompt.addButton("Torna alla misura", QMessageBox::RejectRole);
            prompt.setDefaultButton(stay); prompt.setEscapeButton(stay); prompt.exec();
            if (prompt.clickedButton() != close) { event->ignore(); return; }
        }
        event->accept();
    }
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
    void exerciseSampleDeletion() {
        const qint64 original = watchId;
        QSqlQuery q(db); bool ok = q.exec("INSERT INTO watches(name,kind,sort_order) VALUES('Deletion test','Quarzo',3)");
        sqlCheck(ok, q.lastError()); const qint64 fixture = q.lastInsertId().toLongLong();
        q.prepare("INSERT INTO samples(watch,series,utc_ns,expected_ms,offset,ntp_bound,source) VALUES(?,0,?,0,?,0.02,'test')");
        for (int i = 0; i < 3; ++i) {
            q.bindValue(0, fixture); q.bindValue(1, qint64(i + 1) * 86400000000000LL); q.bindValue(2, i * 2.0); query(q);
        }
        reloadWatches(fixture); history->selectRow(0);
        const auto first = selectedSamples();
        if (first.size() != 1 || !removeSamples->isEnabled()) throw std::runtime_error("sample selection");
        reference = parseReference("C0A80101,192.168.1.1,3,0,0,0,0,0,0,0,0.04,0.004,64,Normal", ns(CLOCK_BOOTTIME));
        prepare();
        if (eraseSamples(first) || removeSamples->isEnabled()) throw std::runtime_error("sample deletion while armed");
        cancel();
        prepare(); record(stamp());
        if (eraseSamples(first) || removeSamples->isEnabled() || !pending)
            throw std::runtime_error("sample deletion while reading pending");
        cancel();
        if (!eraseSamples(first) || history->rowCount() != 2 || plot->points.size() != 2
            || !rate->text().contains("2.000 s/giorno"))
            throw std::runtime_error("single sample deletion and graph refresh");
        history->selectAll();
        if (selectedSamples().size() != 2 || !eraseSamples(selectedSamples())
            || history->rowCount() || !plot->points.isEmpty() || !latest->text().contains("Nessuna")
            || !rate->text().contains("servono"))
            throw std::runtime_error("multiple sample deletion and empty history");
        reloadWatches(original);
        if (history->rowCount() != 2) throw std::runtime_error("other watch samples changed");
        eraseWatch(fixture); reloadWatches(original);
    }
    void exerciseCollection() {
        const qint64 original = watchId;
        QSqlQuery q(db);
        bool ok = q.exec("INSERT INTO watches(name,kind,sort_order) VALUES('Alpha','Meccanico',1)"); sqlCheck(ok, q.lastError());
        const qint64 alpha = q.lastInsertId().toLongLong();
        ok = q.exec("INSERT INTO watches(name,kind,sort_order) VALUES('Beta','Quarzo',2)"); sqlCheck(ok, q.lastError());
        const qint64 beta = q.lastInsertId().toLongLong();
        reloadWatches(beta);
        if (watchId != beta || history->rowCount() != 0) throw std::runtime_error("watch selection");
        moveWatch(-1); reloadWatches(beta);
        if (watches->currentRow() != 1 || watchId != beta) throw std::runtime_error("persistent ordering");
        // Exercise the same model move signal emitted by drag and drop.
        if (!watches->model()->moveRow({}, 1, {}, 0)) throw std::runtime_error("drag model move");
        reloadWatches(beta);
        if (watches->currentRow() != 0) throw std::runtime_error("drag ordering persistence");
        alphabetical(); reloadWatches(beta);
        if (watches->item(0)->data(Qt::UserRole).toLongLong() != alpha)
            throw std::runtime_error("alphabetical ordering");
        reference = parseReference("C0A80101,192.168.1.1,3,0,0,0,0,0,0,0,0.04,0.004,64,Normal", ns(CLOCK_BOOTTIME));
        prepare(); moveWatch(-1);
        if (eraseWatch(beta)) throw std::runtime_error("delete while armed");
        cancel();
        if (!eraseWatch(beta) || watches->count() != 2) throw std::runtime_error("watch deletion");
        reloadWatches(original);
        if (history->rowCount() != 2) throw std::runtime_error("other watch history lost");
        if (!eraseWatch(original)) throw std::runtime_error("watch with history deletion");
        q.exec("SELECT count(*) FROM samples"); q.next();
        if (q.value(0).toInt() != 0) throw std::runtime_error("orphan samples");
        if (!eraseWatch(alpha) || watchId || pages->currentIndex() != 0)
            throw std::runtime_error("empty collection state");
    }
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
        QSqlQuery columns("PRAGMA table_info(watches)", db);
        bool ordered = false;
        while (columns.next()) ordered |= columns.value(1).toString() == "sort_order";
        if (!ordered) {
            QSqlQuery migration(db); bool ok = migration.exec("ALTER TABLE watches ADD COLUMN sort_order INTEGER NOT NULL DEFAULT 0");
            sqlCheck(ok, migration.lastError());
        }
        setWindowTitle("Watchrate"); setWindowIcon(QIcon::fromTheme("chronometer")); resize(1080, 820);
        auto central = new QWidget; setCentralWidget(central);
        auto outer = new QHBoxLayout(central); outer->setContentsMargins(18, 18, 18, 18); outer->setSpacing(22);
        auto sidebar = new QFrame; sidebar->setObjectName("sidebar"); sidebar->setFixedWidth(220);
        auto side = new QVBoxLayout(sidebar); side->setContentsMargins(12, 12, 12, 12); side->setSpacing(10);
        auto brand = new QLabel("Watchrate"); brand->setObjectName("brand"); side->addWidget(brand);
        auto caption = new QLabel("LA TUA COLLEZIONE"); caption->setObjectName("muted"); side->addWidget(caption);
        watches = new QListWidget; watches->setDragDropMode(QAbstractItemView::InternalMove);
        watches->setDefaultDropAction(Qt::MoveAction); watches->setSpacing(4);
        watches->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        watches->setSelectionMode(QAbstractItemView::SingleSelection); side->addWidget(watches, 1);
        auto ordering = new QHBoxLayout;
        up = new QPushButton("↑"); down = new QPushButton("↓");
        up->setToolTip("Sposta in alto"); down->setToolTip("Sposta in basso");
        sortButton = new QToolButton; sortButton->setText("A–Z"); sortButton->setToolTip("Ordina per nome");
        ordering->addWidget(up); ordering->addWidget(down); ordering->addWidget(sortButton); side->addLayout(ordering);
        auto reorderHint = new QLabel("Trascina per riordinare"); reorderHint->setObjectName("muted"); side->addWidget(reorderHint);
        add = new QPushButton("+  Aggiungi orologio"); side->addWidget(add);
        auto management = new QHBoxLayout;
        edit = new QPushButton("Modifica"); remove = new QPushButton("Elimina");
        management->addWidget(edit); management->addWidget(remove); side->addLayout(management); outer->addWidget(sidebar);
        pages = new QStackedWidget; outer->addWidget(pages, 1);
        auto empty = new QWidget; auto emptyLayout = new QVBoxLayout(empty);
        emptyLayout->addStretch(); auto emptyTitle = new QLabel("Ogni orologio ha il suo ritmo."); emptyTitle->setObjectName("heading");
        emptyTitle->setWordWrap(true); emptyTitle->setAlignment(Qt::AlignCenter); emptyLayout->addWidget(emptyTitle);
        auto emptyText = new QLabel("Aggiungi il primo orologio per misurare anticipo, ritardo e marcia nel tempo.");
        emptyText->setWordWrap(true); emptyText->setAlignment(Qt::AlignCenter); emptyLayout->addWidget(emptyText);
        auto first = new QPushButton("Aggiungi il primo orologio"); emptyLayout->addWidget(first, 0, Qt::AlignCenter);
        emptyLayout->addStretch(); pages->addWidget(empty);
        auto detail = new QWidget; pages->addWidget(detail);
        auto layout = new QVBoxLayout(detail); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(14);
        auto header = new QHBoxLayout; auto names = new QVBoxLayout;
        watchTitle = new QLabel; watchTitle->setObjectName("heading"); watchTitle->setWordWrap(true);
        watchKind = new QLabel; watchKind->setObjectName("muted"); names->addWidget(watchTitle); names->addWidget(watchKind); header->addLayout(names, 1);
        options = new QToolButton; options->setText("•••"); options->setToolTip("Azioni dell’orologio");
        options->setPopupMode(QToolButton::InstantPopup); auto menu = new QMenu(options);
        auto reset = menu->addAction("Ho regolato l’ora · nuova serie");
        auto exportAction = menu->addAction("Esporta storico CSV");
        menu->addSeparator(); auto info = menu->addAction("Come funziona la misura"); options->setMenu(menu); header->addWidget(options); layout->addLayout(header);
        auto card = new QFrame; card->setObjectName("measurement");
        auto measurement = new QVBoxLayout(card); measurement->setContentsMargins(20, 16, 20, 16); measurement->setSpacing(12);
        phase = new QLabel; phase->setObjectName("phase"); measurement->addWidget(phase);
        clock = new QLabel; QFont clockFont = font(); clockFont.setPointSize(32); clockFont.setStyleHint(QFont::Monospace);
        clock->setFont(clockFont); clock->setAlignment(Qt::AlignCenter); measurement->addWidget(clock);
        referenceLabel = new QLabel; referenceLabel->setObjectName("muted"); referenceLabel->setAlignment(Qt::AlignCenter);
        referenceLabel->setWordWrap(true); measurement->addWidget(referenceLabel);
        result = new QLabel; result->setAlignment(Qt::AlignCenter); result->setWordWrap(true); measurement->addWidget(result);
        reading = new QWidget; auto form = new QVBoxLayout(reading); form->setContentsMargins(0, 2, 0, 2);
        auto readingLabel = new QLabel("Che ora segnava il tuo orologio allo Stop?"); form->addWidget(readingLabel);
        expected = new QDateTimeEdit(QDateTime::currentDateTime()); expected->setDisplayFormat("dd/MM/yyyy HH:mm:ss");
        expected->setCalendarPopup(true); expected->setMinimumHeight(40); form->addWidget(expected); measurement->addWidget(reading);
        auto controls = new QHBoxLayout;
        arm = new QPushButton("Nuova misura"); capture = new QPushButton("Stop · Spazio"); discard = new QPushButton("Scarta");
        arm->setObjectName("primary"); capture->setObjectName("primary"); arm->setMinimumHeight(48); capture->setMinimumHeight(48);
        controls->addWidget(arm, 1); controls->addWidget(capture, 1); controls->addWidget(discard); measurement->addLayout(controls); layout->addWidget(card);
        latest = new QLabel; latest->setObjectName("summary"); layout->addWidget(latest);
        rate = new QLabel; rate->setWordWrap(true); layout->addWidget(rate);
        plot = new Plot; layout->addWidget(plot, 1);
        auto historyHeader = new QHBoxLayout;
        auto historyTitle = new QLabel("Storico delle misure"); historyTitle->setObjectName("section"); historyHeader->addWidget(historyTitle, 1);
        removeSamples = new QPushButton("Elimina misura"); removeSamples->setEnabled(false); historyHeader->addWidget(removeSamples); layout->addLayout(historyHeader);
        history = new QTableWidget(0, 4); history->setHorizontalHeaderLabels({"Data e ora", "Scarto · s", "NTP · ms", "Serie"});
        history->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); history->verticalHeader()->hide();
        history->setSelectionBehavior(QAbstractItemView::SelectRows); history->setEditTriggers(QAbstractItemView::NoEditTriggers);
        history->setSelectionMode(QAbstractItemView::ExtendedSelection);
        history->setToolTip("Seleziona una misura. Usa Ctrl o Maiusc per selezionarne più di una.");
        history->setAlternatingRowColors(true); history->setMinimumHeight(140); layout->addWidget(history, 1);
        connect(add, &QPushButton::clicked, this, [this] { watchDialog(false); });
        connect(first, &QPushButton::clicked, this, [this] { watchDialog(false); });
        connect(edit, &QPushButton::clicked, this, [this] { watchDialog(true); });
        connect(remove, &QPushButton::clicked, this, [this] { deleteWatch(); });
        connect(removeSamples, &QPushButton::clicked, this, [this] { deleteSamples(); });
        connect(history, &QTableWidget::itemSelectionChanged, this, [this] { updateSampleAction(); });
        connect(up, &QPushButton::clicked, this, [this] { moveWatch(-1); });
        connect(down, &QPushButton::clicked, this, [this] { moveWatch(1); });
        connect(sortButton, &QToolButton::clicked, this, [this] { alphabetical(); });
        connect(reset, &QAction::triggered, this, [this] { newSeries(); });
        connect(exportAction, &QAction::triggered, this, [this] { exportCsv(); });
        connect(info, &QAction::triggered, this, [this] {
            QMessageBox::information(this, "Come funziona", "1. Prepara la misura.\n2. Premi Stop o Spazio al passaggio su un secondo preciso.\n"
                "3. Inserisci l’ora segnata dall’orologio allo Stop e salva.\n\nScarto = lettura − riferimento congelato. + anticipo, − ritardo.\n"
                "I millisecondi indicano la risoluzione: la reazione e la latenza di input non sono compensate. Il limite NTP riguarda solo il riferimento.");
        });
        connect(watches, &QListWidget::currentRowChanged, this, [this] { selectWatch(); });
        connect(watches, &QListWidget::itemDoubleClicked, this, [this] { watchDialog(true); });
        connect(watches->model(), &QAbstractItemModel::rowsMoved, this, [this] { storeOrder(); });
        connect(arm, &QPushButton::clicked, this, [this] { prepare(); });
        connect(discard, &QPushButton::clicked, this, [this] { cancel(); });
        connect(capture, &QPushButton::clicked, this, [this] { record(stamp()); });
        setStyleSheet("QFrame#sidebar, QFrame#measurement { background: palette(base); border-radius: 14px; }"
            "QListWidget { background: transparent; border: none; }"
            "QListWidget::item { border-radius: 8px; padding: 6px; }"
            "QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }"
            "QLabel#brand { font-size: 22px; font-weight: 700; } QLabel#heading { font-size: 23px; font-weight: 600; }"
            "QLabel#phase { color: palette(highlight); font-weight: 600; }"
            "QLabel#section, QLabel#summary { font-weight: 600; }"
            "QLabel#muted { color: palette(window-text); }"
            "QPushButton#primary { background: palette(highlight); color: palette(highlighted-text); border: none; border-radius: 9px; font-weight: 600; padding: 10px; }"
            "QPushButton#primary:disabled { background: palette(button); color: palette(mid); }"
            "QPushButton#primary:pressed { background: palette(button); color: palette(button-text); }");
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
                ? QString("%1 · ±%2 ms")
                    .arg(pending ? "Riferimento allo stop" : "NTP sincronizzato")
                    .arg(shown.bound * 1000, 0, 'f', 1)
                : reference.valid ? "Stato NTP scaduto: attendo una nuova verifica" : reference.reason);
            referenceLabel->setToolTip(QString("Sorgente: %1\nLimite stimato del solo riferimento NTP; errore del gesto escluso.").arg(shown.source));
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
    QTemporaryDir fixture;
    const QString legacyFile = fixture.filePath("legacy.sqlite");
    {
        auto seed = QSqlDatabase::addDatabase("QSQLITE", "legacy-fixture"); seed.setDatabaseName(legacyFile);
        bool ok = seed.open(); sqlCheck(ok, seed.lastError());
        QSqlQuery q(seed);
        ok = q.exec("CREATE TABLE watches(id INTEGER PRIMARY KEY,name TEXT NOT NULL,kind TEXT NOT NULL,series INTEGER NOT NULL DEFAULT 0)");
        sqlCheck(ok, q.lastError());
    }
    QSqlDatabase::removeDatabase("legacy-fixture");
    Window window(legacyFile);
    window.exerciseCapture();
    window.exerciseSampleDeletion();
    window.exerciseCollection();
    std::puts("PASS: measurement, frozen-reference save, clock checks, rate, theme, legacy migration, selection, persistent button/drag/alphabetic order, deletion and empty state");
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
