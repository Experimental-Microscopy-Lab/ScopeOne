#include "ImageGalleryWidget.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QShortcut>
#include <QSizePolicy>
#include <QVariant>
#include <QVBoxLayout>
#include <algorithm>
#include <cstring>
#include <limits>

namespace scopeone::ui
{
    namespace
    {
        using RecordingSessionData = scopeone::core::ScopeOneCore::RecordingSessionData;

        constexpr int kSessionIdRole = Qt::UserRole + 1;

        // Return the generated snapshot suffix index
        qsizetype snapshotSuffixIndex(const QString& baseName)
        {
            return baseName.lastIndexOf(QStringLiteral("_capture_"));
        }

        // Return true for captures created by Snap to Gallery
        bool isSnapshotSession(const RecordingSessionData& session)
        {
            return snapshotSuffixIndex(session.capturePlan().baseName) > 0;
        }

        // Return the user visible source name without generated capture suffix
        QString displaySourceName(const RecordingSessionData& session)
        {
            const QString baseName = session.capturePlan().baseName.trimmed();
            const qsizetype suffixIndex = snapshotSuffixIndex(baseName);
            if (suffixIndex > 0)
            {
                return baseName.left(suffixIndex).trimmed();
            }
            return baseName;
        }

        // Return true when a session should appear in the gallery
        bool isGallerySession(
            const std::shared_ptr<RecordingSessionData>& session)
        {
            return session && (session->hasRecordedOutput() || session->isSaved());
        }

        // Return true when a session can be passed to the core frame graph
        bool canPreviewSession(
            const std::shared_ptr<RecordingSessionData>& session)
        {
            return session && session->hasRecordedOutput();
        }

        // Count cameras that have at least one stored frame
        int sessionCameraCount(const RecordingSessionData& session)
        {
            int count = 0;
            for (const QString& cameraId : session.recordedCameraIds())
            {
                if (session.recordedFrameCount(cameraId) > 0)
                {
                    ++count;
                }
            }
            if (count > 0)
            {
                return count;
            }
            const int outputCount = session.outputFiles().size();
            if (outputCount > 0)
            {
                return outputCount;
            }
            return session.capturePlan().cameraIds.size();
        }

        // Count buffered frames or streamed frames written to disk
        qint64 sessionFrameCount(const RecordingSessionData& session)
        {
            return session.recordedFrameCount();
        }

        // Build a square grayscale thumbnail from a stored camera frame
        QIcon frameThumbnail(const scopeone::core::ImageFrame& frame)
        {
            if (!frame.isValid())
            {
                return {};
            }

            QImage image(frame.width, frame.height, QImage::Format_Grayscale8);
            if (frame.isMono8())
            {
                for (int y = 0; y < frame.height; ++y)
                {
                    const char* source = frame.bytes.constData()
                                         + static_cast<qint64>(y) * frame.stride;
                    std::memcpy(image.scanLine(y), source, static_cast<size_t>(frame.width));
                }
            }
            else
            {
                quint16 minimum = (std::numeric_limits<quint16>::max)();
                quint16 maximum = 0;
                for (int y = 0; y < frame.height; ++y)
                {
                    const auto* source = reinterpret_cast<const quint16*>(
                        frame.bytes.constData() + static_cast<qint64>(y) * frame.stride);
                    for (int x = 0; x < frame.width; ++x)
                    {
                        minimum = (std::min)(minimum, source[x]);
                        maximum = (std::max)(maximum, source[x]);
                    }
                }

                const int range = static_cast<int>(maximum) - static_cast<int>(minimum);
                for (int y = 0; y < frame.height; ++y)
                {
                    const auto* source = reinterpret_cast<const quint16*>(
                        frame.bytes.constData() + static_cast<qint64>(y) * frame.stride);
                    uchar* target = image.scanLine(y);
                    for (int x = 0; x < frame.width; ++x)
                    {
                        target[x] = range > 0
                                        ? static_cast<uchar>(
                                              (static_cast<int>(source[x]) - minimum) * 255 / range)
                                        : static_cast<uchar>(source[x] > 0 ? 255 : 0);
                    }
                }
            }

            const QImage scaled = image.scaled(QSize(48, 48), Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation);
            QPixmap pixmap(48, 48);
            pixmap.fill(QColor(QStringLiteral("#1c2229")));
            QPainter painter(&pixmap);
            painter.drawImage((48 - scaled.width()) / 2, (48 - scaled.height()) / 2, scaled);
            return QIcon(pixmap);
        }
    }

    // Create the image gallery panel
    ImageGalleryWidget::ImageGalleryWidget(scopeone::core::ScopeOneCore* core,
                                           QWidget* parent)
        : QWidget(parent),
          m_core(core)
    {
        if (!core)
        {
            qFatal("ImageGalleryWidget requires ScopeOneCore");
        }
        setupUI();
        updateButtons();
        updateEmptyState();
        connect(m_core, &scopeone::core::ScopeOneCore::recordingSessionClosed,
                this, [this](const QString& sessionId)
                {
                    for (int row = m_sessionList->count() - 1; row >= 0; --row)
                    {
                        if (m_sessionList->item(row)->data(kSessionIdRole).toString() == sessionId)
                        {
                            delete m_sessionList->takeItem(row);
                        }
                    }
                    updateButtons();
                    updateEmptyState();
                });
        connect(m_core, &scopeone::core::ScopeOneCore::recordingSessionFrameReady,
                this,
                [this](quint64,
                       const std::shared_ptr<RecordingSessionData>& session,
                       const QString&,
                       int,
                       const scopeone::core::ImageFrame& frame)
                {
                    if (session && frame.isValid())
                    {
                        updateSessionThumbnail(session->capturePlan().experimentId, frame);
                    }
                });
    }

    // Add one acquired image session to the gallery
    void ImageGalleryWidget::addSession(
        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
        const QString& title)
    {
        if (!isGallerySession(session))
        {
            return;
        }

        const QString id = session->capturePlan().experimentId.trimmed();
        if (id.isEmpty() || m_core->recordingSession(id) != session)
        {
            return;
        }
        for (int row = 0; row < m_sessionList->count(); ++row)
        {
            if (m_sessionList->item(row)->data(kSessionIdRole).toString() == id)
            {
                m_sessionList->setCurrentRow(row);
                return;
            }
        }

        auto* item = new QListWidgetItem(m_sessionList);
        item->setText(displayTitle(*session, title) + QLatin1Char('\n') + itemSubtitle(*session));
        item->setSizeHint(QSize(0, 64));
        item->setData(kSessionIdRole, id);
        if (!session->isSaved())
        {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
        }
        const QStringList cameraIds = session->recordedCameraIds();
        for (const QString& cameraId : cameraIds)
        {
            if (session->recordedFrameCount(cameraId) > 0)
            {
                m_core->requestRecordingSessionFrame(session, cameraId, 0);
                break;
            }
        }
        m_sessionList->setCurrentItem(item);

        updateButtons();
        updateEmptyState();
    }

    // Mark a gallery session as saved after the writer completes
    void ImageGalleryWidget::markSessionSaved(
        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
    {
        if (!session)
        {
            return;
        }
        const QString sessionId = session->capturePlan().experimentId.trimmed();
        for (int row = 0; row < m_sessionList->count(); ++row)
        {
            QListWidgetItem* item = m_sessionList->item(row);
            const QString id = item->data(kSessionIdRole).toString();
            if (id != sessionId)
            {
                continue;
            }
            const QString firstLine = item->text().section(QLatin1Char('\n'), 0, 0);
            item->setText(firstLine + QLatin1Char('\n') + itemSubtitle(*session));
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            item->setData(Qt::CheckStateRole, QVariant());
            return;
        }
    }

    // Return sessions that still need an explicit save
    QList<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>> ImageGalleryWidget::unsavedSessions()
        const
    {
        QList<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>> sessions;
        for (int row = 0; row < m_sessionList->count(); ++row)
        {
            const auto session = m_core->recordingSession(
                m_sessionList->item(row)->data(kSessionIdRole).toString());
            if (session && !session->isSaved())
            {
                sessions.append(session);
            }
        }
        return sessions;
    }

    // Build the gallery list and action buttons
    void ImageGalleryWidget::setupUI()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        m_emptyLabel = new QLabel(QStringLiteral("No captured images"), this);
        m_emptyLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_emptyLabel);

        m_sessionList = new QListWidget(this);
        m_sessionList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_sessionList->setIconSize(QSize(48, 48));
        m_sessionList->setSpacing(4);
        m_sessionList->setUniformItemSizes(true);
        m_sessionList->setTextElideMode(Qt::ElideRight);
        m_sessionList->setContextMenuPolicy(Qt::CustomContextMenu);
        m_sessionList->setStyleSheet(QStringLiteral(
            "QListWidget { border: 1px solid #3a424b; border-radius: 4px; padding: 2px; }"
            "QListWidget::item { padding: 6px; border-radius: 4px; }"
            "QListWidget::item:selected { background: #31485d; }"));
        layout->addWidget(m_sessionList, 1);

        auto* buttonLayout = new QHBoxLayout();
        buttonLayout->setSpacing(6);
        m_deleteButton = new QPushButton(QStringLiteral("Delete"), this);
        m_saveCheckedButton = new QPushButton(QStringLiteral("Save Checked"), this);
        m_saveCheckedButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_deleteButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        m_deleteButton->setToolTip(QStringLiteral("Delete the selected gallery session"));
        m_saveCheckedButton->setToolTip(QStringLiteral("Save all checked unsaved sessions"));
        buttonLayout->addWidget(m_saveCheckedButton, 1);
        buttonLayout->addWidget(m_deleteButton);
        layout->addLayout(buttonLayout);

        connect(m_sessionList, &QListWidget::currentItemChanged, this,
                [this](QListWidgetItem*, QListWidgetItem*)
                {
                    updateButtons();
                });
        connect(m_sessionList, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem*) { openCurrentSession(); });
        connect(m_sessionList, &QListWidget::itemChanged, this, [this](QListWidgetItem*) { updateButtons(); });
        connect(m_sessionList, &QListWidget::customContextMenuRequested,
                this, &ImageGalleryWidget::showContextMenu);
        connect(m_saveCheckedButton, &QPushButton::clicked,
                this, &ImageGalleryWidget::saveCheckedSessions);
        connect(m_deleteButton, &QPushButton::clicked, this, &ImageGalleryWidget::deleteCurrentSession);

        auto* returnShortcut = new QShortcut(QKeySequence(Qt::Key_Return), m_sessionList);
        connect(returnShortcut, &QShortcut::activated, this, &ImageGalleryWidget::openCurrentSession);
        auto* enterShortcut = new QShortcut(QKeySequence(Qt::Key_Enter), m_sessionList);
        connect(enterShortcut, &QShortcut::activated, this, &ImageGalleryWidget::openCurrentSession);
        auto* deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), m_sessionList);
        connect(deleteShortcut, &QShortcut::activated, this, &ImageGalleryWidget::deleteCurrentSession);
        auto* backspaceShortcut = new QShortcut(QKeySequence(Qt::Key_Backspace), m_sessionList);
        connect(backspaceShortcut, &QShortcut::activated, this, &ImageGalleryWidget::deleteCurrentSession);
    }

    // Enable actions only when they have valid targets
    void ImageGalleryWidget::updateButtons()
    {
        const auto session = currentSession();
        const bool hasCurrent = session != nullptr;
        m_deleteButton->setEnabled(hasCurrent);
        m_saveCheckedButton->setEnabled(!checkedSessions().isEmpty());
    }

    // Open the currently selected session
    void ImageGalleryWidget::openCurrentSession()
    {
        const auto session = currentSession();
        if (canPreviewSession(session))
        {
            emit sessionOpenRequested(session);
        }
    }

    // Remove the currently selected session from the gallery
    void ImageGalleryWidget::deleteCurrentSession()
    {
        QListWidgetItem* item = m_sessionList->currentItem();
        if (!item)
        {
            return;
        }
        const auto session = m_core->recordingSession(item->data(kSessionIdRole).toString());
        delete m_sessionList->takeItem(m_sessionList->row(item));
        if (session)
        {
            emit sessionRemoved(session);
        }
        updateButtons();
        updateEmptyState();
    }

    // Save all checked unsaved sessions
    void ImageGalleryWidget::saveCheckedSessions()
    {
        const auto sessions = checkedSessions();
        if (!sessions.isEmpty())
        {
            emit saveSessionsRequested(sessions);
        }
    }

    // Check or uncheck every session that supports gallery selection
    void ImageGalleryWidget::setAllSessionsChecked(bool checked)
    {
        for (int row = 0; row < m_sessionList->count(); ++row)
        {
            QListWidgetItem* item = m_sessionList->item(row);
            if (item->flags() & Qt::ItemIsUserCheckable)
            {
                item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            }
        }
    }

    // Show gallery actions for the item under the pointer
    void ImageGalleryWidget::showContextMenu(const QPoint& position)
    {
        if (QListWidgetItem* item = m_sessionList->itemAt(position))
        {
            m_sessionList->setCurrentItem(item);
        }
        else
        {
            m_sessionList->clearSelection();
            m_sessionList->setCurrentItem(nullptr);
        }

        const auto session = currentSession();
        QMenu menu(this);
        QAction* openAction = menu.addAction(QStringLiteral("Open Preview (Enter)"));
        QAction* saveAsAction = menu.addAction(QStringLiteral("Save Selected As..."));
        QAction* deleteAction = menu.addAction(QStringLiteral("Delete (Del)"));
        menu.addSeparator();
        QAction* selectAllAction = menu.addAction(QStringLiteral("Select All"));
        QAction* unselectAllAction = menu.addAction(QStringLiteral("Unselect All"));
        QAction* saveCheckedAction = menu.addAction(QStringLiteral("Save Checked"));

        openAction->setEnabled(canPreviewSession(session));
        saveAsAction->setEnabled(session != nullptr);
        deleteAction->setEnabled(session != nullptr);
        saveCheckedAction->setEnabled(!checkedSessions().isEmpty());

        connect(openAction, &QAction::triggered, this, &ImageGalleryWidget::openCurrentSession);
        connect(saveAsAction, &QAction::triggered, this,
                [this]()
                {
                    const auto selected = currentSession();
                    if (selected)
                    {
                        emit saveSessionAsRequested(selected);
                    }
                });
        connect(deleteAction, &QAction::triggered, this, &ImageGalleryWidget::deleteCurrentSession);
        connect(selectAllAction, &QAction::triggered, this,
                [this]() { setAllSessionsChecked(true); });
        connect(unselectAllAction, &QAction::triggered, this,
                [this]() { setAllSessionsChecked(false); });
        connect(saveCheckedAction, &QAction::triggered,
                this, &ImageGalleryWidget::saveCheckedSessions);
        menu.exec(m_sessionList->viewport()->mapToGlobal(position));
    }

    // Apply an asynchronously loaded frame to its gallery item
    void ImageGalleryWidget::updateSessionThumbnail(
        const QString& sessionId,
        const scopeone::core::ImageFrame& frame)
    {
        for (int row = 0; row < m_sessionList->count(); ++row)
        {
            QListWidgetItem* item = m_sessionList->item(row);
            if (item->data(kSessionIdRole).toString() == sessionId.trimmed())
            {
                item->setIcon(frameThumbnail(frame));
                return;
            }
        }
    }

    // Show a simple empty state when no sessions exist
    void ImageGalleryWidget::updateEmptyState()
    {
        const bool empty = m_sessionList->count() == 0;
        m_emptyLabel->setVisible(empty);
        m_sessionList->setVisible(!empty);
    }

    // Build the visible title for one gallery item
    QString ImageGalleryWidget::displayTitle(
        const RecordingSessionData& session,
        const QString& title)
    {
        const QString trimmedTitle = title.trimmed();
        if (!trimmedTitle.isEmpty())
        {
            return trimmedTitle;
        }

        if (isSnapshotSession(session))
        {
            return QStringLiteral("Snapshot %1")
                .arg(m_nextSnapshotTitleIndex++, 3, 10, QLatin1Char('0'));
        }

        const QString sourceName = displaySourceName(session);
        if (!sourceName.isEmpty())
        {
            return sourceName;
        }

        return QStringLiteral("Recording %1")
            .arg(m_nextRecordingTitleIndex++, 3, 10, QLatin1Char('0'));
    }

    // Build the secondary text for one gallery item
    QString ImageGalleryWidget::itemSubtitle(const RecordingSessionData& session) const
    {
        const QString saveState = session.isSaved()
                                      ? (session.streamedToDisk() ? QStringLiteral("saved to disk")
                                                                 : QStringLiteral("saved"))
                                      : QStringLiteral("unsaved");
        QStringList details;
        const QString sourceName = displaySourceName(session);
        if (!sourceName.isEmpty())
        {
            details.append(sourceName);
        }
        details.append(QStringLiteral("%1 frame(s)").arg(sessionFrameCount(session)));
        details.append(QStringLiteral("%1 camera(s)").arg(sessionCameraCount(session)));
        details.append(saveState);
        return details.join(QStringLiteral(", "));
    }

    // Return the session behind the current list item
    std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> ImageGalleryWidget::currentSession() const
    {
        const QListWidgetItem* item = m_sessionList->currentItem();
        if (!item)
        {
            return {};
        }
        return m_core->recordingSession(item->data(kSessionIdRole).toString());
    }

    // Collect checked sessions for batch saving
    QList<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>> ImageGalleryWidget::checkedSessions()
        const
    {
        QList<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>> sessions;
        for (int row = 0; row < m_sessionList->count(); ++row)
        {
            const QListWidgetItem* item = m_sessionList->item(row);
            if (item->checkState() != Qt::Checked)
            {
                continue;
            }
            auto session = m_core->recordingSession(item->data(kSessionIdRole).toString());
            if (session && !session->isSaved())
            {
                sessions.append(session);
            }
        }
        return sessions;
    }
}
