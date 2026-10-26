#include "internal/ImageOperationModules.h"
#include "internal/FrameBufferUtils.h"
#include "internal/ProcessingModuleRegistry.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
#include <opencv2/imgproc.hpp>

namespace scopeone::core::internal
{
    namespace
    {
        ProcessingParameterDescriptor integerParam(const char* key, const char* name, int value,
                                                   int minimum, int maximum, int step = 1)
        {
            return {QString::fromLatin1(key), QString::fromLatin1(name), ProcessingParameterType::Integer,
                    value, minimum, maximum, step};
        }

        ProcessingParameterDescriptor realParam(const char* key, const char* name, double value,
                                                double minimum, double maximum, double step, int decimals)
        {
            ProcessingParameterDescriptor descriptor{QString::fromLatin1(key), QString::fromLatin1(name),
                                                     ProcessingParameterType::Real, value, minimum, maximum, step};
            descriptor.decimals = decimals;
            return descriptor;
        }

        ProcessingParameterDescriptor boolParam(const char* key, const char* name, bool value)
        {
            return {QString::fromLatin1(key), QString::fromLatin1(name), ProcessingParameterType::Boolean, value};
        }

        ProcessingParameterDescriptor choiceParam(const char* key, const char* name, int value,
                                                  std::initializer_list<const char*> choices)
        {
            ProcessingParameterDescriptor descriptor{QString::fromLatin1(key), QString::fromLatin1(name),
                                                     ProcessingParameterType::Choice, value};
            int index = 0;
            for (const char* choice : choices)
            {
                descriptor.choices.append({QString::fromLatin1(choice), index++});
            }
            return descriptor;
        }

        int limitOf(const ImageFrame& frame)
        {
            return frame.isMono16() ? ((1 << std::clamp(frame.bitsPerSample, 1, 16)) - 1) : 255;
        }

        // Runs a table over every sample of a frame
        cv::Mat applyTable(const cv::Mat& src, const std::vector<float>& table, int limit)
        {
            cv::Mat dst(src.rows, src.cols, src.type());
            cv::parallel_for_(cv::Range(0, src.rows), [&](const cv::Range& rows)
            {
                for (int y = rows.start; y < rows.end; ++y)
                {
                    if (src.type() == CV_8UC1)
                    {
                        const uchar* in = src.ptr<uchar>(y);
                        uchar* out = dst.ptr<uchar>(y);
                        for (int x = 0; x < src.cols; ++x)
                        {
                            out[x] = static_cast<uchar>(table[in[x]]);
                        }
                    }
                    else
                    {
                        const quint16* in = src.ptr<quint16>(y);
                        quint16* out = dst.ptr<quint16>(y);
                        for (int x = 0; x < src.cols; ++x)
                        {
                            out[x] = static_cast<quint16>(table[std::min<int>(in[x], limit)]);
                        }
                    }
                }
            });
            return dst;
        }

        cv::Mat fillHolesOperation(const cv::Mat& mask);
        cv::Mat skeletonOperation(const cv::Mat& mask);

        float rounded(double value, int limit)
        {
            if (!(value > 0.0))
            {
                return 0.0f;
            }
            return static_cast<float>(std::min<double>(std::floor(value + 0.5), limit));
        }

        cv::Mat mathOperation(const cv::Mat& src, int limit, int operation, double value)
        {
            const double top = limit;
            std::vector<float> table(static_cast<size_t>(limit) + 1);
            for (int v = 0; v <= limit; ++v)
            {
                double r = v;
                switch (operation)
                {
                case 0: r = v + value; break;
                case 1: r = v - value; break;
                case 2: r = v * value; break;
                case 3: r = value != 0.0 ? v / value : top; break;
                case 4: r = std::max<double>(v, value); break;
                case 5: r = std::min<double>(v, value); break;
                case 6: r = std::pow(v / top, value) * top; break;
                case 7: r = v > 0 ? std::log(static_cast<double>(v)) * (top / std::log(top)) : 0.0; break;
                case 8: r = std::exp(v * (std::log(top) / top)); break;
                case 9: r = static_cast<double>(v) * v; break;
                case 10: r = std::sqrt(static_cast<double>(v)); break;
                default: break;
                }
                table[static_cast<size_t>(v)] = rounded(r, limit);
            }
            return applyTable(src, table, limit);
        }

        cv::Mat discKernel(double radius, bool normalized)
        {
            const double r = std::max(0.5, radius);
            const int half = static_cast<int>(std::floor(r));
            const int size = 2 * half + 1;
            cv::Mat kernel = cv::Mat::zeros(size, size, normalized ? CV_32F : CV_8U);
            const double limit = r * r + 1.0;
            for (int y = -half; y <= half; ++y)
            {
                for (int x = -half; x <= half; ++x)
                {
                    if (x * x + y * y <= limit)
                    {
                        if (normalized)
                        {
                            kernel.at<float>(y + half, x + half) = 1.0f;
                        }
                        else
                        {
                            kernel.at<uchar>(y + half, x + half) = 1;
                        }
                    }
                }
            }
            if (normalized)
            {
                kernel /= cv::sum(kernel)[0];
            }
            return kernel;
        }

        // Median over a disc, for sizes the median blur of OpenCV does not take
        cv::Mat discMedian(const cv::Mat& src, double radius)
        {
            const cv::Mat mask = discKernel(radius, false);
            const int half = mask.rows / 2;
            cv::Mat padded;
            cv::copyMakeBorder(src, padded, half, half, half, half, cv::BORDER_REPLICATE);
            std::vector<cv::Point> offsets;
            for (int y = 0; y < mask.rows; ++y)
            {
                for (int x = 0; x < mask.cols; ++x)
                {
                    if (mask.at<uchar>(y, x))
                    {
                        offsets.emplace_back(x, y);
                    }
                }
            }
            cv::Mat dst(src.rows, src.cols, src.type());
            cv::parallel_for_(cv::Range(0, src.rows), [&](const cv::Range& rows)
            {
                std::vector<quint16> window(offsets.size());
                for (int y = rows.start; y < rows.end; ++y)
                {
                    for (int x = 0; x < src.cols; ++x)
                    {
                        for (size_t k = 0; k < offsets.size(); ++k)
                        {
                            const cv::Point& o = offsets[k];
                            window[k] = src.type() == CV_8UC1 ? padded.at<uchar>(y + o.y, x + o.x)
                                                              : padded.at<quint16>(y + o.y, x + o.x);
                        }
                        std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
                        if (src.type() == CV_8UC1)
                        {
                            dst.at<uchar>(y, x) = static_cast<uchar>(window[window.size() / 2]);
                        }
                        else
                        {
                            dst.at<quint16>(y, x) = window[window.size() / 2];
                        }
                    }
                }
            });
            return dst;
        }

        cv::Mat filterOperation(const cv::Mat& src, int kind, double radius)
        {
            cv::Mat dst;
            switch (kind)
            {
            case 0:
                cv::filter2D(src, dst, -1, discKernel(radius, true), cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
                break;
            case 1:
            {
                const int size = 2 * static_cast<int>(std::floor(std::max(0.5, radius))) + 1;
                if (size <= 5 || src.type() == CV_8UC1)
                {
                    cv::medianBlur(src, dst, size);
                }
                else
                {
                    dst = discMedian(src, radius);
                }
                break;
            }
            case 2:
                cv::erode(src, dst, discKernel(radius, false), cv::Point(-1, -1), 1, cv::BORDER_REPLICATE);
                break;
            case 3:
                cv::dilate(src, dst, discKernel(radius, false), cv::Point(-1, -1), 1, cv::BORDER_REPLICATE);
                break;
            case 4:
                cv::GaussianBlur(src, dst, cv::Size(0, 0), std::max(0.1, radius), 0, cv::BORDER_REPLICATE);
                break;
            default:
            {
                cv::Mat values;
                src.convertTo(values, CV_32F);
                const cv::Mat kernel = discKernel(radius, true);
                cv::Mat mean;
                cv::Mat meanOfSquares;
                cv::filter2D(values, mean, -1, kernel, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
                cv::filter2D(values.mul(values), meanOfSquares, -1, kernel, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
                cv::Mat variance = cv::max(meanOfSquares - mean.mul(mean), 0.0);
                variance.convertTo(dst, src.type());
                break;
            }
            }
            return dst;
        }

        std::vector<double> histogramOf(const cv::Mat& src, int limit)
        {
            std::vector<double> histogram(static_cast<size_t>(limit) + 1, 0.0);
            for (int y = 0; y < src.rows; ++y)
            {
                for (int x = 0; x < src.cols; ++x)
                {
                    const int v = src.type() == CV_8UC1 ? src.at<uchar>(y, x) : src.at<quint16>(y, x);
                    histogram[static_cast<size_t>(std::min(v, limit))] += 1.0;
                }
            }
            return histogram;
        }

        int autoLevel(const std::vector<double>& histogram, int method)
        {
            const int n = static_cast<int>(histogram.size());
            double total = 0.0;
            double sum = 0.0;
            for (int i = 0; i < n; ++i)
            {
                total += histogram[static_cast<size_t>(i)];
                sum += i * histogram[static_cast<size_t>(i)];
            }
            if (total <= 0.0)
            {
                return 0;
            }
            const double mean = sum / total;
            if (method == 2)
            {
                return static_cast<int>(mean);
            }
            if (method == 1)
            {
                // Isodata: the level halfway between the means on both sides
                int level = static_cast<int>(mean);
                for (int iteration = 0; iteration < 1000; ++iteration)
                {
                    double lowCount = 0.0, lowSum = 0.0, highCount = 0.0, highSum = 0.0;
                    for (int i = 0; i < n; ++i)
                    {
                        const double c = histogram[static_cast<size_t>(i)];
                        if (i <= level)
                        {
                            lowCount += c;
                            lowSum += i * c;
                        }
                        else
                        {
                            highCount += c;
                            highSum += i * c;
                        }
                    }
                    if (lowCount <= 0.0 || highCount <= 0.0)
                    {
                        break;
                    }
                    const int next = static_cast<int>((lowSum / lowCount + highSum / highCount) / 2.0);
                    if (next == level)
                    {
                        break;
                    }
                    level = next;
                }
                return level;
            }
            if (method == 0)
            {
                // Otsu: the level that tells the two sides apart best
                double lowCount = 0.0, lowSum = 0.0, best = -1.0;
                int level = 0;
                for (int i = 0; i < n; ++i)
                {
                    lowCount += histogram[static_cast<size_t>(i)];
                    lowSum += i * histogram[static_cast<size_t>(i)];
                    const double highCount = total - lowCount;
                    if (lowCount <= 0.0 || highCount <= 0.0)
                    {
                        continue;
                    }
                    const double difference = lowSum / lowCount - (sum - lowSum) / highCount;
                    const double between = lowCount * highCount * difference * difference;
                    if (between > best)
                    {
                        best = between;
                        level = i;
                    }
                }
                return level;
            }
            // Triangle: the level furthest from the line from the peak to the far end
            int peak = 0;
            int first = 0;
            int last = n - 1;
            while (first < n - 1 && histogram[static_cast<size_t>(first)] == 0.0) ++first;
            while (last > 0 && histogram[static_cast<size_t>(last)] == 0.0) --last;
            for (int i = first; i <= last; ++i)
            {
                if (histogram[static_cast<size_t>(i)] > histogram[static_cast<size_t>(peak)])
                {
                    peak = i;
                }
            }
            const bool flip = (peak - first) < (last - peak);
            const int end = flip ? first : last;
            const double dx = end - peak;
            const double dy = -histogram[static_cast<size_t>(peak)];
            const double norm = std::sqrt(dx * dx + dy * dy);
            double bestDistance = -1.0;
            int level = peak;
            const int from = std::min(peak, end);
            const int to = std::max(peak, end);
            for (int i = from; i <= to; ++i)
            {
                const double distance = std::abs(dy * (i - peak) - dx * (histogram[static_cast<size_t>(i)]
                                                                         - histogram[static_cast<size_t>(peak)]))
                                        / (norm > 0.0 ? norm : 1.0);
                if (distance > bestDistance)
                {
                    bestDistance = distance;
                    level = i;
                }
            }
            return level;
        }

        cv::Mat thresholdOperation(const cv::Mat& src, int limit, const QVariantMap& p)
        {
            const int method = p.value("method").toInt();
            cv::Mat mask;
            if (method == 0)
            {
                const double low = p.value("low").toDouble();
                const double high = p.value("high").toDouble();
                cv::inRange(src, cv::Scalar(low), cv::Scalar(high), mask);
                return mask;
            }
            const int level = autoLevel(histogramOf(src, limit), method - 1);
            if (p.value("select").toInt() == 0)
            {
                cv::compare(src, level, mask, cv::CMP_GT);
            }
            else
            {
                cv::compare(src, level, mask, cv::CMP_LE);
            }
            return mask;
        }

        cv::Mat binaryOperation(const cv::Mat& src, int operation, int iterations)
        {
            cv::Mat mask;
            cv::compare(src, 0, mask, cv::CMP_GT);
            const cv::Mat element = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
            const int count = std::max(1, iterations);
            cv::Mat result;
            switch (operation)
            {
            case 0:
                cv::erode(mask, result, element, cv::Point(-1, -1), count, cv::BORDER_CONSTANT, cv::Scalar(0));
                break;
            case 1:
                cv::dilate(mask, result, element, cv::Point(-1, -1), count, cv::BORDER_CONSTANT, cv::Scalar(0));
                break;
            case 2:
                cv::erode(mask, result, element, cv::Point(-1, -1), count, cv::BORDER_CONSTANT, cv::Scalar(0));
                cv::dilate(result, result, element, cv::Point(-1, -1), count, cv::BORDER_CONSTANT, cv::Scalar(0));
                break;
            case 3:
                cv::dilate(mask, result, element, cv::Point(-1, -1), count, cv::BORDER_CONSTANT, cv::Scalar(0));
                cv::erode(result, result, element, cv::Point(-1, -1), count, cv::BORDER_CONSTANT, cv::Scalar(0));
                break;
            case 4:
            {
                cv::Mat eroded;
                cv::erode(mask, eroded, element, cv::Point(-1, -1), 1, cv::BORDER_CONSTANT, cv::Scalar(0));
                result = mask - eroded;
                break;
            }
            case 5:
                cv::bitwise_not(mask, result);
                break;
            case 6:
                result = fillHolesOperation(mask);
                break;
            default:
                result = skeletonOperation(mask);
                break;
            }
            return result;
        }

        cv::Mat sharpenOperation(const cv::Mat& src)
        {
            const cv::Mat kernel = (cv::Mat_<float>(3, 3) << -1, -1, -1, -1, 12, -1, -1, -1, -1) / 4.0f;
            cv::Mat dst;
            cv::filter2D(src, dst, -1, kernel, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
            return dst;
        }

        cv::Mat edgeOperation(const cv::Mat& src)
        {
            cv::Mat gx, gy, magnitude, dst;
            cv::Sobel(src, gx, CV_32F, 1, 0, 3, 1.0, 0, cv::BORDER_REPLICATE);
            cv::Sobel(src, gy, CV_32F, 0, 1, 3, 1.0, 0, cv::BORDER_REPLICATE);
            cv::magnitude(gx, gy, magnitude);
            magnitude.convertTo(dst, src.type());
            return dst;
        }

        cv::Mat unsharpOperation(const cv::Mat& src, double sigma, double weight)
        {
            cv::Mat blurred, dst;
            cv::GaussianBlur(src, blurred, cv::Size(0, 0), std::max(0.1, sigma), 0, cv::BORDER_REPLICATE);
            cv::addWeighted(src, 1.0 / (1.0 - weight), blurred, -weight / (1.0 - weight), 0.0, dst, src.depth());
            return dst;
        }

        cv::Mat topHatOperation(const cv::Mat& src, double radius, bool lightBackground)
        {
            cv::Mat dst;
            cv::morphologyEx(src, dst, lightBackground ? cv::MORPH_BLACKHAT : cv::MORPH_TOPHAT,
                             discKernel(radius, false), cv::Point(-1, -1), 1, cv::BORDER_REPLICATE);
            return dst;
        }

        cv::Mat claheOperation(const cv::Mat& src, int blockSize, double slope)
        {
            const int tilesX = std::max(1, static_cast<int>(std::lround(static_cast<double>(src.cols) / std::max(8, blockSize))));
            const int tilesY = std::max(1, static_cast<int>(std::lround(static_cast<double>(src.rows) / std::max(8, blockSize))));
            cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(slope, cv::Size(tilesX, tilesY));
            cv::Mat dst;
            clahe->apply(src, dst);
            return dst;
        }

        // Spreads the values so that all of them are about equally frequent
        cv::Mat equalizeOperation(const cv::Mat& src, int limit, bool squareRoot)
        {
            const std::vector<double> histogram = histogramOf(src, limit);
            std::vector<double> weights(histogram.size());
            double total = 0.0;
            for (size_t i = 0; i < histogram.size(); ++i)
            {
                weights[i] = squareRoot ? std::sqrt(histogram[i]) : histogram[i];
                total += weights[i];
            }
            std::vector<float> table(histogram.size());
            double running = 0.0;
            for (size_t i = 0; i < histogram.size(); ++i)
            {
                running += weights[i];
                table[i] = total > 0.0 ? rounded(running / total * limit, limit) : static_cast<float>(i);
            }
            return applyTable(src, table, limit);
        }

        cv::Mat objectMask(const cv::Mat& src)
        {
            cv::Mat mask;
            cv::compare(src, 0, mask, cv::CMP_GT);
            return mask;
        }

        // Thins every object to a line one pixel wide
        cv::Mat skeletonOperation(const cv::Mat& mask)
        {
            cv::Mat padded;
            cv::copyMakeBorder(mask / 255, padded, 1, 1, 1, 1, cv::BORDER_CONSTANT, cv::Scalar(0));
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (int pass = 0; pass < 2; ++pass)
                {
                    std::vector<cv::Point> remove;
                    for (int y = 1; y < padded.rows - 1; ++y)
                    {
                        for (int x = 1; x < padded.cols - 1; ++x)
                        {
                            if (!padded.at<uchar>(y, x))
                            {
                                continue;
                            }
                            const int p2 = padded.at<uchar>(y - 1, x), p3 = padded.at<uchar>(y - 1, x + 1),
                                      p4 = padded.at<uchar>(y, x + 1), p5 = padded.at<uchar>(y + 1, x + 1),
                                      p6 = padded.at<uchar>(y + 1, x), p7 = padded.at<uchar>(y + 1, x - 1),
                                      p8 = padded.at<uchar>(y, x - 1), p9 = padded.at<uchar>(y - 1, x - 1);
                            const int neighbours = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
                            if (neighbours < 2 || neighbours > 6)
                            {
                                continue;
                            }
                            const int ring[9] = {p2, p3, p4, p5, p6, p7, p8, p9, p2};
                            int transitions = 0;
                            for (int i = 0; i < 8; ++i)
                            {
                                transitions += (ring[i] == 0 && ring[i + 1] == 1);
                            }
                            if (transitions != 1)
                            {
                                continue;
                            }
                            const bool first = pass == 0 ? (p2 * p4 * p6 == 0 && p4 * p6 * p8 == 0)
                                                         : (p2 * p4 * p8 == 0 && p2 * p6 * p8 == 0);
                            if (first)
                            {
                                remove.emplace_back(x, y);
                            }
                        }
                    }
                    for (const cv::Point& point : remove)
                    {
                        padded.at<uchar>(point) = 0;
                    }
                    changed = changed || !remove.empty();
                }
            }
            return padded(cv::Rect(1, 1, mask.cols, mask.rows)).clone() * 255;
        }

        cv::Mat fillHolesOperation(const cv::Mat& mask)
        {
            cv::Mat padded;
            cv::copyMakeBorder(mask, padded, 1, 1, 1, 1, cv::BORDER_CONSTANT, cv::Scalar(0));
            cv::Mat flooded = padded.clone();
            cv::floodFill(flooded, cv::Point(0, 0), cv::Scalar(128));
            cv::Mat holes;
            cv::compare(flooded, 0, holes, cv::CMP_EQ);
            cv::Mat result;
            cv::bitwise_or(padded, holes, result);
            return result(cv::Rect(1, 1, mask.cols, mask.rows)).clone();
        }

        // Splits touching objects where the distance to the background has a neck. Tolerance is how much
        // lower than its peak that distance may fall between two objects before they are told apart.
        cv::Mat watershedOperation(const cv::Mat& mask, double tolerance)
        {
            cv::Mat distance;
            cv::distanceTransform(mask, distance, cv::DIST_L2, 3);
            const float h = static_cast<float>(std::max(0.01, tolerance));
            cv::Mat marker = cv::max(distance - h, 0.0);
            const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
            for (int iteration = 0; iteration < 5000; ++iteration)
            {
                cv::Mat grown;
                cv::dilate(marker, grown, kernel);
                grown = cv::min(grown, distance);
                if (cv::countNonZero(grown != marker) == 0)
                {
                    break;
                }
                marker = grown;
            }
            cv::Mat cores;
            cv::compare(distance - marker, h - 1.0e-3f, cores, cv::CMP_GE);
            cv::bitwise_and(cores, mask, cores);
            cv::Mat labels;
            cv::connectedComponents(cores, labels, 8, CV_32S);
            cv::Mat distance8;
            cv::normalize(distance, distance8, 0, 255, cv::NORM_MINMAX, CV_8U);
            cv::Mat inverted = 255 - distance8;
            cv::Mat color;
            cv::cvtColor(inverted, color, cv::COLOR_GRAY2BGR);
            cv::watershed(color, labels);
            cv::Mat result = mask.clone();
            result.setTo(0, labels == -1);
            return result;
        }

        struct Spec
        {
            const char* id;
            const char* name;
            QList<ProcessingParameterDescriptor> parameters;
            bool resettable;
        };

        QList<Spec> specs()
        {
            return {
                {"math", "Math",
                 {choiceParam("operation", "Operation", 0,
                              {"Add", "Subtract", "Multiply", "Divide", "Minimum", "Maximum", "Gamma",
                               "Log", "Exp", "Square", "Square root"}),
                  realParam("value", "Value", 25.0, -1000000.0, 1000000.0, 1.0, 3)},
                 false},
                {"invert", "Invert", {}, false},
                {"flip", "Flip",
                 {choiceParam("direction", "Direction", 0, {"Horizontally", "Vertically"})}, false},
                {"rotate_90", "Rotate 90 Degrees",
                 {choiceParam("direction", "Direction", 0, {"Right", "Left"})}, false},
                {"threshold", "Threshold",
                 {choiceParam("method", "Method", 1, {"Fixed", "Otsu", "IsoData", "Mean", "Triangle"}),
                  realParam("low", "Lower level", 0.0, 0.0, 65535.0, 1.0, 0),
                  realParam("high", "Upper level", 255.0, 0.0, 65535.0, 1.0, 0),
                  choiceParam("select", "Select", 0, {"Above level", "Below level"})},
                 false},
                {"sharpen", "Sharpen", {}, false},
                {"find_edges", "Find Edges", {}, false},
                {"filter", "Filter",
                 {choiceParam("kind", "Filter", 0, {"Mean", "Median", "Minimum", "Maximum", "Gaussian", "Variance"}),
                  realParam("radius", "Radius", 2.0, 0.5, 100.0, 0.5, 1)},
                 false},
                {"unsharp_mask", "Unsharp Mask",
                 {realParam("sigma", "Radius (sigma)", 1.0, 0.1, 100.0, 0.5, 1),
                  realParam("weight", "Mask weight", 0.6, 0.1, 0.9, 0.05, 2)},
                 false},
                {"top_hat", "Top Hat",
                 {realParam("radius", "Radius", 10.0, 0.5, 200.0, 0.5, 1),
                  boolParam("light_background", "Light background", false)},
                 false},
                {"binary", "Binary",
                 {choiceParam("operation", "Operation", 0, {"Erode", "Dilate", "Open", "Close", "Outline", "Invert", "Fill Holes", "Skeletonize"}),
                  integerParam("iterations", "Iterations", 1, 1, 100)},
                 false},
                {"clahe", "Local Contrast (CLAHE)",
                 {integerParam("block_size", "Block size (pixels)", 127, 8, 2000),
                  realParam("max_slope", "Maximum slope", 3.0, 1.0, 100.0, 0.5, 1)},
                 false},
                {"equalize", "Equalize Histogram",
                 {boolParam("sqrt_counts", "Use the square root of the counts", true)}, false},
                {"distance_map", "Distance Map", {}, false},
                {"watershed", "Watershed",
                 {realParam("tolerance", "Tolerance", 0.5, 0.0, 50.0, 0.1, 2)}, false},
                {"rolling_projection", "Rolling Z Projection",
                 {choiceParam("method", "Method", 0, {"Maximum", "Average", "Sum"}),
                  integerParam("window", "Frames", 8, 1, 256)},
                 true},
            };
        }
    } // namespace

    ImageOperationModule::ImageOperationModule(QString id, QString name, QVariantMap defaults)
        : m_id(std::move(id)), m_name(std::move(name)), m_params(std::move(defaults))
    {
    }

    std::unique_ptr<ProcessingModule> ImageOperationModule::createRuntime() const
    {
        return std::make_unique<ImageOperationModule>(m_id, m_name, m_params);
    }

    void ImageOperationModule::setParameters(const QVariantMap& params)
    {
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        {
            if (m_params.contains(it.key()))
            {
                m_params.insert(it.key(), it.value());
            }
        }
        if (m_id == QLatin1String("rolling_projection"))
        {
            resetState();
        }
    }

    bool ImageOperationModule::resetState()
    {
        m_window.clear();
        m_sum.release();
        return true;
    }

    ProcessingResult ImageOperationModule::rollingProjection(const ImageFrame& frame)
    {
        const cv::Mat src(frame.height, frame.width, frame.isMono16() ? CV_16UC1 : CV_8UC1,
                          const_cast<char*>(frame.bytes.constData()), frame.stride);
        if (src.cols != m_windowWidth || src.rows != m_windowHeight || src.type() != m_windowType)
        {
            resetState();
            m_windowWidth = src.cols;
            m_windowHeight = src.rows;
            m_windowType = src.type();
        }
        const int count = std::max(1, m_params.value("window").toInt());
        const int method = m_params.value("method").toInt();
        if (m_sum.empty())
        {
            m_sum = cv::Mat::zeros(src.rows, src.cols, CV_32SC1);
        }
        m_window.push_back(src.clone());
        cv::Mat widened;
        m_window.back().convertTo(widened, CV_32S);
        m_sum += widened;
        while (static_cast<int>(m_window.size()) > count)
        {
            m_window.front().convertTo(widened, CV_32S);
            m_sum -= widened;
            m_window.pop_front();
        }

        cv::Mat result;
        if (method == 0)
        {
            result = m_window.front().clone();
            for (size_t i = 1; i < m_window.size(); ++i)
            {
                cv::max(result, m_window[i], result);
            }
        }
        else if (method == 1)
        {
            cv::Mat mean;
            m_sum.convertTo(mean, CV_32F, 1.0 / static_cast<double>(m_window.size()));
            mean.convertTo(result, src.type());
        }
        else
        {
            m_sum.convertTo(result, src.type());
        }
        QByteArray bytes = copyMatBytes(result);
        return {makeFrameLike(frame, result.cols, result.rows, std::move(bytes)), {}};
    }

    ProcessingResult ImageOperationModule::process(const ImageFrame& frame, int processingBitDepth)
    {
        if (!frame.isValid())
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Invalid input"));
        }

        try
        {
            ImageFrame working;
            if (!convertFrameForProcessing(frame, working, processingBitDepth))
            {
                return ProcessingResult(ImageFrame{}, QStringLiteral("Unsupported input frame"));
            }
            if (m_id == QLatin1String("rolling_projection"))
            {
                return rollingProjection(working);
            }

            const int limit = limitOf(working);
            const cv::Mat src(working.height, working.width, working.isMono16() ? CV_16UC1 : CV_8UC1,
                              const_cast<char*>(working.bytes.constData()), working.stride);
            cv::Mat result;
            bool eightBitResult = false;

            if (m_id == QLatin1String("math"))
            {
                result = mathOperation(src, limit, m_params.value("operation").toInt(),
                                       m_params.value("value").toDouble());
            }
            else if (m_id == QLatin1String("invert"))
            {
                std::vector<float> table(static_cast<size_t>(limit) + 1);
                for (int v = 0; v <= limit; ++v)
                {
                    table[static_cast<size_t>(v)] = static_cast<float>(limit - v);
                }
                result = applyTable(src, table, limit);
            }
            else if (m_id == QLatin1String("flip"))
            {
                cv::flip(src, result, m_params.value("direction").toInt() == 0 ? 1 : 0);
            }
            else if (m_id == QLatin1String("rotate_90"))
            {
                cv::rotate(src, result, m_params.value("direction").toInt() == 0 ? cv::ROTATE_90_CLOCKWISE
                                                                                  : cv::ROTATE_90_COUNTERCLOCKWISE);
            }
            else if (m_id == QLatin1String("threshold"))
            {
                result = thresholdOperation(src, limit, m_params);
                eightBitResult = true;
            }
            else if (m_id == QLatin1String("sharpen"))
            {
                result = sharpenOperation(src);
            }
            else if (m_id == QLatin1String("find_edges"))
            {
                result = edgeOperation(src);
            }
            else if (m_id == QLatin1String("filter"))
            {
                result = filterOperation(src, m_params.value("kind").toInt(), m_params.value("radius").toDouble());
            }
            else if (m_id == QLatin1String("unsharp_mask"))
            {
                result = unsharpOperation(src, m_params.value("sigma").toDouble(), m_params.value("weight").toDouble());
            }
            else if (m_id == QLatin1String("top_hat"))
            {
                result = topHatOperation(src, m_params.value("radius").toDouble(),
                                         m_params.value("light_background").toBool());
            }
            else if (m_id == QLatin1String("clahe"))
            {
                result = claheOperation(src, m_params.value("block_size").toInt(), m_params.value("max_slope").toDouble());
            }
            else if (m_id == QLatin1String("equalize"))
            {
                result = equalizeOperation(src, limit, m_params.value("sqrt_counts").toBool());
            }
            else if (m_id == QLatin1String("distance_map"))
            {
                cv::Mat distance;
                cv::distanceTransform(objectMask(src), distance, cv::DIST_L2, 3);
                distance.convertTo(result, CV_16U);
                ImageFrame out = makeMono16Frame(working.cameraId, result.cols, result.rows, copyMatBytes(result), 16);
                copyFrameMetadata(working, out);
                return {out, {}};
            }
            else if (m_id == QLatin1String("watershed"))
            {
                result = watershedOperation(objectMask(src), m_params.value("tolerance").toDouble());
                eightBitResult = true;
            }
            else if (m_id == QLatin1String("binary"))
            {
                result = binaryOperation(src, m_params.value("operation").toInt(), m_params.value("iterations").toInt());
                eightBitResult = true;
            }
            else
            {
                return ProcessingResult(ImageFrame{}, QStringLiteral("Unknown operation"));
            }

            QByteArray bytes = copyMatBytes(result);
            if (bytes.isEmpty())
            {
                return ProcessingResult(ImageFrame{}, QStringLiteral("Failed to allocate output"));
            }
            if (eightBitResult)
            {
                ImageFrame out = makeMono8Frame(working.cameraId, result.cols, result.rows, std::move(bytes));
                copyFrameMetadata(working, out);
                return {out, {}};
            }
            return {makeFrameLike(working, result.cols, result.rows, std::move(bytes)), {}};
        }
        catch (const std::exception& e)
        {
            return ProcessingResult(ImageFrame{}, QString("%1 failed: %2").arg(m_name, e.what()));
        }
    }

    void registerImageOperationModules(ProcessingModuleRegistry& registry)
    {
        for (const Spec& spec : specs())
        {
            QVariantMap defaults;
            for (const ProcessingParameterDescriptor& parameter : spec.parameters)
            {
                defaults.insert(parameter.key, parameter.defaultValue);
            }
            const QString id = QString::fromLatin1(spec.id);
            const QString name = QString::fromLatin1(spec.name);
            registry.registerModule({id, name, 1, spec.parameters, spec.resettable},
                                    [id, name, defaults]()
                                    {
                                        return std::make_unique<ImageOperationModule>(id, name, defaults);
                                    });
        }
    }
} // namespace scopeone::core::internal
