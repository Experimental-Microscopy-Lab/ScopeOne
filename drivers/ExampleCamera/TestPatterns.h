#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <vector>

namespace example_camera
{
    enum class TestPattern
    {
        MovingSpot,
        Gradient,
        Hologram,
        RedBloodCells,
        FluorescentBeads
    };

    QStringList testPatternNames();
    QString testPatternName(TestPattern pattern);
    bool testPatternFromName(const QString& name, TestPattern& pattern);

    // The scenes the example camera can look at. A real driver reads its images from the vendor SDK
    // instead. The scenes are laid out once for the size of the sensor and read from any thread.
    class TestPatterns
    {
    public:
        TestPatterns(int sensorWidth, int sensorHeight);

        // The light on each pixel of the region, on a scale where 255 fills an 8-bit camera at the
        // default exposure. Moving scenes advance with the frame index.
        void render(TestPattern pattern,
                    quint64 frameIndex,
                    int roiX,
                    int roiY,
                    int roiWidth,
                    int roiHeight,
                    std::vector<double>& intensities) const;

    private:
        void layOutBloodCells();
        void layOutBeads();

        int m_sensorWidth;
        int m_sensorHeight;
        std::vector<float> m_cellAmplitude;
        std::vector<float> m_cellPhase;
        std::vector<float> m_beads;
    };
}
