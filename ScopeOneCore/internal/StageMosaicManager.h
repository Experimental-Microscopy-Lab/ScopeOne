#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QObject>
#include <memory>

namespace scopeone::core::internal
{
    class StageMosaicManager : public QObject
    {
        Q_OBJECT

    public:
        explicit StageMosaicManager(ScopeOneCore* core, QObject* parent = nullptr);
        ~StageMosaicManager() override;

        bool start(const ScopeOneCore::StageMosaicPlan& plan, QString* errorMessage);
        void cancel();
        bool isRunning() const
        {
            return m_status.state == ScopeOneCore::StageMosaicState::Running;
        }
        ScopeOneCore::StageMosaicStatus status() const { return m_status; }

    signals:
        void progressChanged(int completedTiles, int totalTiles, const QString& message);
        void frameUpdated(const scopeone::core::ImageFrame& frame);
        void finished(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
            const QString& message,
            bool canceled);

    private:
        class Storage;

        void captureNextTile(quint64 generation);
        void waitForTileFrame(quint64 generation);
        void handleRawFrame(const ImageFrame& frame);
        bool initializeMosaic(const ImageFrame& frame, QString& errorMessage);
        bool appendTile(const ImageFrame& frame, int row, int column);
        ImageFrame publishMosaicFrame();
        void reportProgress(int completedTiles, int totalTiles, const QString& message);
        void finish(bool success, bool canceled, const QString& message);

        ScopeOneCore* m_core{nullptr};
        ScopeOneCore::StageMosaicPlan m_plan;
        std::unique_ptr<Storage> m_storage;
        ImageFrame m_referenceFrame;
        double m_originX{0.0};
        double m_originY{0.0};
        int m_currentTile{0};
        int m_frameWaitSerial{0};
        quint64 m_generation{0};
        bool m_waitingForFrame{false};
        bool m_startedPreview{false};
        ScopeOneCore::StageMosaicStatus m_status;
    };
}
