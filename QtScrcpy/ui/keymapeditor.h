#ifndef KEYMAPEDITOR_H
#define KEYMAPEDITOR_H

#include <QJsonObject>
#include <QPointF>
#include <QWidget>

class QLabel;
class QPushButton;

// K2K Mirror: edit a game key map directly on top of the mirrored screen.
// Click an empty spot and press a key to add a tap button, drag buttons to
// move them, right-click for more. Saves the standard QtScrcpy key map JSON.
class KeymapEditor : public QWidget
{
    Q_OBJECT
public:
    struct Node
    {
        enum Kind { Click, ClickTwice, Wheel, Look };
        Kind kind = Click;
        QString key;            // Qt key or mouse button name, e.g. "Key_W", "RightButton"
        QPointF pos;            // relative 0..1
        bool hold = false;      // Click: keep the map active after the tap (switchMap)
        double offset = 0.1;    // Wheel: stick reach
        QString up = "Key_W", down = "Key_S", left = "Key_A", right = "Key_D";
    };

    explicit KeymapEditor(QWidget *videoWidget, const QString &serial);

    bool loadFile(const QString &path);
    QString toJson() const;
    QString filePath() const { return m_path; }

signals:
    void applied(const QString &json);
    void closed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QPointF toRel(const QPoint &p) const;
    QPoint toPx(const QPointF &r) const;
    int hitTest(const QPoint &p) const;
    void startCapture(int index, const QString &prompt);
    void finishCapture(const QString &keyName);
    void showNodeMenu(int index, const QPoint &globalPos);
    void addWheel();
    void addLook();
    void save();
    void open();
    void setStatus(const QString &text);
    static QString keyLabel(const QString &keyName);

    QString m_serial;
    QString m_path;
    QList<Node> m_nodes;
    QString m_switchKey = "Key_QuoteLeft";
    double m_lookSpeedX = 2.0, m_lookSpeedY = 1.0;

    int m_drag = -1;
    QPoint m_dragOffset;
    int m_capture = -1;         // node waiting for a key
    int m_captureField = 0;     // Wheel: 0 up, 1 down, 2 left, 3 right; else 0
    bool m_captureIsNew = false;

    QWidget *m_bar = nullptr;
    QLabel *m_status = nullptr;
};

#endif // KEYMAPEDITOR_H
