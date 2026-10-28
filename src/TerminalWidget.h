#pragma once

#include <QFont>
#include <QList>
#include <QPoint>
#include <QStringDecoder>
#include <QVector>
#include <QWidget>

namespace scopeone::ui
{
    class PtyProcess;

    // A small xterm compatible terminal: enough for shells, git and claude
    class TerminalWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit TerminalWidget(QWidget* parent = nullptr);

    protected:
        bool event(QEvent* event) override;
        bool focusNextPrevChild(bool next) override;
        void paintEvent(QPaintEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void showEvent(QShowEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void inputMethodEvent(QInputMethodEvent* event) override;
        QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
        void wheelEvent(QWheelEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;

    private:
        static constexpr quint32 kDefaultColor = 0x100;

        enum CellFlag : quint8
        {
            Bold = 1,
            Dim = 2,
            Underline = 4,
            Inverse = 8,
            WideTail = 16
        };

        struct Cell
        {
            char32_t ch{U' '};
            quint32 fg{kDefaultColor};
            quint32 bg{kDefaultColor};
            quint8 flags{0};
        };
        using Line = QVector<Cell>;

        enum class ParseState
        {
            Ground,
            Escape,
            Charset,
            Csi,
            String,
            StringEscape
        };

        void startShell();
        void onShellFinished(int exitCode);
        void sendToShell(const QByteArray& data);
        void receive(const QByteArray& data);
        void process(char32_t ch);
        void control(char32_t ch);
        void escape(char32_t ch);
        void csi(char32_t finalChar);
        void selectGraphicRendition(const QList<QVector<int>>& params);
        void setPrivateMode(int mode, bool enabled);
        void print(char32_t ch);

        void lineFeed();
        void reverseLineFeed();
        void scrollUp(int count);
        void scrollDown(int count);
        void eraseCells(int row, int from, int to);
        void saveCursor();
        void restoreCursor();
        void resetScreen();
        void resizeScreen(int columns, int rows);
        void clampCursor();
        Line blankLine() const;
        Cell blankCell() const;

        const Line& lineAt(int absoluteLine) const;
        QPoint cellAt(const QPoint& position) const;
        bool isSelected(int absoluteLine, int column) const;
        QString selectedText() const;
        void copySelection();
        void paste();
        QByteArray keyToBytes(const QKeyEvent* event) const;
        QColor resolveColor(quint32 color, bool foreground) const;

        PtyProcess* m_pty{nullptr};
        bool m_exited{false};
        QStringDecoder m_decoder{QStringDecoder::Utf8};

        QFont m_font;
        QFont m_boldFont;
        qreal m_cellWidth{8};
        qreal m_cellHeight{16};
        qreal m_ascent{12};

        int m_columns{80};
        int m_rows{24};
        QList<Line> m_screen;
        QList<Line> m_savedMainScreen;
        QList<Line> m_scrollback;
        bool m_alternateScreen{false};
        int m_scrollOffset{0};

        int m_cursorX{0};
        int m_cursorY{0};
        bool m_wrapPending{false};
        Cell m_pen;
        int m_savedCursorX{0};
        int m_savedCursorY{0};
        Cell m_savedPen;
        int m_scrollTop{0};
        int m_scrollBottom{23};

        bool m_cursorVisible{true};
        bool m_autoWrap{true};
        bool m_applicationCursorKeys{false};
        bool m_bracketedPaste{false};

        ParseState m_state{ParseState::Ground};
        QString m_sequence;

        // Selection ends as (column, absolute line), absolute lines count scrollback first
        QPoint m_selectionAnchor;
        QPoint m_selectionEnd;
        bool m_hasSelection{false};
    };
}
