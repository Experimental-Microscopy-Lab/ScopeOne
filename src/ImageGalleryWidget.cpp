#include "ImageGalleryWidget.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

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
        item->setData(kSessionIdRole, id);
        if (!session->isSaved())
        {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
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
        layout->addWidget(m_sessionList, 1);

        auto* buttonLayout = new QHBoxLayout();
        m_liveButton = new QPushButton(QStringLiteral("Live"), this);
        m_openButton = new QPushButton(QStringLiteral("Preview"), this);
        m_saveCheckedButton = new QPushButton(QStringLiteral("Save Checked"), this);
        m_deleteButton = new QPushButton(QStringLiteral("Delete"), this);
        buttonLayout->addWidget(m_liveButton);
        buttonLayout->addWidget(m_openButton);
        buttonLayout->addWidget(m_saveCheckedButton);
        buttonLayout->addWidget(m_deleteButton);
        layout->addLayout(buttonLayout);

        connect(m_sessionList, &QListWidget::currentItemChanged, this,
                [this](QListWidgetItem*, QListWidgetItem*)
                {
                    updateButtons();
                });
        connect(m_sessionList, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem*)
                {
                    auto session = currentSession();
                    if (canPreviewSession(session))
                    {
                        emit sessionOpenRequested(session);
                    }
                });
        connect(m_sessionList, &QListWidget::itemChanged, this, [this](QListWidgetItem*) { updateButtons(); });
        connect(m_liveButton, &QPushButton::clicked, this, &ImageGalleryWidget::livePreviewRequested);
        connect(m_openButton, &QPushButton::clicked, this,
                [this]()
                {
                    auto session = currentSession();
                    if (canPreviewSession(session))
                    {
                        emit sessionOpenRequested(session);
                    }
                });
        connect(m_saveCheckedButton, &QPushButton::clicked, this,
                [this]()
                {
                    const auto sessions = checkedSessions();
                    if (!sessions.isEmpty())
                    {
                        emit saveSessionsRequested(sessions);
                    }
                });
        connect(m_deleteButton, &QPushButton::clicked, this,
                [this]()
                {
                    QListWidgetItem* item = m_sessionList->currentItem();
                    if (!item)
                    {
                        return;
                    }
                    auto session = m_core->recordingSession(item->data(kSessionIdRole).toString());
                    delete m_sessionList->takeItem(m_sessionList->row(item));
                    if (session)
                    {
                        emit sessionRemoved(session);
                    }
                    updateButtons();
                    updateEmptyState();
                });
    }

    // Enable actions only when they have valid targets
    void ImageGalleryWidget::updateButtons()
    {
        const auto session = currentSession();
        const bool hasCurrent = session != nullptr;
        m_openButton->setEnabled(canPreviewSession(session));
        m_deleteButton->setEnabled(hasCurrent);
        m_saveCheckedButton->setEnabled(!checkedSessions().isEmpty());
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
