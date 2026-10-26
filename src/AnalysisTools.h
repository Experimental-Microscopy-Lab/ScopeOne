#pragma once

#include "PreviewWidget.h"
#include "ToolIcons.h"

#include <array>

namespace scopeone::ui
{
    // The selection tools of the image, shared by the tool strip beside the viewer and the Analyze panel
    struct AnalysisToolSpec
    {
        ToolIconKind icon;
        PreviewWidget::AnalysisTool tool;
        const char* tip;
    };

    inline const std::array<AnalysisToolSpec, 8>& analysisToolSpecs()
    {
        static const std::array<AnalysisToolSpec, 8> specs{{
            {ToolIconKind::Select, PreviewWidget::AnalysisTool::Select,
             "Select: click a shape to select it, drag it to move it"},
            {ToolIconKind::Rectangle, PreviewWidget::AnalysisTool::Rectangle,
             "Rectangle: drag on the image"},
            {ToolIconKind::Ellipse, PreviewWidget::AnalysisTool::Ellipse,
             "Ellipse: drag on the image over the rectangle that holds it"},
            {ToolIconKind::Polygon, PreviewWidget::AnalysisTool::Polygon,
             "Polygon: click the corners. Double-click, Enter or a click on the first corner closes it. "
             "Backspace takes back a corner and Esc cancels."},
            {ToolIconKind::Freehand, PreviewWidget::AnalysisTool::Freehand,
             "Freehand: drag along the outline. It closes when the button is released."},
            {ToolIconKind::Line, PreviewWidget::AnalysisTool::Line,
             "Line: drag on the image to plot the profile along it"},
            {ToolIconKind::Point, PreviewWidget::AnalysisTool::Point,
             "Point: click to read one pixel"},
            {ToolIconKind::Wand, PreviewWidget::AnalysisTool::Wand,
             "Wand: click a region to select the connected pixels of a similar value. "
             "The tolerance is set in Analyze > Wand Tolerance."},
        }};
        return specs;
    }
}
