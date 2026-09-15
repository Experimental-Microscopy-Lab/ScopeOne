#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QList>
#include <QWidget>
#include <memory>

class QLabel;
class QListWidget;
class QPushButton;
class QPoint;

namespace scopeone::ui
{
    class ImageGalleryWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit ImageGalleryWidget(scopeone::core::ScopeOneCore* core,
                                    QWidget* parent = nullptr);

        void addSession(const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
                        const QString& title = QString());
        void markSessionSaved(const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session);
        QList<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>> unsavedSessions() const;

    signals:
        void sessionOpenRequested(const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session);
        void sessionRemoved(const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session);
        void saveSessionAsRequested(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session);
        void saveSessionsRequested(
            const QList<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>>& sessions);

    private:
        void setupUI();
        void updateButtons();
        void updateEmptyState();
        QString displayTitle(const scopeone::core::ScopeOneCore::RecordingSessionData& session, const QString& title);
        QString itemSubtitle(const scopeone::core::ScopeOneCore::RecordingSessionData& session) const;
        std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData> currentSession() const;
        QList<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>> checkedSessions() const;
        void openCurrentSession();
        void deleteCurrentSession();
        void saveCheckedSessions();
        void setAllSessionsChecked(bool checked);
        void showContextMenu(const QPoint& position);
        void updateSessionThumbnail(const QString& sessionId,
                                    const scopeone::core::ImageFrame& frame);

        scopeone::core::ScopeOneCore* m_core{nullptr};
        QListWidget* m_sessionList{nullptr};
        QLabel* m_emptyLabel{nullptr};
        QPushButton* m_saveCheckedButton{nullptr};
        QPushButton* m_deleteButton{nullptr};
        int m_nextSnapshotTitleIndex{1};
        int m_nextRecordingTitleIndex{1};
    };
}
