#include "keymapeditor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegExp>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStandardPaths>
#include <QDateTime>

namespace {
const QColor kAccent(0x5B, 0x3D, 0xF5);
const int kRadius = 18;

QString keymapDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/keymap";
    QDir().mkpath(dir);
    return dir;
}

QString keyName(int key)
{
    const char *name = QMetaEnum::fromType<Qt::Key>().valueToKey(key);
    return name ? QString::fromLatin1(name) : QString();
}

QString buttonName(Qt::MouseButton b)
{
    const char *name = QMetaEnum::fromType<Qt::MouseButtons>().valueToKey(b);
    return name ? QString::fromLatin1(name) : QString();
}

QJsonObject posObj(const QPointF &p)
{
    QJsonObject o;
    o.insert("x", qBound(0.0, p.x(), 1.0));
    o.insert("y", qBound(0.0, p.y(), 1.0));
    return o;
}

QPointF readPos(const QJsonValue &v)
{
    const QJsonObject o = v.toObject();
    return QPointF(o.value("x").toDouble(0.5), o.value("y").toDouble(0.5));
}
} // namespace

KeymapEditor::KeymapEditor(QWidget *videoWidget, const QString &serial)
    : QWidget(videoWidget)
    , m_serial(serial)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    videoWidget->installEventFilter(this);
    setGeometry(videoWidget->rect());

    QString safe = serial;
    safe.replace(QRegExp("[^A-Za-z0-9_.-]"), "_");
    m_path = keymapDir() + "/k2k_" + safe + ".json";

    m_bar = new QWidget(this);
    m_bar->setStyleSheet("QWidget{background:rgba(20,16,40,200);border-radius:8px;}"
                         "QPushButton{color:white;background:#5B3DF5;border:none;border-radius:6px;padding:4px 8px;}"
                         "QPushButton:hover{background:#7C5CFA;}"
                         "QLabel{color:white;background:transparent;}");
    auto *lay = new QHBoxLayout(m_bar);
    lay->setContentsMargins(6, 4, 6, 4);
    lay->setSpacing(4);
    auto addBtn = [&](const QString &text, void (KeymapEditor::*slot)()) {
        auto *b = new QPushButton(text, m_bar);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QPushButton::clicked, this, slot);
        lay->addWidget(b);
    };
    addBtn(tr("WASD stick"), &KeymapEditor::addWheel);
    addBtn(tr("Mouse look"), &KeymapEditor::addLook);
    m_recBtn = new QPushButton(tr("Record macro"), m_bar);
    m_recBtn->setFocusPolicy(Qt::NoFocus);
    connect(m_recBtn, &QPushButton::clicked, this, &KeymapEditor::toggleRecord);
    lay->addWidget(m_recBtn);
    addBtn(tr("Open"), &KeymapEditor::open);
    addBtn(tr("Save and apply"), &KeymapEditor::save);
    auto *closeBtn = new QPushButton(tr("Close"), m_bar);
    closeBtn->setFocusPolicy(Qt::NoFocus);
    connect(closeBtn, &QPushButton::clicked, this, [this]() {
        emit closed();
        close();
    });
    lay->addWidget(closeBtn);
    m_status = new QLabel(m_bar);
    lay->addWidget(m_status);
    m_bar->adjustSize();
    m_bar->move(8, 8);

    if (QFile::exists(m_path)) {
        loadFile(m_path);
    }
    setStatus(tr("Click the screen, then press a key"));
    setFocus();
}

bool KeymapEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}

QPointF KeymapEditor::toRel(const QPoint &p) const
{
    return QPointF(qBound(0.0, double(p.x()) / qMax(1, width()), 1.0),
                   qBound(0.0, double(p.y()) / qMax(1, height()), 1.0));
}

QPoint KeymapEditor::toPx(const QPointF &r) const
{
    return QPoint(int(r.x() * width()), int(r.y() * height()));
}

QPoint KeymapEditor::wheelPoint(const Node &n, int k) const
{
    const QPoint c = toPx(n.pos);
    switch (k) {
    case 0: return c + QPoint(0, -int(n.uo * height()));
    case 1: return c + QPoint(0, int(n.dn * height()));
    case 2: return c + QPoint(-int(n.lo * width()), 0);
    default: return c + QPoint(int(n.ro * width()), 0);
    }
}

int KeymapEditor::hitTest(const QPoint &p, int *part) const
{
    if (part) {
        *part = -1;
    }
    for (int i = m_nodes.size() - 1; i >= 0; --i) {
        const Node &n = m_nodes[i];
        if (n.kind == Node::Wheel) {
            for (int k = 0; k < 4; ++k) {
                if ((wheelPoint(n, k) - p).manhattanLength() <= 16) {
                    if (part) {
                        *part = k;
                    }
                    return i;
                }
            }
            if ((toPx(n.pos) - p).manhattanLength() <= 20) {
                return i;
            }
            continue;
        }
        if ((toPx(n.pos) - p).manhattanLength() <= kRadius) {
            return i;
        }
    }
    return -1;
}

QString KeymapEditor::keyLabel(const QString &keyName)
{
    QString s = keyName;
    if (s.startsWith("Key_")) {
        s = s.mid(4);
    }
    if (s == "LeftButton") return QStringLiteral("L");
    if (s == "RightButton") return QStringLiteral("R");
    if (s == "MiddleButton") return QStringLiteral("M");
    if (s == "QuoteLeft") return QStringLiteral("~");
    return s.isEmpty() ? QStringLiteral("?") : s;
}

void KeymapEditor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), m_recording ? QColor(120, 0, 0, 60) : QColor(0, 0, 0, 70));
    if (m_recording) {
        p.setPen(QPen(Qt::white, 2));
        for (int s = 0; s < m_recSteps.size(); ++s) {
            const QPoint sp = toPx(m_recSteps[s].second);
            p.setBrush(QColor(0xE5, 0x48, 0x4D, 200));
            p.drawEllipse(sp, 11, 11);
            p.drawText(QRect(sp - QPoint(11, 11), QSize(22, 22)), Qt::AlignCenter, QString::number(s + 1));
        }
    }
    QFont f = p.font();
    f.setBold(true);
    f.setPointSize(9);
    p.setFont(f);
    for (int i = 0; i < m_nodes.size(); ++i) {
        const Node &n = m_nodes[i];
        const QPoint c = toPx(n.pos);
        const bool waiting = i == m_capture;
        QColor fill = waiting ? QColor(0xFF, 0xB4, 0x00, 220) : QColor(kAccent.red(), kAccent.green(), kAccent.blue(), 200);
        p.setPen(QPen(Qt::white, 2));
        if (n.kind == Node::Wheel) {
            const int rx = int((n.lo + n.ro) / 2 * width());
            const int ry = int((n.uo + n.dn) / 2 * height());
            const QPoint mid = c + QPoint(int((n.ro - n.lo) / 2 * width()), int((n.dn - n.uo) / 2 * height()));
            p.setBrush(QColor(255, 255, 255, 40));
            p.drawEllipse(mid, rx, ry);
            const QString labels[4] = { keyLabel(n.up), keyLabel(n.down), keyLabel(n.left), keyLabel(n.right) };
            const QPoint at[4] = { wheelPoint(n, 0), wheelPoint(n, 1), wheelPoint(n, 2), wheelPoint(n, 3) };
            for (int k = 0; k < 4; ++k) {
                p.setBrush(waiting && m_captureField == k ? QColor(0xFF, 0xB4, 0x00, 220) : fill);
                p.drawEllipse(at[k], 13, 13);
                p.drawText(QRect(at[k] - QPoint(13, 13), QSize(26, 26)), Qt::AlignCenter, labels[k]);
            }
            p.setBrush(fill);
            p.drawEllipse(c, 6, 6);
            continue;
        }
        if (n.kind == Node::Macro) {
            for (int s = 0; s < n.steps.size(); ++s) {
                const QPoint sp = toPx(n.steps[s].second);
                p.setBrush(QColor(0xFF, 0x6B, 0x3D, 160));
                p.drawEllipse(sp, 9, 9);
                p.drawText(QRect(sp - QPoint(9, 9), QSize(18, 18)), Qt::AlignCenter, QString::number(s + 1));
            }
            p.setBrush(waiting ? QColor(0xFF, 0xB4, 0x00, 220) : QColor(0xFF, 0x6B, 0x3D, 220));
            p.drawRoundedRect(QRect(c - QPoint(kRadius + 6, kRadius), QSize(kRadius * 2 + 12, kRadius * 2)), 8, 8);
            p.drawText(QRect(c - QPoint(kRadius + 6, kRadius), QSize(kRadius * 2 + 12, kRadius * 2)), Qt::AlignCenter,
                       waiting ? QStringLiteral("M …") : QStringLiteral("M ") + keyLabel(n.key));
            continue;
        }
        if (n.kind == Node::Look) {
            p.setBrush(QColor(0x2F, 0xBF, 0x71, 200));
            p.drawRect(QRect(c - QPoint(kRadius, kRadius), QSize(kRadius * 2, kRadius * 2)));
            p.drawText(QRect(c - QPoint(kRadius, kRadius), QSize(kRadius * 2, kRadius * 2)), Qt::AlignCenter, tr("Look"));
            continue;
        }
        p.setBrush(fill);
        p.drawEllipse(c, kRadius, kRadius);
        QString label = waiting ? QStringLiteral("…") : keyLabel(n.key);
        if (n.kind == Node::ClickTwice) {
            label += QStringLiteral("×2");
        }
        p.drawText(QRect(c - QPoint(kRadius, kRadius), QSize(kRadius * 2, kRadius * 2)), Qt::AlignCenter, label);
    }
}

void KeymapEditor::setStatus(const QString &text)
{
    m_status->setText(text);
    m_bar->adjustSize();
}

void KeymapEditor::startCapture(int index, const QString &prompt)
{
    m_capture = index;
    setStatus(prompt);
    setFocus();
    update();
}

void KeymapEditor::finishCapture(const QString &keyName)
{
    if (m_capture < 0 || m_capture >= m_nodes.size()) {
        return;
    }
    Node &n = m_nodes[m_capture];
    if (n.kind == Node::Wheel) {
        QString *fields[4] = { &n.up, &n.down, &n.left, &n.right };
        *fields[m_captureField] = keyName;
        if (m_captureIsNew && m_captureField < 3) {
            ++m_captureField;
            const QString names[4] = { tr("up"), tr("down"), tr("left"), tr("right") };
            setStatus(tr("Press the key for %1").arg(names[m_captureField]));
            update();
            return;
        }
    } else {
        n.key = keyName;
    }
    m_capture = -1;
    m_captureField = 0;
    m_captureIsNew = false;
    setStatus(tr("Saved on 'Save and apply'"));
    update();
}

void KeymapEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        return;
    }
    if (m_capture < 0) {
        if (event->key() == Qt::Key_Escape) {
            emit closed();
            close();
        }
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (m_captureIsNew && m_nodes[m_capture].kind != Node::Wheel) {
            m_nodes.removeAt(m_capture);
        }
        m_capture = -1;
        m_captureField = 0;
        m_captureIsNew = false;
        setStatus(tr("Cancelled"));
        update();
        return;
    }
    const QString name = keyName(event->key());
    if (name.isEmpty()) {
        return;
    }
    finishCapture(name);
}

void KeymapEditor::mousePressEvent(QMouseEvent *event)
{
    if (m_recording) {
        if (event->button() == Qt::LeftButton) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const int delay = m_recSteps.isEmpty() ? 0 : int(qBound<qint64>(30, now - m_recLast, 10000));
            m_recLast = now;
            if (m_recSteps.size() < 50) {
                m_recSteps.append({ delay, toRel(event->pos()) });
            }
            setStatus(tr("Recording: %1 taps").arg(m_recSteps.size()));
            update();
        }
        return;
    }
    if (m_capture >= 0 && event->button() != Qt::LeftButton) {
        // Mouse buttons other than the left one can be mapped too.
        finishCapture(buttonName(event->button()));
        return;
    }
    int part = -1;
    const int hit = hitTest(event->pos(), &part);
    if (event->button() == Qt::RightButton) {
        if (hit >= 0) {
            showNodeMenu(hit, event->globalPos());
        }
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (hit >= 0) {
        m_drag = hit;
        m_dragPart = part;
        m_dragOffset = toPx(m_nodes[hit].pos) - event->pos();
        return;
    }
    Node n;
    n.pos = toRel(event->pos());
    m_nodes.append(n);
    m_captureIsNew = true;
    startCapture(m_nodes.size() - 1, tr("Press a key (or right/middle mouse button) for this spot"));
}

void KeymapEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag >= 0 && m_drag < m_nodes.size()) {
        Node &n = m_nodes[m_drag];
        if (n.kind == Node::Wheel && m_dragPart >= 0) {
            const QPoint c = toPx(n.pos);
            const double dx = qAbs(event->pos().x() - c.x()) / double(qMax(1, width()));
            const double dy = qAbs(event->pos().y() - c.y()) / double(qMax(1, height()));
            const double minR = 0.02;
            switch (m_dragPart) {
            case 0: n.uo = qMax(minR, dy); break;
            case 1: n.dn = qMax(minR, dy); break;
            case 2: n.lo = qMax(minR, dx); break;
            default: n.ro = qMax(minR, dx); break;
            }
            update();
            return;
        }
        const QPointF next = toRel(event->pos() + m_dragOffset);
        if (n.kind == Node::Macro) {
            const QPointF d = next - n.pos;
            for (auto &s : n.steps) {
                s.second += d;
            }
        }
        n.pos = next;
        update();
    }
}

void KeymapEditor::mouseReleaseEvent(QMouseEvent *)
{
    m_drag = -1;
    m_dragPart = -1;
}

void KeymapEditor::wheelEvent(QWheelEvent *event)
{
    const int hit = hitTest(event->position().toPoint());
    if (hit < 0 || m_nodes[hit].kind != Node::Wheel) {
        return;
    }
    Node &n = m_nodes[hit];
    const double f = event->angleDelta().y() > 0 ? 1.08 : 1 / 1.08;
    n.lo = qBound(0.02, n.lo * f, 0.45);
    n.ro = qBound(0.02, n.ro * f, 0.45);
    n.uo = qBound(0.02, n.uo * f, 0.45);
    n.dn = qBound(0.02, n.dn * f, 0.45);
    update();
}

void KeymapEditor::showNodeMenu(int index, const QPoint &globalPos)
{
    QMenu menu(this);
    Node &n = m_nodes[index];
    QAction *changeKey = nullptr;
    QAction *twice = nullptr;
    QAction *hold = nullptr;
    QAction *bigger = nullptr;
    QAction *smaller = nullptr;
    QAction *faster = nullptr;
    QAction *slower = nullptr;
    if (n.kind == Node::Click || n.kind == Node::ClickTwice) {
        changeKey = menu.addAction(tr("Change key"));
        twice = menu.addAction(tr("Tap twice"));
        twice->setCheckable(true);
        twice->setChecked(n.kind == Node::ClickTwice);
        hold = menu.addAction(tr("Leave key map mode after tap"));
        hold->setCheckable(true);
        hold->setChecked(n.hold);
    } else if (n.kind == Node::Wheel) {
        changeKey = menu.addAction(tr("Change keys"));
        bigger = menu.addAction(tr("Larger stick"));
        smaller = menu.addAction(tr("Smaller stick"));
    } else if (n.kind == Node::Macro) {
        changeKey = menu.addAction(tr("Change key"));
    } else if (n.kind == Node::Look) {
        faster = menu.addAction(tr("Faster look"));
        slower = menu.addAction(tr("Slower look"));
    }
    menu.addSeparator();
    QAction *remove = menu.addAction(tr("Delete"));
    QAction *chosen = menu.exec(globalPos);
    if (!chosen) {
        return;
    }
    if (chosen == remove) {
        m_nodes.removeAt(index);
    } else if (chosen == changeKey) {
        m_captureIsNew = n.kind == Node::Wheel;
        m_captureField = 0;
        startCapture(index, n.kind == Node::Wheel ? tr("Press the key for %1").arg(tr("up"))
                                                  : tr("Press the new key"));
        return;
    } else if (chosen == twice) {
        n.kind = twice->isChecked() ? Node::ClickTwice : Node::Click;
    } else if (chosen == hold) {
        n.hold = hold->isChecked();
    } else if (chosen == bigger) {
        n.lo = qMin(0.45, n.lo * 1.15);
        n.ro = qMin(0.45, n.ro * 1.15);
        n.uo = qMin(0.45, n.uo * 1.15);
        n.dn = qMin(0.45, n.dn * 1.15);
    } else if (chosen == smaller) {
        n.lo = qMax(0.02, n.lo / 1.15);
        n.ro = qMax(0.02, n.ro / 1.15);
        n.uo = qMax(0.02, n.uo / 1.15);
        n.dn = qMax(0.02, n.dn / 1.15);
    } else if (chosen == faster) {
        m_lookSpeedX *= 0.8;
        m_lookSpeedY *= 0.8;
    } else if (chosen == slower) {
        m_lookSpeedX *= 1.25;
        m_lookSpeedY *= 1.25;
    }
    update();
}

void KeymapEditor::toggleRecord()
{
    if (m_capture >= 0) {
        setStatus(tr("Press a key first, or Esc to cancel"));
        return;
    }
    if (!m_recording) {
        m_recording = true;
        m_recSteps.clear();
        m_recBtn->setText(tr("Stop recording"));
        setStatus(tr("Tap the spots in order, at the speed you want"));
        update();
        return;
    }
    m_recording = false;
    m_recBtn->setText(tr("Record macro"));
    if (m_recSteps.isEmpty()) {
        setStatus(tr("Nothing recorded"));
        update();
        return;
    }
    Node n;
    n.kind = Node::Macro;
    n.steps = m_recSteps;
    n.pos = m_recSteps.first().second;
    m_nodes.append(n);
    m_recSteps.clear();
    m_captureIsNew = true;
    startCapture(m_nodes.size() - 1, tr("Press the key that plays this macro"));
}

void KeymapEditor::addWheel()
{
    for (const Node &n : m_nodes) {
        if (n.kind == Node::Wheel) {
            setStatus(tr("A stick already exists; drag it to move"));
            return;
        }
    }
    Node n;
    n.kind = Node::Wheel;
    n.pos = QPointF(0.22, 0.62);
    m_nodes.append(n);
    setStatus(tr("Drag the centre onto the game's joystick, drag W/A/S/D to fit its size (mouse wheel resizes all)"));
    update();
}

void KeymapEditor::addLook()
{
    for (const Node &n : m_nodes) {
        if (n.kind == Node::Look) {
            setStatus(tr("Mouse look already exists; drag it to move"));
            return;
        }
    }
    Node n;
    n.kind = Node::Look;
    n.pos = QPointF(0.6, 0.4);
    m_nodes.append(n);
    setStatus(tr("Drag it onto an empty area used to turn the camera"));
    update();
}

QString KeymapEditor::toJson() const
{
    QJsonObject root;
    root.insert("comment", QStringLiteral("K2K Mirror"));
    root.insert("switchKey", m_switchKey);
    QJsonArray nodes;
    for (const Node &n : m_nodes) {
        QJsonObject o;
        switch (n.kind) {
        case Node::Click:
            o.insert("type", "KMT_CLICK");
            o.insert("key", n.key);
            o.insert("pos", posObj(n.pos));
            o.insert("switchMap", n.hold);
            break;
        case Node::ClickTwice:
            o.insert("type", "KMT_CLICK_TWICE");
            o.insert("key", n.key);
            o.insert("pos", posObj(n.pos));
            break;
        case Node::Wheel:
            o.insert("type", "KMT_STEER_WHEEL");
            o.insert("centerPos", posObj(n.pos));
            o.insert("leftOffset", n.lo);
            o.insert("rightOffset", n.ro);
            o.insert("upOffset", n.uo);
            o.insert("downOffset", n.dn);
            o.insert("leftKey", n.left);
            o.insert("rightKey", n.right);
            o.insert("upKey", n.up);
            o.insert("downKey", n.down);
            break;
        case Node::Macro: {
            o.insert("type", "KMT_CLICK_MULTI");
            o.insert("key", n.key);
            QJsonArray steps;
            for (const auto &s : n.steps) {
                QJsonObject st;
                st.insert("delay", s.first);
                st.insert("pos", posObj(s.second));
                steps.append(st);
            }
            o.insert("clickNodes", steps);
            break;
        }
        case Node::Look: {
            QJsonObject mm;
            mm.insert("startPos", posObj(n.pos));
            mm.insert("speedRatioX", m_lookSpeedX);
            mm.insert("speedRatioY", m_lookSpeedY);
            root.insert("mouseMoveMap", mm);
            continue;
        }
        }
        if (n.kind != Node::Wheel && n.key.isEmpty()) {
            continue;
        }
        nodes.append(o);
    }
    root.insert("keyMapNodes", nodes);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool KeymapEditor::loadFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    if (root.isEmpty()) {
        return false;
    }
    m_nodes.clear();
    m_switchKey = root.value("switchKey").toString("Key_QuoteLeft");
    if (root.contains("mouseMoveMap")) {
        const QJsonObject mm = root.value("mouseMoveMap").toObject();
        Node n;
        n.kind = Node::Look;
        n.pos = readPos(mm.value("startPos"));
        m_lookSpeedX = mm.value("speedRatioX").toDouble(mm.value("speedRatio").toDouble(2.0));
        m_lookSpeedY = mm.value("speedRatioY").toDouble(mm.value("speedRatio").toDouble(1.0));
        m_nodes.append(n);
    }
    for (const QJsonValue &v : root.value("keyMapNodes").toArray()) {
        const QJsonObject o = v.toObject();
        const QString type = o.value("type").toString();
        Node n;
        if (type == "KMT_CLICK" || type == "KMT_CLICK_TWICE") {
            n.kind = type == "KMT_CLICK" ? Node::Click : Node::ClickTwice;
            n.key = o.value("key").toString();
            n.pos = readPos(o.value("pos"));
            n.hold = o.value("switchMap").toBool(false);
        } else if (type == "KMT_STEER_WHEEL") {
            n.kind = Node::Wheel;
            n.pos = readPos(o.value("centerPos"));
            n.lo = o.value("leftOffset").toDouble(0.07);
            n.ro = o.value("rightOffset").toDouble(0.07);
            n.uo = o.value("upOffset").toDouble(0.12);
            n.dn = o.value("downOffset").toDouble(0.12);
            n.up = o.value("upKey").toString("Key_W");
            n.down = o.value("downKey").toString("Key_S");
            n.left = o.value("leftKey").toString("Key_A");
            n.right = o.value("rightKey").toString("Key_D");
        } else if (type == "KMT_CLICK_MULTI") {
            n.kind = Node::Macro;
            n.key = o.value("key").toString();
            for (const QJsonValue &sv : o.value("clickNodes").toArray()) {
                const QJsonObject so = sv.toObject();
                n.steps.append({ int(so.value("delay").toDouble(0)), readPos(so.value("pos")) });
            }
            if (n.steps.isEmpty()) {
                continue;
            }
            n.pos = n.steps.first().second;
        } else {
            // Kinds the editor does not draw yet are kept out of the editor.
            continue;
        }
        m_nodes.append(n);
    }
    m_path = path;
    update();
    return true;
}

void KeymapEditor::save()
{
    if (m_capture >= 0) {
        setStatus(tr("Press a key first, or Esc to cancel"));
        return;
    }
    const QString json = toJson();
    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("K2K Mirror"), tr("Could not save %1").arg(m_path));
        return;
    }
    f.write(json.toUtf8());
    f.close();
    emit applied(json);
    setStatus(tr("Applied. Press ~ to switch key map mode on and off"));
}

void KeymapEditor::open()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open key map"), keymapDir(), "JSON (*.json)");
    if (path.isEmpty()) {
        return;
    }
    if (!loadFile(path)) {
        QMessageBox::warning(this, QStringLiteral("K2K Mirror"), tr("Could not read %1").arg(path));
        return;
    }
    setStatus(tr("Opened %1").arg(QFileInfo(path).fileName()));
}
