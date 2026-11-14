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

int KeymapEditor::hitTest(const QPoint &p) const
{
    for (int i = m_nodes.size() - 1; i >= 0; --i) {
        const int reach = m_nodes[i].kind == Node::Wheel ? kRadius * 2 : kRadius;
        if ((toPx(m_nodes[i].pos) - p).manhattanLength() <= reach) {
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
    p.fillRect(rect(), QColor(0, 0, 0, 70));
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
            const int r = int(n.offset * width());
            p.setBrush(QColor(255, 255, 255, 40));
            p.drawEllipse(c, r, r);
            const QString labels[4] = { keyLabel(n.up), keyLabel(n.down), keyLabel(n.left), keyLabel(n.right) };
            const QPoint at[4] = { c + QPoint(0, -r), c + QPoint(0, r), c + QPoint(-r, 0), c + QPoint(r, 0) };
            for (int k = 0; k < 4; ++k) {
                p.setBrush(waiting && m_captureField == k ? QColor(0xFF, 0xB4, 0x00, 220) : fill);
                p.drawEllipse(at[k], 13, 13);
                p.drawText(QRect(at[k] - QPoint(13, 13), QSize(26, 26)), Qt::AlignCenter, labels[k]);
            }
            p.setBrush(fill);
            p.drawEllipse(c, 6, 6);
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
    if (m_capture >= 0 && event->button() != Qt::LeftButton) {
        // Mouse buttons other than the left one can be mapped too.
        finishCapture(buttonName(event->button()));
        return;
    }
    const int hit = hitTest(event->pos());
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
        m_nodes[m_drag].pos = toRel(event->pos() + m_dragOffset);
        update();
    }
}

void KeymapEditor::mouseReleaseEvent(QMouseEvent *)
{
    m_drag = -1;
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
        n.offset = qMin(0.3, n.offset + 0.02);
    } else if (chosen == smaller) {
        n.offset = qMax(0.04, n.offset - 0.02);
    } else if (chosen == faster) {
        m_lookSpeedX *= 0.8;
        m_lookSpeedY *= 0.8;
    } else if (chosen == slower) {
        m_lookSpeedX *= 1.25;
        m_lookSpeedY *= 1.25;
    }
    update();
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
    n.pos = QPointF(0.18, 0.72);
    m_nodes.append(n);
    setStatus(tr("Drag the stick onto the game's joystick"));
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
            o.insert("leftOffset", n.offset);
            o.insert("rightOffset", n.offset);
            o.insert("upOffset", n.offset * 1.6);
            o.insert("downOffset", n.offset * 1.6);
            o.insert("leftKey", n.left);
            o.insert("rightKey", n.right);
            o.insert("upKey", n.up);
            o.insert("downKey", n.down);
            break;
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
            n.offset = o.value("leftOffset").toDouble(0.1);
            n.up = o.value("upKey").toString("Key_W");
            n.down = o.value("downKey").toString("Key_S");
            n.left = o.value("leftKey").toString("Key_A");
            n.right = o.value("rightKey").toString("Key_D");
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
