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
        constexpr int kSessionIdRole = Qt::UserRole + 1;

        // Return the generated snapshot suffix index
        qsizetype snapshotSuffixIndex(const QString& baseName)
        {
            return baseName.lastIndexOf(QStringLiteral("_capture_"));
        }

        // Return true for sessions created by Snap to Gallery
        bool isSnapshotSession(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            return session && snapshotSuffixIndex(session->capturePlan().baseName) > 0;
        }

        // Return the user visible source name without generated capture suffix
        QString displaySourceName(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            if (!session)
            {
                return {};
            }

            const QString baseName = session->capturePlan().baseName.trimmed();
            const qsizetype suffixIndex = snapshotSuffixIndex(baseName);
            if (suffixIndex > 0)
            {
                return baseName.left(suffixIndex).trimmed();
            }
            return baseName;
        }

        // Return true when a session should appear in the gallery
        bool isGallerySession(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            return session && (session->hasAnyFrames() || session->isSaved());
        }

        // Return true when a session can be opened in the image viewer
        bool canPreviewSession(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            return session && session->hasAnyFrames();
        }

        // Count cameras that have at least one stored frame
        int sessionCameraCount(const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            if (!session)
            {
                return 0;
            }

            int count = 0;
            for (const QString& cameraId : session->recordedCameraIds())
            {
                const auto* frames = session->framesForCamera(cameraId);
                if (frames && !frames->empty())
                {
                    ++count;
                }
            }
            if (count > 0)
            {
                return count;
            }
            const int outputCount = session->outputFiles().size();
            if (outputCount > 0)
            {
                return outputCount;
            }
            return session->capturePlan().cameraIds.size();
        }

        // Count buffered frames or streamed frames written to disk
        qint64 sessionFrameCount(const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session)
        {
            if (!session)
            {
                return 0;
            }
            if (session->hasAnyFrames())
            {
                return session->frameCount();
            }

            qint64 count = 0;
            const auto& outputFiles = session->outputFiles();
            for (auto it = outputFiles.constBegin(); it != outputFiles.constEnd(); ++it)
            {
                count += it.value().framesWritten;
            }
            return count;
        }
    }

    // Create the image gallery panel
    ImageGalleryWidget::ImageGalleryWidget(QWidget* parent)
        : QWidget(parent)
    {
        setupUI();
        updateButtons();
        updateEmptyState();
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

        const QString id = sessionId();
        m_sessions.insert(id, session);

        auto* item = new QListWidgetItem(m_sessionList);
        item->setText(displayTitle(session, title) + QLatin1Char('\n') + itemSubtitle(session));
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

        for (int row = 0; row < m_sessionList->count(); ++row)
        {
            QListWidgetItem* item = m_sessionList->item(row);
            const QString id = item->data(kSessionIdRole).toString();
            if (m_sessions.value(id) != session)
            {
                continue;
            }
            const QString firstLine = item->text().section(QLatin1Char('\n'), 0, 0);
            item->setText(firstLine + QLatin1Char('\n') + itemSubtitle(session));
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
        for (const auto& session : m_sessions)
        {
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
        m_openButton = new QPushButton(QStringLiteral("Open"), this);
        m_saveCheckedButton = new QPushButton(QStringLiteral("Save Checked"), this);
        m_removeButton = new QPushButton(QStringLiteral("Remove"), this);
        buttonLayout->addWidget(m_openButton);
        buttonLayout->addWidget(m_saveCheckedButton);
        buttonLayout->addWidget(m_removeButton);
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
        connect(m_removeButton, &QPushButton::clicked, this,
                [this]()
                {
                    QListWidgetItem* item = m_sessionList->currentItem();
                    if (!item)
                    {
                        return;
                    }
                    m_sessions.remove(item->data(kSessionIdRole).toString());
                    delete m_sessionList->takeItem(m_sessionList->row(item));
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
        m_removeButton->setEnabled(hasCurrent);
        m_saveCheckedButton->setEnabled(!checkedSessions().isEmpty());
    }

    // Show a simple empty state when no sessions exist
    void ImageGalleryWidget::updateEmptyState()
    {
        const bool empty = m_sessionList->count() == 0;
        m_emptyLabel->setVisible(empty);
        m_sessionList->setVisible(!empty);
    }

    // Allocate a unique runtime id for one gallery item
    QString ImageGalleryWidget::sessionId()
    {
        return QStringLiteral("session_%1").arg(m_nextSessionId++);
    }

    // Build the visible title for one gallery item
    QString ImageGalleryWidget::displayTitle(
        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
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
    QString ImageGalleryWidget::itemSubtitle(
        const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session) const
    {
        if (!session)
        {
            return QStringLiteral("Invalid session");
        }

        const QString saveState = session->isSaved()
                                      ? (session->streamedToDisk() ? QStringLiteral("saved to disk")
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
        return m_sessions.value(item->data(kSessionIdRole).toString());
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
            auto session = m_sessions.value(item->data(kSessionIdRole).toString());
            if (session && !session->isSaved())
            {
                sessions.append(session);
            }
        }
        return sessions;
    }
}
