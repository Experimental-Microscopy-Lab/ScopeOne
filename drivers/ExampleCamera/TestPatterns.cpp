#include "TestPatterns.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace example_camera
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kTwoPi = 2.0 * kPi;

        struct Bead
        {
            double x;
            double y;
            double brightness;
        };

        constexpr std::array<Bead, 32> kBeads{{
            {0.08, 0.10, 0.91}, {0.19, 0.08, 0.84}, {0.32, 0.12, 0.98},
            {0.46, 0.07, 0.88}, {0.62, 0.11, 0.94}, {0.77, 0.08, 0.82},
            {0.91, 0.13, 0.96}, {0.13, 0.25, 0.87}, {0.29, 0.29, 1.00},
            {0.48, 0.23, 0.83}, {0.69, 0.27, 0.92}, {0.87, 0.31, 0.86},
            {0.08, 0.43, 0.95}, {0.24, 0.47, 0.81}, {0.42, 0.39, 0.90},
            {0.61, 0.45, 0.97}, {0.80, 0.42, 0.85}, {0.94, 0.50, 0.93},
            {0.14, 0.63, 0.89}, {0.34, 0.58, 0.99}, {0.53, 0.66, 0.84},
            {0.72, 0.61, 0.96}, {0.89, 0.68, 0.80}, {0.07, 0.82, 0.92},
            {0.22, 0.88, 0.86}, {0.39, 0.79, 0.95}, {0.58, 0.86, 0.82},
            {0.76, 0.81, 0.98}, {0.92, 0.89, 0.88}, {0.47, 0.94, 0.91},
            {0.30, 0.70, 0.85}, {0.67, 0.75, 0.94}
        }};

        // Pairs closer than the resolution, to judge how well an analysis separates them
        constexpr std::array<Bead, 8> kBeadDoublets{{
            {0.175, 0.176, 1.00}, {0.191, 0.176, 0.92},
            {0.535, 0.335, 0.88}, {0.551, 0.335, 0.96},
            {0.365, 0.515, 0.98}, {0.381, 0.515, 0.86},
            {0.705, 0.925, 0.91}, {0.721, 0.925, 1.00}
        }};

        // A separable Gaussian blur that repeats the edge pixels beyond the border
        void blur(std::vector<float>& image, int width, int height, double sigma)
        {
            const int radius = static_cast<int>(std::ceil(3.0 * sigma));
            std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
            float sum = 0.0f;
            for (int offset = -radius; offset <= radius; ++offset)
            {
                const float weight = static_cast<float>(std::exp(-offset * offset / (2.0 * sigma * sigma)));
                kernel[static_cast<std::size_t>(offset + radius)] = weight;
                sum += weight;
            }
            for (float& weight : kernel)
            {
                weight /= sum;
            }

            std::vector<float> pass(image.size());
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    float value = 0.0f;
                    for (int offset = -radius; offset <= radius; ++offset)
                    {
                        const int source = std::clamp(x + offset, 0, width - 1);
                        value += kernel[static_cast<std::size_t>(offset + radius)]
                               * image[static_cast<std::size_t>(y) * width + source];
                    }
                    pass[static_cast<std::size_t>(y) * width + x] = value;
                }
            }
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    float value = 0.0f;
                    for (int offset = -radius; offset <= radius; ++offset)
                    {
                        const int source = std::clamp(y + offset, 0, height - 1);
                        value += kernel[static_cast<std::size_t>(offset + radius)]
                               * pass[static_cast<std::size_t>(source) * width + x];
                    }
                    image[static_cast<std::size_t>(y) * width + x] = value;
                }
            }
        }
    }

    QStringList testPatternNames()
    {
        return {QStringLiteral("Moving Spot"),
                QStringLiteral("Gradient"),
                QStringLiteral("Hologram"),
                QStringLiteral("Red Blood Cells"),
                QStringLiteral("Fluorescent Beads")};
    }

    QString testPatternName(TestPattern pattern)
    {
        return testPatternNames().at(static_cast<int>(pattern));
    }

    bool testPatternFromName(const QString& name, TestPattern& pattern)
    {
        const qsizetype index = testPatternNames().indexOf(name);
        if (index < 0)
        {
            return false;
        }
        pattern = static_cast<TestPattern>(index);
        return true;
    }

    TestPatterns::TestPatterns(int sensorWidth, int sensorHeight)
        : m_sensorWidth(sensorWidth)
          , m_sensorHeight(sensorHeight)
    {
        layOutBloodCells();
        layOutBeads();
    }

    void TestPatterns::render(TestPattern pattern,
                              quint64 frameIndex,
                              int roiX,
                              int roiY,
                              int roiWidth,
                              int roiHeight,
                              std::vector<double>& intensities) const
    {
        intensities.resize(static_cast<std::size_t>(roiWidth) * roiHeight);
        const double angle = kTwoPi * static_cast<double>(frameIndex % 200) / 200.0;
        const double spotX = m_sensorWidth * (0.5 + 0.3 * std::cos(angle));
        const double spotY = m_sensorHeight * (0.5 + 0.3 * std::sin(angle));
        const double spotSigma = 0.04 * std::min(m_sensorWidth, m_sensorHeight);
        for (int y = 0; y < roiHeight; ++y)
        {
            const int sensorY = roiY + y;
            for (int x = 0; x < roiWidth; ++x)
            {
                const int sensorX = roiX + x;
                const std::size_t sensorIndex = static_cast<std::size_t>(sensorY) * m_sensorWidth + sensorX;
                // The +1 order of the hologram carriers lies in the upper half of the spectrum, at (48, -32), where
                // the DHM reconstruction looks for it, so that the phase comes out with the right sign
                const double carrier = kTwoPi * (48.0 * sensorX / m_sensorWidth - 32.0 * sensorY / m_sensorHeight);
                double intensity = 0.0;
                switch (pattern)
                {
                case TestPattern::MovingSpot:
                {
                    // A bright spot circles over a dim gradient
                    const double dx = sensorX - spotX;
                    const double dy = sensorY - spotY;
                    intensity = 10.0 + 20.0 * sensorX / m_sensorWidth
                              + 200.0 * std::exp(-(dx * dx + dy * dy) / (2.0 * spotSigma * spotSigma));
                    break;
                }
                case TestPattern::Gradient:
                    intensity = static_cast<double>((sensorX + sensorY + frameIndex) & 0xffu);
                    break;
                case TestPattern::Hologram:
                {
                    // An off axis hologram of two smooth objects whose phase drifts slowly
                    const double nx = (sensorX - 0.5 * m_sensorWidth) / m_sensorWidth;
                    const double ny = (sensorY - 0.5 * m_sensorHeight) / m_sensorHeight;
                    const double amplitude = 0.65 * std::exp(-35.0 * (nx * nx + ny * ny))
                                           + 0.35 * std::exp(-90.0 * ((nx - 0.18) * (nx - 0.18)
                                                                      + (ny + 0.12) * (ny + 0.12)));
                    const double phase = 18.0 * (nx * nx + ny * ny) + 0.015 * static_cast<double>(frameIndex);
                    intensity = 70.0 + 55.0 * amplitude * amplitude + 120.0 * amplitude * std::cos(carrier + phase);
                    break;
                }
                case TestPattern::RedBloodCells:
                {
                    // An off axis hologram of a thin layer of red blood cells. The reference wave is much stronger
                    // than the light of the cells, as in a real setup, so the fringes have a contrast of about one
                    // half, and the cells take some of the light, which dims the fringes over them.
                    constexpr double kReferenceAmplitude = 11.8;
                    constexpr double kObjectScale = 4.1;
                    const double object = kObjectScale * 0.8 * m_cellAmplitude[sensorIndex];
                    intensity = kReferenceAmplitude * kReferenceAmplitude + object * object
                              + 2.0 * kReferenceAmplitude * object * std::cos(carrier + m_cellPhase[sensorIndex]);
                    break;
                }
                case TestPattern::FluorescentBeads:
                    intensity = m_beads[sensorIndex];
                    break;
                }
                intensities[static_cast<std::size_t>(y) * roiWidth + x] = intensity;
            }
        }
    }

    // Lays out red blood cells that do not touch, each a biconcave disc, and works out the amplitude and the phase
    // that a plane wave takes on passing through them. The thickness follows the shape of Evans and Fung, scaled to a
    // rim of 2.4 micrometers, and the phase is 2 pi times the difference of the refractive index of the cell and the
    // medium (0.06) times the thickness, over the wavelength (0.633 micrometers).
    void TestPatterns::layOutBloodCells()
    {
        const std::size_t count = static_cast<std::size_t>(m_sensorWidth) * m_sensorHeight;
        m_cellAmplitude.assign(count, 1.0f);
        m_cellPhase.assign(count, 0.0f);

        constexpr double kC0 = 0.207161;
        constexpr double kC2 = 2.002558;
        constexpr double kC4 = -1.122762;
        const auto shape = [](double rho)
        {
            return std::sqrt(std::max(0.0, 1.0 - rho * rho)) * (kC0 + kC2 * rho * rho + kC4 * rho * rho * rho * rho);
        };
        double peak = 0.0;
        for (int step = 0; step <= 1000; ++step)
        {
            peak = std::max(peak, shape(step / 1000.0));
        }
        constexpr double kPeakThicknessUm = 2.4;
        constexpr double kPhasePerMicrometer = kTwoPi * 0.06 / 0.633;

        // The field is about 60 micrometers wide and a cell about 7.8 micrometers across
        const double radiusBase = 0.065 * std::min(m_sensorWidth, m_sensorHeight);
        struct Cell
        {
            double x;
            double y;
            double radius;
            double squeeze;
            double angle;
        };
        std::vector<Cell> cells;
        std::mt19937 layout(20240601u);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        for (int attempt = 0; attempt < 4000 && cells.size() < 24; ++attempt)
        {
            Cell cell;
            cell.radius = radiusBase * (0.9 + 0.2 * unit(layout));
            cell.squeeze = 0.85 + 0.15 * unit(layout);
            cell.angle = kPi * unit(layout);
            cell.x = cell.radius + unit(layout) * (m_sensorWidth - 2.0 * cell.radius);
            cell.y = cell.radius + unit(layout) * (m_sensorHeight - 2.0 * cell.radius);
            const bool free = std::none_of(cells.begin(), cells.end(), [&](const Cell& other)
            {
                return std::hypot(cell.x - other.x, cell.y - other.y) <= 1.08 * (cell.radius + other.radius);
            });
            if (free)
            {
                cells.push_back(cell);
            }
        }

        for (const Cell& cell : cells)
        {
            const int reach = static_cast<int>(std::ceil(cell.radius * 1.05));
            const int minX = std::max(0, static_cast<int>(cell.x) - reach);
            const int maxX = std::min(m_sensorWidth - 1, static_cast<int>(cell.x) + reach);
            const int minY = std::max(0, static_cast<int>(cell.y) - reach);
            const int maxY = std::min(m_sensorHeight - 1, static_cast<int>(cell.y) + reach);
            const double cosine = std::cos(cell.angle);
            const double sine = std::sin(cell.angle);
            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    const double dx = x + 0.5 - cell.x;
                    const double dy = y + 0.5 - cell.y;
                    // The cell is a little flattened along one direction
                    const double u = (dx * cosine + dy * sine) / cell.radius;
                    const double v = (-dx * sine + dy * cosine) / (cell.radius * cell.squeeze);
                    const double rho = std::sqrt(u * u + v * v);
                    if (rho >= 1.0)
                    {
                        continue;
                    }
                    const double thicknessUm = kPeakThicknessUm * shape(rho) / peak;
                    const std::size_t index = static_cast<std::size_t>(y) * m_sensorWidth + x;
                    m_cellPhase[index] = static_cast<float>(kPhasePerMicrometer * thicknessUm);
                    // Hemoglobin takes light, more where the cell is thick
                    m_cellAmplitude[index] = static_cast<float>(1.0 - 0.5 * thicknessUm / kPeakThicknessUm);
                }
            }
        }

        // A microscope does not see the sharp edge of a cell: its resolution and the size of a pixel soften it.
        // Without this the thickness drops to zero at the rim with an infinite slope, which spreads over the whole
        // spectrum and comes back from the reconstruction as ripples around the cells.
        const double blurSigma = std::max(1.0, 0.004 * std::min(m_sensorWidth, m_sensorHeight));
        blur(m_cellPhase, m_sensorWidth, m_sensorHeight, blurSigma);
        blur(m_cellAmplitude, m_sensorWidth, m_sensorHeight, blurSigma);
    }

    // A still field of beads, each a Gaussian point spread function of about 4 pixels FWHM at 256 pixels
    void TestPatterns::layOutBeads()
    {
        m_beads.assign(static_cast<std::size_t>(m_sensorWidth) * m_sensorHeight, 3.0f);
        const double resolutionScale = std::min(m_sensorWidth, m_sensorHeight) / 256.0;
        const double sigma = 4.0 * resolutionScale / 2.354820045;
        const int radius = static_cast<int>(std::ceil(4.0 * sigma));
        const auto drawBead = [&](const Bead& bead)
        {
            const double centerX = bead.x * m_sensorWidth;
            const double centerY = bead.y * m_sensorHeight;
            const int minX = std::max(0, static_cast<int>(std::floor(centerX)) - radius);
            const int maxX = std::min(m_sensorWidth - 1, static_cast<int>(std::floor(centerX)) + radius);
            const int minY = std::max(0, static_cast<int>(std::floor(centerY)) - radius);
            const int maxY = std::min(m_sensorHeight - 1, static_cast<int>(std::floor(centerY)) + radius);
            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    const double dx = x + 0.5 - centerX;
                    const double dy = y + 0.5 - centerY;
                    m_beads[static_cast<std::size_t>(y) * m_sensorWidth + x]
                        += static_cast<float>(245.0 * bead.brightness * std::exp(-(dx * dx + dy * dy)
                                                                                 / (2.0 * sigma * sigma)));
                }
            }
        };
        for (const Bead& bead : kBeads)
        {
            drawBead(bead);
        }
        for (const Bead& bead : kBeadDoublets)
        {
            drawBead(bead);
        }
    }
}
