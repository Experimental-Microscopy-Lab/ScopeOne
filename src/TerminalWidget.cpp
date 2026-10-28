#include "TerminalWidget.h"
#include "PtyProcess.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QWheelEvent>
#include <cmath>
#include <utility>

namespace scopeone::ui
{
    namespace
    {
        constexpr int kMaxScrollback = 5000;
        const QColor kDefaultForeground(0xde, 0xe2, 0xe6);
        const QColor kDefaultBackground(0x1e, 0x1e, 0x1e);
        const QColor kSelectionBackground(0x36, 0x4f, 0xc7);

        struct WideRange
        {
            char32_t first;
            char32_t last;
        };

        // East Asian wide and emoji ranges, close to what wcwidth and Node's string-width report
        constexpr WideRange kWideRanges[] = {
            {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
            {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
            {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
            {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
            {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
            {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
            {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
            {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
            {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
            {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
            {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
            {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1B000, 0x1B2FF},
            {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
            {0x1F200, 0x1F251}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB},
            {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
        };

        int charWidth(char32_t ch)
        {
            if (ch < 0x300)
            {
                return 1;
            }
            const QChar::Category category = QChar::category(ch);
            if (category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing
                || category == QChar::Other_Format)
            {
                return 0;
            }
            for (const WideRange& range : kWideRanges)
            {
                if (ch >= range.first && ch <= range.last)
                {
                    return 2;
                }
            }
            return 1;
        }

        // xterm 256 color palette with the 16 base colors taken from VS Code's dark theme
        QColor paletteColor(int index)
        {
            static const QRgb basic[16] = {
                0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
                0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff
            };
            if (index < 16)
            {
                return QColor(basic[index]);
            }
            if (index < 232)
            {
                static const int levels[6] = {0, 95, 135, 175, 215, 255};
                index -= 16;
                return QColor(levels[index / 36], levels[(index / 6) % 6], levels[index % 6]);
            }
            const int gray = 8 + (index - 232) * 10;
            return QColor(gray, gray, gray);
        }
    }

    // Set up the cell font; the shell starts the first time the widget is shown
    TerminalWidget::TerminalWidget(QWidget* parent)
        : QWidget(parent)
    {
#ifdef Q_OS_WIN
        m_font = QFont(QStringLiteral("Consolas"), 10);
#else
        m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        m_font.setPointSize(10);
#endif
        m_boldFont = m_font;
        m_boldFont.setBold(true);
        const QFontMetricsF metrics(m_font);
        m_cellWidth = metrics.horizontalAdvance(QLatin1Char('M'));
        m_cellHeight = std::ceil(metrics.height());
        m_ascent = metrics.ascent();

        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_InputMethodEnabled);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setCursor(Qt::IBeamCursor);
        setMinimumSize(qRound(m_cellWidth * 20), qRound(m_cellHeight * 4));
        resetScreen();
    }

    void TerminalWidget::startShell()
    {
        resetScreen();
        m_scrollback.clear();
        m_scrollOffset = 0;
        m_exited = false;
        m_decoder.resetState();
        m_state = ParseState::Ground;

        m_pty = new PtyProcess(this);
        connect(m_pty, &PtyProcess::readyRead, this, &TerminalWidget::receive);
        connect(m_pty, &PtyProcess::finished, this, &TerminalWidget::onShellFinished);
        m_pty->start(m_columns, m_rows);
        update();
    }

    void TerminalWidget::onShellFinished(int exitCode)
    {
        m_pty->deleteLater();
        m_pty = nullptr;
        m_exited = true;
        receive(tr("\r\n[Process exited with code %1, press Enter to restart]\r\n")
                    .arg(exitCode).toUtf8());
    }

    void TerminalWidget::sendToShell(const QByteArray& data)
    {
        if (m_pty)
        {
            m_pty->write(data);
        }
    }

    void TerminalWidget::receive(const QByteArray& data)
    {
        const QString text = m_decoder.decode(data);
        for (const uint code : text.toUcs4())
        {
            process(static_cast<char32_t>(code));
        }
        update();
    }

    // One step of the VT state machine
    void TerminalWidget::process(char32_t ch)
    {
        switch (m_state)
        {
        case ParseState::Ground:
            if (ch == 0x1b)
            {
                m_state = ParseState::Escape;
            }
            else if (ch < 0x20 || ch == 0x7f)
            {
                control(ch);
            }
            else
            {
                print(ch);
            }
            break;
        case ParseState::Escape:
            escape(ch);
            break;
        case ParseState::Charset:
            m_state = ParseState::Ground;
            break;
        case ParseState::Csi:
            if (ch >= 0x40 && ch <= 0x7e)
            {
                m_state = ParseState::Ground;
                csi(ch);
            }
            else if (ch < 0x20)
            {
                control(ch);
            }
            else
            {
                m_sequence += QChar(static_cast<char16_t>(ch));
            }
            break;
        case ParseState::String:
            // OSC, DCS, APC and PM strings (titles, hyperlinks) are swallowed
            if (ch == 0x07)
            {
                m_state = ParseState::Ground;
            }
            else if (ch == 0x1b)
            {
                m_state = ParseState::StringEscape;
            }
            break;
        case ParseState::StringEscape:
            m_state = ParseState::Ground;
            if (ch != U'\\')
            {
                escape(ch);
            }
            break;
        }
    }

    void TerminalWidget::control(char32_t ch)
    {
        switch (ch)
        {
        case 0x08:
            m_cursorX = qMax(0, m_cursorX - 1);
            m_wrapPending = false;
            break;
        case 0x09:
            m_cursorX = qMin(m_columns - 1, (m_cursorX / 8 + 1) * 8);
            break;
        case 0x0a:
        case 0x0b:
        case 0x0c:
            m_wrapPending = false;
            lineFeed();
            break;
        case 0x0d:
            m_cursorX = 0;
            m_wrapPending = false;
            break;
        default:
            break;
        }
    }

    void TerminalWidget::escape(char32_t ch)
    {
        m_state = ParseState::Ground;
        switch (ch)
        {
        case U'[':
            m_state = ParseState::Csi;
            m_sequence.clear();
            break;
        case U']':
        case U'P':
        case U'_':
        case U'^':
        case U'X':
            m_state = ParseState::String;
            break;
        case U'(':
        case U')':
        case U'*':
        case U'+':
        case U'#':
        case U'%':
            m_state = ParseState::Charset;
            break;
        case U'7':
            saveCursor();
            break;
        case U'8':
            restoreCursor();
            break;
        case U'D':
            lineFeed();
            break;
        case U'E':
            m_cursorX = 0;
            lineFeed();
            break;
        case U'M':
            reverseLineFeed();
            break;
        case U'c':
            resetScreen();
            break;
        default:
            break;
        }
    }

    void TerminalWidget::csi(char32_t finalChar)
    {
        QString body = m_sequence;
        QChar prefix;
        if (!body.isEmpty() && QStringLiteral("?<=>").contains(body.front()))
        {
            prefix = body.front();
            body.remove(0, 1);
        }
        // Sequences with intermediate bytes (cursor shape, soft reset) only change looks
        if (!body.isEmpty() && body.back().unicode() >= 0x20 && body.back().unicode() <= 0x2f)
        {
            return;
        }

        QList<QVector<int>> params;
        if (!body.isEmpty())
        {
            for (const QString& group : body.split(QLatin1Char(';')))
            {
                QVector<int> values;
                for (const QString& value : group.split(QLatin1Char(':')))
                {
                    values.append(value.isEmpty() ? -1 : value.toInt());
                }
                params.append(values);
            }
        }
        const auto arg = [&params](int index, int fallback)
        {
            return index < params.size() && params[index][0] > 0 ? params[index][0] : fallback;
        };

        if (finalChar != U'm')
        {
            m_wrapPending = false;
        }
        if (prefix == QLatin1Char('?'))
        {
            if (finalChar == U'h' || finalChar == U'l')
            {
                for (const QVector<int>& param : params)
                {
                    setPrivateMode(param[0], finalChar == U'h');
                }
            }
            return;
        }
        if (!prefix.isNull())
        {
            if (prefix == QLatin1Char('>') && finalChar == U'c')
            {
                sendToShell("\x1b[>0;10;1c");
            }
            return;
        }

        const int count = arg(0, 1);
        switch (finalChar)
        {
        case U'A':
            m_cursorY -= count;
            break;
        case U'B':
        case U'e':
            m_cursorY += count;
            break;
        case U'C':
        case U'a':
            m_cursorX += count;
            break;
        case U'D':
            m_cursorX -= count;
            break;
        case U'E':
            m_cursorY += count;
            m_cursorX = 0;
            break;
        case U'F':
            m_cursorY -= count;
            m_cursorX = 0;
            break;
        case U'G':
        case U'`':
            m_cursorX = count - 1;
            break;
        case U'd':
            m_cursorY = count - 1;
            break;
        case U'H':
        case U'f':
            m_cursorY = arg(0, 1) - 1;
            m_cursorX = arg(1, 1) - 1;
            break;
        case U'J':
            {
                const int mode = arg(0, 0);
                if (mode == 0)
                {
                    eraseCells(m_cursorY, m_cursorX, m_columns);
                    for (int row = m_cursorY + 1; row < m_rows; ++row)
                    {
                        eraseCells(row, 0, m_columns);
                    }
                }
                else if (mode == 1)
                {
                    for (int row = 0; row < m_cursorY; ++row)
                    {
                        eraseCells(row, 0, m_columns);
                    }
                    eraseCells(m_cursorY, 0, m_cursorX + 1);
                }
                else if (mode == 2)
                {
                    for (int row = 0; row < m_rows; ++row)
                    {
                        eraseCells(row, 0, m_columns);
                    }
                }
                else if (mode == 3)
                {
                    m_scrollback.clear();
                    m_scrollOffset = 0;
                    m_hasSelection = false;
                }
                break;
            }
        case U'K':
            {
                const int mode = arg(0, 0);
                if (mode == 0)
                {
                    eraseCells(m_cursorY, m_cursorX, m_columns);
                }
                else if (mode == 1)
                {
                    eraseCells(m_cursorY, 0, m_cursorX + 1);
                }
                else
                {
                    eraseCells(m_cursorY, 0, m_columns);
                }
                break;
            }
        case U'L':
        case U'M':
            if (m_cursorY >= m_scrollTop && m_cursorY <= m_scrollBottom)
            {
                const int lines = qMin(count, m_scrollBottom - m_cursorY + 1);
                for (int i = 0; i < lines; ++i)
                {
                    if (finalChar == U'L')
                    {
                        m_screen.removeAt(m_scrollBottom);
                        m_screen.insert(m_cursorY, blankLine());
                    }
                    else
                    {
                        m_screen.removeAt(m_cursorY);
                        m_screen.insert(m_scrollBottom, blankLine());
                    }
                }
                m_cursorX = 0;
            }
            break;
        case U'@':
            {
                Line& line = m_screen[m_cursorY];
                line.insert(m_cursorX, qMin(count, m_columns - m_cursorX), blankCell());
                line.resize(m_columns);
                break;
            }
        case U'P':
            {
                Line& line = m_screen[m_cursorY];
                const int cells = qMin(count, m_columns - m_cursorX);
                line.remove(m_cursorX, cells);
                line.insert(line.size(), cells, blankCell());
                break;
            }
        case U'X':
            eraseCells(m_cursorY, m_cursorX, qMin(m_columns, m_cursorX + count));
            break;
        case U'S':
            scrollUp(count);
            break;
        case U'T':
            scrollDown(count);
            break;
        case U'm':
            selectGraphicRendition(params);
            break;
        case U'r':
            {
                const int top = arg(0, 1) - 1;
                const int bottom = arg(1, m_rows) - 1;
                if (top < bottom && bottom < m_rows)
                {
                    m_scrollTop = top;
                    m_scrollBottom = bottom;
                    m_cursorX = 0;
                    m_cursorY = 0;
                }
                break;
            }
        case U's':
            saveCursor();
            break;
        case U'u':
            restoreCursor();
            break;
        case U'n':
            if (arg(0, 0) == 6)
            {
                sendToShell(QStringLiteral("\x1b[%1;%2R")
                                .arg(m_cursorY + 1).arg(m_cursorX + 1).toLatin1());
            }
            else if (arg(0, 0) == 5)
            {
                sendToShell("\x1b[0n");
            }
            break;
        case U'c':
            sendToShell("\x1b[?1;2c");
            break;
        default:
            break;
        }
        clampCursor();
    }

    void TerminalWidget::selectGraphicRendition(const QList<QVector<int>>& params)
    {
        if (params.isEmpty())
        {
            m_pen = Cell();
            return;
        }
        const auto value = [&params](int index)
        {
            return index < params.size() ? qMax(0, params[index][0]) : 0;
        };
        const auto rgb = [](int red, int green, int blue)
        {
            return 0xFF000000u | (static_cast<quint32>(qBound(0, red, 255)) << 16)
                | (static_cast<quint32>(qBound(0, green, 255)) << 8)
                | static_cast<quint32>(qBound(0, blue, 255));
        };

        for (int i = 0; i < params.size(); ++i)
        {
            const QVector<int>& group = params[i];
            const int code = qMax(0, group[0]);
            if (code == 38 || code == 48)
            {
                quint32 color = kDefaultColor;
                if (group.size() > 1)
                {
                    // Colon form, 38:5:n or 38:2:r:g:b with an optional color space slot
                    if (group[1] == 5 && group.size() > 2)
                    {
                        color = static_cast<quint32>(qBound(0, group[2], 255));
                    }
                    else if (group[1] == 2 && group.size() >= 5)
                    {
                        const int offset = group.size() >= 6 ? 3 : 2;
                        color = rgb(group[offset], group[offset + 1], group[offset + 2]);
                    }
                }
                else if (value(i + 1) == 5)
                {
                    color = static_cast<quint32>(qBound(0, value(i + 2), 255));
                    i += 2;
                }
                else if (value(i + 1) == 2)
                {
                    color = rgb(value(i + 2), value(i + 3), value(i + 4));
                    i += 4;
                }
                (code == 38 ? m_pen.fg : m_pen.bg) = color;
                continue;
            }

            switch (code)
            {
            case 0:
                m_pen = Cell();
                break;
            case 1:
                m_pen.flags |= Bold;
                break;
            case 2:
                m_pen.flags |= Dim;
                break;
            case 4:
                if (group.size() > 1 && group[1] == 0)
                {
                    m_pen.flags &= ~Underline;
                }
                else
                {
                    m_pen.flags |= Underline;
                }
                break;
            case 7:
                m_pen.flags |= Inverse;
                break;
            case 22:
                m_pen.flags &= ~(Bold | Dim);
                break;
            case 24:
                m_pen.flags &= ~Underline;
                break;
            case 27:
                m_pen.flags &= ~Inverse;
                break;
            case 39:
                m_pen.fg = kDefaultColor;
                break;
            case 49:
                m_pen.bg = kDefaultColor;
                break;
            default:
                if (code >= 30 && code <= 37)
                {
                    m_pen.fg = static_cast<quint32>(code - 30);
                }
                else if (code >= 40 && code <= 47)
                {
                    m_pen.bg = static_cast<quint32>(code - 40);
                }
                else if (code >= 90 && code <= 97)
                {
                    m_pen.fg = static_cast<quint32>(code - 90 + 8);
                }
                else if (code >= 100 && code <= 107)
                {
                    m_pen.bg = static_cast<quint32>(code - 100 + 8);
                }
                break;
            }
        }
    }

    void TerminalWidget::setPrivateMode(int mode, bool enabled)
    {
        switch (mode)
        {
        case 1:
            m_applicationCursorKeys = enabled;
            break;
        case 7:
            m_autoWrap = enabled;
            break;
        case 25:
            m_cursorVisible = enabled;
            break;
        case 2004:
            m_bracketedPaste = enabled;
            break;
        case 47:
        case 1047:
        case 1049:
            if (enabled == m_alternateScreen)
            {
                break;
            }
            if (mode == 1049 && enabled)
            {
                saveCursor();
            }
            if (enabled)
            {
                m_savedMainScreen = m_screen;
                m_screen.clear();
                for (int row = 0; row < m_rows; ++row)
                {
                    m_screen.append(blankLine());
                }
            }
            else
            {
                m_screen = m_savedMainScreen;
                m_savedMainScreen.clear();
            }
            m_alternateScreen = enabled;
            m_scrollOffset = 0;
            m_hasSelection = false;
            if (mode == 1049 && !enabled)
            {
                restoreCursor();
            }
            break;
        default:
            break;
        }
    }

    void TerminalWidget::print(char32_t ch)
    {
        const int width = charWidth(ch);
        if (width == 0)
        {
            return;
        }
        if (m_wrapPending)
        {
            m_wrapPending = false;
            if (m_autoWrap)
            {
                m_cursorX = 0;
                lineFeed();
            }
        }
        if (width == 2 && m_cursorX == m_columns - 1)
        {
            eraseCells(m_cursorY, m_cursorX, m_columns);
            if (!m_autoWrap)
            {
                return;
            }
            m_cursorX = 0;
            lineFeed();
        }

        Line& line = m_screen[m_cursorY];
        // Overwriting half of a wide character blanks its other half
        if ((line[m_cursorX].flags & WideTail) && m_cursorX > 0)
        {
            line[m_cursorX - 1] = blankCell();
        }
        if (m_cursorX + width < m_columns && (line[m_cursorX + width].flags & WideTail))
        {
            line[m_cursorX + width] = blankCell();
        }
        Cell cell = m_pen;
        cell.ch = ch;
        line[m_cursorX] = cell;
        if (width == 2)
        {
            cell.ch = U' ';
            cell.flags |= WideTail;
            line[m_cursorX + 1] = cell;
        }
        m_cursorX += width;
        if (m_cursorX >= m_columns)
        {
            m_cursorX = m_columns - 1;
            m_wrapPending = true;
        }
    }

    void TerminalWidget::lineFeed()
    {
        if (m_cursorY == m_scrollBottom)
        {
            scrollUp(1);
        }
        else if (m_cursorY < m_rows - 1)
        {
            ++m_cursorY;
        }
    }

    void TerminalWidget::reverseLineFeed()
    {
        if (m_cursorY == m_scrollTop)
        {
            scrollDown(1);
        }
        else if (m_cursorY > 0)
        {
            --m_cursorY;
        }
    }

    // Scroll the region up; lines leaving the top of the main screen go to history
    void TerminalWidget::scrollUp(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            Line line = m_screen.takeAt(m_scrollTop);
            if (m_scrollTop == 0 && !m_alternateScreen)
            {
                m_scrollback.append(std::move(line));
                if (m_scrollback.size() > kMaxScrollback)
                {
                    m_scrollback.removeFirst();
                }
                if (m_scrollOffset > 0)
                {
                    m_scrollOffset = qMin(m_scrollOffset + 1, static_cast<int>(m_scrollback.size()));
                }
            }
            m_screen.insert(m_scrollBottom, blankLine());
        }
    }

    void TerminalWidget::scrollDown(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            m_screen.removeAt(m_scrollBottom);
            m_screen.insert(m_scrollTop, blankLine());
        }
    }

    void TerminalWidget::eraseCells(int row, int from, int to)
    {
        Line& line = m_screen[row];
        for (int column = qMax(0, from); column < qMin(to, m_columns); ++column)
        {
            line[column] = blankCell();
        }
    }

    void TerminalWidget::saveCursor()
    {
        m_savedCursorX = m_cursorX;
        m_savedCursorY = m_cursorY;
        m_savedPen = m_pen;
    }

    void TerminalWidget::restoreCursor()
    {
        m_cursorX = m_savedCursorX;
        m_cursorY = m_savedCursorY;
        m_pen = m_savedPen;
        m_wrapPending = false;
        clampCursor();
    }

    void TerminalWidget::resetScreen()
    {
        m_pen = Cell();
        m_screen.clear();
        for (int row = 0; row < m_rows; ++row)
        {
            m_screen.append(blankLine());
        }
        m_savedMainScreen.clear();
        m_alternateScreen = false;
        m_cursorX = 0;
        m_cursorY = 0;
        m_wrapPending = false;
        m_scrollTop = 0;
        m_scrollBottom = m_rows - 1;
        m_cursorVisible = true;
        m_autoWrap = true;
        m_applicationCursorKeys = false;
        m_bracketedPaste = false;
        m_hasSelection = false;
    }

    void TerminalWidget::resizeScreen(int columns, int rows)
    {
        for (Line& line : m_screen)
        {
            line.resize(columns);
        }
        for (Line& line : m_savedMainScreen)
        {
            line.resize(columns);
        }
        m_columns = columns;

        // Shrinking drops empty lines below the cursor first, then pushes lines above it into history
        while (m_screen.size() > rows && m_cursorY < m_screen.size() - 1)
        {
            m_screen.removeLast();
        }
        while (m_screen.size() > rows)
        {
            Line line = m_screen.takeFirst();
            if (!m_alternateScreen)
            {
                m_scrollback.append(std::move(line));
            }
            --m_cursorY;
        }
        while (m_screen.size() < rows)
        {
            m_screen.append(blankLine());
        }
        if (!m_savedMainScreen.isEmpty())
        {
            while (m_savedMainScreen.size() > rows)
            {
                m_scrollback.append(m_savedMainScreen.takeFirst());
            }
            while (m_savedMainScreen.size() < rows)
            {
                m_savedMainScreen.append(Line(columns));
            }
        }
        while (m_scrollback.size() > kMaxScrollback)
        {
            m_scrollback.removeFirst();
        }

        m_rows = rows;
        m_scrollTop = 0;
        m_scrollBottom = rows - 1;
        m_wrapPending = false;
        m_scrollOffset = qMin(m_scrollOffset, static_cast<int>(m_scrollback.size()));
        m_hasSelection = false;
        clampCursor();
    }

    void TerminalWidget::clampCursor()
    {
        m_cursorX = qBound(0, m_cursorX, m_columns - 1);
        m_cursorY = qBound(0, m_cursorY, m_rows - 1);
    }

    TerminalWidget::Line TerminalWidget::blankLine() const
    {
        return Line(m_columns, blankCell());
    }

    // Erased cells keep the current background color, as xterm does
    TerminalWidget::Cell TerminalWidget::blankCell() const
    {
        Cell cell;
        cell.bg = m_pen.bg;
        return cell;
    }

    const TerminalWidget::Line& TerminalWidget::lineAt(int absoluteLine) const
    {
        return absoluteLine < m_scrollback.size()
                   ? m_scrollback[absoluteLine]
                   : m_screen[absoluteLine - m_scrollback.size()];
    }

    QPoint TerminalWidget::cellAt(const QPoint& position) const
    {
        const int column = qBound(0, static_cast<int>(position.x() / m_cellWidth), m_columns - 1);
        const int row = qBound(0, static_cast<int>(position.y() / m_cellHeight), m_rows - 1);
        return QPoint(column, static_cast<int>(m_scrollback.size()) - m_scrollOffset + row);
    }

    bool TerminalWidget::isSelected(int absoluteLine, int column) const
    {
        if (!m_hasSelection)
        {
            return false;
        }
        QPoint start = m_selectionAnchor;
        QPoint end = m_selectionEnd;
        if (end.y() < start.y() || (end.y() == start.y() && end.x() < start.x()))
        {
            std::swap(start, end);
        }
        return (absoluteLine > start.y() || (absoluteLine == start.y() && column >= start.x()))
            && (absoluteLine < end.y() || (absoluteLine == end.y() && column <= end.x()));
    }

    QString TerminalWidget::selectedText() const
    {
        QPoint start = m_selectionAnchor;
        QPoint end = m_selectionEnd;
        if (end.y() < start.y() || (end.y() == start.y() && end.x() < start.x()))
        {
            std::swap(start, end);
        }

        QStringList lines;
        for (int absoluteLine = start.y(); absoluteLine <= end.y(); ++absoluteLine)
        {
            const Line& line = lineAt(absoluteLine);
            const int first = absoluteLine == start.y() ? start.x() : 0;
            const int last = absoluteLine == end.y() ? end.x() : m_columns - 1;
            QString text;
            for (int column = first; column <= last && column < line.size(); ++column)
            {
                if (!(line[column].flags & WideTail))
                {
                    text += QString::fromUcs4(&line[column].ch, 1);
                }
            }
            while (text.endsWith(QLatin1Char(' ')))
            {
                text.chop(1);
            }
            lines.append(text);
        }
        return lines.join(QLatin1Char('\n'));
    }

    void TerminalWidget::copySelection()
    {
        if (m_hasSelection)
        {
            QGuiApplication::clipboard()->setText(selectedText());
        }
    }

    void TerminalWidget::paste()
    {
        QString text = QGuiApplication::clipboard()->text();
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
        text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
        QByteArray data = text.toUtf8();
        if (m_bracketedPaste)
        {
            data = "\x1b[200~" + data + "\x1b[201~";
        }
        m_scrollOffset = 0;
        sendToShell(data);
        update();
    }

    // Translate a key press into the bytes xterm would send
    QByteArray TerminalWidget::keyToBytes(const QKeyEvent* event) const
    {
        const Qt::KeyboardModifiers modifiers = event->modifiers();
        const bool shift = modifiers & Qt::ShiftModifier;
        const bool alt = modifiers & Qt::AltModifier;
#ifdef Q_OS_MACOS
        const bool control = modifiers & Qt::MetaModifier;
#else
        const bool control = modifiers & Qt::ControlModifier;
#endif
        const int modifierCode = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (control ? 4 : 0);
        const auto cursorKey = [&](char finalChar) -> QByteArray
        {
            if (modifierCode > 1)
            {
                return "\x1b[1;" + QByteArray::number(modifierCode) + finalChar;
            }
            return QByteArray(m_applicationCursorKeys ? "\x1bO" : "\x1b[") + finalChar;
        };
        const auto functionKey = [&](char finalChar) -> QByteArray
        {
            if (modifierCode > 1)
            {
                return "\x1b[1;" + QByteArray::number(modifierCode) + finalChar;
            }
            return QByteArray("\x1bO") + finalChar;
        };
        const auto tildeKey = [&](int code) -> QByteArray
        {
            if (modifierCode > 1)
            {
                return "\x1b[" + QByteArray::number(code) + ';'
                    + QByteArray::number(modifierCode) + '~';
            }
            return "\x1b[" + QByteArray::number(code) + '~';
        };

        switch (event->key())
        {
        case Qt::Key_Up: return cursorKey('A');
        case Qt::Key_Down: return cursorKey('B');
        case Qt::Key_Right: return cursorKey('C');
        case Qt::Key_Left: return cursorKey('D');
        case Qt::Key_Home: return cursorKey('H');
        case Qt::Key_End: return cursorKey('F');
        case Qt::Key_Insert: return tildeKey(2);
        case Qt::Key_Delete: return tildeKey(3);
        case Qt::Key_PageUp: return tildeKey(5);
        case Qt::Key_PageDown: return tildeKey(6);
        case Qt::Key_F1: return functionKey('P');
        case Qt::Key_F2: return functionKey('Q');
        case Qt::Key_F3: return functionKey('R');
        case Qt::Key_F4: return functionKey('S');
        case Qt::Key_F5: return tildeKey(15);
        case Qt::Key_F6: return tildeKey(17);
        case Qt::Key_F7: return tildeKey(18);
        case Qt::Key_F8: return tildeKey(19);
        case Qt::Key_F9: return tildeKey(20);
        case Qt::Key_F10: return tildeKey(21);
        case Qt::Key_F11: return tildeKey(23);
        case Qt::Key_F12: return tildeKey(24);
        // Shift+Enter becomes Alt+Enter, which claude and readline take as a newline
        case Qt::Key_Return:
        case Qt::Key_Enter: return (shift || alt) ? QByteArray("\x1b\r") : QByteArray("\r");
        case Qt::Key_Backspace:
            return control ? QByteArray("\x08") : alt ? QByteArray("\x1b\x7f") : QByteArray("\x7f");
        case Qt::Key_Tab: return QByteArray("\t");
        case Qt::Key_Backtab: return QByteArray("\x1b[Z");
        case Qt::Key_Escape: return QByteArray("\x1b");
        default:
            break;
        }

        // Ctrl+Alt is left alone because Windows reports AltGr that way
        const int key = event->key();
        if (control && !alt && key >= Qt::Key_A && key <= Qt::Key_Z)
        {
            return QByteArray(1, static_cast<char>(key - Qt::Key_A + 1));
        }
        if (control && !alt && key == Qt::Key_Space)
        {
            return QByteArray(1, '\0');
        }
        const QByteArray text = event->text().toUtf8();
        if (text.isEmpty())
        {
            return {};
        }
        return alt && !control ? "\x1b" + text : text;
    }

    QColor TerminalWidget::resolveColor(quint32 color, bool foreground) const
    {
        if (color == kDefaultColor)
        {
            return foreground ? kDefaultForeground : kDefaultBackground;
        }
        if (color < 256)
        {
            return paletteColor(static_cast<int>(color));
        }
        return QColor(static_cast<QRgb>(color & 0xFFFFFF));
    }

    // Take every key, including Tab and the application shortcuts, while focused
    bool TerminalWidget::event(QEvent* event)
    {
        if (event->type() == QEvent::ShortcutOverride)
        {
            event->accept();
            return true;
        }
        return QWidget::event(event);
    }

    bool TerminalWidget::focusNextPrevChild(bool)
    {
        return false;
    }

    void TerminalWidget::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.fillRect(rect(), kDefaultBackground);

        const auto cellRect = [this](int column, int row, int span)
        {
            return QRect(QPoint(qRound(column * m_cellWidth), qRound(row * m_cellHeight)),
                         QPoint(qRound((column + span) * m_cellWidth) - 1,
                                qRound((row + 1) * m_cellHeight) - 1));
        };

        struct Style
        {
            QColor fg;
            QColor bg;
            quint8 flags;
        };
        QVector<Style> styles(m_columns);
        const int firstLine = static_cast<int>(m_scrollback.size()) - m_scrollOffset;
        const bool showCursor = m_cursorVisible && m_scrollOffset == 0;

        for (int row = 0; row < m_rows; ++row)
        {
            const int absoluteLine = firstLine + row;
            const Line& line = lineAt(absoluteLine);

            // Backgrounds first, with inverse, selection and the cursor block applied
            for (int column = 0; column < m_columns; ++column)
            {
                const Cell cell = column < line.size() ? line[column] : Cell();
                QColor fg = resolveColor(cell.fg, true);
                QColor bg = resolveColor(cell.bg, false);
                if (cell.flags & Inverse)
                {
                    std::swap(fg, bg);
                }
                if (isSelected(absoluteLine, column))
                {
                    fg = Qt::white;
                    bg = kSelectionBackground;
                }
                if (showCursor && hasFocus() && row == m_cursorY && column == m_cursorX)
                {
                    std::swap(fg, bg);
                }
                if (cell.flags & Dim)
                {
                    fg.setAlpha(150);
                }
                styles[column] = {fg, bg, cell.flags};
                if (bg != kDefaultBackground)
                {
                    painter.fillRect(cellRect(column, row, 1), bg);
                }
            }

            // Text in runs of plain ASCII with one style, everything else cell by cell
            const qreal baseline = row * m_cellHeight + m_ascent;
            for (int column = 0; column < m_columns;)
            {
                if (column >= line.size() || line[column].ch == U' ' || (line[column].flags & WideTail))
                {
                    ++column;
                    continue;
                }
                const Cell& cell = line[column];
                const Style& style = styles[column];
                QString text = QString::fromUcs4(&cell.ch, 1);
                int end = column + 1;
                if (cell.ch < 0x80)
                {
                    while (end < m_columns && end < line.size() && line[end].ch < 0x80
                        && styles[end].fg == style.fg
                        && (styles[end].flags & Bold) == (style.flags & Bold))
                    {
                        text += QChar(static_cast<char16_t>(line[end].ch));
                        ++end;
                    }
                }
                else if (end < line.size() && (line[end].flags & WideTail))
                {
                    ++end;
                }
                painter.setFont((style.flags & Bold) ? m_boldFont : m_font);
                painter.setPen(style.fg);
                painter.drawText(QPointF(column * m_cellWidth, baseline), text);
                column = end;
            }

            for (int column = 0; column < m_columns; ++column)
            {
                if (styles[column].flags & Underline)
                {
                    const QRect rect = cellRect(column, row, 1);
                    painter.setPen(styles[column].fg);
                    painter.drawLine(rect.bottomLeft(), rect.bottomRight());
                }
            }
        }

        if (showCursor && !hasFocus())
        {
            painter.setPen(kDefaultForeground);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(cellRect(m_cursorX, m_cursorY, 1).adjusted(0, 0, -1, -1));
        }
    }

    void TerminalWidget::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        const int columns = qMax(2, static_cast<int>(width() / m_cellWidth));
        const int rows = qMax(1, static_cast<int>(height() / m_cellHeight));
        if (columns != m_columns || rows != m_rows)
        {
            resizeScreen(columns, rows);
            if (m_pty)
            {
                m_pty->resize(columns, rows);
            }
        }
    }

    void TerminalWidget::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        if (!m_pty && !m_exited)
        {
            startShell();
        }
    }

    void TerminalWidget::keyPressEvent(QKeyEvent* event)
    {
        const Qt::KeyboardModifiers modifiers = event->modifiers();
        const bool copyPaste = (modifiers & Qt::ControlModifier) && (modifiers & Qt::ShiftModifier);
        if (copyPaste && event->key() == Qt::Key_C)
        {
            copySelection();
            return;
        }
        if ((copyPaste && event->key() == Qt::Key_V)
            || (modifiers == Qt::ShiftModifier && event->key() == Qt::Key_Insert))
        {
            paste();
            return;
        }
        if (modifiers == Qt::ShiftModifier
            && (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown))
        {
            const int step = event->key() == Qt::Key_PageUp ? m_rows - 1 : 1 - m_rows;
            const int maximum = m_alternateScreen ? 0 : static_cast<int>(m_scrollback.size());
            m_scrollOffset = qBound(0, m_scrollOffset + step, maximum);
            update();
            return;
        }
        if (!m_pty)
        {
            if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            {
                startShell();
            }
            return;
        }

        const QByteArray bytes = keyToBytes(event);
        if (bytes.isEmpty())
        {
            return;
        }
        m_scrollOffset = 0;
        m_hasSelection = false;
        sendToShell(bytes);
        update();
    }

    void TerminalWidget::inputMethodEvent(QInputMethodEvent* event)
    {
        if (!event->commitString().isEmpty())
        {
            m_scrollOffset = 0;
            sendToShell(event->commitString().toUtf8());
        }
        event->accept();
    }

    // Place the input method candidate window at the terminal cursor
    QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
    {
        if (query == Qt::ImCursorRectangle)
        {
            return QRect(qRound(m_cursorX * m_cellWidth), qRound(m_cursorY * m_cellHeight),
                         qRound(m_cellWidth), qRound(m_cellHeight));
        }
        if (query == Qt::ImFont)
        {
            return m_font;
        }
        return QWidget::inputMethodQuery(query);
    }

    void TerminalWidget::wheelEvent(QWheelEvent* event)
    {
        if (m_alternateScreen)
        {
            return;
        }
        const int delta = event->angleDelta().y();
        int lines = delta / 40;
        if (lines == 0 && delta != 0)
        {
            lines = delta > 0 ? 1 : -1;
        }
        m_scrollOffset = qBound(0, m_scrollOffset + lines, static_cast<int>(m_scrollback.size()));
        update();
    }

    void TerminalWidget::mousePressEvent(QMouseEvent* event)
    {
        setFocus();
        if (event->button() == Qt::LeftButton)
        {
            m_selectionAnchor = cellAt(event->position().toPoint());
            m_selectionEnd = m_selectionAnchor;
            m_hasSelection = false;
            update();
        }
    }

    void TerminalWidget::mouseMoveEvent(QMouseEvent* event)
    {
        if (event->buttons() & Qt::LeftButton)
        {
            m_selectionEnd = cellAt(event->position().toPoint());
            m_hasSelection = m_selectionEnd != m_selectionAnchor;
            update();
        }
    }

    void TerminalWidget::contextMenuEvent(QContextMenuEvent* event)
    {
        QMenu menu(this);
        QAction* copyAction = menu.addAction(tr("Copy"));
        copyAction->setEnabled(m_hasSelection);
        connect(copyAction, &QAction::triggered, this, &TerminalWidget::copySelection);
        QAction* pasteAction = menu.addAction(tr("Paste"));
        connect(pasteAction, &QAction::triggered, this, &TerminalWidget::paste);
        menu.exec(event->globalPos());
    }
}
