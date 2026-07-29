#include "AppVersion.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QStringList>
#include <QtEndian>

#include <cmath>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace
{
    constexpr quint32 kMaxMessageBytes = 64 * 1024 * 1024;
    constexpr int kConnectTimeoutMs = 3000;
    constexpr int kRequestTimeoutMs = 120000;
    const QString kMcpProtocolVersion = QStringLiteral("2025-06-18");
#if defined(_WIN32)
    const QString kServerName = QStringLiteral(R"(\\.\pipe\ScopeOne.Api.local)");
#else
    const QString kServerName = QStringLiteral("ScopeOne.Api.local");
#endif

    enum class ToolAccess
    {
        ReadOnly,
        StateChanging,
        Confirmed,
        Destructive
    };

    bool requiresConfirmation(ToolAccess access)
    {
        return access == ToolAccess::Confirmed || access == ToolAccess::Destructive;
    }

    struct ToolSpec
    {
        QString name;
        QString description;
        QJsonObject inputSchema;
        ToolAccess access{ToolAccess::ReadOnly};
    };

    QJsonObject inputProperty(const QString& type,
                              const QString& description,
                              const QJsonValue& defaultValue = QJsonValue(QJsonValue::Undefined))
    {
        QJsonObject property;
        property.insert(QStringLiteral("type"), type);
        property.insert(QStringLiteral("description"), description);
        if (!defaultValue.isUndefined())
        {
            property.insert(QStringLiteral("default"), defaultValue);
        }
        return property;
    }

    QJsonObject withMinimum(QJsonObject property, double minimum)
    {
        property.insert(QStringLiteral("minimum"), minimum);
        return property;
    }

    QJsonObject withMaximum(QJsonObject property, double maximum)
    {
        property.insert(QStringLiteral("maximum"), maximum);
        return property;
    }

    QJsonObject withEnum(QJsonObject property, std::initializer_list<QJsonValue> values)
    {
        QJsonArray allowedValues;
        for (const QJsonValue& value : values)
        {
            allowedValues.append(value);
        }
        property.insert(QStringLiteral("enum"), allowedValues);
        return property;
    }

    QJsonObject arrayProperty(const QString& description, const QString& itemType)
    {
        QJsonObject property = inputProperty(QStringLiteral("array"), description);
        QJsonObject items;
        items.insert(QStringLiteral("type"), itemType);
        property.insert(QStringLiteral("items"), items);
        return property;
    }

    using PropertyEntry = std::pair<QString, QJsonObject>;

    QJsonObject inputProperties(std::initializer_list<PropertyEntry> entries)
    {
        QJsonObject properties;
        for (const auto& [name, property] : entries)
        {
            properties.insert(name, property);
        }
        return properties;
    }

    ToolSpec makeTool(const QString& name,
                      const QString& description,
                      QJsonObject properties = {},
                      QStringList required = {},
                      ToolAccess access = ToolAccess::ReadOnly)
    {
        if (requiresConfirmation(access))
        {
            properties.insert(
                QStringLiteral("confirm"),
                inputProperty(
                    QStringLiteral("boolean"),
                    QStringLiteral("Set to true only after the user explicitly approves this operation")));
            required.append(QStringLiteral("confirm"));
        }

        QJsonObject schema;
        schema.insert(QStringLiteral("type"), QStringLiteral("object"));
        schema.insert(QStringLiteral("properties"), properties);
        schema.insert(QStringLiteral("additionalProperties"), false);
        if (!required.isEmpty())
        {
            schema.insert(QStringLiteral("required"), QJsonArray::fromStringList(required));
        }
        return ToolSpec{name, description, schema, access};
    }

    const std::vector<ToolSpec>& toolSpecs()
    {
        static const std::vector<ToolSpec> specs{
            makeTool(
                QStringLiteral("capabilities"),
                QStringLiteral(
                    "Discover the complete ScopeOne Local API operation catalog and safety classifications")),
            makeTool(
                QStringLiteral("state_snapshot"),
                QStringLiteral(
                    "Observe current configuration, hardware, preview, processing, scene, experiment, "
                    "and session state")),
            makeTool(
                QStringLiteral("ping"),
                QStringLiteral("Check that the ScopeOne Local API is responding")),
            makeTool(
                QStringLiteral("version"),
                QStringLiteral("Read ScopeOne application and Core versions")),
            makeTool(
                QStringLiteral("status"),
                QStringLiteral("Read a compact ScopeOne runtime status summary")),
            makeTool(
                QStringLiteral("load_config"),
                QStringLiteral("Load a Micro-Manager configuration and initialize its devices"),
                inputProperties({
                    {QStringLiteral("configPath"),
                     inputProperty(
                         QStringLiteral("string"),
                         QStringLiteral("Absolute path to a Micro-Manager cfg file"))}
                }),
                {QStringLiteral("configPath")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("unload_config"),
                QStringLiteral("Stop acquisition and unload the current device configuration"),
                {},
                {},
                ToolAccess::Destructive),
            makeTool(
                QStringLiteral("camera_ids"),
                QStringLiteral("List configured camera IDs")),
            makeTool(
                QStringLiteral("loaded_devices"),
                QStringLiteral("List loaded Micro-Manager devices")),
            makeTool(
                QStringLiteral("config_groups"),
                QStringLiteral("List available Micro-Manager configuration groups")),
            makeTool(
                QStringLiteral("configs"),
                QStringLiteral("List presets in a Micro-Manager configuration group"),
                inputProperties({
                    {QStringLiteral("group"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Configuration group name"))}
                }),
                {QStringLiteral("group")}),
            makeTool(
                QStringLiteral("current_config"),
                QStringLiteral("Read the active preset in a Micro-Manager configuration group"),
                inputProperties({
                    {QStringLiteral("group"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Configuration group name"))}
                }),
                {QStringLiteral("group")}),
            makeTool(
                QStringLiteral("set_config"),
                QStringLiteral("Apply a Micro-Manager configuration preset"),
                inputProperties({
                    {QStringLiteral("group"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Configuration group name"))},
                    {QStringLiteral("config"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Preset name"))}
                }),
                {QStringLiteral("group"), QStringLiteral("config")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("start_preview"),
                QStringLiteral("Start live camera preview"),
                inputProperties({
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID or All"), QStringLiteral("All"))}
                }),
                {},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("stop_preview"),
                QStringLiteral("Stop live camera preview"),
                inputProperties({
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID or All"), QStringLiteral("All"))}
                }),
                {},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("list_layers"),
                QStringLiteral("List image layers and their current display state")),
            makeTool(
                QStringLiteral("layer_options"),
                QStringLiteral("List supported layer layouts, colormaps, and blending modes")),
            makeTool(
                QStringLiteral("set_layer_layout"),
                QStringLiteral("Set the preview layer layout"),
                inputProperties({
                    {QStringLiteral("layout"),
                     withEnum(
                         inputProperty(QStringLiteral("string"), QStringLiteral("Layer layout")),
                         {QStringLiteral("side_by_side"), QStringLiteral("overlay")})}
                }),
                {QStringLiteral("layout")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("set_visible_layers"),
                QStringLiteral("Set the ordered list of visible image layers"),
                inputProperties({
                    {QStringLiteral("layerKeys"),
                     arrayProperty(QStringLiteral("Layer keys from list_layers"), QStringLiteral("string"))}
                }),
                {QStringLiteral("layerKeys")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("set_layer_display"),
                QStringLiteral("Update display settings for one image layer"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer key from list_layers"))},
                    {QStringLiteral("visible"),
                     inputProperty(QStringLiteral("boolean"), QStringLiteral("Layer visibility"))},
                    {QStringLiteral("opacityPercent"),
                     withMaximum(
                         withMinimum(
                             inputProperty(QStringLiteral("integer"), QStringLiteral("Opacity percentage")),
                             0.0),
                         100.0)},
                    {QStringLiteral("gamma"),
                     withMaximum(
                         withMinimum(
                             inputProperty(QStringLiteral("number"), QStringLiteral("Display gamma")),
                             0.2),
                         2.0)},
                    {QStringLiteral("colormap"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Colormap from layer_options"))},
                    {QStringLiteral("blending"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Blending mode from layer_options"))},
                    {QStringLiteral("minLevel"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Minimum display level")),
                                 0.0)},
                    {QStringLiteral("maxLevel"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Maximum display level")),
                                 0.0)},
                    {QStringLiteral("maxPossible"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Display level domain maximum")),
                                 1.0)}
                }),
                {QStringLiteral("layerKey")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("auto_layer_levels"),
                QStringLiteral("Set one image layer to histogram based automatic display levels"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer key from list_layers"))}
                }),
                {QStringLiteral("layerKey")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("full_layer_levels"),
                QStringLiteral("Set one image layer to its full intensity range"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer key from list_layers"))}
                }),
                {QStringLiteral("layerKey")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("set_layer_auto_stretch"),
                QStringLiteral("Enable or disable continuous histogram based display stretching"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer key from list_layers"))},
                    {QStringLiteral("enabled"),
                     inputProperty(QStringLiteral("boolean"), QStringLiteral("Continuous auto stretch state"))}
                }),
                {QStringLiteral("layerKey"), QStringLiteral("enabled")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("get_source_display_transform"),
                QStringLiteral("Read preview alignment for one image source"),
                inputProperties({
                    {QStringLiteral("sourceId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Source ID from list_layers"))}
                }),
                {QStringLiteral("sourceId")}),
            makeTool(
                QStringLiteral("set_source_display_transform"),
                QStringLiteral("Update preview alignment for one image source"),
                inputProperties({
                    {QStringLiteral("sourceId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Source ID from list_layers"))},
                    {QStringLiteral("offsetX"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Horizontal pixel offset"))},
                    {QStringLiteral("offsetY"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Vertical pixel offset"))},
                    {QStringLiteral("zoomPercent"),
                     withMaximum(
                         withMinimum(
                             inputProperty(QStringLiteral("integer"), QStringLiteral("Source zoom percentage")),
                             10.0),
                         500.0)},
                    {QStringLiteral("flipX"),
                     inputProperty(QStringLiteral("boolean"), QStringLiteral("Horizontal source flip"))},
                    {QStringLiteral("flipY"),
                     inputProperty(QStringLiteral("boolean"), QStringLiteral("Vertical source flip"))}
                }),
                {QStringLiteral("sourceId")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("reset_source_display_transform"),
                QStringLiteral("Reset preview alignment for one image source"),
                inputProperties({
                    {QStringLiteral("sourceId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Source ID from list_layers"))}
                }),
                {QStringLiteral("sourceId")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("move_layer"),
                QStringLiteral("Move an image layer in display order"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer key from list_layers"))},
                    {QStringLiteral("offset"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Relative order offset"))}
                }),
                {QStringLiteral("layerKey"), QStringLiteral("offset")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("remove_static_layer"),
                QStringLiteral("Remove one static image layer"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Static layer key"))}
                }),
                {QStringLiteral("layerKey")},
                ToolAccess::Destructive),
            makeTool(
                QStringLiteral("clear_static_layers"),
                QStringLiteral("Remove all static image layers"),
                {},
                {},
                ToolAccess::Destructive),
            makeTool(
                QStringLiteral("get_layer_histogram"),
                QStringLiteral("Compute histogram and summary statistics for a current image layer"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer key from state_snapshot"))}
                }),
                {QStringLiteral("layerKey")}),
            makeTool(
                QStringLiteral("get_pixel_value"),
                QStringLiteral("Read one image-space pixel value from a current layer"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer key from state_snapshot"))},
                    {QStringLiteral("x"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Image X coordinate"))},
                    {QStringLiteral("y"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Image Y coordinate"))}
                }),
                {QStringLiteral("layerKey"), QStringLiteral("x"), QStringLiteral("y")}),
            makeTool(
                QStringLiteral("get_line_profile"),
                QStringLiteral("Sample pixel values along an image-space line in a current layer"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer key from state_snapshot"))},
                    {QStringLiteral("x1"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Start X coordinate"))},
                    {QStringLiteral("y1"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Start Y coordinate"))},
                    {QStringLiteral("x2"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("End X coordinate"))},
                    {QStringLiteral("y2"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("End Y coordinate"))}
                }),
                {QStringLiteral("layerKey"), QStringLiteral("x1"), QStringLiteral("y1"),
                 QStringLiteral("x2"), QStringLiteral("y2")}),
            makeTool(
                QStringLiteral("detect_particles"),
                QStringLiteral("Measure thresholded particles and optionally export or display their mask"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer key from state_snapshot"))},
                    {QStringLiteral("threshold"),
                     withMinimum(
                         inputProperty(QStringLiteral("integer"), QStringLiteral("Inclusive intensity threshold")),
                         0.0)},
                    {QStringLiteral("minArea"),
                     withMinimum(
                         inputProperty(QStringLiteral("integer"), QStringLiteral("Minimum particle area in pixels")),
                         1.0)},
                    {QStringLiteral("maxArea"),
                     withMinimum(
                         inputProperty(QStringLiteral("integer"), QStringLiteral("Maximum particle area in pixels")),
                         1.0)},
                    {QStringLiteral("maxParticles"),
                     withMaximum(
                         withMinimum(
                             inputProperty(QStringLiteral("integer"),
                                           QStringLiteral("Maximum returned particle count"),
                                           1000),
                             1.0),
                         10000.0)},
                    {QStringLiteral("exportMask"),
                     inputProperty(QStringLiteral("boolean"),
                                   QStringLiteral("Export the particle mask to shared memory"), false)},
                    {QStringLiteral("publishMask"),
                     inputProperty(QStringLiteral("boolean"),
                                   QStringLiteral("Publish the particle mask as a preview layer"), false)}
                }),
                {QStringLiteral("layerKey"), QStringLiteral("threshold"),
                 QStringLiteral("minArea"), QStringLiteral("maxArea")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("create_line_markup"),
                QStringLiteral("Create an image-space line markup"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Target layer key"))},
                    {QStringLiteral("x1"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Start X coordinate"))},
                    {QStringLiteral("y1"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Start Y coordinate"))},
                    {QStringLiteral("x2"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("End X coordinate"))},
                    {QStringLiteral("y2"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("End Y coordinate"))},
                    {QStringLiteral("label"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Markup label"), QString())},
                    {QStringLiteral("role"),
                     withEnum(
                         inputProperty(QStringLiteral("string"), QStringLiteral("Line markup role"),
                                       QStringLiteral("generic")),
                         {QStringLiteral("generic"), QStringLiteral("cross_section"),
                          QStringLiteral("measurement")})}
                }),
                {QStringLiteral("layerKey"), QStringLiteral("x1"), QStringLiteral("y1"),
                 QStringLiteral("x2"), QStringLiteral("y2")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("create_rect_markup"),
                QStringLiteral("Create an image-space rectangle markup"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Target layer key"))},
                    {QStringLiteral("x"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Left coordinate"))},
                    {QStringLiteral("y"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Top coordinate"))},
                    {QStringLiteral("width"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Rectangle width")), 1.0)},
                    {QStringLiteral("height"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Rectangle height")), 1.0)},
                    {QStringLiteral("label"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Markup label"), QString())},
                    {QStringLiteral("role"),
                     withEnum(
                         inputProperty(QStringLiteral("string"), QStringLiteral("Rectangle markup role"),
                                       QStringLiteral("generic")),
                         {QStringLiteral("generic"), QStringLiteral("roi"),
                          QStringLiteral("measurement")})}
                }),
                {QStringLiteral("layerKey"), QStringLiteral("x"), QStringLiteral("y"),
                 QStringLiteral("width"), QStringLiteral("height")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("list_markups"),
                QStringLiteral("List image markups, optionally filtered by layer"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Optional layer key"))}
                })),
            makeTool(
                QStringLiteral("update_markup"),
                QStringLiteral("Update markup label, visibility, selection, or geometry"),
                inputProperties({
                    {QStringLiteral("markupId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Markup ID"))},
                    {QStringLiteral("label"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Markup label"))},
                    {QStringLiteral("visible"),
                     inputProperty(QStringLiteral("boolean"), QStringLiteral("Markup visibility"))},
                    {QStringLiteral("selected"),
                     inputProperty(QStringLiteral("boolean"), QStringLiteral("Markup selection state"))},
                    {QStringLiteral("x1"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Line start X coordinate"))},
                    {QStringLiteral("y1"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Line start Y coordinate"))},
                    {QStringLiteral("x2"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Line end X coordinate"))},
                    {QStringLiteral("y2"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Line end Y coordinate"))},
                    {QStringLiteral("x"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Rectangle left coordinate"))},
                    {QStringLiteral("y"),
                     inputProperty(QStringLiteral("integer"), QStringLiteral("Rectangle top coordinate"))},
                    {QStringLiteral("width"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Rectangle width")), 1.0)},
                    {QStringLiteral("height"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Rectangle height")), 1.0)}
                }),
                {QStringLiteral("markupId")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("remove_markup"),
                QStringLiteral("Remove one image markup"),
                inputProperties({
                    {QStringLiteral("markupId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Markup ID"))}
                }),
                {QStringLiteral("markupId")},
                ToolAccess::Destructive),
            makeTool(
                QStringLiteral("clear_markups"),
                QStringLiteral("Remove all markups, optionally restricted to one layer"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Optional layer key"))}
                }),
                {},
                ToolAccess::Destructive),
            makeTool(
                QStringLiteral("device_properties"),
                QStringLiteral("Read device property metadata and values"),
                inputProperties({
                    {QStringLiteral("device"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Device label"))},
                    {QStringLiteral("fromCache"),
                     inputProperty(
                         QStringLiteral("boolean"),
                         QStringLiteral("Use cached values instead of reading hardware"),
                         false)}
                }),
                {QStringLiteral("device")}),
            makeTool(
                QStringLiteral("device_property_names"),
                QStringLiteral("List property names for one loaded device"),
                inputProperties({
                    {QStringLiteral("device"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Device label"))}
                }),
                {QStringLiteral("device")}),
            makeTool(
                QStringLiteral("get_property"),
                QStringLiteral("Read one device property"),
                inputProperties({
                    {QStringLiteral("device"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Device label"))},
                    {QStringLiteral("property"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Property name"))},
                    {QStringLiteral("fromCache"),
                     inputProperty(
                         QStringLiteral("boolean"),
                         QStringLiteral("Use the cached value instead of reading hardware"),
                         false)}
                }),
                {QStringLiteral("device"), QStringLiteral("property")}),
            makeTool(
                QStringLiteral("set_property"),
                QStringLiteral("Set one device property"),
                inputProperties({
                    {QStringLiteral("device"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Device label"))},
                    {QStringLiteral("property"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Property name"))},
                    {QStringLiteral("value"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Micro-Manager property value"))}
                }),
                {QStringLiteral("device"), QStringLiteral("property"), QStringLiteral("value")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("read_exposure"),
                QStringLiteral("Read camera exposure in milliseconds"),
                inputProperties({
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID or All"), QStringLiteral("All"))}
                })),
            makeTool(
                QStringLiteral("set_exposure"),
                QStringLiteral("Set camera exposure in milliseconds and return the applied value"),
                inputProperties({
                    {QStringLiteral("camera"),
                     inputProperty(
                         QStringLiteral("string"),
                         QStringLiteral("Camera ID or All"),
                         QStringLiteral("All"))},
                    {QStringLiteral("exposureMs"),
                     withMinimum(
                         inputProperty(QStringLiteral("number"),
                                       QStringLiteral("Exposure in milliseconds")),
                         0.0)}
                }),
                {QStringLiteral("exposureMs")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("get_roi"),
                QStringLiteral("Read the current camera ROI"),
                inputProperties({
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID"))}
                }),
                {QStringLiteral("camera")}),
            makeTool(
                QStringLiteral("set_roi"),
                QStringLiteral("Set a camera ROI and return the applied rectangle"),
                inputProperties({
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID"))},
                    {QStringLiteral("x"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Left coordinate")), 0.0)},
                    {QStringLiteral("y"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Top coordinate")), 0.0)},
                    {QStringLiteral("width"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("ROI width")), 1.0)},
                    {QStringLiteral("height"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("ROI height")), 1.0)}
                }),
                {QStringLiteral("camera"), QStringLiteral("x"), QStringLiteral("y"),
                 QStringLiteral("width"), QStringLiteral("height")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("set_half_roi"),
                QStringLiteral("Set a centered ROI with half the current camera width and height"),
                inputProperties({
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID"))}
                }),
                {QStringLiteral("camera")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("clear_roi"),
                QStringLiteral("Restore one camera or all cameras to their full sensor area"),
                inputProperties({
                    {QStringLiteral("camera"),
                     inputProperty(
                         QStringLiteral("string"),
                         QStringLiteral("Camera ID or All"),
                         QStringLiteral("All"))}
                }),
                {},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("xy_stage_devices"),
                QStringLiteral("List loaded XY stage devices")),
            makeTool(
                QStringLiteral("z_stage_devices"),
                QStringLiteral("List loaded focus or Z stage devices")),
            makeTool(
                QStringLiteral("current_xy_stage_device"),
                QStringLiteral("Read the current XY stage device label")),
            makeTool(
                QStringLiteral("current_focus_device"),
                QStringLiteral("Read the current focus device label")),
            makeTool(
                QStringLiteral("read_xy_position"),
                QStringLiteral("Read the current XY stage position"),
                inputProperties({
                    {QStringLiteral("device"),
                     inputProperty(
                         QStringLiteral("string"),
                         QStringLiteral("XY stage label or empty for the current stage"),
                         QString())}
                })),
            makeTool(
                QStringLiteral("read_z_position"),
                QStringLiteral("Read the current focus position"),
                inputProperties({
                    {QStringLiteral("device"),
                     inputProperty(
                         QStringLiteral("string"),
                         QStringLiteral("Focus device label or empty for the current device"),
                         QString())}
                })),
            makeTool(
                QStringLiteral("move_xy_relative"),
                QStringLiteral("Move an XY stage by a relative offset"),
                inputProperties({
                    {QStringLiteral("device"),
                     inputProperty(
                         QStringLiteral("string"),
                         QStringLiteral("XY stage label or empty for the current stage"),
                         QString())},
                    {QStringLiteral("dx"),
                     inputProperty(QStringLiteral("number"), QStringLiteral("Relative X distance in device units"))},
                    {QStringLiteral("dy"),
                     inputProperty(QStringLiteral("number"), QStringLiteral("Relative Y distance in device units"))}
                }),
                {QStringLiteral("dx"), QStringLiteral("dy")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("move_z_relative"),
                QStringLiteral("Move a focus device by a relative offset"),
                inputProperties({
                    {QStringLiteral("device"),
                     inputProperty(
                         QStringLiteral("string"),
                         QStringLiteral("Focus device label or empty for the current device"),
                         QString())},
                    {QStringLiteral("dz"),
                     inputProperty(QStringLiteral("number"), QStringLiteral("Relative Z distance in device units"))}
                }),
                {QStringLiteral("dz")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("move_xy_to"),
                QStringLiteral("Move an XY stage to an absolute position"),
                inputProperties({
                    {QStringLiteral("device"),
                     inputProperty(
                         QStringLiteral("string"),
                         QStringLiteral("XY stage label or empty for the current stage"),
                         QString())},
                    {QStringLiteral("x"),
                     inputProperty(QStringLiteral("number"), QStringLiteral("Absolute X position in device units"))},
                    {QStringLiteral("y"),
                     inputProperty(QStringLiteral("number"), QStringLiteral("Absolute Y position in device units"))}
                }),
                {QStringLiteral("x"), QStringLiteral("y")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("move_z_to"),
                QStringLiteral("Move a focus device to an absolute position"),
                inputProperties({
                    {QStringLiteral("device"),
                     inputProperty(
                         QStringLiteral("string"),
                         QStringLiteral("Focus device label or empty for the current device"),
                         QString())},
                    {QStringLiteral("z"),
                     inputProperty(QStringLiteral("number"), QStringLiteral("Absolute Z position in device units"))}
                }),
                {QStringLiteral("z")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("start_stage_mosaic"),
                QStringLiteral("Start asynchronous XY stage mosaic acquisition"),
                inputProperties({
                    {QStringLiteral("cameraId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID"))},
                    {QStringLiteral("xyStageId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("XY stage device label"))},
                    {QStringLiteral("rows"),
                     withMaximum(
                         withMinimum(
                             inputProperty(QStringLiteral("integer"), QStringLiteral("Mosaic row count"), 1),
                             1.0),
                         10000.0)},
                    {QStringLiteral("columns"),
                     withMaximum(
                         withMinimum(
                             inputProperty(QStringLiteral("integer"), QStringLiteral("Mosaic column count"), 1),
                             1.0),
                         10000.0)},
                    {QStringLiteral("pixelSizeUm"),
                     withMinimum(
                         inputProperty(QStringLiteral("number"), QStringLiteral("Image pixel size in micrometers"),
                                       1.0),
                         1e-12)},
                    {QStringLiteral("stepXUm"),
                     inputProperty(QStringLiteral("number"), QStringLiteral("Horizontal tile step in micrometers"),
                                   0.0)},
                    {QStringLiteral("stepYUm"),
                     inputProperty(QStringLiteral("number"), QStringLiteral("Vertical tile step in micrometers"),
                                   0.0)},
                    {QStringLiteral("settleMs"),
                     withMinimum(
                         inputProperty(QStringLiteral("integer"), QStringLiteral("Stage settle time in milliseconds"),
                                       150),
                         0.0)},
                    {QStringLiteral("returnToStart"),
                     inputProperty(QStringLiteral("boolean"), QStringLiteral("Return the stage to its initial position"),
                                   true)},
                    {QStringLiteral("gallerySaveDir"),
                     inputProperty(QStringLiteral("string"),
                                   QStringLiteral("Default directory for saving the resulting Gallery session"))}
                }),
                {QStringLiteral("cameraId"), QStringLiteral("xyStageId")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("stage_mosaic_status"),
                QStringLiteral("Read current or last XY stage mosaic status")),
            makeTool(
                QStringLiteral("cancel_stage_mosaic"),
                QStringLiteral("Cancel the running XY stage mosaic"),
                {},
                {},
                ToolAccess::Destructive),
            makeTool(
                QStringLiteral("processing_modules"),
                QStringLiteral("Read processing bit depth, real-time state, and pipeline modules")),
            makeTool(
                QStringLiteral("set_processing_bit_depth"),
                QStringLiteral("Set the processing pipeline bit depth"),
                inputProperties({
                    {QStringLiteral("bitDepth"),
                     withEnum(
                         inputProperty(QStringLiteral("integer"), QStringLiteral("Processing bit depth")),
                         {QJsonValue(8), QJsonValue(16)})}
                }),
                {QStringLiteral("bitDepth")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("set_realtime_processing"),
                QStringLiteral("Enable or disable real-time image processing"),
                inputProperties({
                    {QStringLiteral("enabled"),
                     inputProperty(QStringLiteral("boolean"), QStringLiteral("Real-time processing state"))}
                }),
                {QStringLiteral("enabled")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("add_processing_module"),
                QStringLiteral("Append a module to the image-processing pipeline"),
                inputProperties({
                    {QStringLiteral("kind"),
                     withEnum(
                         inputProperty(QStringLiteral("string"), QStringLiteral("Processing module kind")),
                         {QStringLiteral("fft"), QStringLiteral("background_calibration"),
                          QStringLiteral("spatiotemporal_binning"), QStringLiteral("gaussian_blur"),
                          QStringLiteral("differential_rolling")})},
                    {QStringLiteral("parameters"),
                     inputProperty(QStringLiteral("object"), QStringLiteral("Initial module parameters"))}
                }),
                {QStringLiteral("kind")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("remove_processing_module"),
                QStringLiteral("Remove one module from the image-processing pipeline"),
                inputProperties({
                    {QStringLiteral("index"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Module index")), 0.0)}
                }),
                {QStringLiteral("index")},
                ToolAccess::Destructive),
            makeTool(
                QStringLiteral("set_processing_module_parameters"),
                QStringLiteral("Replace parameters for one processing module"),
                inputProperties({
                    {QStringLiteral("index"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Module index")), 0.0)},
                    {QStringLiteral("parameters"),
                     inputProperty(QStringLiteral("object"), QStringLiteral("Module parameters"))}
                }),
                {QStringLiteral("index"), QStringLiteral("parameters")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("reset_processing_module_state"),
                QStringLiteral("Reset accumulated runtime state for one processing module"),
                inputProperties({
                    {QStringLiteral("index"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Module index")), 0.0)}
                }),
                {QStringLiteral("index")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("experiment_document"),
                QStringLiteral("Read the current experiment document")),
            makeTool(
                QStringLiteral("validate_experiment"),
                QStringLiteral("Validate and normalize an experiment document without starting acquisition"),
                inputProperties({
                    {QStringLiteral("document"),
                     inputProperty(QStringLiteral("object"), QStringLiteral("Experiment document"))}
                }),
                {QStringLiteral("document")}),
            makeTool(
                QStringLiteral("save_experiment"),
                QStringLiteral("Validate and save an experiment document to disk"),
                inputProperties({
                    {QStringLiteral("filePath"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Destination experiment file path"))},
                    {QStringLiteral("document"),
                     inputProperty(QStringLiteral("object"), QStringLiteral("Experiment document"))}
                }),
                {QStringLiteral("filePath"), QStringLiteral("document")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("load_experiment"),
                QStringLiteral("Load an experiment document from disk and replace the current document"),
                inputProperties({
                    {QStringLiteral("filePath"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Experiment file path"))}
                }),
                {QStringLiteral("filePath")},
                ToolAccess::Destructive),
            makeTool(
                QStringLiteral("start_experiment"),
                QStringLiteral("Start a validated Draft experiment document"),
                inputProperties({
                    {QStringLiteral("document"),
                     inputProperty(QStringLiteral("object"), QStringLiteral("Experiment document"))}
                }),
                {QStringLiteral("document")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("experiment_status"),
                QStringLiteral("Read experiment execution status and output manifest"),
                inputProperties({
                    {QStringLiteral("experimentId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Experiment ID"))}
                }),
                {QStringLiteral("experimentId")}),
            makeTool(
                QStringLiteral("cancel_experiment"),
                QStringLiteral("Request cancellation of a running experiment"),
                inputProperties({
                    {QStringLiteral("experimentId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Experiment ID"))}
                }),
                {QStringLiteral("experimentId")},
                ToolAccess::Destructive),
            makeTool(
                QStringLiteral("record"),
                QStringLiteral("Run a blocking in-memory recording and retain the resulting session"),
                inputProperties({
                    {QStringLiteral("frames"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Frames per camera")), 1.0)},
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID or All"),
                                   QStringLiteral("All"))},
                    {QStringLiteral("timeoutMs"),
                     withMinimum(
                         inputProperty(QStringLiteral("integer"), QStringLiteral("Recording timeout in milliseconds"),
                                       120000),
                         1.0)},
                    {QStringLiteral("mdaIntervalMs"),
                     withMinimum(
                         inputProperty(QStringLiteral("number"), QStringLiteral("MDA time interval in milliseconds"),
                                       0.0),
                         0.0)},
                    {QStringLiteral("pixelSizeUm"),
                     withMinimum(
                         inputProperty(QStringLiteral("number"),
                                       QStringLiteral("Sample pixel size in micrometers or zero when unknown"),
                                       0.0),
                         0.0)},
                    {QStringLiteral("zPositions"),
                     arrayProperty(QStringLiteral("Optional absolute Z positions"), QStringLiteral("number"))},
                    {QStringLiteral("positions"),
                     inputProperty(QStringLiteral("array"), QStringLiteral("Optional XY position pairs"))},
                    {QStringLiteral("order"),
                     arrayProperty(QStringLiteral("Optional acquisition axis order using time, z, and xy"),
                                   QStringLiteral("string"))}
                }),
                {QStringLiteral("frames")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("session_info"),
                QStringLiteral("Read camera and frame counts for a retained recording session"),
                inputProperties({
                    {QStringLiteral("sessionId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Recording session ID"))}
                }),
                {QStringLiteral("sessionId")}),
            makeTool(
                QStringLiteral("session_close"),
                QStringLiteral("Close and remove a retained recording session"),
                inputProperties({
                    {QStringLiteral("sessionId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Recording session ID"))}
                }),
                {QStringLiteral("sessionId")},
                ToolAccess::Destructive),
            makeTool(
                QStringLiteral("session_frame"),
                QStringLiteral("Export one retained recording frame to the shared frame mapping"),
                inputProperties({
                    {QStringLiteral("sessionId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Recording session ID"))},
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID"))},
                    {QStringLiteral("index"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Frame index")), 0.0)}
                }),
                {QStringLiteral("sessionId"), QStringLiteral("camera"), QStringLiteral("index")}),
            makeTool(
                QStringLiteral("latest_raw_frame"),
                QStringLiteral("Export the latest raw camera frame to the shared frame mapping"),
                inputProperties({
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID"))}
                }),
                {QStringLiteral("camera")}),
            makeTool(
                QStringLiteral("layer_frame"),
                QStringLiteral("Export the current frame of any image layer to shared memory"),
                inputProperties({
                    {QStringLiteral("layerKey"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer key from state_snapshot"))}
                }),
                {QStringLiteral("layerKey")}),
            makeTool(
                QStringLiteral("session_process_frame"),
                QStringLiteral("Process one retained session frame and export it to shared memory"),
                inputProperties({
                    {QStringLiteral("sessionId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Recording session ID"))},
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Camera ID"))},
                    {QStringLiteral("index"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Frame index")), 0.0)},
                    {QStringLiteral("startModuleIndex"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("First module to run")), 0.0)},
                    {QStringLiteral("endModuleIndex"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Last module to run")), 0.0)}
                }),
                {QStringLiteral("sessionId"), QStringLiteral("camera"), QStringLiteral("index")},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("frame_mapping_info"),
                QStringLiteral("Read shared frame mapping name, size, header, and pixel format information")),
            makeTool(
                QStringLiteral("process_frame_mapping"),
                QStringLiteral("Process the frame currently stored in shared memory"),
                inputProperties({
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Optional frame camera ID"))},
                    {QStringLiteral("startModuleIndex"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("First module to run")), 0.0)},
                    {QStringLiteral("endModuleIndex"),
                     withMinimum(inputProperty(QStringLiteral("integer"), QStringLiteral("Last module to run")), 0.0)}
                }),
                {},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("show_frame_mapping_as_layer"),
                QStringLiteral("Publish the frame in shared memory as a static preview layer"),
                inputProperties({
                    {QStringLiteral("layerId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Static layer source ID"),
                                   QStringLiteral("agent_result"))},
                    {QStringLiteral("name"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Layer display name"),
                                   QStringLiteral("Agent Result"))},
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Optional frame camera ID"))}
                }),
                {},
                ToolAccess::StateChanging),
            makeTool(
                QStringLiteral("save_frame_mapping"),
                QStringLiteral("Save the frame in shared memory to disk"),
                inputProperties({
                    {QStringLiteral("saveDir"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Destination directory"))},
                    {QStringLiteral("baseName"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Output base name"))},
                    {QStringLiteral("format"),
                     withEnum(
                         inputProperty(QStringLiteral("string"), QStringLiteral("Output format"),
                                       QStringLiteral("ome-tiff")),
                          {QStringLiteral("ome-tiff"), QStringLiteral("ome-zarr"), QStringLiteral("tiff"), QStringLiteral("binary")})},
                    {QStringLiteral("compression"),
                      inputProperty(QStringLiteral("boolean"), QStringLiteral("Enable output compression"), false)},
                    {QStringLiteral("compressionLevel"),
                     withMaximum(
                         withMinimum(
                             inputProperty(QStringLiteral("integer"), QStringLiteral("Compression level"), 6),
                             0.0),
                         9.0)},
                    {QStringLiteral("camera"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Optional frame camera ID"))}
                }),
                {QStringLiteral("saveDir"), QStringLiteral("baseName")},
                ToolAccess::Confirmed),
            makeTool(
                QStringLiteral("session_save"),
                QStringLiteral("Save a retained recording session to disk"),
                inputProperties({
                    {QStringLiteral("sessionId"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Recording session ID"))},
                    {QStringLiteral("saveDir"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Destination directory"))},
                    {QStringLiteral("baseName"),
                     inputProperty(QStringLiteral("string"), QStringLiteral("Output base name"))},
                    {QStringLiteral("format"),
                     withEnum(
                         inputProperty(QStringLiteral("string"), QStringLiteral("Output format"),
                                       QStringLiteral("ome-tiff")),
                          {QStringLiteral("ome-tiff"), QStringLiteral("ome-zarr"), QStringLiteral("tiff"), QStringLiteral("binary")})},
                    {QStringLiteral("compression"),
                      inputProperty(QStringLiteral("boolean"), QStringLiteral("Enable output compression"), false)},
                    {QStringLiteral("compressionLevel"),
                     withMaximum(
                         withMinimum(
                             inputProperty(QStringLiteral("integer"), QStringLiteral("Compression level"), 6),
                             0.0),
                         9.0)}
                }),
                {QStringLiteral("sessionId"), QStringLiteral("saveDir"), QStringLiteral("baseName")},
                ToolAccess::Confirmed)
        };
        return specs;
    }

    const ToolSpec* findTool(const QString& name)
    {
        for (const ToolSpec& spec : toolSpecs())
        {
            if (spec.name == name)
            {
                return &spec;
            }
        }
        return nullptr;
    }

    QJsonObject toolToJson(const ToolSpec& spec)
    {
        const bool confirmationRequired = requiresConfirmation(spec.access);
        QJsonObject annotations;
        annotations.insert(QStringLiteral("readOnlyHint"), spec.access == ToolAccess::ReadOnly);
        annotations.insert(QStringLiteral("destructiveHint"), spec.access == ToolAccess::Destructive);
        annotations.insert(QStringLiteral("openWorldHint"), confirmationRequired);

        QJsonObject tool;
        tool.insert(QStringLiteral("name"), spec.name);
        tool.insert(
            QStringLiteral("description"),
            confirmationRequired
                ? spec.description
                    + QStringLiteral(". Requires confirm=true after explicit user approval")
                : spec.description);
        tool.insert(QStringLiteral("inputSchema"), spec.inputSchema);
        tool.insert(QStringLiteral("annotations"), annotations);
        return tool;
    }

    bool valueMatchesType(const QJsonValue& value, const QString& type)
    {
        if (type == QStringLiteral("string"))
        {
            return value.isString();
        }
        if (type == QStringLiteral("boolean"))
        {
            return value.isBool();
        }
        if (type == QStringLiteral("number"))
        {
            return value.isDouble() && std::isfinite(value.toDouble());
        }
        if (type == QStringLiteral("integer"))
        {
            return value.isDouble() && std::isfinite(value.toDouble())
                && std::trunc(value.toDouble()) == value.toDouble();
        }
        if (type == QStringLiteral("object"))
        {
            return value.isObject();
        }
        if (type == QStringLiteral("array"))
        {
            return value.isArray();
        }
        return false;
    }

    bool prepareArguments(const ToolSpec& spec, QJsonObject& arguments, QString& error)
    {
        const QJsonObject properties = spec.inputSchema.value(QStringLiteral("properties")).toObject();
        for (auto it = arguments.constBegin(); it != arguments.constEnd(); ++it)
        {
            if (!properties.contains(it.key()))
            {
                error = QStringLiteral("Unsupported argument: %1").arg(it.key());
                return false;
            }
        }

        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it)
        {
            const QJsonObject property = it.value().toObject();
            if (!arguments.contains(it.key()) && property.contains(QStringLiteral("default")))
            {
                arguments.insert(it.key(), property.value(QStringLiteral("default")));
            }
        }

        const QJsonArray required = spec.inputSchema.value(QStringLiteral("required")).toArray();
        for (const QJsonValue& requiredValue : required)
        {
            const QString name = requiredValue.toString();
            if (!arguments.contains(name) || arguments.value(name).isNull())
            {
                error = QStringLiteral("Missing required argument: %1").arg(name);
                return false;
            }
        }

        for (auto it = arguments.constBegin(); it != arguments.constEnd(); ++it)
        {
            const QJsonObject property = properties.value(it.key()).toObject();
            const QString type = property.value(QStringLiteral("type")).toString();
            if (!valueMatchesType(it.value(), type))
            {
                error = QStringLiteral("Argument %1 must be %2").arg(it.key(), type);
                return false;
            }
            if (property.contains(QStringLiteral("minimum"))
                && it.value().toDouble() < property.value(QStringLiteral("minimum")).toDouble())
            {
                error = QStringLiteral("Argument %1 is below its minimum").arg(it.key());
                return false;
            }
            if (property.contains(QStringLiteral("maximum"))
                && it.value().toDouble() > property.value(QStringLiteral("maximum")).toDouble())
            {
                error = QStringLiteral("Argument %1 is above its maximum").arg(it.key());
                return false;
            }
            const QJsonArray allowedValues = property.value(QStringLiteral("enum")).toArray();
            if (!allowedValues.isEmpty() && !allowedValues.contains(it.value()))
            {
                error = QStringLiteral("Argument %1 has an unsupported value").arg(it.key());
                return false;
            }
            const QString itemType = property.value(QStringLiteral("items"))
                                         .toObject()
                                         .value(QStringLiteral("type"))
                                         .toString();
            if (!itemType.isEmpty())
            {
                const QJsonArray values = it.value().toArray();
                for (qsizetype index = 0; index < values.size(); ++index)
                {
                    if (!valueMatchesType(values.at(index), itemType))
                    {
                        error = QStringLiteral("Argument %1[%2] must be %3")
                                    .arg(it.key())
                                    .arg(index)
                                    .arg(itemType);
                        return false;
                    }
                }
            }
        }

        if (requiresConfirmation(spec.access)
            && !arguments.value(QStringLiteral("confirm")).toBool(false))
        {
            error = QStringLiteral("Explicit user approval is required; set confirm=true only after approval");
            return false;
        }
        arguments.remove(QStringLiteral("confirm"));
        return true;
    }

    class LocalApiClient
    {
    public:
        bool request(QJsonObject request, QJsonObject& response, QString& error)
        {
            if (!ensureConnected(error))
            {
                return false;
            }

            const double requestId = static_cast<double>(m_nextRequestId++);
            request.insert(QStringLiteral("requestId"), requestId);
            const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
            if (payload.isEmpty() || payload.size() > static_cast<qsizetype>(kMaxMessageBytes))
            {
                error = QStringLiteral("Local API request is invalid or too large");
                return false;
            }

            QByteArray message;
            message.resize(static_cast<int>(sizeof(quint32)));
            qToLittleEndian<quint32>(static_cast<quint32>(payload.size()),
                                     reinterpret_cast<uchar*>(message.data()));
            message += payload;

            qint64 offset = 0;
            while (offset < message.size())
            {
                const qint64 written = m_socket.write(message.constData() + offset,
                                                      message.size() - offset);
                if (written <= 0)
                {
                    error = QStringLiteral("Failed to write Local API request: %1")
                                .arg(m_socket.errorString());
                    m_socket.abort();
                    return false;
                }
                offset += written;
                while (m_socket.bytesToWrite() > 0)
                {
                    if (!m_socket.waitForBytesWritten(kRequestTimeoutMs))
                    {
                        error = QStringLiteral("Timed out writing Local API request: %1")
                                    .arg(m_socket.errorString());
                        m_socket.abort();
                        return false;
                    }
                }
            }

            QByteArray header;
            if (!readExact(static_cast<qsizetype>(sizeof(quint32)), header, error))
            {
                return false;
            }
            const quint32 payloadSize = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar*>(header.constData()));
            if (payloadSize == 0 || payloadSize > kMaxMessageBytes)
            {
                error = QStringLiteral("Local API response has an invalid size");
                m_socket.abort();
                return false;
            }

            QByteArray responsePayload;
            if (!readExact(static_cast<qsizetype>(payloadSize), responsePayload, error))
            {
                return false;
            }
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(responsePayload, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
            {
                error = QStringLiteral("Local API returned invalid JSON: %1").arg(parseError.errorString());
                m_socket.abort();
                return false;
            }

            response = document.object();
            if (!response.value(QStringLiteral("requestId")).isDouble()
                || response.value(QStringLiteral("requestId")).toDouble() != requestId)
            {
                error = QStringLiteral("Local API response requestId mismatch");
                m_socket.abort();
                return false;
            }
            return true;
        }

    private:
        bool ensureConnected(QString& error)
        {
            if (m_socket.state() == QLocalSocket::ConnectedState)
            {
                return true;
            }
            m_socket.abort();
            m_socket.connectToServer(kServerName, QIODevice::ReadWrite);
            if (!m_socket.waitForConnected(kConnectTimeoutMs))
            {
                error = QStringLiteral("Cannot connect to ScopeOne. Start ScopeOne first: %1")
                            .arg(m_socket.errorString());
                return false;
            }
            return true;
        }

        bool readExact(qsizetype size, QByteArray& data, QString& error)
        {
            data.clear();
            data.reserve(size);
            QElapsedTimer timer;
            timer.start();
            while (data.size() < size)
            {
                if (m_socket.bytesAvailable() == 0)
                {
                    const int remaining = kRequestTimeoutMs - static_cast<int>(timer.elapsed());
                    if (remaining <= 0 || !m_socket.waitForReadyRead(remaining))
                    {
                        error = QStringLiteral("Timed out reading Local API response: %1")
                                    .arg(m_socket.errorString());
                        m_socket.abort();
                        return false;
                    }
                }
                const QByteArray chunk = m_socket.read(size - data.size());
                if (chunk.isEmpty())
                {
                    error = QStringLiteral("Local API connection closed while reading a response");
                    m_socket.abort();
                    return false;
                }
                data += chunk;
            }
            return true;
        }

        QLocalSocket m_socket;
        qint64 m_nextRequestId{1};
    };

    QJsonObject jsonRpcResponse(const QJsonValue& id, const QJsonObject& result)
    {
        QJsonObject response;
        response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        response.insert(QStringLiteral("id"), id.isUndefined() ? QJsonValue(QJsonValue::Null) : id);
        response.insert(QStringLiteral("result"), result);
        return response;
    }

    QJsonObject jsonRpcError(const QJsonValue& id, int code, const QString& message)
    {
        QJsonObject error;
        error.insert(QStringLiteral("code"), code);
        error.insert(QStringLiteral("message"), message);

        QJsonObject response;
        response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        response.insert(QStringLiteral("id"), id.isUndefined() ? QJsonValue(QJsonValue::Null) : id);
        response.insert(QStringLiteral("error"), error);
        return response;
    }

    QJsonObject toolResult(const QJsonObject& data, bool isError = false)
    {
        QJsonObject textContent;
        textContent.insert(QStringLiteral("type"), QStringLiteral("text"));
        textContent.insert(QStringLiteral("text"),
                           QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)));

        QJsonArray content;
        content.append(textContent);

        QJsonObject result;
        result.insert(QStringLiteral("content"), content);
        result.insert(QStringLiteral("structuredContent"), data);
        if (isError)
        {
            result.insert(QStringLiteral("isError"), true);
        }
        return result;
    }

    QJsonObject toolError(const QString& message)
    {
        QJsonObject data;
        data.insert(QStringLiteral("ok"), false);
        data.insert(QStringLiteral("error"), message);
        return toolResult(data, true);
    }

    void logToolCall(const QString& name, bool ok)
    {
        const QByteArray timestamp = QDateTime::currentDateTimeUtc()
                                         .toString(Qt::ISODateWithMs)
                                         .toUtf8();
        const QByteArray toolName = name.toUtf8();
        std::cerr << timestamp.constData() << " tool=" << toolName.constData()
                  << " result=" << (ok ? "ok" : "error") << '\n';
    }

    QJsonObject callTool(LocalApiClient& client, const QJsonObject& params)
    {
        const QString name = params.value(QStringLiteral("name")).toString();
        const ToolSpec* spec = findTool(name);
        if (!spec)
        {
            return toolError(QStringLiteral("Unknown ScopeOne tool: %1").arg(name));
        }

        const QJsonValue argumentsValue = params.value(QStringLiteral("arguments"));
        if (!argumentsValue.isUndefined() && !argumentsValue.isObject())
        {
            logToolCall(name, false);
            return toolError(QStringLiteral("Tool arguments must be an object"));
        }
        QJsonObject arguments = argumentsValue.toObject();
        QString error;
        if (!prepareArguments(*spec, arguments, error))
        {
            logToolCall(name, false);
            return toolError(error);
        }

        QJsonObject request = arguments;
        request.insert(QStringLiteral("type"), spec->name);
        QJsonObject response;
        if (!client.request(request, response, error))
        {
            logToolCall(name, false);
            return toolError(error);
        }

        const bool ok = response.value(QStringLiteral("ok")).toBool(false);
        logToolCall(name, ok);
        return toolResult(response, !ok);
    }

    std::optional<QJsonObject> handleMessage(LocalApiClient& client, const QJsonObject& message)
    {
        const QJsonValue id = message.value(QStringLiteral("id"));
        const bool notification = id.isUndefined();
        if (message.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")
            || !message.value(QStringLiteral("method")).isString())
        {
            if (notification)
            {
                return std::nullopt;
            }
            return jsonRpcError(id, -32600, QStringLiteral("Invalid Request"));
        }

        const QString method = message.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("notifications/initialized")
            || method == QStringLiteral("notifications/cancelled"))
        {
            return std::nullopt;
        }
        if (notification)
        {
            return std::nullopt;
        }

        if (method == QStringLiteral("initialize"))
        {
            QJsonObject toolsCapability;
            toolsCapability.insert(QStringLiteral("listChanged"), false);
            QJsonObject capabilities;
            capabilities.insert(QStringLiteral("tools"), toolsCapability);

            QJsonObject serverInfo;
            serverInfo.insert(QStringLiteral("name"), QStringLiteral("scopeone"));
            serverInfo.insert(QStringLiteral("version"), QStringLiteral(SCOPEONE_APP_VERSION_STRING));

            QJsonObject result;
            result.insert(QStringLiteral("protocolVersion"), kMcpProtocolVersion);
            result.insert(QStringLiteral("capabilities"), capabilities);
            result.insert(QStringLiteral("serverInfo"), serverInfo);
            result.insert(
                QStringLiteral("instructions"),
                QStringLiteral(
                    "ScopeOne must be running. Observe state before acting. Never set confirm=true until "
                    "the user explicitly approves the requested hardware or destructive operation."));
            return jsonRpcResponse(id, result);
        }

        if (method == QStringLiteral("ping"))
        {
            return jsonRpcResponse(id, {});
        }

        if (method == QStringLiteral("tools/list"))
        {
            QJsonArray tools;
            for (const ToolSpec& spec : toolSpecs())
            {
                tools.append(toolToJson(spec));
            }
            QJsonObject result;
            result.insert(QStringLiteral("tools"), tools);
            return jsonRpcResponse(id, result);
        }

        if (method == QStringLiteral("tools/call"))
        {
            const QJsonValue paramsValue = message.value(QStringLiteral("params"));
            if (!paramsValue.isObject())
            {
                return jsonRpcError(id, -32602, QStringLiteral("tools/call params must be an object"));
            }
            return jsonRpcResponse(id, callTool(client, paramsValue.toObject()));
        }

        return jsonRpcError(id, -32601, QStringLiteral("Method not found"));
    }

    void writeMessage(const QJsonObject& message)
    {
        const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
        std::cout.write(json.constData(), json.size());
        std::cout.put('\n');
        std::cout.flush();
    }
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ScopeOneMcpServer"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SCOPEONE_APP_VERSION_STRING));

    LocalApiClient client;
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty())
        {
            continue;
        }
        if (line.size() > kMaxMessageBytes)
        {
            writeMessage(jsonRpcError({}, -32700, QStringLiteral("MCP message exceeds 64 MiB")));
            continue;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            writeMessage(jsonRpcError({}, -32700, QStringLiteral("Parse error")));
            continue;
        }

        const std::optional<QJsonObject> response = handleMessage(client, document.object());
        if (response)
        {
            writeMessage(*response);
        }
    }
    return 0;
}
