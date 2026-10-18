#include "preflight-bench-window.hpp"

#include "obs-preflight-collector.h"
#include "obs-preflight.h"
#include "preflight-cloudflare-speed-test.hpp"
#include "preflight-apply-plan.hpp"
#include "preflight-loupe-capture.hpp"
#include "preflight-gpu-sampler.hpp"
#include "preflight-test-mode.hpp"

#include <properties-view.hpp>
#include <qt-wrappers.hpp>

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#ifndef PREFLIGHT_UI_RENDER_TEST
#include <widgets/OBSBasic.hpp>
#endif
#if defined(YOUTUBE_ENABLED) && !defined(PREFLIGHT_UI_RENDER_TEST)
#include <utility/YoutubeApiWrappers.hpp>
#endif

#include <QColor>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QImage>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <map>
#include <numeric>
#include <string>
#include <vector>

#ifdef PREFLIGHT_UI_RENDER_TEST
// The recorded-data test never creates live outputs. Keep its link independent
// of OBSBasic's generated Qt UI while compiling the production Apply code.
class OBSBasic {
public:
	struct Profile {
		std::string name;
		std::filesystem::path path;
	};

	static OBSBasic *Get() { return nullptr; }
	config_t *Config() const { return nullptr; }
	Profile GetCurrentProfile() const { return {}; }
	bool Active() const { return false; }
	void ResetOutputs() {}
};
#endif

namespace preflight_ui_detail {

using preflight::bench::policy::OBS_CONGESTION_DANGER_PERCENT;
using preflight::bench::policy::OBS_CONGESTION_WARNING_PERCENT;
using preflight::bench::policy::OBS_ENCODER_SKIP_STATUS_MIN_COUNT;
using preflight::bench::policy::OBS_ENCODER_SKIP_STATUS_PERCENT;
using preflight::bench::policy::OBS_RENDER_DANGER_FRACTION;
using preflight::bench::policy::OBS_RENDER_WARNING_FRACTION;
using preflight::bench::policy::OBS_STATS_DANGER_PERCENT;
using preflight::bench::policy::OBS_STATS_WARNING_PERCENT;

constexpr uint64_t SAMPLE_WINDOW_MS = 500;
// Four GPU reads per 500 ms engine window smooth provider jitter while keeping the data fresh.
constexpr uint64_t GPU_SAMPLE_INTERVAL_MS = 125;
// The existing Auto-Configuration wizard measures each encoder step for five seconds.
constexpr uint64_t BAKEOFF_CANDIDATE_DURATION_MS = 5'000;
constexpr size_t MAX_ENGINE_WINDOWS = 120;
constexpr size_t CHART_HISTORY_SIZE = 120;
constexpr int MAX_BAKEOFF_TABLE_VIEW_HEIGHT = 280;
constexpr int MAX_IMPACT_TABLE_VIEW_HEIGHT = 270;
constexpr int MIN_IMPACT_GROUP_VIEW_HEIGHT = 80;
constexpr int MAX_IMPACT_GROUP_VIEW_HEIGHT = MAX_IMPACT_TABLE_VIEW_HEIGHT + 40;
// At the 720 px laptop fixture, keep Impact compact; each extra window pixel can reveal more rows.
constexpr int IMPACT_GROUP_WINDOW_HEIGHT_OFFSET = 640;
// Reserve enough vertical room for the 4:3 encoded picture to clear the 2x render-size regression check.
constexpr int MIN_LOUPE_VIEW_HEIGHT = 230;

// Reads the text colour the active OBS theme gives a class such as "text-warning", so painted
// charts use the same warning and danger colours as the Stats window.
QColor theme_class_color(QWidget *context, const char *classes)
{
	QLabel probe(context);
	probe.hide();
	setClasses(&probe, classes);
	probe.ensurePolished();
	return probe.palette().color(QPalette::WindowText);
}

class VisibleCheckBox final : public QCheckBox {
public:
	using QCheckBox::QCheckBox;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		const QRect box(1, (height() - 16) / 2, 16, 16);
		const bool enabled = isEnabled();
		const QColor outline = enabled ? palette().color(QPalette::Mid)
					       : palette().color(QPalette::Disabled, QPalette::Mid);
		painter.setPen(outline);
		painter.setBrush(isChecked() ? palette().color(QPalette::Highlight) : palette().color(QPalette::Base));
		painter.drawRoundedRect(box, 2, 2);
		if (isChecked()) {
			painter.setPen(QPen(palette().color(QPalette::HighlightedText), 2.0, Qt::SolidLine,
					    Qt::RoundCap, Qt::RoundJoin));
			QPainterPath check;
			check.moveTo(box.left() + box.width() * 0.22, box.center().y());
			check.lineTo(box.left() + box.width() * 0.43, box.bottom() - box.height() * 0.24);
			check.lineTo(box.right() - box.width() * 0.18, box.top() + box.height() * 0.22);
			painter.drawPath(check);
		}
		painter.setPen(enabled ? palette().color(QPalette::Text)
				       : palette().color(QPalette::Disabled, QPalette::Text));
		painter.drawText(rect().adjusted(24, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, text());
	}
};

void set_visible_checkbox_indicator(QCheckBox *checkbox)
{
	if (!checkbox) {
		return;
	}
	// Yami's theme: SVG lookup is not available to the offscreen render. This
	// remains a QCheckBox; its small subclass paints an explicit palette check.
	checkbox->setProperty("preflightVisibleIndicator", true);
}

struct CombinedChartSeries {
	int table_row = -1;
	QString label;
	QColor color;
	std::vector<std::optional<double>> points;
};

struct ChartHoverLine {
	QString text;
	QColor swatch;
	QColor text_color;
};

struct ChartHoverSample {
	QString time;
	std::vector<ChartHoverLine> lines;
};

class CombinedMetricChart final : public QWidget {
	std::vector<CombinedChartSeries> series;
	std::vector<ChartHoverSample> hover_samples;
	QRectF plot_rect;
	int hovered_index = -1;
	int hovered_series_row = -1;
	qreal hovered_fraction = 0.0;
	double plot_maximum = 120.0;

	static double nice_step(double raw_step)
	{
		if (raw_step <= 0.0 || !std::isfinite(raw_step)) {
			return 1.0;
		}
		const double magnitude = std::pow(10.0, std::floor(std::log10(raw_step)));
		const double fraction = raw_step / magnitude;
		const double rounded = fraction <= 1.0   ? 1.0
				       : fraction <= 2.0 ? 2.0
				       : fraction <= 2.5 ? 2.5
				       : fraction <= 5.0 ? 5.0
							 : 10.0;
		return rounded * magnitude;
	}

public:
	explicit CombinedMetricChart(QWidget *parent = nullptr) : QWidget(parent)
	{
		setObjectName(QStringLiteral("preflight-combined-chart"));
		setMinimumHeight(190);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		setMouseTracking(true);
		setProperty("preflightStartsAtZero", true);
		setProperty("preflightHoverOverlay", false);
	}

	void SetSeries(std::vector<CombinedChartSeries> values)
	{
		series = std::move(values);
		update();
	}

	void SetHoverSamples(std::vector<ChartHoverSample> values)
	{
		const bool was_hovered = hovered_index >= 0;
		hover_samples = std::move(values);
		if (was_hovered && !hover_samples.empty()) {
			hovered_index = std::clamp(qRound(hovered_fraction * (hover_samples.size() - 1)), 0,
						   static_cast<int>(hover_samples.size()) - 1);
		} else {
			hovered_index = -1;
			hovered_series_row = -1;
		}
		setProperty("preflightHoverIndex", hovered_index);
		setProperty("preflightHoveredSeriesRow", hovered_series_row);
		setProperty("preflightHoverText", hover_text());
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.fillRect(rect(), palette().color(QPalette::Base));
		constexpr qreal HOVER_READOUT_HEIGHT = 74.0;
		const bool show_hover_readout = hovered_index >= 0 &&
						hovered_index < static_cast<int>(hover_samples.size());
		const QRectF plot =
			QRectF(rect()).adjusted(54, 24 + (show_hover_readout ? HOVER_READOUT_HEIGHT : 0.0), -10, -16);
		plot_rect = plot;
		setProperty("preflightHoverBandHeight", show_hover_readout ? HOVER_READOUT_HEIGHT : 0.0);
		setProperty("preflightHoverDetailCount",
			    show_hover_readout
				    ? static_cast<int>(hover_samples[static_cast<size_t>(hovered_index)].lines.size())
				    : 0);
		setProperty("preflightHoverLinesFit", false);
		setProperty("preflightPlotTop", plot.top());
		setProperty("preflightPlotWidth", qRound(plot.width()));
		setProperty("preflightPlotHeight", qRound(plot.height()));
		QStringList series_names;
		QStringList series_colors;
		QStringList latest_values;
		double data_maximum = 0.0;
		bool has_points = false;
		for (const auto &line : series) {
			series_names.push_back(line.label);
			series_colors.push_back(line.color.name(QColor::HexRgb).toUpper());
			latest_values.push_back(line.points.empty() || !line.points.back()
							? QString()
							: QString::number(*line.points.back(), 'f', 2));
			for (const auto &value : line.points) {
				if (value && std::isfinite(*value)) {
					data_maximum = std::max(data_maximum, *value);
					has_points = true;
				}
			}
		}
		setProperty("preflightSeriesNames", series_names);
		setProperty("preflightSeriesColors", series_colors);
		setProperty("preflightLatestValues", latest_values);
		// Preserve the specified 120% floor; above it, round the data maximum to a clean tick.
		const double axis_step = data_maximum <= 120.0 ? 30.0 : nice_step(data_maximum / 4.0);
		const double maximum = data_maximum <= 120.0 ? 120.0 : std::ceil(data_maximum / axis_step) * axis_step;
		plot_maximum = maximum;
		QStringList y_axis_labels;
		painter.setPen(palette().color(QPalette::Mid));
		for (double value = maximum; value >= -0.001; value -= axis_step) {
			const qreal y = plot.bottom() - plot.height() * value / maximum;
			const QString label = QString::number(qRound(value));
			y_axis_labels.push_back(label);
			painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
			painter.setPen(palette().color(QPalette::Text));
			painter.drawText(QRectF(0, y - 9, plot.left() - 8, 18), Qt::AlignRight | Qt::AlignVCenter,
					 label);
			painter.setPen(palette().color(QPalette::Mid));
		}
		setProperty("preflightYAxisLabels", y_axis_labels);
		setProperty("preflightYAxisMaximum", maximum);
		setProperty("preflightYAxisTitle", QStringLiteral("% of limit"));
		painter.save();
		painter.translate(14, plot.center().y());
		painter.rotate(-90);
		painter.setPen(palette().color(QPalette::Text));
		painter.drawText(QRectF(-plot.height() / 2.0, -10, plot.height(), 20), Qt::AlignCenter,
				 QStringLiteral("% of limit"));
		painter.restore();

		const qreal limit_y = plot.bottom() - plot.height() * (100.0 / maximum);
		const QColor danger = theme_class_color(this, "text-danger");
		painter.setPen(QPen(danger, 1.2, Qt::DashLine));
		painter.drawLine(QPointF(plot.left(), limit_y), QPointF(plot.right(), limit_y));
		QRectF limit_label(plot.right() - 52, limit_y - 19, 48, 16);
		painter.fillRect(limit_label, palette().color(QPalette::Base));
		painter.setPen(danger);
		painter.drawText(limit_label, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Limit"));
		setProperty("preflightLimitLine", true);
		setProperty("preflightLimitLabel", QStringLiteral("Limit"));
		setProperty("preflightLimitLineY", limit_y);

		for (const auto &line : series) {
			if (line.points.size() < 2) {
				continue;
			}
			QPainterPath path;
			bool active_segment = false;
			for (size_t index = 0; index < line.points.size(); ++index) {
				if (!line.points[index] || !std::isfinite(*line.points[index])) {
					active_segment = false;
					continue;
				}
				const qreal x = plot.left() + plot.width() * static_cast<qreal>(index) /
								      static_cast<qreal>(line.points.size() - 1);
				const qreal y = plot.bottom() - plot.height() * (*line.points[index] / maximum);
				const QPointF point(x, y);
				if (active_segment) {
					path.lineTo(point);
				} else {
					path.moveTo(point);
					active_segment = true;
				}
			}
			QColor stroke = line.color;
			const int active_emphasis = hovered_series_row;
			const bool emphasized = active_emphasis < 0 || line.table_row == active_emphasis;
			if (!emphasized) {
				stroke.setAlpha(72);
			}
			painter.setPen(QPen(stroke, active_emphasis == line.table_row ? 3.4 : 1.8));
			painter.drawPath(path);
		}
		if (show_hover_readout) {
			const auto &sample = hover_samples[static_cast<size_t>(hovered_index)];
			const qreal x = plot.left() + plot.width() * hovered_index /
							      std::max(1, static_cast<int>(hover_samples.size()) - 1);
			painter.setPen(QPen(palette().color(QPalette::Text), 1, Qt::DashLine));
			painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
			for (const auto &line : series) {
				if (static_cast<size_t>(hovered_index) >= line.points.size() ||
				    !line.points[static_cast<size_t>(hovered_index)]) {
					continue;
				}
				const qreal y =
					plot.bottom() -
					plot.height() * (*line.points[static_cast<size_t>(hovered_index)] / maximum);
				painter.setPen(QPen(palette().color(QPalette::Base), 1.5));
				painter.setBrush(line.color);
				painter.drawEllipse(QPointF(x, y), 4.0, 4.0);
			}
			QFont readout_font = font();
			readout_font.setPointSizeF(std::max(7.5, readout_font.pointSizeF() - 2.0));
			const QFontMetrics metrics(readout_font);
			painter.setFont(readout_font);
			painter.setPen(palette().color(QPalette::Text));
			painter.drawText(QRectF(7, 1, width() - 14, metrics.height() + 2),
					 Qt::AlignLeft | Qt::AlignVCenter, sample.time);
			const qreal left_column = 8;
			const qreal right_column = width() / 2.0 + 4;
			const qreal row_top = metrics.height() + 5;
			const qreal row_step = (HOVER_READOUT_HEIGHT - row_top - 1) / 4.0;
			bool hover_lines_fit = true;
			for (size_t index = 0; index < sample.lines.size(); ++index) {
				const qreal column = (index % 2) == 0 ? left_column : right_column;
				const qreal y = row_top + row_step * static_cast<qreal>(index / 2);
				const QRectF line_bounds(column, y, width() / 2.0 - 14, row_step);
				painter.setPen(sample.lines[index].swatch);
				painter.drawText(line_bounds, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("■"));
				painter.setPen(sample.lines[index].text_color);
				const qreal swatch_width = metrics.horizontalAdvance(QStringLiteral("■ "));
				const QRectF text_bounds = line_bounds.adjusted(swatch_width + 2.0, 0, -2.0, 0);
				hover_lines_fit = hover_lines_fit &&
						  metrics.horizontalAdvance(sample.lines[index].text) <=
							  text_bounds.width();
				painter.drawText(text_bounds, Qt::AlignLeft | Qt::AlignVCenter,
						 sample.lines[index].text);
			}
			setProperty("preflightHoverLinesFit", hover_lines_fit);
		}
		if (!has_points) {
			painter.setPen(palette().color(QPalette::Text));
			painter.drawText(plot, Qt::AlignCenter, QStringLiteral("Waiting"));
		}
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (hover_samples.empty() || !plot_rect.contains(event->position())) {
			return;
		}
		const qreal fraction = (event->position().x() - plot_rect.left()) / plot_rect.width();
		hovered_fraction = std::clamp(fraction, 0.0, 1.0);
		hovered_index = std::clamp(qRound(fraction * (hover_samples.size() - 1)), 0,
					   static_cast<int>(hover_samples.size()) - 1);
		qreal nearest_distance = 10.0;
		hovered_series_row = -1;
		for (const auto &line : series) {
			if (static_cast<size_t>(hovered_index) >= line.points.size() ||
			    !line.points[static_cast<size_t>(hovered_index)]) {
				continue;
			}
			const qreal sample_y =
				plot_rect.bottom() -
				plot_rect.height() * (*line.points[static_cast<size_t>(hovered_index)] / plot_maximum);
			const qreal distance = std::abs(event->position().y() - sample_y);
			if (distance < nearest_distance) {
				nearest_distance = distance;
				hovered_series_row = line.table_row;
			}
		}
		setProperty("preflightHoverIndex", hovered_index);
		setProperty("preflightHoveredSeriesRow", hovered_series_row);
		setProperty("preflightHoverText", hover_text());
		update();
	}

	void leaveEvent(QEvent *event) override
	{
		hovered_index = -1;
		hovered_series_row = -1;
		setProperty("preflightHoverIndex", -1);
		setProperty("preflightHoveredSeriesRow", -1);
		setProperty("preflightHoverText", QString());
		update();
		QWidget::leaveEvent(event);
	}

private:
	QString hover_text() const
	{
		if (hovered_index < 0 || static_cast<size_t>(hovered_index) >= hover_samples.size()) {
			return {};
		}
		QStringList lines{hover_samples[static_cast<size_t>(hovered_index)].time};
		for (const auto &line : hover_samples[static_cast<size_t>(hovered_index)].lines) {
			lines.push_back(line.text);
		}
		return lines.join(QLatin1Char('\n'));
	}
};

class LoupeView final : public QWidget {
	std::shared_ptr<const preflight::loupe::FramePair> stream_pair;
	std::shared_ptr<const preflight::loupe::FramePair> recording_pair;
	std::optional<preflight::loupe::CropRegion> selection;
	std::optional<preflight::loupe::CropRegion> dragging_region;
	QRectF preview_image_rect;
	QRectF preview_panel;
	QRectF source_zoom_panel;
	QRectF encoded_zoom_panel;
	QRectF recording_zoom_panel;
	QPoint drag_start;
	QPoint moving_offset;
	std::optional<preflight::loupe::PixelRect> moving_crop;
	bool dragging = false;
	bool moving_selection = false;

	static uint32_t source_width(const preflight::loupe::PixelFrame &frame)
	{
		return frame.source_width ? frame.source_width : frame.width;
	}

	static uint32_t source_height(const preflight::loupe::PixelFrame &frame)
	{
		return frame.source_height ? frame.source_height : frame.height;
	}

	static QImage image_for(const preflight::loupe::PixelFrame &frame)
	{
		return QImage(frame.rgba.data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
			      static_cast<int>(frame.stride), QImage::Format_RGBA8888);
	}

	static QRectF fitted_image_rect(const QRectF &bounds, const preflight::loupe::PixelFrame &frame)
	{
		if (frame.width == 0 || frame.height == 0 || bounds.width() <= 0 || bounds.height() <= 0) {
			return {};
		}
		const qreal scale = std::min(bounds.width() / frame.width, bounds.height() / frame.height);
		const QSizeF size(frame.width * scale, frame.height * scale);
		return QRectF(QPointF(bounds.center().x() - size.width() / 2.0,
				      bounds.center().y() - size.height() / 2.0),
			      size);
	}

	std::optional<QPoint> source_point(const QPointF &point) const
	{
		if (!stream_pair || !stream_pair->source || !preview_image_rect.contains(point) ||
		    preview_image_rect.width() <= 0 || preview_image_rect.height() <= 0) {
			return std::nullopt;
		}
		const auto &frame = *stream_pair->source;
		const uint32_t logical_width = source_width(frame);
		const uint32_t logical_height = source_height(frame);
		const qreal x = (point.x() - preview_image_rect.left()) / preview_image_rect.width();
		const qreal y = (point.y() - preview_image_rect.top()) / preview_image_rect.height();
		return QPoint(std::clamp(static_cast<int>(x * logical_width), 0, static_cast<int>(logical_width) - 1),
			      std::clamp(static_cast<int>(y * logical_height), 0,
					 static_cast<int>(logical_height) - 1));
	}

	std::optional<preflight::loupe::CropRegion> bounded_drag_region(const QPoint &end) const
	{
		if (!stream_pair || !stream_pair->source) {
			return std::nullopt;
		}
		const auto start = source_point(drag_start);
		const auto finish = source_point(end);
		if (!start || !finish) {
			return std::nullopt;
		}
		const auto &frame = *stream_pair->source;
		const uint32_t logical_width = source_width(frame);
		const uint32_t logical_height = source_height(frame);
		const qreal zoom_width =
			std::min({source_zoom_panel.width(), encoded_zoom_panel.width(), recording_zoom_panel.width()});
		const qreal zoom_height = std::min(
			{source_zoom_panel.height(), encoded_zoom_panel.height(), recording_zoom_panel.height()});
		const int max_width = std::max(1, static_cast<int>((zoom_width - 8) / 4.0));
		const int max_height = std::max(1, static_cast<int>((zoom_height - 28) / 4.0));
		int left = std::min(start->x(), finish->x());
		int top = std::min(start->y(), finish->y());
		int right = std::max(start->x(), finish->x());
		int bottom = std::max(start->y(), finish->y());
		if (right - left > max_width) {
			if (finish->x() >= start->x()) {
				right = start->x() + max_width;
			} else {
				left = start->x() - max_width;
			}
		}
		if (bottom - top > max_height) {
			if (finish->y() >= start->y()) {
				bottom = start->y() + max_height;
			} else {
				top = start->y() - max_height;
			}
		}
		return preflight::loupe::make_crop_region(left, top, right, bottom, logical_width, logical_height);
	}

	void draw_zoom(QPainter &painter, const QRectF &panel, const QString &label,
		       const preflight::loupe::PixelFrame &frame, const preflight::loupe::CropRegion &region) const
	{
		painter.save();
		QFont label_font = painter.font();
		label_font.setPointSizeF(std::max(8.0, label_font.pointSizeF() - 2.0));
		painter.setFont(label_font);
		painter.setPen(palette().color(QPalette::Text));
		painter.drawText(panel.adjusted(2, 2, -2, -4), Qt::AlignTop | Qt::AlignHCenter, label);
		painter.restore();
		QRectF viewport = panel.adjusted(4, 24, -4, -4);
		painter.setPen(palette().color(QPalette::Mid));
		painter.drawRect(viewport);
		const auto zoomed = preflight::loupe::crop_at_4x(frame, region);
		if (!zoomed) {
			painter.setPen(palette().color(QPalette::Text));
			painter.drawText(viewport, Qt::AlignCenter, tr("Select a smaller area"));
			return;
		}
		const QRectF source_rect(0, 0, zoomed->width, zoomed->height);
		const QSizeF zoom_size(zoomed->width, zoomed->height);
		const QRectF target(QPointF(viewport.center().x() - zoom_size.width() / 2.0,
					    viewport.center().y() - zoom_size.height() / 2.0),
				    zoom_size);
		painter.save();
		painter.setClipRect(viewport);
		painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
		painter.drawImage(target, image_for(*zoomed), source_rect);
		painter.restore();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.fillRect(rect(), palette().color(QPalette::Base));
		const QRectF bounds = QRectF(rect()).adjusted(2, 2, -2, -2);
		const qreal gap = 5;
		const qreal preview_width = bounds.width() * 0.60;
		const qreal zoom_width = bounds.width() - preview_width - gap;
		const qreal zoom_height = (bounds.height() - gap * 2) / 3;
		setProperty("preflightMainPreviewShare", preview_width / bounds.width());
		preview_panel = QRectF(bounds.left(), bounds.top(), preview_width, bounds.height());
		source_zoom_panel = QRectF(preview_panel.right() + gap, bounds.top(), zoom_width, zoom_height);
		encoded_zoom_panel =
			QRectF(preview_panel.right() + gap, source_zoom_panel.bottom() + gap, zoom_width, zoom_height);
		recording_zoom_panel =
			QRectF(preview_panel.right() + gap, encoded_zoom_panel.bottom() + gap, zoom_width, zoom_height);
		const std::array<QRectF, 4> panels = {preview_panel, source_zoom_panel, encoded_zoom_panel,
						      recording_zoom_panel};
		setProperty("preflightLoupePanelsInsideView",
			    std::all_of(panels.begin(), panels.end(),
					[&bounds](const QRectF &panel) { return bounds.contains(panel); }));
		for (const QRectF &panel : panels) {
			painter.setPen(palette().color(QPalette::Mid));
			painter.drawRect(panel);
		}
		painter.setPen(palette().color(QPalette::Text));
		painter.drawText(preview_panel.adjusted(4, 2, -4, -4), Qt::AlignTop | Qt::AlignHCenter,
				 tr("Stream · drag to select"));
		if (!stream_pair || !stream_pair->source || !stream_pair->encoded) {
			painter.drawText(bounds, Qt::AlignCenter, tr("Waiting"));
			return;
		}

		const auto &source = *stream_pair->source;
		const auto &encoded = *stream_pair->encoded;
		const QRectF preview_bounds = preview_panel.adjusted(2, 22, -2, -2);
		preview_image_rect = fitted_image_rect(preview_bounds, encoded);
		setProperty("preflightPreviewImageRect", preview_image_rect.toRect());
		setProperty("preflightPreviewImageWidth", qRound(preview_image_rect.width()));
		setProperty("preflightPreviewImageHeight", qRound(preview_image_rect.height()));
		painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
		painter.drawImage(preview_image_rect, image_for(encoded));

		if (!selection) {
			const uint32_t logical_width = source_width(source);
			const uint32_t logical_height = source_height(source);
			const uint32_t visible_crop_width = static_cast<uint32_t>(
				std::max<qreal>(1, (std::min({source_zoom_panel.width(), encoded_zoom_panel.width(),
							      recording_zoom_panel.width()}) -
						    8) / 4.0));
			const uint32_t visible_crop_height = static_cast<uint32_t>(
				std::max<qreal>(1, (std::min({source_zoom_panel.height(), encoded_zoom_panel.height(),
							      recording_zoom_panel.height()}) -
						    28) / 4.0));
			const uint32_t crop_width =
				std::min({64U, visible_crop_width, std::max<uint32_t>(1, logical_width / 8)});
			const uint32_t crop_height =
				std::min({36U, visible_crop_height, std::max<uint32_t>(1, logical_height / 8)});
			selection = preflight::loupe::make_crop_region(
				static_cast<int32_t>((logical_width - crop_width) / 2),
				static_cast<int32_t>((logical_height - crop_height) / 2),
				static_cast<int32_t>((logical_width + crop_width) / 2),
				static_cast<int32_t>((logical_height + crop_height) / 2), logical_width,
				logical_height);
		}
		const auto active_region = dragging_region ? dragging_region : selection;
		if (!active_region) {
			return;
		}
		const auto stream_crop = preflight::loupe::project_crop(*active_region, encoded.width, encoded.height);
		if (stream_crop) {
			const QRectF selection_rect(
				preview_image_rect.left() +
					preview_image_rect.width() * stream_crop->left / encoded.width,
				preview_image_rect.top() +
					preview_image_rect.height() * stream_crop->top / encoded.height,
				preview_image_rect.width() * stream_crop->width / encoded.width,
				preview_image_rect.height() * stream_crop->height / encoded.height);
			painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
			painter.drawRect(selection_rect);
		}
		draw_zoom(painter, source_zoom_panel, tr("Source"), source, *active_region);
		draw_zoom(painter, encoded_zoom_panel, tr("Stream"), encoded, *active_region);
		if (recording_pair && recording_pair->encoded) {
			draw_zoom(painter, recording_zoom_panel, tr("Recording"), *recording_pair->encoded,
				  *active_region);
		} else {
			painter.setPen(palette().color(QPalette::Text));
			painter.drawText(recording_zoom_panel, Qt::AlignCenter, tr("Not available"));
		}
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		const auto point = source_point(event->position());
		if (event->button() == Qt::LeftButton && point) {
			dragging = true;
			drag_start = event->position().toPoint();
			moving_selection = false;
			moving_crop.reset();
			if (selection && stream_pair && stream_pair->source) {
				const auto projected =
					preflight::loupe::project_crop(*selection, source_width(*stream_pair->source),
								       source_height(*stream_pair->source));
				if (projected && point->x() >= static_cast<int>(projected->left) &&
				    point->x() < static_cast<int>(projected->left + projected->width) &&
				    point->y() >= static_cast<int>(projected->top) &&
				    point->y() < static_cast<int>(projected->top + projected->height)) {
					moving_selection = true;
					moving_crop = projected;
					moving_offset = QPoint(point->x() - static_cast<int>(projected->left),
							       point->y() - static_cast<int>(projected->top));
					dragging_region =
						preflight::loupe::CropRegion{source_width(*stream_pair->source),
									     source_height(*stream_pair->source),
									     projected->left,
									     projected->top,
									     projected->width,
									     projected->height};
				}
			}
			if (!moving_selection) {
				dragging_region = bounded_drag_region(drag_start);
			}
			update();
			event->accept();
			return;
		}
		QWidget::mousePressEvent(event);
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (dragging) {
			if (moving_selection && moving_crop && stream_pair && stream_pair->source) {
				const auto point = source_point(event->position());
				if (point) {
					const int max_left = static_cast<int>(source_width(*stream_pair->source) -
									      moving_crop->width);
					const int max_top = static_cast<int>(source_height(*stream_pair->source) -
									     moving_crop->height);
					const uint32_t left = static_cast<uint32_t>(
						std::clamp(point->x() - moving_offset.x(), 0, max_left));
					const uint32_t top = static_cast<uint32_t>(
						std::clamp(point->y() - moving_offset.y(), 0, max_top));
					dragging_region =
						preflight::loupe::CropRegion{source_width(*stream_pair->source),
									     source_height(*stream_pair->source),
									     left,
									     top,
									     moving_crop->width,
									     moving_crop->height};
				}
			} else {
				dragging_region = bounded_drag_region(event->position().toPoint());
			}
			update();
			event->accept();
			return;
		}
		QWidget::mouseMoveEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (dragging && event->button() == Qt::LeftButton) {
			if (dragging_region) {
				selection = dragging_region;
			}
			dragging_region.reset();
			moving_crop.reset();
			dragging = false;
			moving_selection = false;
			update();
			event->accept();
			return;
		}
		QWidget::mouseReleaseEvent(event);
	}

public:
	explicit LoupeView(QWidget *parent = nullptr) : QWidget(parent)
	{
		setMinimumHeight(MIN_LOUPE_VIEW_HEIGHT);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		setMouseTracking(true);
		setToolTip(tr("Drag to compare at 400%"));
	}

	void SetPairs(std::shared_ptr<const preflight::loupe::FramePair> new_stream_pair,
		      std::shared_ptr<const preflight::loupe::FramePair> new_recording_pair)
	{
		if (stream_pair == new_stream_pair && recording_pair == new_recording_pair) {
			return;
		}
		stream_pair = std::move(new_stream_pair);
		recording_pair = std::move(new_recording_pair);
		if (!stream_pair) {
			selection.reset();
		} else if (selection && stream_pair->source &&
			   (selection->source_width != source_width(*stream_pair->source) ||
			    selection->source_height != source_height(*stream_pair->source))) {
			const auto remapped = preflight::loupe::project_crop(
				*selection, source_width(*stream_pair->source), source_height(*stream_pair->source));
			if (remapped) {
				selection = preflight::loupe::CropRegion{source_width(*stream_pair->source),
									 source_height(*stream_pair->source),
									 remapped->left,
									 remapped->top,
									 remapped->width,
									 remapped->height};
			} else {
				selection.reset();
			}
		}
		update();
	}

	std::optional<preflight::loupe::CropRegion> SelectedCrop() const { return selection; }
};

struct MetricRowWidgets {
	QLabel *swatch = nullptr;
	QLabel *value = nullptr;
	QLabel *suffix = nullptr;
};

struct BenchPoint {
	uint64_t end_ms = 0;
	uint64_t encoder_frames = 0;
	double cpu_percent = 0.0;
	std::optional<double> gpu_percent;
	std::optional<double> gpu_renderer_percent;
	std::optional<double> gpu_tiler_percent;
	std::optional<double> gpu_video_encode_percent;
	double average_frame_time_ms = 0.0;
	double frame_interval_ms = 0.0;
	double encoder_skipped_percent = 0.0;
	double render_lag_percent = 0.0;
	double dropped_percent = 0.0;
	double bitrate_kbps = 0.0;
	double congestion_percent = 0.0;
	double connect_time_ms = 0.0;
	int32_t verdict = OBS_PREFLIGHT_FAIL;
};

struct BakeoffCandidate {
	std::string encoder_id;
	obs_preflight_mode mode{};
};

OBSDataAutoRelease clone_settings(obs_data_t *settings)
{
	OBSDataAutoRelease copy = obs_data_create();
	if (copy && settings) {
		obs_data_apply(copy, settings);
	}
	return copy;
}

QJsonObject json_object_from_settings(obs_data_t *settings)
{
	if (!settings) {
		return {};
	}
	const char *json = obs_data_get_json(settings);
	if (!json) {
		return {};
	}
	const QJsonDocument document = QJsonDocument::fromJson(QByteArray(json));
	return document.isObject() ? document.object() : QJsonObject{};
}

QJsonObject video_config_snapshot(config_t *config)
{
	QJsonObject values;
	values.insert(QStringLiteral("OutputCX"), static_cast<double>(config_get_uint(config, "Video", "OutputCX")));
	values.insert(QStringLiteral("OutputCY"), static_cast<double>(config_get_uint(config, "Video", "OutputCY")));
	values.insert(QStringLiteral("FPSType"), static_cast<double>(config_get_uint(config, "Video", "FPSType")));
	const char *fps_common = config_get_string(config, "Video", "FPSCommon");
	values.insert(QStringLiteral("FPSCommon"), QString::fromUtf8(fps_common ? fps_common : ""));
	values.insert(QStringLiteral("FPSInt"), static_cast<double>(config_get_uint(config, "Video", "FPSInt")));
	values.insert(QStringLiteral("FPSNum"), static_cast<double>(config_get_uint(config, "Video", "FPSNum")));
	values.insert(QStringLiteral("FPSDen"), static_cast<double>(config_get_uint(config, "Video", "FPSDen")));
	return values;
}

QJsonObject simple_recording_config_snapshot(config_t *config)
{
	QJsonObject values;
	for (const char *key : {"RecQuality", "RecEncoder", "RecFormat2"}) {
		const char *value = config_get_string(config, "SimpleOutput", key);
		values.insert(QString::fromUtf8(key), QString::fromUtf8(value ? value : ""));
	}
	return values;
}

QString output_error(obs_output_t *output, const QString &fallback)
{
	/* Raw OBS error strings vary by service/plugin; keep visible errors stable and label-shaped. */
	(void)output;
	return fallback;
}

QString test_mode_label(preflight::UplinkTestMode mode)
{
	switch (mode) {
	case preflight::UplinkTestMode::TwitchBandwidth:
		return QObject::tr("Twitch · bandwidthtest");
	case preflight::UplinkTestMode::AmazonIvsBandwidth:
		return QObject::tr("IVS · bandwidthtest");
	case preflight::UplinkTestMode::RestreamBandwidth:
		return QObject::tr("Restream · test=true");
	case preflight::UplinkTestMode::YouTubeThrowawayStream:
		return QObject::tr("YouTube · private test stream");
	case preflight::UplinkTestMode::CloudflareSpeedOnly:
		return QObject::tr("Cloudflare · speed test only");
	}
	return {};
}

} // namespace preflight_ui_detail

using namespace preflight_ui_detail;

struct PreflightBenchWindow::Impl : preflight::bench::DataApi {
	PreflightBenchWindow *dialog;
	QPushButton *run_button = nullptr;
	QPushButton *bakeoff_button = nullptr;
	QLabel *destination_label = nullptr;
	QLabel *uplink_result_label = nullptr;
	QLabel *loupe_status_label = nullptr;
	QLabel *state_label = nullptr;
	QLabel *verdict_label = nullptr;
	QLabel *reason_label = nullptr;
	QLabel *recording_path_label = nullptr;
	QLabel *bakeoff_status_label = nullptr;
	QTableWidget *bakeoff_table = nullptr;
	QTableWidget *metric_table = nullptr;
	QTableWidget *impact_table = nullptr;
	QGroupBox *impact_group = nullptr;
	CombinedMetricChart *combined_chart = nullptr;
	QGroupBox *apply_group = nullptr;
	QLabel *apply_mode_label = nullptr;
	QLabel *apply_note = nullptr;
	QTableWidget *apply_table = nullptr;
	QPushButton *apply_button = nullptr;
	QTimer *sample_timer = nullptr;
	QTimer *gpu_sample_timer = nullptr;
	QTimer *loupe_timer = nullptr;
	QTimer *settings_timer = nullptr;
	QTimer *uplink_timer = nullptr;
	OBSPropertiesView *encoder_properties = nullptr;
	OBSPropertiesView *recording_encoder_properties = nullptr;
	QComboBox *resolution_combo = nullptr;
	QComboBox *fps_combo = nullptr;
	QComboBox *simple_recording_quality_combo = nullptr;
	QVBoxLayout *stream_settings_layout = nullptr;
	QCheckBox *captions_toggle = nullptr;
	QVBoxLayout *toggle_layout = nullptr;
	std::map<std::string, obs_source_t *> toggle_filters;
	std::map<std::string, QCheckBox *> toggle_widgets;
	obs_data_t *initial_encoder_settings = nullptr;
	obs_data_t *initial_recording_encoder_settings = nullptr;
	OBSServiceAutoRelease test_service;
	OBSOutputAutoRelease stream_output;
	OBSOutputAutoRelease recording_output;
	OBSEncoderAutoRelease stream_video_encoder;
	OBSEncoderAutoRelease stream_audio_encoder;
	OBSEncoderAutoRelease recording_video_encoder;
	OBSEncoderAutoRelease recording_audio_encoder;
	OBSEncoderAutoRelease bakeoff_encoder;
	obs_data_t *bakeoff_audio_settings = nullptr;
	obs_preflight_collector_t *collector = nullptr;
	LoupeView *loupe_view = nullptr;
	PreflightLoupeCapture loupe_capture;
	PreflightLoupeCapture recording_loupe_capture;
	preflight::CloudflareSpeedTest cloudflare_speed_test;
	std::unique_ptr<preflight::gpu::Sampler> gpu_sampler;
	preflight::gpu::WindowAverager gpu_window_average;
	obs_preflight_engine_t *engine = nullptr;
	obs_preflight_engine_t *bakeoff_engine = nullptr;
	obs_preflight_bakeoff_t *bakeoff_planner = nullptr;
	std::vector<BenchPoint> points;
	std::vector<preflight::bench::MetricWindow> metric_history;
	preflight::bench::ImpactTracker impact_tracker;
	uint64_t impact_window_sequence = 0;
	QJsonObject last_stream_editor_settings;
	QJsonObject last_recording_editor_settings;
	QJsonObject simple_quality_expected_settings;
	std::vector<BakeoffCandidate> bakeoff_candidates;
	obs_preflight_candidate_plan active_candidate{};
	std::array<MetricRowWidgets, 8> metric_rows;
	QLabel *throughput_summary_label = nullptr;
	QElapsedTimer bakeoff_elapsed;
	QString supported_test_mode;
	QString recording_path;
	QString setup_error;
	QString youtube_test_error;
	QString stop_completion_status;
	QString bakeoff_candidate_reason;
	QString initial_output_mode;
	QString simple_output_encoder;
	QString simple_recording_encoder;
	QString initial_simple_recording_quality;
	QString current_simple_recording_quality;
	QString stream_video_encoder_id;
	QString recording_video_encoder_id;
	QString apply_feedback;
	std::optional<preflight::apply::Plan> apply_plan;
	std::optional<preflight::apply::Plan> recorded_apply_preview;
	preflight::bench::VideoSettings video_settings;
	preflight::bench::VideoSettings initial_video_settings;
	std::vector<preflight::bench::FeatureToggle> feature_toggles;
	bool enable_recorded_apply_controls_for_test = false;
	preflight::apply::TargetIdentity initial_apply_target;
	std::string configured_service_name;
	uint32_t current_output_width = 0;
	uint32_t current_output_height = 0;
	uint32_t current_fps_num = 0;
	uint32_t current_fps_den = 0;
	int64_t candidate_render_headroom_ns = 0;
	int64_t stream_bitrate_kbps = 0;
	double candidate_encoder_skip_percent = 0.0;
	int32_t candidate_verdict = OBS_PREFLIGHT_PASS;
	size_t active_bakeoff_row = 0;
	uint64_t last_window_end_ms = 0;
	uint64_t candidate_last_window_end_ms = 0;
	bool restart_stream_when_stopped = false;
	bool restart_recording_when_stopped = false;
	bool stream_settings_pending = false;
	bool recording_settings_pending = false;
	bool video_settings_pending = false;
	bool bench_running = false;
	bool stopping = false;
	bool bakeoff_running = false;
	bool bakeoff_waiting_for_stop = false;
	bool bakeoff_cancelled = false;
	bool bakeoff_candidate_active = false;
	bool bakeoff_candidate_has_samples = false;
	bool cloudflare_speed_only = false;
	bool requires_youtube_throwaway_stream = false;
	bool apply_preview_ready = false;
	bool apply_completed = false;
	bool close_when_stopped = false;
	bool simple_output_advanced = false;

	explicit Impl(PreflightBenchWindow *dialog_)
		: dialog(dialog_),
		  gpu_sampler(preflight::gpu::CreatePlatformSampler())
	{
	}

	preflight::bench::Snapshot GetSnapshot() const override
	{
		preflight::bench::Snapshot snapshot;
		snapshot.stream_frame = loupe_capture.LatestPair();
		snapshot.recording_frame = recording_loupe_capture.LatestPair();
		if (!metric_history.empty()) {
			snapshot.latest_metrics = metric_history.back();
			snapshot.metric_history = metric_history;
		}
		snapshot.stream_encoder.encoder_id = stream_video_encoder_id.toStdString();
		snapshot.stream_encoder.values = preflight::bench::SettingsObject::Retain(
			encoder_properties ? encoder_properties->GetSettings() : nullptr);
		snapshot.recording_encoder.encoder_id = recording_video_encoder_id.toStdString();
		snapshot.recording_encoder.values = preflight::bench::SettingsObject::Retain(
			recording_encoder_properties ? recording_encoder_properties->GetSettings() : nullptr);
		snapshot.video = video_settings;
		snapshot.impacts = impact_tracker.Entries();
		snapshot.toggles = feature_toggles;
		return snapshot;
	}

	bool SetEncoderSettings(preflight::bench::EncoderRole role, const preflight::bench::SettingsObject &settings,
				const std::vector<preflight::bench::SettingChange> &changes) override
	{
		OBSPropertiesView *view = role == preflight::bench::EncoderRole::Stream ? encoder_properties
											: recording_encoder_properties;
		if (!view || !settings) {
			return false;
		}
		impact_tracker.RecordChanges(changes);
		obs_data_clear(view->GetSettings());
		obs_data_apply(view->GetSettings(), settings.get());
		view->ReloadProperties();
		if (role == preflight::bench::EncoderRole::Stream) {
			stream_settings_pending = true;
			last_stream_editor_settings = json_object_from_settings(view->GetSettings());
		} else {
			recording_settings_pending = true;
			last_recording_editor_settings = json_object_from_settings(view->GetSettings());
		}
		RenderImpactEntries(impact_tracker.Entries());
		InvalidateApplyPreview();
		if (settings_timer) {
			settings_timer->start(0);
		}
		return true;
	}

	bool SetVideoSettings(const preflight::bench::VideoSettings &settings,
			      const std::vector<preflight::bench::SettingChange> &changes) override
	{
		if (settings.base_width != video_settings.base_width ||
		    settings.base_height != video_settings.base_height || settings.output_width == 0 ||
		    settings.output_height == 0 || settings.fps_num == 0 || settings.fps_den == 0 ||
		    settings.fps_divisor == 0) {
			return false;
		}
		const int fps_index = fps_combo ? fps_combo->findData(static_cast<uint>(settings.fps_divisor)) : 0;
		if (fps_index < 0) {
			return false;
		}
		impact_tracker.RecordChanges(changes);
		video_settings = settings;
		video_settings_pending = true;
		if (resolution_combo) {
			const QSignalBlocker blocker(resolution_combo);
			resolution_combo->setCurrentText(
				QStringLiteral("%1 × %2").arg(settings.output_width).arg(settings.output_height));
		}
		if (fps_combo) {
			const QSignalBlocker blocker(fps_combo);
			fps_combo->setCurrentIndex(fps_index);
		}
		RenderImpactEntries(impact_tracker.Entries());
		InvalidateApplyPreview();
		if (settings_timer) {
			settings_timer->start(0);
		}
		return true;
	}

	bool SetFeatureToggle(obs_source_t *filter, const std::string &name, const std::string &source_filter_id,
			      bool enabled) override
	{
		if (!filter || obs_source_get_type(filter) != OBS_SOURCE_TYPE_FILTER) {
			return false;
		}
		const char *actual_id = obs_source_get_id(filter);
		if (!actual_id || source_filter_id != actual_id) {
			return false;
		}
		const bool old_enabled = obs_source_enabled(filter);
		if (old_enabled != enabled) {
			impact_tracker.RecordChanges(
				{{name, source_filter_id, old_enabled ? "on" : "off", enabled ? "on" : "off"}});
		}
		obs_source_set_enabled(filter, enabled);
		const auto existing = std::find_if(feature_toggles.begin(), feature_toggles.end(),
						   [&source_filter_id](const auto &item) {
							   return item.source_filter_id == source_filter_id;
						   });
		preflight::bench::FeatureToggle toggle{name, source_filter_id, enabled, true, "Applies immediately"};
		if (existing == feature_toggles.end()) {
			feature_toggles.push_back(std::move(toggle));
		} else {
			*existing = std::move(toggle);
		}
		const auto widget = toggle_widgets.find(source_filter_id);
		if (widget != toggle_widgets.end()) {
			const QSignalBlocker blocker(widget->second);
			widget->second->setChecked(enabled);
		}
		RenderImpactEntries(impact_tracker.Entries());
		return true;
	}

	bool RegisterFeatureToggle(obs_source_t *filter, const std::string &name,
				   const std::string &source_filter_id) override
	{
		if (!filter || obs_source_get_type(filter) != OBS_SOURCE_TYPE_FILTER) {
			return false;
		}
		const char *actual_id = obs_source_get_id(filter);
		if (!actual_id || source_filter_id != actual_id) {
			return false;
		}
		obs_source_t *retained = obs_source_get_ref(filter);
		if (!retained) {
			return false;
		}
		auto bound = toggle_filters.find(source_filter_id);
		if (bound != toggle_filters.end()) {
			obs_source_release(bound->second);
		}
		toggle_filters[source_filter_id] = retained;
		const bool enabled = obs_source_enabled(filter);
		const auto existing = std::find_if(feature_toggles.begin(), feature_toggles.end(),
						   [&source_filter_id](const auto &item) {
							   return item.source_filter_id == source_filter_id;
						   });
		preflight::bench::FeatureToggle toggle{name, source_filter_id, enabled, true, "Applies immediately"};
		if (existing == feature_toggles.end()) {
			feature_toggles.push_back(std::move(toggle));
		} else {
			*existing = std::move(toggle);
		}
		QCheckBox *toggle_widget = nullptr;
		auto widget = toggle_widgets.find(source_filter_id);
		if (widget != toggle_widgets.end()) {
			toggle_widget = widget->second;
		} else if (name == "Live captions (on-device)" && captions_toggle) {
			toggle_widget = captions_toggle;
			toggle_widget->setText(QString::fromStdString(name));
			toggle_widgets[source_filter_id] = toggle_widget;
			QObject::connect(
				toggle_widget, &QCheckBox::toggled, dialog, [this, source_filter_id, name](bool on) {
					auto live_filter = toggle_filters.find(source_filter_id);
					if (live_filter == toggle_filters.end() ||
					    !SetFeatureToggle(live_filter->second, name, source_filter_id, on)) {
						SetStatus(QObject::tr("Live filter: not available"));
					}
				});
		} else if (toggle_layout) {
			toggle_widget =
				new VisibleCheckBox(QString::fromStdString(name), toggle_layout->parentWidget());
			toggle_widget->setObjectName(QStringLiteral("preflight-toggle-%1").arg(toggle_widgets.size()));
			set_visible_checkbox_indicator(toggle_widget);
			toggle_layout->insertWidget(std::max(0, toggle_layout->count() - 1), toggle_widget);
			toggle_widgets[source_filter_id] = toggle_widget;
			QObject::connect(
				toggle_widget, &QCheckBox::toggled, dialog, [this, source_filter_id, name](bool on) {
					auto live_filter = toggle_filters.find(source_filter_id);
					if (live_filter == toggle_filters.end() ||
					    !SetFeatureToggle(live_filter->second, name, source_filter_id, on)) {
						SetStatus(QObject::tr("Live filter: not available"));
					}
				});
		}
		if (toggle_widget) {
			toggle_widgets[source_filter_id] = toggle_widget;
			const QSignalBlocker blocker(toggle_widget);
			toggle_widget->setChecked(enabled);
			toggle_widget->setEnabled(true);
			toggle_widget->setToolTip(QObject::tr("Applies immediately"));
		}
		return true;
	}

	void BindLiveCaptionsFilter()
	{
		constexpr const char *filter_id = "mac_live_captions_filter";
		constexpr const char *display_name = "Live captions (on-device)";
		struct SearchState {
			Impl *self;
			bool found = false;
		} state{this};
		obs_enum_all_sources(
			[](void *opaque, obs_source_t *source) {
				auto *search = static_cast<SearchState *>(opaque);
				obs_source_enum_filters(
					source,
					[](obs_source_t *, obs_source_t *filter, void *filter_opaque) {
						auto *filter_search = static_cast<SearchState *>(filter_opaque);
						if (filter_search->found) {
							return;
						}
						const char *id = obs_source_get_id(filter);
						if (id && std::strcmp(id, "mac_live_captions_filter") == 0) {
							filter_search->found =
								filter_search->self->RegisterFeatureToggle(
									filter, "Live captions (on-device)",
									"mac_live_captions_filter");
						}
					},
					search);
				return !search->found;
			},
			&state);
		if (!state.found) {
			captions_toggle->setEnabled(false);
			captions_toggle->setToolTip(QObject::tr("Not available"));
			feature_toggles.push_back({display_name, filter_id, false, false, "Not available"});
		}
	}

	bool ReplaceStreamEncoderEditor(const std::string &encoder_id, const preflight::bench::SettingsObject &settings)
	{
		if (!stream_settings_layout || !settings || encoder_id.empty()) {
			return false;
		}
		const QByteArray id = QByteArray::fromStdString(encoder_id);
		OBSDataAutoRelease editor_settings = clone_settings(settings.get());
		if (!editor_settings) {
			return false;
		}
		auto *replacement = new OBSPropertiesView(editor_settings.Get(), id.constData(),
							  (PropertiesReloadCallback)obs_get_encoder_properties, 190);
		replacement->setFrameShape(QFrame::NoFrame);
		replacement->setScrolling(true);
		QObject::connect(replacement, &OBSPropertiesView::Changed, dialog, [this]() {
			RecordEditorChanges(preflight::bench::EncoderRole::Stream);
			stream_settings_pending = true;
			InvalidateApplyPreview();
			settings_timer->start(600);
		});
		if (encoder_properties) {
			stream_settings_layout->removeWidget(encoder_properties);
			delete encoder_properties;
		}
		encoder_properties = replacement;
		stream_settings_layout->insertWidget(0, encoder_properties);
		stream_video_encoder_id = QString::fromUtf8(id);
		last_stream_editor_settings = json_object_from_settings(encoder_properties->GetSettings());
		return true;
	}

	bool LoadCandidate(const preflight::bench::CandidateSettings &candidate) override
	{
		if (candidate.video.base_width != video_settings.base_width ||
		    candidate.video.base_height != video_settings.base_height || bench_running || bakeoff_running ||
		    stopping || !candidate.stream_encoder.values) {
			return false;
		}
		const std::string previous_encoder = stream_video_encoder_id.toStdString();
		const QJsonObject previous_settings =
			encoder_properties ? json_object_from_settings(encoder_properties->GetSettings())
					   : QJsonObject{};
		if (candidate.stream_encoder.encoder_id != previous_encoder &&
		    !ReplaceStreamEncoderEditor(candidate.stream_encoder.encoder_id, candidate.stream_encoder.values)) {
			return false;
		}
		const QJsonObject next_settings = json_object_from_settings(candidate.stream_encoder.values.get());
		std::vector<preflight::bench::SettingChange> encoder_changes;
		if (previous_encoder != candidate.stream_encoder.encoder_id) {
			encoder_changes.push_back({"Stream encoder", "encoder_id", previous_encoder,
						   candidate.stream_encoder.encoder_id});
		}
		for (auto it = next_settings.constBegin(); it != next_settings.constEnd(); ++it) {
			const QJsonValue previous = previous_settings.value(it.key());
			if (previous != it.value()) {
				encoder_changes.push_back(
					{"Stream encoder", it.key().toStdString(),
					 preflight::apply::ValueForDisplay(previous).toStdString(),
					 preflight::apply::ValueForDisplay(it.value()).toStdString()});
			}
		}
		if (!SetEncoderSettings(preflight::bench::EncoderRole::Stream, candidate.stream_encoder.values,
					encoder_changes)) {
			return false;
		}
		std::vector<preflight::bench::SettingChange> video_changes;
		if (candidate.video.output_width != video_settings.output_width ||
		    candidate.video.output_height != video_settings.output_height) {
			video_changes.push_back({"Video", "output resolution",
						 std::to_string(video_settings.output_width) + "x" +
							 std::to_string(video_settings.output_height),
						 std::to_string(candidate.video.output_width) + "x" +
							 std::to_string(candidate.video.output_height)});
		}
		if (static_cast<uint64_t>(candidate.video.fps_num) * video_settings.fps_den !=
		    static_cast<uint64_t>(video_settings.fps_num) * candidate.video.fps_den) {
			video_changes.push_back(
				{"Video", "FPS",
				 std::to_string(video_settings.fps_num) + "/" + std::to_string(video_settings.fps_den),
				 std::to_string(candidate.video.fps_num) + "/" +
					 std::to_string(candidate.video.fps_den)});
		}
		return SetVideoSettings(candidate.video, video_changes);
	}

	~Impl()
	{
		StopLoupe();
		StopGpuSampling();
		if (sample_timer) {
			sample_timer->stop();
		}
		if (settings_timer) {
			settings_timer->stop();
		}
		if (stream_output && obs_output_active(stream_output)) {
			obs_output_stop(stream_output);
		}
		if (recording_output && obs_output_active(recording_output)) {
			obs_output_stop(recording_output);
		}
		obs_preflight_collector_destroy(collector);
		obs_preflight_engine_destroy(engine);
		obs_preflight_engine_destroy(bakeoff_engine);
		obs_preflight_bakeoff_destroy(bakeoff_planner);
		for (const auto &[id, filter] : toggle_filters) {
			(void)id;
			obs_source_release(filter);
		}
		if (initial_encoder_settings) {
			obs_data_release(initial_encoder_settings);
		}
		if (initial_recording_encoder_settings) {
			obs_data_release(initial_recording_encoder_settings);
		}
		if (bakeoff_audio_settings) {
			obs_data_release(bakeoff_audio_settings);
		}
	}

	void SetStatus(const QString &text) { state_label->setText(text); }

	void StartGpuSampling()
	{
		gpu_window_average.TakeAverage();
		if (gpu_sample_timer && gpu_sampler) {
			gpu_sample_timer->start(static_cast<int>(GPU_SAMPLE_INTERVAL_MS));
		}
	}

	void StopGpuSampling()
	{
		if (gpu_sample_timer) {
			gpu_sample_timer->stop();
		}
		gpu_window_average.TakeAverage();
	}

	void SampleGpu()
	{
		if (gpu_sampler && bench_running && !stopping && !bakeoff_running) {
			gpu_window_average.Add(gpu_sampler->Read());
		}
	}

	void StopOutputs(const QString &status, const QString &completion_status = QString())
	{
		StopLoupe();
		StopGpuSampling();
		bench_running = false;
		restart_stream_when_stopped = false;
		stopping = true;
		if (stream_output && obs_output_active(stream_output)) {
			obs_output_stop(stream_output);
		}
		if (recording_output && obs_output_active(recording_output)) {
			obs_output_stop(recording_output);
		}
		stop_completion_status = completion_status.isEmpty() ? QObject::tr("Stopped") : completion_status;
		run_button->setEnabled(false);
		run_button->setText(QObject::tr("Stopping…"));
		SetStatus(status);
		sample_timer->start(100);
	}

	void UpdateLoupe()
	{
		if (loupe_view) {
			loupe_view->SetPairs(loupe_capture.LatestPair(), recording_loupe_capture.LatestPair());
			if (const auto crop = loupe_view->SelectedCrop()) {
				loupe_capture.SetSourceCrop(*crop);
				recording_loupe_capture.SetSourceCrop(*crop);
			}
		}
		if (loupe_status_label) {
			const QString stream_status = QString::fromStdString(loupe_capture.Status());
			const QString recording_status = QString::fromStdString(recording_loupe_capture.Status());
			loupe_status_label->setText(recording_status.isEmpty()
							    ? stream_status
							    : stream_status + QStringLiteral(" · ") + recording_status);
		}
	}

	void StartLoupe(obs_encoder_t *encoder)
	{
		if (!stream_output || !encoder) {
			return;
		}
		if (loupe_capture.Start(stream_output, encoder)) {
			loupe_timer->start(100);
		} else if (loupe_timer) {
			loupe_timer->stop();
		}
		UpdateLoupe();
	}

	void StartRecordingLoupe()
	{
		if (!recording_output || !recording_video_encoder) {
			return;
		}
		if (recording_loupe_capture.Start(recording_output, recording_video_encoder)) {
			loupe_timer->start(100);
		}
		UpdateLoupe();
	}

	void StopLoupe()
	{
		if (loupe_timer) {
			loupe_timer->stop();
		}
		loupe_capture.Stop();
		recording_loupe_capture.Stop();
		UpdateLoupe();
	}

	void FinishStopping()
	{
		stopping = false;
		sample_timer->stop();
		run_button->setEnabled(true);
		run_button->setText(QObject::tr("Start bench"));
		SetStatus(stop_completion_status);
		stop_completion_status.clear();
		if (close_when_stopped) {
			close_when_stopped = false;
			dialog->close();
		}
	}

	QJsonObject SimpleConfigSnapshot(OBSBasic *main) const
	{
		QJsonObject settings;
		config_t *config = main->Config();
		settings.insert(QStringLiteral("VBitrate"),
				static_cast<double>(config_get_uint(config, "SimpleOutput", "VBitrate")));
		settings.insert(QStringLiteral("UseAdvanced"), config_get_bool(config, "SimpleOutput", "UseAdvanced"));
		const QString preset_key = preflight::apply::SimplePresetConfigKey(simple_output_encoder);
		if (!preset_key.isEmpty()) {
			const QByteArray key = preset_key.toUtf8();
			const char *preset = config_get_string(config, "SimpleOutput", key.constData());
			settings.insert(preset_key, QString::fromUtf8(preset ? preset : ""));
		}
		if (simple_output_advanced && (simple_output_encoder == QStringLiteral("x264") ||
					       simple_output_encoder == QStringLiteral("x264_lowcpu"))) {
			const char *custom = config_get_string(config, "SimpleOutput", "x264Settings");
			settings.insert(QStringLiteral("x264Settings"), QString::fromUtf8(custom ? custom : ""));
		}
		return settings;
	}

	bool ApplySimpleRecordingQuality(const QString &quality)
	{
		if (!recording_encoder_properties || quality == QStringLiteral("Lossless")) {
			return false;
		}
		OBSDataAutoRelease settings(obs_data_create());
		if (!settings) {
			return false;
		}
		if (quality == QStringLiteral("Stream")) {
			if (stream_video_encoder_id != recording_video_encoder_id || !encoder_properties) {
				return false;
			}
			obs_data_apply(settings, encoder_properties->GetSettings());
		} else {
			const QString encoder_id = recording_video_encoder_id.toLower();
			const bool low_cpu_x264 =
				simple_recording_encoder.contains(QStringLiteral("lowcpu"), Qt::CaseInsensitive);
			const bool x264 = encoder_id.contains(QStringLiteral("x264"));
			const bool apple = encoder_id.contains(QStringLiteral("videotoolbox")) ||
					   encoder_id.contains(QStringLiteral("apple"));
			const bool qsv = encoder_id.contains(QStringLiteral("qsv"));
			const bool hardware_cqp = qsv || encoder_id.contains(QStringLiteral("nvenc")) ||
						  encoder_id.contains(QStringLiteral("amf")) ||
						  encoder_id.contains(QStringLiteral("amd"));
			if (!x264 && !apple && !hardware_cqp) {
				return false;
			}
			const double diagonal = std::hypot(static_cast<double>(video_settings.output_width),
							   static_cast<double>(video_settings.output_height));
			const int crf_reduction = static_cast<int>((1.0 - std::min(2000.0, diagonal) / 2000.0) * 10.0);
			const int quality_value =
				(quality == QStringLiteral("HQ") ? 16 : 23) - crf_reduction - (low_cpu_x264 ? 2 : 0);
			if (x264) {
				obs_data_set_string(settings, "rate_control", "CRF");
				obs_data_set_int(settings, "crf", quality_value);
				obs_data_set_bool(settings, "use_bufsize", true);
				obs_data_set_string(settings, "profile", "high");
				obs_data_set_string(settings, "preset", low_cpu_x264 ? "ultrafast" : "veryfast");
			} else if (apple) {
				obs_data_set_string(settings, "rate_control", "CRF");
				obs_data_set_string(settings, "profile",
						    encoder_id.contains(QStringLiteral("hevc")) ? "main" : "high");
				obs_data_set_int(settings, "quality", quality == QStringLiteral("HQ") ? 70 : 50);
			} else {
				bool supports_icq = false;
				if (qsv && !encoder_id.contains(QStringLiteral("av1"))) {
					OBSProperties properties = obs_encoder_properties(recording_video_encoder);
					obs_property_t *rate_control = obs_properties_get(properties, "rate_control");
					for (size_t i = 0;
					     rate_control && i < obs_property_list_item_count(rate_control); ++i) {
						const char *value = obs_property_list_item_string(rate_control, i);
						if (value && strcmp(value, "ICQ") == 0) {
							supports_icq = true;
							break;
						}
					}
				}
				const bool main_profile = encoder_id.contains(QStringLiteral("nvenc")) &&
							  (encoder_id.contains(QStringLiteral("hevc")) ||
							   encoder_id.contains(QStringLiteral("av1")));
				obs_data_set_string(settings, "rate_control", supports_icq ? "ICQ" : "CQP");
				obs_data_set_string(settings, "profile", main_profile ? "main" : "high");
				obs_data_set_int(settings, supports_icq ? "icq_quality" : "cqp", quality_value);
				if (encoder_id.contains(QStringLiteral("amf")) ||
				    encoder_id.contains(QStringLiteral("amd"))) {
					obs_data_set_string(settings, "preset", "quality");
				}
			}
		}
		obs_data_clear(recording_encoder_properties->GetSettings());
		obs_data_apply(recording_encoder_properties->GetSettings(), settings);
		recording_encoder_properties->ReloadProperties();
		last_recording_editor_settings = json_object_from_settings(recording_encoder_properties->GetSettings());
		simple_quality_expected_settings = last_recording_editor_settings;
		recording_settings_pending = true;
		if (settings_timer) {
			settings_timer->start(0);
		}
		return true;
	}

	preflight::apply::TargetIdentity MakeApplyTarget(OBSBasic *main, const QString &mode,
							 const char *actual_encoder_id) const
	{
		preflight::apply::TargetIdentity target;
		if (!main) {
			return target;
		}
		try {
			const auto &profile = main->GetCurrentProfile();
			target.profile_name =
				QString::fromUtf8(profile.name.data(), static_cast<qsizetype>(profile.name.size()));
			const std::string path_utf8 = profile.path.u8string();
			target.profile_path =
				QString::fromUtf8(path_utf8.data(), static_cast<qsizetype>(path_utf8.size()));
			target.encoder_id = QString::fromUtf8(actual_encoder_id ? actual_encoder_id : "");
			const char *encoder_selection = nullptr;
			if (mode == QStringLiteral("Simple")) {
				encoder_selection = config_get_string(main->Config(), "SimpleOutput", "StreamEncoder");
			} else if (mode == QStringLiteral("Advanced")) {
				encoder_selection = config_get_string(main->Config(), "AdvOut", "Encoder");
			}
			target.encoder_selection = QString::fromUtf8(encoder_selection ? encoder_selection : "");
			OBSOutputAutoRelease configured_recording = obs_frontend_get_recording_output();
			OBSEncoderAutoRelease configured_recording_video =
				configured_recording ? obs_output_get_video_encoder(configured_recording) : nullptr;
			const char *recording_id =
				configured_recording_video ? obs_encoder_get_id(configured_recording_video) : nullptr;
			target.recording_encoder_id = QString::fromUtf8(recording_id ? recording_id : "");
			const char *recording_selection = nullptr;
			if (mode == QStringLiteral("Simple")) {
				recording_selection = config_get_string(main->Config(), "SimpleOutput", "RecEncoder");
			} else if (mode == QStringLiteral("Advanced")) {
				recording_selection = config_get_string(main->Config(), "AdvOut", "RecEncoder");
			}
			target.recording_encoder_selection =
				QString::fromUtf8(recording_selection ? recording_selection : "");
		} catch (const std::exception &) {
			return {};
		}
		return target;
	}

	preflight::apply::TargetIdentity CurrentApplyTarget(OBSBasic *main, const QString &mode) const
	{
		OBSOutputAutoRelease configured_stream = obs_frontend_get_streaming_output();
		OBSEncoderAutoRelease configured_video =
			configured_stream ? obs_output_get_video_encoder(configured_stream) : nullptr;
		const char *encoder_id = configured_video ? obs_encoder_get_id(configured_video) : nullptr;
		return MakeApplyTarget(main, mode, encoder_id);
	}

	preflight::apply::Plan BuildCurrentApplyPlan(OBSBasic *main) const
	{
		preflight::apply::Plan plan;
		if (!main || !encoder_properties || !initial_encoder_settings) {
			plan.error = QObject::tr("OBS settings: not available");
			return plan;
		}
		const char *mode_value = config_get_string(main->Config(), "Output", "Mode");
		const QString mode = QString::fromUtf8(mode_value ? mode_value : "");
		plan.mode = mode == QStringLiteral("Simple")     ? preflight::apply::OutputMode::Simple
			    : mode == QStringLiteral("Advanced") ? preflight::apply::OutputMode::Advanced
								 : preflight::apply::OutputMode::Unsupported;
		plan.target = CurrentApplyTarget(main, mode);
		if (mode != initial_output_mode) {
			plan.error = QObject::tr("Output mode: changed; reopen Preflight");
			return plan;
		}
		if (plan.target.profile_name != initial_apply_target.profile_name ||
		    plan.target.profile_path != initial_apply_target.profile_path ||
		    plan.target.encoder_id != initial_apply_target.encoder_id ||
		    plan.target.encoder_selection != initial_apply_target.encoder_selection ||
		    plan.target.recording_encoder_id != initial_apply_target.recording_encoder_id ||
		    plan.target.recording_encoder_selection != initial_apply_target.recording_encoder_selection) {
			plan.error = QObject::tr("Profile/encoder: changed; reopen Preflight");
			return plan;
		}

		const QJsonObject initial = json_object_from_settings(initial_encoder_settings);
		const QJsonObject edited = json_object_from_settings(encoder_properties->GetSettings());
		if (stream_video_encoder_id != initial_apply_target.encoder_id) {
			plan.error = QObject::tr("Apply: encoder differs from stream");
			return plan;
		}
		QJsonObject saved_video = video_config_snapshot(main->Config());
		QJsonObject edited_video = saved_video;
		if (video_settings.output_width != initial_video_settings.output_width ||
		    video_settings.output_height != initial_video_settings.output_height) {
			edited_video.insert(QStringLiteral("OutputCX"),
					    static_cast<double>(video_settings.output_width));
			edited_video.insert(QStringLiteral("OutputCY"),
					    static_cast<double>(video_settings.output_height));
		}
		const bool fps_changed =
			static_cast<uint64_t>(video_settings.fps_num) * initial_video_settings.fps_den !=
			static_cast<uint64_t>(initial_video_settings.fps_num) * video_settings.fps_den;
		if (fps_changed) {
			if (video_settings.fps_den == 1) {
				edited_video.insert(QStringLiteral("FPSType"), 1);
				edited_video.insert(QStringLiteral("FPSInt"),
						    static_cast<double>(video_settings.fps_num));
			} else {
				edited_video.insert(QStringLiteral("FPSType"), 2);
				edited_video.insert(QStringLiteral("FPSNum"),
						    static_cast<double>(video_settings.fps_num));
				edited_video.insert(QStringLiteral("FPSDen"),
						    static_cast<double>(video_settings.fps_den));
			}
		}
		if (mode == QStringLiteral("Advanced")) {
			QJsonObject saved_stream;
			const QString saved_stream_path =
				QDir(plan.target.profile_path).filePath(QStringLiteral("streamEncoder.json"));
			if (!preflight::apply::ReadAdvancedSettings(saved_stream_path, &saved_stream, &plan.error)) {
				return plan;
			}
			plan = preflight::apply::BuildAdvancedPlan(saved_stream, initial, edited, plan.target);
			QJsonObject saved_recording;
			const QString saved_recording_path =
				QDir(plan.target.profile_path).filePath(QStringLiteral("recordEncoder.json"));
			if (!preflight::apply::ReadAdvancedSettings(saved_recording_path, &saved_recording,
								    &plan.error)) {
				return plan;
			}
			preflight::apply::AppendAdvancedEncoderChanges(
				plan, QStringLiteral("recordEncoder.json"), saved_recording,
				json_object_from_settings(initial_recording_encoder_settings),
				json_object_from_settings(recording_encoder_properties->GetSettings()));
			preflight::apply::AppendConfigSectionChanges(plan, QStringLiteral("Video"), saved_video,
								     edited_video);
			return plan;
		}
		if (mode == QStringLiteral("Simple")) {
			plan = preflight::apply::BuildSimplePlan(SimpleConfigSnapshot(main), initial, edited,
								 simple_output_encoder, simple_output_advanced,
								 plan.target);
			const QJsonObject initial_recording =
				json_object_from_settings(initial_recording_encoder_settings);
			const QJsonObject edited_recording =
				json_object_from_settings(recording_encoder_properties->GetSettings());
			if (initial_recording != edited_recording &&
			    edited_recording != simple_quality_expected_settings) {
				plan.error = QObject::tr("Recording encoder: unsupported Simple mode");
				return plan;
			}
			QJsonObject saved_recording = simple_recording_config_snapshot(main->Config());
			QJsonObject edited_recording_config = saved_recording;
			if (simple_recording_quality_combo && simple_recording_quality_combo->currentIndex() >= 0) {
				edited_recording_config.insert(
					QStringLiteral("RecQuality"),
					simple_recording_quality_combo->currentData().toString());
			}
			preflight::apply::AppendConfigSectionChanges(plan, QStringLiteral("SimpleOutput"),
								     saved_recording, edited_recording_config);
			preflight::apply::AppendConfigSectionChanges(plan, QStringLiteral("Video"), saved_video,
								     edited_video);
			return plan;
		}
		plan.error = QObject::tr("Output mode: unsupported Apply mapping");
		return plan;
	}

	void InvalidateApplyPreview()
	{
		apply_preview_ready = false;
		apply_completed = false;
		apply_feedback.clear();
		if (apply_button) {
			apply_button->setEnabled(false);
			apply_button->setText(QObject::tr("Apply"));
		}
		if (apply_note) {
			apply_note->setText(QObject::tr("Settling"));
		}
		if (settings_timer) {
			settings_timer->start(600);
		}
	}

	void RefreshApplyPreview()
	{
		if (!apply_table || !apply_button) {
			return;
		}
		apply_preview_ready = false;
		apply_button->setEnabled(false);
		if (recorded_apply_preview) {
			apply_plan = *recorded_apply_preview;
			apply_mode_label->setText(QObject::tr("%1 (recorded preview)")
							  .arg(apply_plan->mode == preflight::apply::OutputMode::Simple
								       ? QObject::tr("Simple")
								       : QObject::tr("Advanced")));
		} else {
			OBSBasic *main = OBSBasic::Get();
			const char *mode_value = main ? config_get_string(main->Config(), "Output", "Mode") : nullptr;
			const QString mode = QString::fromUtf8(mode_value ? mode_value : "");
			apply_mode_label->setText(
				QObject::tr("%1 (stays unchanged)").arg(mode.isEmpty() ? QObject::tr("unknown") : mode));
			apply_plan = BuildCurrentApplyPlan(main);
		}

		const auto &plan = *apply_plan;
		apply_table->setRowCount(static_cast<int>(plan.changes.size()));
		for (size_t row = 0; row < plan.changes.size(); ++row) {
			const auto &change = plan.changes[row];
			const int table_row = static_cast<int>(row);
			QString visible_key = change.path;
			if (change.path == QStringLiteral("streamEncoder.json/bitrate")) {
				visible_key = QObject::tr("Stream bitrate");
			} else if (change.path == QStringLiteral("streamEncoder.json/preset")) {
				visible_key = QObject::tr("Stream preset");
			} else if (change.path == QStringLiteral("recordEncoder.json/crf")) {
				visible_key = QObject::tr("Record CRF");
			} else if (change.path == QStringLiteral("Video.OutputCX")) {
				visible_key = QObject::tr("Output width");
			} else if (change.path == QStringLiteral("Video.OutputCY")) {
				visible_key = QObject::tr("Output height");
			} else if (change.path.startsWith(QStringLiteral("Video.FPS"))) {
				visible_key = QObject::tr("Output FPS");
			}
			auto *key = new QTableWidgetItem(visible_key);
			key->setToolTip(change.path);
			auto *before = new QTableWidgetItem(preflight::apply::ValueForDisplay(change.before));
			auto *after = new QTableWidgetItem(preflight::apply::ValueForDisplay(change.after));
			before->setToolTip(change.path);
			after->setToolTip(change.path);
			for (QTableWidgetItem *item : {key, before, after}) {
				item->setFlags(item->flags() & ~Qt::ItemIsEditable);
			}
			apply_table->setItem(table_row, 0, key);
			apply_table->setItem(table_row, 1, before);
			apply_table->setItem(table_row, 2, after);
		}
		ResizeApplyTableToContents();
		apply_button->setText(apply_completed ? QObject::tr("Applied") : QObject::tr("Apply"));
		apply_preview_ready = !apply_completed && plan.applicable() &&
				      (!recorded_apply_preview || enable_recorded_apply_controls_for_test);
		apply_button->setEnabled(apply_preview_ready);
		if (!apply_feedback.isEmpty()) {
			apply_note->setText(apply_feedback);
		} else if (!plan.error.isEmpty()) {
			apply_note->setText(plan.error);
		} else if (plan.changes.empty()) {
			apply_note->setText(QObject::tr("Writes on Apply only"));
		} else {
			apply_note->setText(QObject::tr("Writes on Apply only"));
		}
	}

	void ApplyConfigChanges()
	{
		if (!apply_table || !apply_button || !apply_preview_ready || !apply_plan || !apply_plan->applicable()) {
			return;
		}
		if (recorded_apply_preview) {
			// Recorded render fixtures exercise the button transition without writing OBS config.
			apply_completed = true;
			apply_feedback = QObject::tr("Applied · %1 mode kept")
						 .arg(apply_plan->mode == preflight::apply::OutputMode::Simple
							      ? QObject::tr("Simple")
							      : QObject::tr("Advanced"));
			RefreshApplyPreview();
			return;
		}
		OBSBasic *main = OBSBasic::Get();
		if (!main) {
			return;
		}
		const preflight::apply::Plan displayed = *apply_plan;
		const preflight::apply::Plan current = BuildCurrentApplyPlan(main);
		if (!preflight::apply::SamePlan(displayed, current)) {
			apply_feedback = QObject::tr("Apply: target changed; review updated diff");
			apply_preview_ready = false;
			apply_button->setEnabled(false);
			RefreshApplyPreview();
			return;
		}
		QString write_error;
		const bool wrote =
			displayed.mode == preflight::apply::OutputMode::Advanced
				? preflight::apply::WriteAdvancedSettings(main->Config(), displayed, current,
									  &write_error)
			: displayed.mode == preflight::apply::OutputMode::Simple
				? preflight::apply::WriteSimpleConfig(main->Config(), displayed, current, &write_error)
				: false;
		if (!wrote) {
			apply_feedback = write_error.isEmpty()
						 ? QObject::tr("Settings write: target changed; review preview")
						 : write_error;
			RefreshApplyPreview();
			return;
		}

		if (displayed.mode != preflight::apply::OutputMode::Simple &&
		    displayed.mode != preflight::apply::OutputMode::Advanced) {
			return;
		}

		OBSDataAutoRelease new_baseline = clone_settings(encoder_properties->GetSettings());
		if (new_baseline) {
			obs_data_addref(new_baseline);
			if (initial_encoder_settings) {
				obs_data_release(initial_encoder_settings);
			}
			initial_encoder_settings = new_baseline.Get();
		}
		if (!main->Active()) {
			main->ResetOutputs();
		}
		apply_completed = true;
		apply_feedback = QObject::tr("Applied · %1 mode kept")
					 .arg(displayed.mode == preflight::apply::OutputMode::Simple
						      ? QObject::tr("Simple")
						      : QObject::tr("Advanced"));
		RefreshApplyPreview();
	}

	bool BuildOutputs()
	{
		OBSBasic *main = OBSBasic::Get();
		if (!main) {
			setup_error = QObject::tr("OBS settings: not ready");
			return false;
		}
		const char *mode = config_get_string(main->Config(), "Output", "Mode");
		initial_output_mode = QString::fromUtf8(mode ? mode : "");
		if (initial_output_mode == QStringLiteral("Simple")) {
			const char *encoder = config_get_string(main->Config(), "SimpleOutput", "StreamEncoder");
			simple_output_encoder = QString::fromUtf8(encoder ? encoder : "");
			const char *recording_encoder = config_get_string(main->Config(), "SimpleOutput", "RecEncoder");
			simple_recording_encoder = QString::fromUtf8(recording_encoder ? recording_encoder : "");
			simple_output_advanced = config_get_bool(main->Config(), "SimpleOutput", "UseAdvanced");
			const char *recording_quality = config_get_string(main->Config(), "SimpleOutput", "RecQuality");
			initial_simple_recording_quality =
				QString::fromUtf8(recording_quality ? recording_quality : "Stream");
			current_simple_recording_quality = initial_simple_recording_quality;
		}
		OBSServiceAutoRelease selected_service = obs_frontend_get_streaming_service();
		if (!selected_service) {
			setup_error = QObject::tr("Stream service: not set");
			return false;
		}

		OBSDataAutoRelease selected_service_settings = obs_service_get_settings(selected_service);
		if (!selected_service_settings) {
			setup_error = QObject::tr("Stream service: settings unreadable");
			return false;
		}
		const char *service_name = obs_data_get_string(selected_service_settings, "service");
		const char *service_id = obs_service_get_id(selected_service);
		configured_service_name = service_name ? service_name : "";
		const auto uplink_mode = preflight::uplink_test_mode_for_service(configured_service_name);
		supported_test_mode = test_mode_label(uplink_mode);
		if (uplink_mode == preflight::UplinkTestMode::CloudflareSpeedOnly) {
			cloudflare_speed_only = true;
			setup_error = QObject::tr("Stream service: no test mode");
			return false;
		}

		auto safe_key = preflight::make_non_public_test_key(
			service_name, obs_data_get_string(selected_service_settings, "key"));
		requires_youtube_throwaway_stream = uplink_mode == preflight::UplinkTestMode::YouTubeThrowawayStream;
#ifndef YOUTUBE_ENABLED
		if (requires_youtube_throwaway_stream) {
			setup_error = QObject::tr("YouTube test key: unavailable in this build");
			return false;
		}
#endif
		if ((!safe_key && !requires_youtube_throwaway_stream) || supported_test_mode.isEmpty() || !service_id ||
		    strcmp(service_id, "rtmp_common") != 0) {
			setup_error = QObject::tr("Stream service: no supported test mode");
			return false;
		}

		OBSDataAutoRelease test_service_settings = clone_settings(selected_service_settings);
		if (!test_service_settings) {
			setup_error = QObject::tr("Stream service: settings copy failed");
			return false;
		}
		obs_data_set_string(test_service_settings, "key", safe_key ? safe_key->c_str() : "");
		test_service = obs_service_create(obs_service_get_id(selected_service), "preflight_non_public_test",
						  test_service_settings, nullptr);
		if (!test_service) {
			setup_error = QObject::tr("Test service: could not create");
			return false;
		}

		OBSOutputAutoRelease configured_stream = obs_frontend_get_streaming_output();
		OBSOutputAutoRelease configured_recording = obs_frontend_get_recording_output();
		OBSEncoderAutoRelease configured_stream_video =
			configured_stream ? obs_output_get_video_encoder(configured_stream) : nullptr;
		OBSEncoderAutoRelease configured_stream_audio =
			configured_stream ? obs_output_get_audio_encoder(configured_stream, 0) : nullptr;
		OBSEncoderAutoRelease configured_recording_video =
			configured_recording ? obs_output_get_video_encoder(configured_recording) : nullptr;
		OBSEncoderAutoRelease configured_recording_audio =
			configured_recording ? obs_output_get_audio_encoder(configured_recording, 0) : nullptr;

		if (!configured_stream_video) {
			setup_error = QObject::tr("Stream encoder: not configured");
			return false;
		}

		OBSDataAutoRelease stream_video_settings = obs_encoder_get_settings(configured_stream_video);
		OBSDataAutoRelease stream_audio_settings =
			configured_stream_audio ? OBSDataAutoRelease(obs_encoder_get_settings(configured_stream_audio))
						: OBSDataAutoRelease(obs_data_create());
		OBSDataAutoRelease recording_video_settings =
			configured_recording_video
				? OBSDataAutoRelease(obs_encoder_get_settings(configured_recording_video))
				: clone_settings(stream_video_settings);
		OBSDataAutoRelease recording_audio_settings =
			configured_recording_audio
				? OBSDataAutoRelease(obs_encoder_get_settings(configured_recording_audio))
				: clone_settings(stream_audio_settings);
		if (!stream_video_settings || !stream_audio_settings || !recording_video_settings ||
		    !recording_audio_settings) {
			setup_error = QObject::tr("Encoder settings: unreadable");
			return false;
		}
		bakeoff_audio_settings = stream_audio_settings.Get();
		obs_data_addref(bakeoff_audio_settings);
		stream_bitrate_kbps = obs_data_get_int(stream_video_settings, "bitrate");
		const char *stream_video_id = obs_encoder_get_id(configured_stream_video);
		stream_video_encoder_id = QString::fromUtf8(stream_video_id ? stream_video_id : "");
		initial_apply_target = MakeApplyTarget(main, initial_output_mode, stream_video_id);
		const char *stream_audio_id = configured_stream_audio ? obs_encoder_get_id(configured_stream_audio)
								      : "ffmpeg_aac";
		const char *recording_video_id =
			configured_recording_video ? obs_encoder_get_id(configured_recording_video) : stream_video_id;
		const char *recording_audio_id =
			configured_recording_audio ? obs_encoder_get_id(configured_recording_audio) : stream_audio_id;
		const size_t recording_audio_mixer =
			configured_recording_audio ? obs_encoder_get_mixer_index(configured_recording_audio)
			: configured_stream_audio  ? obs_encoder_get_mixer_index(configured_stream_audio)
						   : 0;
		if (recording_audio_mixer >= std::numeric_limits<size_t>::digits) {
			setup_error = QObject::tr("Recording audio: mixer unsupported");
			return false;
		}

		if (!stream_audio_settings) {
			setup_error = QObject::tr("Stream audio: settings unreadable");
			return false;
		}
		obs_service_apply_encoder_settings(test_service, stream_video_settings, stream_audio_settings);
		OBSDataAutoRelease property_settings = clone_settings(stream_video_settings);
		if (!property_settings) {
			setup_error = QObject::tr("Stream encoder: settings unavailable");
			return false;
		}
		OBSDataAutoRelease initial_settings = clone_settings(property_settings);
		if (!initial_settings) {
			setup_error = QObject::tr("Stream encoder: snapshot failed");
			return false;
		}
		obs_data_addref(initial_settings);
		initial_encoder_settings = initial_settings.Get();
		encoder_properties = new OBSPropertiesView(property_settings.Get(), stream_video_id,
							   (PropertiesReloadCallback)obs_get_encoder_properties, 190);
		encoder_properties->setFrameShape(QFrame::NoFrame);
		encoder_properties->setScrolling(true);
		QObject::connect(encoder_properties, &OBSPropertiesView::Changed, dialog, [this]() {
			RecordEditorChanges(preflight::bench::EncoderRole::Stream);
			stream_settings_pending = true;
			if (initial_output_mode == QStringLiteral("Simple") &&
			    current_simple_recording_quality == QStringLiteral("Stream")) {
				ApplySimpleRecordingQuality(current_simple_recording_quality);
			}
			InvalidateApplyPreview();
			settings_timer->start(600);
		});
		last_stream_editor_settings = json_object_from_settings(encoder_properties->GetSettings());

		recording_video_encoder_id = QString::fromUtf8(recording_video_id ? recording_video_id : "");
		OBSDataAutoRelease recording_property_settings = clone_settings(recording_video_settings);
		OBSDataAutoRelease initial_recording = clone_settings(recording_property_settings);
		if (!recording_property_settings || !initial_recording) {
			setup_error = QObject::tr("Recording encoder: settings unavailable");
			return false;
		}
		obs_data_addref(initial_recording);
		initial_recording_encoder_settings = initial_recording.Get();
		recording_encoder_properties =
			new OBSPropertiesView(recording_property_settings.Get(), recording_video_id,
					      (PropertiesReloadCallback)obs_get_encoder_properties, 190);
		recording_encoder_properties->setFrameShape(QFrame::NoFrame);
		recording_encoder_properties->setScrolling(true);
		QObject::connect(recording_encoder_properties, &OBSPropertiesView::Changed, dialog, [this]() {
			RecordEditorChanges(preflight::bench::EncoderRole::Recording);
			recording_settings_pending = true;
			InvalidateApplyPreview();
			settings_timer->start(600);
		});
		last_recording_editor_settings = json_object_from_settings(recording_encoder_properties->GetSettings());

		stream_video_encoder = obs_video_encoder_create(stream_video_id, "preflight_stream_video",
								stream_video_settings, nullptr);
		stream_audio_encoder = obs_audio_encoder_create(
			stream_audio_id, "preflight_stream_audio", stream_audio_settings,
			configured_stream_audio ? obs_encoder_get_mixer_index(configured_stream_audio) : 0, nullptr);
		recording_video_encoder = obs_video_encoder_create(recording_video_id, "preflight_recording_video",
								   recording_video_settings, nullptr);
		recording_audio_encoder = obs_audio_encoder_create(recording_audio_id, "preflight_recording_audio",
								   recording_audio_settings, recording_audio_mixer,
								   nullptr);
		if (!stream_video_encoder || !stream_audio_encoder || !recording_video_encoder ||
		    !recording_audio_encoder) {
			setup_error = QObject::tr("Bench encoders: could not create");
			return false;
		}

		video_t *video = obs_get_video();
		audio_t *audio = obs_get_audio();
		if (!video || !audio) {
			setup_error = QObject::tr("OBS audio/video: not initialized");
			return false;
		}
		struct obs_video_info active_video_info = {};
		if (!obs_get_video_info(&active_video_info) || active_video_info.fps_num == 0 ||
		    active_video_info.fps_den == 0) {
			setup_error = QObject::tr("OBS video: settings unavailable");
			return false;
		}
		video_settings.base_width = active_video_info.base_width;
		video_settings.base_height = active_video_info.base_height;
		video_settings.output_width = active_video_info.output_width;
		video_settings.output_height = active_video_info.output_height;
		video_settings.fps_num = active_video_info.fps_num;
		video_settings.fps_den = active_video_info.fps_den;
		video_settings.fps_divisor = 1;
		video_settings.fps_divisor_supported = true;
		initial_video_settings = video_settings;
		obs_encoder_set_video(stream_video_encoder, video);
		obs_encoder_set_audio(stream_audio_encoder, audio);
		obs_encoder_set_video(recording_video_encoder, video);
		obs_encoder_set_audio(recording_audio_encoder, audio);

		const char *service_output_type = obs_service_get_preferred_output_type(test_service);
		const char *stream_output_type =
			service_output_type && (obs_get_output_flags(service_output_type) & OBS_OUTPUT_SERVICE)
				? service_output_type
				: "rtmp_output";
		if ((obs_get_output_flags(stream_output_type) & OBS_OUTPUT_SERVICE) == 0) {
			setup_error = QObject::tr("Stream output: not available");
			return false;
		}

		stream_output = obs_output_create(stream_output_type, "preflight_test_stream", nullptr, nullptr);
		recording_output = obs_output_create("ffmpeg_muxer", "preflight_local_recording", nullptr, nullptr);
		if (!stream_output || !recording_output) {
			setup_error = QObject::tr("Bench outputs: could not create");
			return false;
		}

		OBSDataAutoRelease stream_output_settings = obs_output_get_settings(configured_stream);
		if (!stream_output_settings) {
			setup_error = QObject::tr("Stream output: settings unreadable");
			return false;
		}
		obs_output_update(stream_output, stream_output_settings);

		obs_output_set_video_encoder(stream_output, stream_video_encoder);
		obs_output_set_audio_encoder(stream_output, stream_audio_encoder, 0);
		obs_output_set_service(stream_output, test_service);
		obs_output_set_reconnect_settings(stream_output, 0, 0);
		obs_output_set_video_encoder(recording_output, recording_video_encoder);
		obs_output_set_audio_encoder(recording_output, recording_audio_encoder, 0);
		obs_output_set_mixers(recording_output, size_t{1} << recording_audio_mixer);

		collector = obs_preflight_collector_create();
		engine = obs_preflight_engine_create_with_window(SAMPLE_WINDOW_MS);
		if (!collector || !engine) {
			setup_error = QObject::tr("Stats collector: could not initialize");
			return false;
		}

		return true;
	}

	bool PrepareYouTubeTestKey()
	{
		youtube_test_error.clear();
		if (!requires_youtube_throwaway_stream) {
			return true;
		}
#if defined(YOUTUBE_ENABLED) && !defined(PREFLIGHT_UI_RENDER_TEST)
		OBSBasic *main = OBSBasic::Get();
		auto *youtube = main ? dynamic_cast<YoutubeApiWrappers *>(main->GetAuth()) : nullptr;
		if (!youtube || configured_service_name != youtube->service()) {
			youtube_test_error = QObject::tr("YouTube test key: account mismatch");
			return false;
		}

		// Match the wizard: a new non-reusable ingest stream key is created for each output start.
		StreamDescription stream = {"", "", "OBS Studio Test Stream"};
		if (!youtube->InsertStream(stream) || stream.name.isEmpty()) {
			youtube_test_error = QObject::tr("YouTube test key: could not create");
			return false;
		}

		OBSDataAutoRelease settings = obs_service_get_settings(test_service);
		if (!settings) {
			youtube_test_error = QObject::tr("YouTube test key: update failed");
			return false;
		}
		const QByteArray key = stream.name.toUtf8();
		obs_data_set_string(settings, "key", key.constData());
		obs_service_update(test_service, settings);
		return true;
#else
		youtube_test_error = QObject::tr("YouTube test key: unavailable in this build");
		return false;
#endif
	}

	void ToggleCloudflareSpeedTest()
	{
		if (cloudflare_speed_test.IsRunning()) {
			cloudflare_speed_test.Cancel();
			run_button->setEnabled(false);
			run_button->setText(QObject::tr("Stopping…"));
			SetStatus(QObject::tr("Stopping"));
			return;
		}
		if (!cloudflare_speed_test.Start()) {
			SetStatus(QObject::tr("Cloudflare test: could not start"));
			return;
		}
		run_button->setText(QObject::tr("Cancel speed test"));
		SetStatus(QObject::tr("Cloudflare · speed test only"));
		if (uplink_result_label) {
			uplink_result_label->setText(QObject::tr("Measuring · 4 MiB each way"));
		}
		uplink_timer->start(100);
	}

	void PollCloudflareSpeedTest()
	{
		auto result = cloudflare_speed_test.TakeResult();
		if (!result) {
			return;
		}
		uplink_timer->stop();
		run_button->setEnabled(true);
		run_button->setText(QObject::tr("Run speed test only"));
		if (result->cancelled) {
			SetStatus(QObject::tr("Stopped"));
			uplink_result_label->setText(QObject::tr("Incomplete"));
		} else if (!result->success) {
			SetStatus(QObject::tr("Cloudflare test: failed"));
			uplink_result_label->setText(result->error.empty()
							     ? QObject::tr("Cloudflare result: unavailable")
							     : QString::fromStdString(result->error));
		} else {
			SetStatus(QObject::tr("Complete"));
			uplink_result_label->setText(QObject::tr("Download %1 Mb/s · Upload %2 Mb/s")
							     .arg(result->download_mbps, 0, 'f', 1)
							     .arg(result->upload_mbps, 0, 'f', 1));
		}
		if (close_when_stopped) {
			close_when_stopped = false;
			dialog->close();
		}
	}

	bool BuildBakeoffPlan()
	{
		std::vector<std::string> encoders;
		for (size_t index = 0;; ++index) {
			const char *encoder_id = nullptr;
			if (!obs_enum_encoder_types(index, &encoder_id)) {
				break;
			}
			if (encoder_id && obs_get_encoder_type(encoder_id) == OBS_ENCODER_VIDEO) {
				encoders.emplace_back(encoder_id);
			}
		}
		std::sort(encoders.begin(), encoders.end());
		encoders.erase(std::unique(encoders.begin(), encoders.end()), encoders.end());
		if (encoders.empty()) {
			SetBakeoffStatus(QObject::tr("Video encoders: none registered"));
			return false;
		}

		struct obs_video_info video_info = {};
		if (!obs_get_video_info(&video_info) || video_info.output_width == 0 || video_info.output_height == 0 ||
		    video_info.fps_num == 0 || video_info.fps_den == 0) {
			SetBakeoffStatus(QObject::tr("OBS video: bake-off unavailable"));
			return false;
		}
		current_output_width = video_info.output_width;
		current_output_height = video_info.output_height;
		current_fps_num = video_info.fps_num;
		current_fps_den = video_info.fps_den;

		struct Scale {
			uint32_t numerator;
			uint32_t denominator;
		};
		constexpr std::array<Scale, 3> scales = {{{1, 1}, {2, 3}, {1, 2}}};
		constexpr std::array<uint32_t, 2> frame_rate_divisors = {1, 2};
		std::vector<obs_preflight_mode> modes;
		for (const Scale &scale : scales) {
			const uint64_t width =
				(static_cast<uint64_t>(current_output_width) * scale.numerator / scale.denominator) &
				~UINT64_C(1);
			const uint64_t height =
				(static_cast<uint64_t>(current_output_height) * scale.numerator / scale.denominator) &
				~UINT64_C(1);
			if (width == 0 || height == 0 || width > UINT32_MAX || height > UINT32_MAX) {
				continue;
			}
			for (uint32_t divisor : frame_rate_divisors) {
				const uint64_t fps_den = static_cast<uint64_t>(current_fps_den) * divisor;
				if (fps_den > UINT32_MAX) {
					continue;
				}
				const obs_preflight_mode mode = {
					.width = static_cast<uint32_t>(width),
					.height = static_cast<uint32_t>(height),
					.fps_num = current_fps_num,
					.fps_den = static_cast<uint32_t>(fps_den),
				};
				const auto duplicate =
					std::find_if(modes.begin(), modes.end(), [&mode](const auto &existing) {
						return existing.width == mode.width && existing.height == mode.height &&
						       existing.fps_num == mode.fps_num &&
						       existing.fps_den == mode.fps_den;
					});
				if (duplicate == modes.end()) {
					modes.push_back(mode);
				}
			}
		}
		if (modes.empty()) {
			SetBakeoffStatus(QObject::tr("Bake-off candidates: none available"));
			return false;
		}

		bakeoff_candidates.clear();
		bakeoff_candidates.reserve(encoders.size() * modes.size());
		for (const std::string &encoder_id : encoders) {
			for (const obs_preflight_mode &mode : modes) {
				bakeoff_candidates.push_back({encoder_id, mode});
			}
		}

		std::vector<const char *> encoder_ids;
		encoder_ids.reserve(encoders.size());
		for (const std::string &encoder_id : encoders) {
			encoder_ids.push_back(encoder_id.c_str());
		}
		bakeoff_planner = obs_preflight_bakeoff_create(encoder_ids.data(), encoder_ids.size(), modes.data(),
							       modes.size(), BAKEOFF_CANDIDATE_DURATION_MS);
		if (!bakeoff_planner) {
			SetBakeoffStatus(QObject::tr("Bake-off plan: could not build"));
			return false;
		}

		bakeoff_table->setRowCount(static_cast<int>(bakeoff_candidates.size()));
		for (size_t row = 0; row < bakeoff_candidates.size(); ++row) {
			const BakeoffCandidate &candidate = bakeoff_candidates[row];
			const QString fps =
				QString::number(static_cast<double>(candidate.mode.fps_num) / candidate.mode.fps_den,
						'f', candidate.mode.fps_den == 1 ? 0 : 2);
			QString encoder_label = QString::fromStdString(candidate.encoder_id);
			if (encoder_label.startsWith(QStringLiteral("obs_"))) {
				encoder_label.remove(0, 4);
			}
			if (encoder_label.endsWith(QStringLiteral("_tex"))) {
				encoder_label.chop(4);
			}
			encoder_label.replace(QLatin1Char('_'), QLatin1Char(' '));
			const QString candidate_details = QStringLiteral("%1 · %2×%3 · %4 fps")
								  .arg(encoder_label)
								  .arg(candidate.mode.width)
								  .arg(candidate.mode.height)
								  .arg(fps);
			auto *candidate_item = new QTableWidgetItem(encoder_label);
			candidate_item->setToolTip(
				QObject::tr("%1\nDouble-click to use these settings").arg(candidate_details));
			bakeoff_table->setItem(static_cast<int>(row), 0, candidate_item);
			auto *result_item = new QTableWidgetItem(QObject::tr("Waiting"));
			result_item->setToolTip(candidate_details);
			bakeoff_table->setItem(static_cast<int>(row), 1, result_item);
		}
		ResizeBakeoffTableToContents();
		SetBakeoffStatus(QObject::tr("%1 candidates · %2 s each")
					 .arg(bakeoff_candidates.size())
					 .arg(BAKEOFF_CANDIDATE_DURATION_MS / 1000));
		return true;
	}

	void SetBakeoffStatus(const QString &text)
	{
		if (bakeoff_status_label) {
			bakeoff_status_label->setText(text);
		}
	}

	void SetBakeoffUseActionsEnabled(bool enabled)
	{
		if (bakeoff_table) {
			bakeoff_table->setEnabled(enabled);
		}
	}

	void UseBakeoffCandidate(size_t row)
	{
		if (row >= bakeoff_candidates.size() || bench_running || bakeoff_running) {
			SetStatus(QObject::tr("Bench: stop before loading settings"));
			return;
		}
		const BakeoffCandidate &candidate = bakeoff_candidates[row];
		const QByteArray encoder_id = QByteArray::fromStdString(candidate.encoder_id);
		OBSDataAutoRelease settings = obs_encoder_defaults(encoder_id.constData());
		if (!settings) {
			SetBakeoffStatus(QObject::tr("Encoder settings: could not load"));
			return;
		}
		if (stream_bitrate_kbps > 0) {
			obs_data_set_int(settings, "bitrate", stream_bitrate_kbps);
		}
		if (test_service) {
			obs_service_apply_encoder_settings(test_service, settings, bakeoff_audio_settings);
		}
		preflight::bench::CandidateSettings selected;
		selected.stream_encoder.encoder_id = candidate.encoder_id;
		selected.stream_encoder.values = preflight::bench::SettingsObject::Retain(settings);
		selected.video.base_width = video_settings.base_width;
		selected.video.base_height = video_settings.base_height;
		selected.video.output_width = candidate.mode.width;
		selected.video.output_height = candidate.mode.height;
		selected.video.fps_num = candidate.mode.fps_num;
		selected.video.fps_den = candidate.mode.fps_den;
		selected.video.fps_divisor = current_fps_den && candidate.mode.fps_den % current_fps_den == 0
						     ? candidate.mode.fps_den / current_fps_den
						     : 0;
		selected.video.fps_divisor_supported = selected.video.fps_divisor > 0;
		if (!selected.video.fps_divisor_supported || !LoadCandidate(selected)) {
			SetBakeoffStatus(QObject::tr("Bake-off settings: unavailable while running"));
			return;
		}
		SetBakeoffStatus(QObject::tr("Loaded · not saved"));
	}

	void RenderImpactEntries(const std::vector<preflight::bench::ImpactEntry> &entries)
	{
		if (!impact_table) {
			return;
		}
		impact_table->setRowCount(static_cast<int>(entries.size()));
		auto window_text = [this](const std::optional<preflight::bench::MetricWindow> &window) {
			if (!window) {
				return QStringLiteral("—");
			}
			const QString values =
				window->gpu_percent
					? QStringLiteral("CPU %1% · GPU %2%")
						  .arg(window->value.cpu_percent, 0, 'f', 1)
						  .arg(*window->gpu_percent, 0, 'f', 1)
					: QStringLiteral("CPU %1%").arg(window->value.cpu_percent, 0, 'f', 1);
			return QStringLiteral("%1\n%2").arg(BakeoffVerdictText(window->value.verdict), values);
		};
		auto signed_value = [](double value, int precision) {
			const QString magnitude = QString::number(std::abs(value), 'f', precision);
			return (value < 0.0 ? QStringLiteral("−") : QStringLiteral("+")) + magnitude;
		};
		for (size_t row = 0; row < entries.size(); ++row) {
			const auto &entry = entries[row];
			const QString description = QString::fromStdString(entry.description);
			const QString key = QString::fromStdString(entry.key);
			QString change_label = description;
			if ((description == QStringLiteral("Stream encoder") ||
			     description == QStringLiteral("Stream bitrate")) &&
			    key == QStringLiteral("bitrate")) {
				change_label = QObject::tr("Stream bitrate");
			} else if (description == QStringLiteral("Live captions") && key == QStringLiteral("enabled")) {
				change_label = QObject::tr("Live captions");
			} else if (!key.isEmpty()) {
				change_label += QStringLiteral(" ") + key;
			}
			const QString change = QStringLiteral("%1 %2 → %3")
						       .arg(change_label, QString::fromStdString(entry.old_value),
							    QString::fromStdString(entry.new_value));
			QStringList delta_lines;
			QStringList full_delta_lines;
			if (entry.deltas) {
				const auto &delta = *entry.deltas;
				auto add_delta = [&delta_lines, &full_delta_lines,
						  &signed_value](const QString &name, double value, int precision,
								 const QString &unit) {
					const double smallest_visible = 0.5 * std::pow(10.0, -precision);
					const QString line = QStringLiteral("%1 %2 %3")
								     .arg(name, signed_value(value, precision), unit);
					full_delta_lines.push_back(line);
					if (std::abs(value) >= smallest_visible) {
						delta_lines.push_back(line);
					}
				};
				add_delta(QObject::tr("CPU"), delta.cpu_percentage_points, 1, QObject::tr("pp"));
				if (delta.gpu_percentage_points) {
					add_delta(QObject::tr("GPU"), *delta.gpu_percentage_points, 1,
						  QObject::tr("pp"));
				}
				add_delta(QObject::tr("Render"), delta.render_time_ms, 2, QObject::tr("ms"));
				add_delta(QObject::tr("Skipped"), delta.encoder_skipped_percentage_points, 2,
					  QObject::tr("pp"));
				add_delta(QObject::tr("Missed"), delta.missed_percentage_points, 2, QObject::tr("pp"));
				add_delta(QObject::tr("Dropped"), delta.output_dropped_percentage_points, 2,
					  QObject::tr("pp"));
				add_delta(QObject::tr("Bitrate"), delta.bitrate_kbps, 0, QObject::tr("kb/s"));
			}
			const QString state =
				entry.state == preflight::bench::ImpactState::Settling   ? QObject::tr("Settling")
				: entry.state == preflight::bench::ImpactState::Complete ? QObject::tr("Complete")
				: entry.state == preflight::bench::ImpactState::NoBaseline
					? QObject::tr("Not available")
					: QObject::tr("Waiting");
			const int index = static_cast<int>(row);
			auto *change_item = new QTableWidgetItem(change);
			auto *before_item = new QTableWidgetItem(window_text(entry.baseline));
			QFont compact_values_font = before_item->font();
			compact_values_font.setPointSizeF(9.0);
			compact_values_font.setStretch(90);
			before_item->setFont(compact_values_font);
			QString after_text = entry.after ? window_text(entry.after) : QStringLiteral("—");
			if (entry.state == preflight::bench::ImpactState::Settling) {
				after_text = QObject::tr("Settling");
			} else if (entry.state == preflight::bench::ImpactState::WaitingForBaseline) {
				after_text = QObject::tr("Waiting");
			}
			auto *after_item = new QTableWidgetItem(after_text);
			after_item->setFont(compact_values_font);
			auto *delta_item = new QTableWidgetItem(
				delta_lines.isEmpty() ? QStringLiteral("—")
						      : delta_lines.mid(0, 3).join(QLatin1Char('\n')));
			delta_item->setToolTip(full_delta_lines.isEmpty() ? QStringLiteral("—")
									  : full_delta_lines.join(QLatin1Char('\n')));
			QFont compact_delta_font = delta_item->font();
			compact_delta_font.setPointSizeF(8.5);
			delta_item->setFont(compact_delta_font);
			auto *state_item = new QTableWidgetItem(state);
			state_item->setData(Qt::UserRole, state);
			impact_table->setItem(index, 0, change_item);
			impact_table->setItem(index, 1, before_item);
			impact_table->setItem(index, 2, after_item);
			impact_table->setItem(index, 3, delta_item);
			impact_table->setItem(index, 4, state_item);
		}
		impact_table->resizeRowsToContents();
		const int content_height = impact_table->horizontalHeader()->sizeHint().height() +
					   impact_table->verticalHeader()->length() + impact_table->frameWidth() * 2 +
					   2;
		const int visible_height = std::min(content_height, MAX_IMPACT_TABLE_VIEW_HEIGHT);
		// Impact may scroll in a short window, so it must not impose its full content height.
		impact_table->setMinimumHeight(0);
		impact_table->setMaximumHeight(visible_height);
	}

	bool PrepareRecordingPath()
	{
		const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
		QDir directory(movies.isEmpty() ? QDir::tempPath() : movies);
		const QString output_directory = directory.filePath(QStringLiteral("OBS Preflight"));
		if (!QDir().mkpath(output_directory)) {
			SetStatus(QObject::tr("Recording folder: could not create"));
			return false;
		}

		recording_path = QDir(output_directory)
					 .filePath(QStringLiteral("Preflight-%1.mkv")
							   .arg(QDateTime::currentDateTime().toString(
								   QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
		OBSDataAutoRelease recording_settings = obs_output_get_settings(recording_output);
		if (!recording_settings) {
			SetStatus(QObject::tr("Recording settings: could not prepare"));
			return false;
		}
		obs_data_set_string(recording_settings, "path", recording_path.toUtf8().constData());
		obs_data_set_string(recording_settings, "muxer_settings", "");
		obs_data_set_bool(recording_settings, "allow_overwrite", false);
		obs_output_update(recording_output, recording_settings);
		if (recording_path_label) {
			recording_path_label->setText(QObject::tr("Recording: %1").arg(recording_path));
		}
		return true;
	}

	std::vector<CombinedChartSeries> NormalizedChartSeries() const
	{
		// Okabe–Ito palette from the P6c visual spec; status colours remain theme-driven.
		const std::array<QColor, 7> colors = {QColor("#56B4E9"), QColor("#0072B2"), QColor("#E69F00"),
						      QColor("#CC79A7"), QColor("#F0E442"), QColor("#D55E00"),
						      QColor("#009E73")};
		const std::array<QString, 7> names = {QObject::tr("CPU"),       QObject::tr("GPU"),
						      QObject::tr("Render"),    QObject::tr("Missed"),
						      QObject::tr("Skipped"),   QObject::tr("Dropped"),
						      QObject::tr("Congestion")};
		std::vector<CombinedChartSeries> result;
		result.reserve(names.size());
		for (int row = 0; row < static_cast<int>(names.size()); ++row) {
			CombinedChartSeries series;
			series.table_row = row;
			series.label = names[static_cast<size_t>(row)];
			series.color = colors[static_cast<size_t>(row)];
			series.points.reserve(points.size());
			for (const BenchPoint &point : points) {
				switch (row) {
				case 0:
					series.points.emplace_back(point.cpu_percent);
					break;
				case 1:
					series.points.push_back(point.gpu_percent);
					break;
				case 2:
					series.points.emplace_back(point.frame_interval_ms > 0.0
									   ? point.average_frame_time_ms /
										     point.frame_interval_ms * 100.0
									   : 0.0);
					break;
				case 3:
					series.points.emplace_back(point.render_lag_percent / OBS_STATS_DANGER_PERCENT *
								   100.0);
					break;
				case 4:
					series.points.emplace_back(point.encoder_skipped_percent /
								   OBS_STATS_DANGER_PERCENT * 100.0);
					break;
				case 5:
					series.points.emplace_back(point.dropped_percent / OBS_STATS_DANGER_PERCENT *
								   100.0);
					break;
				case 6:
					series.points.emplace_back(point.congestion_percent /
								   OBS_CONGESTION_DANGER_PERCENT * 100.0);
					break;
				}
			}
			result.push_back(std::move(series));
		}
		return result;
	}

	void ResizeBakeoffTableToContents()
	{
		if (!bakeoff_table) {
			return;
		}
		bakeoff_table->resizeRowsToContents();
		int content_height = bakeoff_table->horizontalHeader()->sizeHint().height() +
				     bakeoff_table->verticalHeader()->length() + bakeoff_table->frameWidth() * 2 + 2;
		content_height = std::max(content_height, bakeoff_table->horizontalHeader()->sizeHint().height() + 8);
		bakeoff_table->setFixedHeight(std::min(content_height, MAX_BAKEOFF_TABLE_VIEW_HEIGHT));
	}

	void ResizeMetricTableToContents()
	{
		if (!metric_table) {
			return;
		}
		constexpr int LEGEND_ROW_HEIGHT = 22;
		for (int row = 0; row < metric_table->rowCount(); ++row) {
			metric_table->setRowHeight(row, LEGEND_ROW_HEIGHT);
		}
		metric_table->setFixedHeight(metric_table->rowCount() * LEGEND_ROW_HEIGHT +
					     metric_table->frameWidth() * 2 + 2);
	}

	void ResizeApplyTableToContents()
	{
		if (!apply_table) {
			return;
		}
		apply_table->resizeRowsToContents();
		int content_height = apply_table->horizontalHeader()->sizeHint().height() +
				     apply_table->verticalHeader()->length() + apply_table->frameWidth() * 2 + 2;
		content_height = std::max(content_height, apply_table->horizontalHeader()->sizeHint().height() + 8);
		apply_table->setFixedHeight(std::min(content_height, 190));
	}

	void ResizeImpactPanelForWindow(int window_height)
	{
		if (!impact_group) {
			return;
		}
		const int maximum_height = std::clamp(window_height - IMPACT_GROUP_WINDOW_HEIGHT_OFFSET,
						      MIN_IMPACT_GROUP_VIEW_HEIGHT, MAX_IMPACT_GROUP_VIEW_HEIGHT);
		impact_group->setMaximumHeight(maximum_height);
	}

	void BuildUi()
	{
		auto *root = new QVBoxLayout(dialog);
		// Keep child size hints from forcing the initial window past the laptop-sized target.
		root->setSizeConstraint(QLayout::SetNoConstraint);
		auto *header = new QHBoxLayout;
		auto *title = new QLabel(QObject::tr("Preflight"), dialog);
		setClasses(title, "text-title");
		state_label = new QLabel(QObject::tr("Waiting"), dialog);
		setClasses(state_label, "text-muted");
		run_button = new QPushButton(
			cloudflare_speed_only ? QObject::tr("Run speed test") : QObject::tr("Start bench"), dialog);
		run_button->setObjectName(QStringLiteral("preflight-start-button"));
		header->addWidget(title);
		auto *summary = new QFrame(dialog);
		summary->setObjectName(QStringLiteral("preflight-verdict-summary"));
		auto *summary_layout = new QHBoxLayout(summary);
		verdict_label = new QLabel(QObject::tr("Waiting"), summary);
		verdict_label->setObjectName(QStringLiteral("preflight-verdict"));
		setClasses(verdict_label, "bg-info text-info");
		reason_label = new QLabel(QObject::tr("Waiting"), summary);
		reason_label->setObjectName(QStringLiteral("preflight-reason"));
		reason_label->setWordWrap(true);
		summary_layout->addWidget(verdict_label, 0, Qt::AlignVCenter);
		summary_layout->addWidget(reason_label, 1);
		header->addWidget(summary, 1);
		header->addWidget(run_button);
		root->addLayout(header);
		auto *status_row = new QHBoxLayout;
		state_label->setObjectName(QStringLiteral("preflight-operation-status"));
		status_row->addWidget(state_label, 1);

		QString destination;
		if (supported_test_mode.isEmpty()) {
			destination = QObject::tr("Stream service: not set");
		} else if (cloudflare_speed_only) {
			destination = supported_test_mode;
		} else {
			destination = QObject::tr("Test mode: %1").arg(supported_test_mode);
		}
		destination_label = new QLabel(destination, dialog);
		destination_label->setObjectName(QStringLiteral("preflight-destination"));
		destination_label->setWordWrap(true);
		setClasses(destination_label, "text-muted");
		destination_label->setVisible(!recorded_apply_preview);
		status_row->addWidget(destination_label, 2);
		if (cloudflare_speed_only) {
			auto *privacy_note = new QLabel(QObject::tr("Cloudflare · 4 MiB each way"), dialog);
			privacy_note->setWordWrap(true);
			status_row->addWidget(privacy_note, 2);
			uplink_result_label = new QLabel(QObject::tr("Waiting"), dialog);
			uplink_result_label->setObjectName(QStringLiteral("preflight-uplink-result"));
			uplink_result_label->setWordWrap(true);
			status_row->addWidget(uplink_result_label, 2);
		}
		root->addLayout(status_row);

		auto *one_screen = new QSplitter(Qt::Horizontal, dialog);
		one_screen->setObjectName(QStringLiteral("preflight-one-screen"));
		root->addWidget(one_screen, 1);
		auto *left_scroll = new QScrollArea(one_screen);
		left_scroll->setObjectName(QStringLiteral("preflight-settings-scroll"));
		left_scroll->setWidgetResizable(true);
		left_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		left_scroll->setMinimumWidth(0);
		auto *settings = new QWidget(left_scroll);
		settings->setObjectName(QStringLiteral("preflight-settings"));
		settings->setMinimumWidth(0);
		settings->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		auto *settings_layout = new QVBoxLayout(settings);
		settings_layout->setContentsMargins(4, 4, 8, 4);
		settings_layout->setSpacing(8);
		settings_layout->setSizeConstraint(QLayout::SetNoConstraint);
		left_scroll->setWidget(settings);
		one_screen->addWidget(left_scroll);

		auto *right_side = new QWidget(one_screen);
		right_side->setObjectName(QStringLiteral("preflight-right-side"));
		auto *right_layout = new QVBoxLayout(right_side);
		right_layout->setContentsMargins(8, 4, 4, 4);
		right_layout->setSpacing(4);
		one_screen->addWidget(right_side);
		one_screen->setStretchFactor(0, 0);
		one_screen->setStretchFactor(1, 1);
		one_screen->setSizes({290, 750});
		connect(one_screen, &QSplitter::splitterMoved, dialog,
			[this]() { QTimer::singleShot(0, dialog, [this]() { ResizeBakeoffTableToContents(); }); });

		auto *video_group = new QGroupBox(QObject::tr("Video"), settings);
		video_group->setObjectName(QStringLiteral("preflight-video-settings"));
		auto *video_form = new QFormLayout(video_group);
		resolution_combo = new QComboBox(video_group);
		resolution_combo->setObjectName(QStringLiteral("preflight-output-resolution"));
		fps_combo = new QComboBox(video_group);
		fps_combo->setObjectName(QStringLiteral("preflight-output-fps"));
		struct obs_video_info initial_video = {};
		const bool have_video = !recorded_apply_preview && obs_get_video_info(&initial_video);
		if (have_video && video_settings.base_width == 0) {
			video_settings = {initial_video.base_width,
					  initial_video.base_height,
					  initial_video.output_width,
					  initial_video.output_height,
					  initial_video.fps_num,
					  initial_video.fps_den,
					  1,
					  true};
			initial_video_settings = video_settings;
		}
		const uint32_t output_width = video_settings.output_width ? video_settings.output_width : 1920;
		const uint32_t output_height = video_settings.output_height ? video_settings.output_height : 1080;
		const std::array<std::pair<uint32_t, uint32_t>, 4> output_sizes = {
			std::pair{output_width, output_height}, std::pair{1920U, 1080U}, std::pair{1280U, 720U},
			std::pair{960U, 540U}};
		for (const auto &[width, height] : output_sizes) {
			const QString label = QStringLiteral("%1 × %2").arg(width).arg(height);
			if (resolution_combo->findText(label) < 0) {
				resolution_combo->addItem(label, QVariant::fromValue(QSize(static_cast<int>(width),
											   static_cast<int>(height))));
			}
		}
		const uint32_t base_fps_num = video_settings.fps_num ? video_settings.fps_num : 60;
		const uint32_t base_fps_den = video_settings.fps_den && video_settings.fps_divisor
						      ? video_settings.fps_den / video_settings.fps_divisor
						      : 1;
		for (uint32_t divisor = 1; divisor <= 4; ++divisor) {
			const double fps = static_cast<double>(base_fps_num) / (base_fps_den * divisor);
			fps_combo->addItem(QString::number(fps, 'f', std::abs(fps - std::round(fps)) < 0.001 ? 0 : 2),
					   QVariant::fromValue(divisor));
		}
		if (have_video) {
			const QString current_resolution =
				QStringLiteral("%1 × %2").arg(output_width).arg(output_height);
			resolution_combo->setCurrentText(current_resolution);
			fps_combo->setCurrentIndex(fps_combo->findData(
				static_cast<uint>(video_settings.fps_divisor ? video_settings.fps_divisor : 1)));
		}
		video_form->addRow(QObject::tr("Resolution"), resolution_combo);
		video_form->addRow(QObject::tr("FPS"), fps_combo);
		auto *canvas_note =
			new QLabel(QObject::tr("Base canvas: %1 × %2 (fixed while running)")
					   .arg(video_settings.base_width ? video_settings.base_width : 1920)
					   .arg(video_settings.base_height ? video_settings.base_height : 1080),
				   video_group);
		canvas_note->setObjectName(QStringLiteral("preflight-base-canvas-note"));
		canvas_note->setWordWrap(true);
		setClasses(canvas_note, "text-muted");
		video_form->addRow(QString(), canvas_note);
		settings_layout->addWidget(video_group);

		metric_table = new QTableWidget(4, 2, right_side);
		metric_table->setObjectName(QStringLiteral("preflight-metric-table"));
		metric_table->horizontalHeader()->hide();
		metric_table->verticalHeader()->hide();
		metric_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		metric_table->setSelectionBehavior(QAbstractItemView::SelectRows);
		metric_table->setSelectionMode(QAbstractItemView::NoSelection);
		metric_table->setShowGrid(false);
		metric_table->setWordWrap(false);
		metric_table->setFocusPolicy(Qt::NoFocus);
		metric_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
		metric_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		metric_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		const std::array<QColor, 7> metric_colors = {QColor("#56B4E9"), QColor("#0072B2"), QColor("#E69F00"),
							     QColor("#CC79A7"), QColor("#F0E442"), QColor("#D55E00"),
							     QColor("#009E73")};
		for (size_t row = 0; row < metric_rows.size(); ++row) {
			auto *cell = new QWidget(metric_table);
			cell->setObjectName(QStringLiteral("preflight-metric-cell-%1").arg(row));
			auto *cell_layout = new QHBoxLayout(cell);
			cell_layout->setContentsMargins(2, 0, 2, 0);
			cell_layout->setSpacing(4);
			auto *swatch = new QLabel(QStringLiteral("■"), cell);
			swatch->setObjectName(QStringLiteral("preflight-metric-swatch-%1").arg(row));
			swatch->setStyleSheet(
				QStringLiteral("color: %1").arg(metric_colors[row % metric_colors.size()].name()));
			auto *value = new QLabel(QStringLiteral("—"), cell);
			value->setObjectName(QStringLiteral("preflight-metric-value-%1").arg(row));
			QFont metric_font = value->font();
			metric_font.setPointSizeF(8.5);
			value->setFont(metric_font);
			setClasses(value, "text-bold");
			auto *suffix = new QLabel(cell);
			suffix->setObjectName(QStringLiteral("preflight-metric-suffix-%1").arg(row));
			suffix->setFont(metric_font);
			setClasses(suffix, "text-muted");
			cell_layout->addWidget(swatch);
			cell_layout->addWidget(value);
			cell_layout->addWidget(suffix);
			cell_layout->addStretch(1);
			metric_table->setCellWidget(static_cast<int>(row / 2), static_cast<int>(row % 2), cell);
			metric_rows[row] = {swatch, value, suffix};
		}
		metric_table->setVisible(false);
		ResizeMetricTableToContents();
		auto *metrics_group = new QGroupBox(QObject::tr("Live stats"), right_side);
		metrics_group->setObjectName(QStringLiteral("preflight-metrics"));
		auto *metrics_layout = new QVBoxLayout(metrics_group);
		metrics_layout->setContentsMargins(4, 4, 4, 4);
		metrics_layout->setSpacing(2);
		metrics_layout->addWidget(metric_table);
		combined_chart = new CombinedMetricChart(metrics_group);
		combined_chart->setObjectName(QStringLiteral("preflight-combined-chart"));
		metrics_layout->addWidget(combined_chart, 1);
		throughput_summary_label = new QLabel(QObject::tr("Waiting"), metrics_group);
		throughput_summary_label->setObjectName(QStringLiteral("preflight-throughput-summary"));
		setClasses(throughput_summary_label, "text-muted");
		throughput_summary_label->setVisible(false);
		metrics_layout->addWidget(throughput_summary_label, 0);
		right_layout->addWidget(metrics_group);

		auto *preview_group = new QGroupBox(QObject::tr("Live encoded preview"), right_side);
		preview_group->setObjectName(QStringLiteral("preflight-live-preview"));
		auto *preview_layout = new QVBoxLayout(preview_group);
		preview_layout->setContentsMargins(3, 3, 3, 3);
		loupe_status_label = new QLabel(QObject::tr("Waiting"), preview_group);
		loupe_status_label->setWordWrap(true);
		loupe_view = new LoupeView(preview_group);
		loupe_view->setObjectName(QStringLiteral("preflight-loupe-view"));
		loupe_view->setMaximumHeight(450);
		preview_layout->addWidget(loupe_view, 1);
		preview_layout->addWidget(loupe_status_label);
		right_layout->insertWidget(0, preview_group, 2);
		impact_group = new QGroupBox(QObject::tr("Impact"), right_side);
		impact_group->setObjectName(QStringLiteral("preflight-impact-group"));
		auto *impact_layout = new QVBoxLayout(impact_group);
		impact_layout->setContentsMargins(2, 1, 2, 2);
		impact_layout->setSpacing(0);
		impact_table = new QTableWidget(0, 5, impact_group);
		impact_table->setObjectName(QStringLiteral("preflight-impact-list"));
		impact_table->setHorizontalHeaderLabels({QObject::tr("Change"), QObject::tr("Before"),
							 QObject::tr("After"), QObject::tr("Δ"), QObject::tr("State")});
		impact_table->verticalHeader()->hide();
		impact_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
		impact_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		impact_table->setWordWrap(true);
		impact_table->setTextElideMode(Qt::ElideNone);
		QFont impact_font = impact_table->font();
		impact_font.setPointSizeF(8.0);
		impact_table->setFont(impact_font);
		QFont impact_header_font = impact_font;
		impact_header_font.setPointSizeF(7.5);
		impact_table->horizontalHeader()->setFont(impact_header_font);
		auto *impact_header = impact_table->horizontalHeader();
		impact_header->setStretchLastSection(false);
		impact_header->setSectionResizeMode(0, QHeaderView::Fixed);
		impact_header->setSectionResizeMode(1, QHeaderView::Fixed);
		impact_header->setSectionResizeMode(2, QHeaderView::Fixed);
		impact_header->setSectionResizeMode(3, QHeaderView::Stretch);
		impact_header->setSectionResizeMode(4, QHeaderView::Fixed);
		impact_header->setMinimumSectionSize(48);
		impact_table->setColumnWidth(0, 130);
		impact_table->setColumnWidth(1, 140);
		impact_table->setColumnWidth(2, 140);
		impact_table->setColumnWidth(4, 74);
		impact_table->setMaximumHeight(MAX_IMPACT_TABLE_VIEW_HEIGHT);
		impact_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		impact_table->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		impact_layout->addWidget(impact_table);
		impact_group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		impact_group->setMaximumHeight(MIN_IMPACT_GROUP_VIEW_HEIGHT);
		right_layout->addWidget(impact_group);
		// Preserve the full preview first, then give the chart the remaining height.
		// Impact uses its content height when it fits and scrolls when it does not.
		right_layout->setStretch(0, 9);
		right_layout->setStretch(1, 4);
		right_layout->setStretch(2, 0);

		auto *stream_group = new QGroupBox(QObject::tr("Stream encoder"), settings);
		stream_group->setObjectName(QStringLiteral("preflight-stream-settings"));
		stream_settings_layout = new QVBoxLayout(stream_group);
		if (encoder_properties) {
			stream_settings_layout->addWidget(encoder_properties);
		} else {
			stream_settings_layout->addWidget(
				new QLabel(setup_error.isEmpty() ? QObject::tr("Stream encoder: settings unavailable")
								 : setup_error,
					   stream_group));
			if (recorded_apply_preview) {
				QLabel *note = qobject_cast<QLabel *>(stream_settings_layout->itemAt(0)->widget());
				if (note) {
					note->setObjectName(QStringLiteral("preflight-stream-settings-note"));
					note->setText(QObject::tr("Settings not recorded"));
					setClasses(note, "text-muted");
				}
			}
		}
		settings_layout->addWidget(stream_group);
		auto *recording_group = new QGroupBox(QObject::tr("Recording encoder"), settings);
		recording_group->setObjectName(QStringLiteral("preflight-recording-settings"));
		auto *recording_layout = new QVBoxLayout(recording_group);
		if (initial_output_mode == QStringLiteral("Simple") && !recorded_apply_preview) {
			auto *quality_form = new QFormLayout;
			simple_recording_quality_combo = new QComboBox(recording_group);
			simple_recording_quality_combo->setObjectName(
				QStringLiteral("preflight-simple-recording-quality"));
			simple_recording_quality_combo->addItem(QObject::tr("Same as stream"),
								QStringLiteral("Stream"));
			simple_recording_quality_combo->addItem(QObject::tr("High quality, medium file"),
								QStringLiteral("Small"));
			simple_recording_quality_combo->addItem(QObject::tr("Indistinguishable, large file"),
								QStringLiteral("HQ"));
			simple_recording_quality_combo->addItem(QObject::tr("Lossless"), QStringLiteral("Lossless"));
			const int quality_index =
				simple_recording_quality_combo->findData(initial_simple_recording_quality);
			if (quality_index >= 0) {
				simple_recording_quality_combo->setCurrentIndex(quality_index);
			}
			simple_recording_quality_combo->setEnabled(recording_encoder_properties != nullptr);
			quality_form->addRow(QObject::tr("Simple recording quality"), simple_recording_quality_combo);
			recording_layout->addLayout(quality_form);
			QObject::connect(
				simple_recording_quality_combo, qOverload<int>(&QComboBox::currentIndexChanged), dialog,
				[this](int index) {
					if (index < 0 || recorded_apply_preview) {
						return;
					}
					const QString next = simple_recording_quality_combo->currentData().toString();
					if (next == current_simple_recording_quality) {
						return;
					}
					const QString previous = current_simple_recording_quality;
					if (!ApplySimpleRecordingQuality(next)) {
						const QSignalBlocker blocker(simple_recording_quality_combo);
						simple_recording_quality_combo->setCurrentIndex(
							simple_recording_quality_combo->findData(previous));
						SetStatus(QObject::tr("Recording quality: unsupported encoder"));
						return;
					}
					impact_tracker.RecordChanges({{"Recording quality", "RecQuality",
								       previous.toStdString(), next.toStdString()}});
					current_simple_recording_quality = next;
					RenderImpactEntries(impact_tracker.Entries());
					InvalidateApplyPreview();
				});
		}
		if (recording_encoder_properties) {
			recording_layout->addWidget(recording_encoder_properties);
		} else {
			auto *recording_note = new QLabel(QObject::tr("Settings unavailable"), recording_group);
			recording_note->setWordWrap(true);
			setClasses(recording_note, "text-muted");
			recording_layout->addWidget(recording_note);
		}
		settings_layout->addWidget(recording_group);
		auto *toggle_group = new QGroupBox(QObject::tr("Live filters"), settings);
		toggle_group->setObjectName(QStringLiteral("preflight-feature-toggles"));
		toggle_layout = new QVBoxLayout(toggle_group);
		captions_toggle = new VisibleCheckBox(QObject::tr("Live captions (on-device)"), toggle_group);
		captions_toggle->setObjectName(QStringLiteral("preflight-toggle-live-captions"));
		set_visible_checkbox_indicator(captions_toggle);
		captions_toggle->setEnabled(false);
		captions_toggle->setToolTip(QObject::tr("Not available"));
		toggle_layout->addWidget(captions_toggle);
		auto *toggle_note = new QLabel(QObject::tr("Applies immediately"), toggle_group);
		toggle_note->setObjectName(QStringLiteral("preflight-toggle-note"));
		toggle_note->setWordWrap(true);
		setClasses(toggle_note, "text-muted");
		toggle_layout->addWidget(toggle_note);
		settings_layout->addWidget(toggle_group);
		if (!recorded_apply_preview && setup_error.isEmpty()) {
			BindLiveCaptionsFilter();
		}

		auto *bakeoff_group = new QGroupBox(QObject::tr("Encoder bake-off"), settings);
		bakeoff_group->setObjectName(QStringLiteral("preflight-bakeoff-group"));
		bakeoff_group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
		auto *bakeoff_layout = new QVBoxLayout(bakeoff_group);
		bakeoff_status_label = new QLabel(QObject::tr("Waiting"), bakeoff_group);
		bakeoff_status_label->setWordWrap(true);
		auto *bakeoff_controls = new QHBoxLayout;
		bakeoff_button = new QPushButton(QObject::tr("Try every encoder…"), bakeoff_group);
		bakeoff_button->setObjectName(QStringLiteral("preflight-bakeoff-button"));
		bakeoff_controls->addStretch(1);
		bakeoff_controls->addWidget(bakeoff_button);
		bakeoff_table = new QTableWidget(0, 2, bakeoff_group);
		bakeoff_table->setObjectName(QStringLiteral("preflight-bakeoff-table"));
		bakeoff_table->setHorizontalHeaderLabels({QObject::tr("Candidate"), QObject::tr("Result")});
		bakeoff_table->verticalHeader()->hide();
		bakeoff_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		bakeoff_table->setSelectionBehavior(QAbstractItemView::SelectRows);
		bakeoff_table->setSortingEnabled(false);
		bakeoff_table->setWordWrap(true);
		bakeoff_table->setTextElideMode(Qt::ElideNone);
		bakeoff_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		bakeoff_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
		bakeoff_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
		bakeoff_table->horizontalHeader()->setStretchLastSection(false);
		bakeoff_table->horizontalHeader()->setMinimumSectionSize(56);
		bakeoff_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
		bakeoff_table->verticalHeader()->setMinimumSectionSize(28);
		bakeoff_table->setMinimumHeight(0);
		bakeoff_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		connect(bakeoff_table, &QTableWidget::cellDoubleClicked, dialog, [this](int row, int) {
			if (row >= 0) {
				UseBakeoffCandidate(static_cast<size_t>(row));
			}
		});
		bakeoff_layout->addWidget(bakeoff_status_label);
		bakeoff_layout->addLayout(bakeoff_controls);
		bakeoff_layout->addWidget(bakeoff_table);
		settings_layout->addWidget(bakeoff_group);

		recording_path_label = new QLabel(QObject::tr("Recording file: created on start"), settings);
		recording_path_label->setObjectName(QStringLiteral("preflight-recording-path"));
		recording_path_label->setWordWrap(true);
		setClasses(recording_path_label, "text-muted");
		settings_layout->addWidget(recording_path_label);

		if (encoder_properties || recorded_apply_preview) {
			apply_group = new QGroupBox(QObject::tr("Apply to OBS"), settings);
			apply_group->setObjectName(QStringLiteral("preflight-apply-preview"));
			auto *apply_layout = new QVBoxLayout(apply_group);
			apply_mode_label = new QLabel(apply_group);
			apply_mode_label->setWordWrap(true);
			apply_table = new QTableWidget(0, 3, apply_group);
			apply_table->setObjectName(QStringLiteral("preflight-apply-diff"));
			apply_table->setHorizontalHeaderLabels(
				{QObject::tr("Setting"), QObject::tr("Current"), QObject::tr("Proposed")});
			apply_table->verticalHeader()->hide();
			apply_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
			apply_table->setWordWrap(true);
			apply_table->setTextElideMode(Qt::ElideNone);
			apply_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			apply_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
			apply_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
			apply_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
			apply_table->setMaximumHeight(190);
			apply_table->setMinimumHeight(0);
			apply_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			apply_note = new QLabel(apply_group);
			apply_note->setObjectName(QStringLiteral("preflight-apply-note"));
			apply_note->setWordWrap(true);
			apply_button = new QPushButton(QObject::tr("Apply"), apply_group);
			apply_button->setObjectName(QStringLiteral("preflight-apply-button"));
			apply_button->setEnabled(false);
			apply_layout->addWidget(apply_mode_label);
			apply_layout->addWidget(apply_table);
			apply_layout->addWidget(apply_note);
			auto *apply_buttons = new QHBoxLayout;
			apply_buttons->addStretch(1);
			apply_buttons->addWidget(apply_button);
			apply_layout->addLayout(apply_buttons);
			settings_layout->addWidget(apply_group);
		}
		settings_layout->addStretch(1);
		QObject::connect(resolution_combo, qOverload<int>(&QComboBox::currentIndexChanged), dialog,
				 [this](int) { ApplyVideoControls(); });
		QObject::connect(fps_combo, qOverload<int>(&QComboBox::currentIndexChanged), dialog,
				 [this](int) { ApplyVideoControls(); });
		if (cloudflare_speed_only) {
			bakeoff_button->setEnabled(false);
			SetStatus(QObject::tr("Waiting · Cloudflare speed test"));
		} else if (!setup_error.isEmpty()) {
			SetStatus(recorded_apply_preview ? QObject::tr("Recorded preview") : setup_error);
			run_button->setEnabled(false);
			bakeoff_button->setEnabled(false);
		} else if (!supported_test_mode.isEmpty()) {
			SetStatus(QObject::tr("Waiting · %1").arg(supported_test_mode));
		}
	}

	void LoadRecordedData(const PreflightBenchWindowRecordedData &recorded_data)
	{
		for (size_t index = 0; index < recorded_data.windows.size(); ++index) {
			const preflight::gpu::Sample gpu = index < recorded_data.gpu_samples.size()
								   ? recorded_data.gpu_samples[index]
								   : preflight::gpu::Sample{};
			PushPoint(recorded_data.windows[index], gpu);
		}

		bakeoff_table->setRowCount(static_cast<int>(recorded_data.bakeoff_rows.size()));
		for (size_t row = 0; row < recorded_data.bakeoff_rows.size(); ++row) {
			const auto &recorded = recorded_data.bakeoff_rows[row];
			const auto &result = recorded.result;
			const int table_row = static_cast<int>(row);
			const QString candidate_details = recorded.encoder + QStringLiteral(" · ") +
							  recorded.resolution + QStringLiteral(" · ") + recorded.fps +
							  QStringLiteral(" fps");
			auto *candidate_item = new QTableWidgetItem(recorded.encoder);
			candidate_item->setToolTip(
				QObject::tr("%1\nDouble-click to use these settings").arg(candidate_details));
			bakeoff_table->setItem(table_row, 0, candidate_item);
			const QString result_detail =
				QStringLiteral("%1 ms headroom · %2% skipped · %3")
					.arg(result.render_headroom_ns / 1'000'000.0, 0, 'f', 2)
					.arg(result.encoder_skip_percent, 0, 'f', 2)
					.arg(QString::fromStdString(preflight::bench::BuildVerdictReason(result)));
			const QString result_summary = QStringLiteral("%1 · %2 ms")
							       .arg(BakeoffVerdictText(result.verdict))
							       .arg(result.render_headroom_ns / 1'000'000.0, 0, 'f', 2);
			auto *result_item = new QTableWidgetItem(result_summary);
			result_item->setToolTip(result_detail);
			bakeoff_table->setItem(table_row, 1, result_item);
		}
		ResizeBakeoffTableToContents();
		bakeoff_status_label->setText(QObject::tr("Recorded results"));
		if (loupe_view) {
			loupe_view->SetPairs(recorded_data.loupe_pair, recorded_data.recording_loupe_pair
									       ? recorded_data.recording_loupe_pair
									       : recorded_data.loupe_pair);
		}
		if (loupe_status_label) {
			loupe_status_label->setText(recorded_data.loupe_pair ? QObject::tr("Recorded preview")
									     : QObject::tr("Not available"));
		}
		RenderImpactEntries(recorded_data.impacts);
		for (const auto &toggle : recorded_data.toggles) {
			if (toggle.name == "Live captions (on-device)" && captions_toggle) {
				captions_toggle->setChecked(toggle.enabled);
				captions_toggle->setEnabled(toggle.available);
				if (!toggle.status.empty()) {
					captions_toggle->setToolTip(QString::fromStdString(toggle.status));
				}
			}
		}
	}

	void PushPoint(const obs_preflight_window &window, const preflight::gpu::Sample &gpu = {})
	{
		if (window.end_ms <= last_window_end_ms) {
			return;
		}
		last_window_end_ms = window.end_ms;
		BenchPoint point;
		point.end_ms = window.end_ms;
		point.encoder_frames = window.encoder_frames;
		point.cpu_percent = window.cpu_percent;
		point.gpu_percent = gpu.gpu_percent;
		point.gpu_renderer_percent = gpu.renderer_percent;
		point.gpu_tiler_percent = gpu.tiler_percent;
		point.gpu_video_encode_percent = gpu.video_encode_percent;
		point.average_frame_time_ms = window.average_frame_time_ns / 1'000'000.0;
		point.frame_interval_ms = window.frame_interval_ns / 1'000'000.0;
		point.encoder_skipped_percent = window.encoder_skip_percent;
		point.render_lag_percent = window.render_lag_percent;
		point.dropped_percent = window.output_drop_percent;
		point.bitrate_kbps = window.output_bitrate_kbps;
		point.congestion_percent = window.congestion_percent;
		point.connect_time_ms = static_cast<double>(window.connect_time_ms);
		point.verdict = window.verdict;
		points.push_back(std::move(point));
		const preflight::bench::MetricWindow completed{++impact_window_sequence, window,
							       gpu.gpu_percent,          gpu.renderer_percent,
							       gpu.tiler_percent,        gpu.video_encode_percent};
		metric_history.push_back(completed);
		if (metric_history.size() > MAX_ENGINE_WINDOWS) {
			metric_history.erase(
				metric_history.begin(),
				metric_history.begin() +
					static_cast<std::ptrdiff_t>(metric_history.size() - MAX_ENGINE_WINDOWS));
		}
		impact_tracker.ObserveCompleteWindow(completed);
		RenderImpactEntries(impact_tracker.Entries());
		if (points.size() > CHART_HISTORY_SIZE) {
			points.erase(points.begin(),
				     points.begin() + static_cast<std::ptrdiff_t>(points.size() - CHART_HISTORY_SIZE));
		}
		UpdateCharts();
		UpdateVerdict();
	}

	void UpdateCharts()
	{
		if (points.empty()) {
			return;
		}
		const BenchPoint &point = points.back();
		auto band = [](double value, double warning, double danger) {
			if (value > danger) {
				return std::pair{QObject::tr("Danger"), "text-danger"};
			}
			if (value > warning) {
				return std::pair{QObject::tr("Warning"), "text-warning"};
			}
			return std::pair{QObject::tr("OK"), ""};
		};
		auto marker = [](const char *status_class) {
			return strcmp(status_class, "text-danger") == 0    ? QStringLiteral("✕ ")
			       : strcmp(status_class, "text-warning") == 0 ? QStringLiteral("▲ ")
									   : QString();
		};
		auto status_color = [this](const char *status_class) {
			return status_class[0] ? theme_class_color(dialog, status_class)
					       : dialog->palette().color(QPalette::Text);
		};
		const double render_fraction =
			point.frame_interval_ms > 0.0 ? point.average_frame_time_ms / point.frame_interval_ms : 0.0;
		const auto render_status =
			band(render_fraction, OBS_RENDER_WARNING_FRACTION, OBS_RENDER_DANGER_FRACTION);
		const auto missed_status =
			band(point.render_lag_percent, OBS_STATS_WARNING_PERCENT, OBS_STATS_DANGER_PERCENT);
		auto skipped_status =
			band(point.encoder_skipped_percent, OBS_STATS_WARNING_PERCENT, OBS_STATS_DANGER_PERCENT);
		if (metric_history.back().value.encoder_frames_skipped > OBS_ENCODER_SKIP_STATUS_MIN_COUNT &&
		    point.encoder_skipped_percent >= OBS_ENCODER_SKIP_STATUS_PERCENT &&
		    point.encoder_skipped_percent <= OBS_STATS_WARNING_PERCENT) {
			/* Match the status-bar overload marker even below the Stats warning band. */
			skipped_status = {QObject::tr("Warning"), "text-warning"};
		}
		const auto dropped_status =
			band(point.dropped_percent, OBS_STATS_WARNING_PERCENT, OBS_STATS_DANGER_PERCENT);
		const auto congestion_status =
			band(point.congestion_percent, OBS_CONGESTION_WARNING_PERCENT, OBS_CONGESTION_DANGER_PERCENT);
		const double interval_ms = point.frame_interval_ms;
		const auto encoder_frames_status = point.encoder_frames == 0
							   ? std::pair{QObject::tr("Fail"), "text-danger"}
							   : std::pair{QObject::tr("OK"), ""};
		const QString stats_limits = QStringLiteral("%1 · %2")
						     .arg(OBS_STATS_WARNING_PERCENT, 0, 'f', 0)
						     .arg(OBS_STATS_DANGER_PERCENT, 0, 'f', 0);
		const QString render_limits = QStringLiteral("%1 · %2 ms")
						      .arg(interval_ms * OBS_RENDER_WARNING_FRACTION, 0, 'f', 2)
						      .arg(interval_ms * OBS_RENDER_DANGER_FRACTION, 0, 'f', 2);
		const QString congestion_limits = QStringLiteral("%1 · %2 %")
							  .arg(OBS_CONGESTION_WARNING_PERCENT, 0, 'f', 2)
							  .arg(OBS_CONGESTION_DANGER_PERCENT, 0, 'f', 2);
		const std::array<QString, 8> values = {
			QStringLiteral("CPU %1 %").arg(point.cpu_percent, 0, 'f', 1),
			point.gpu_percent ? QStringLiteral("GPU %1 %").arg(*point.gpu_percent, 0, 'f', 1)
					  : QObject::tr("GPU unavailable"),
			QStringLiteral("Render %1%2")
				.arg(marker(render_status.second))
				.arg(point.average_frame_time_ms, 0, 'f', 2),
			QStringLiteral("Missed %1%2 %")
				.arg(marker(missed_status.second))
				.arg(point.render_lag_percent, 0, 'f', 2),
			QStringLiteral("Skipped %1%2 %")
				.arg(marker(skipped_status.second))
				.arg(point.encoder_skipped_percent, 0, 'f', 2),
			QStringLiteral("Dropped %1%2 %")
				.arg(marker(dropped_status.second))
				.arg(point.dropped_percent, 0, 'f', 2),
			QStringLiteral("Congestion %1%2 %")
				.arg(marker(congestion_status.second))
				.arg(point.congestion_percent, 0, 'f', 1),
			QStringLiteral("Encoder %1%2 frames")
				.arg(marker(encoder_frames_status.second))
				.arg(point.encoder_frames),
		};
		const std::array<QString, 8> suffixes = {
			QString(),
			QString(),
			QStringLiteral("/ %1").arg(render_limits),
			QStringLiteral("/ %1 %").arg(stats_limits),
			QStringLiteral("/ %1 %").arg(stats_limits),
			QStringLiteral("/ %1 %").arg(stats_limits),
			QStringLiteral("/ %1").arg(congestion_limits),
			QString(),
		};
		const std::array<const char *, 8> classes = {
			"",
			"",
			render_status.second,
			missed_status.second,
			skipped_status.second,
			dropped_status.second,
			congestion_status.second,
			encoder_frames_status.second,
		};
		const std::array<QString, 8> tooltip_limits = {
			QObject::tr("No OBS CPU warning threshold. Information only."),
			QObject::tr("No OBS GPU warning threshold. Information only."),
			QStringLiteral("Warning %1 ms · danger %2 ms · OBS frame timing")
				.arg(interval_ms * OBS_RENDER_WARNING_FRACTION, 0, 'f', 2)
				.arg(interval_ms * OBS_RENDER_DANGER_FRACTION, 0, 'f', 2),
			QStringLiteral("Warning %1% · danger %2% · OBS Stats")
				.arg(OBS_STATS_WARNING_PERCENT, 0, 'f', 0)
				.arg(OBS_STATS_DANGER_PERCENT, 0, 'f', 0),
			QStringLiteral("Warning %1% · danger %2% · OBS encoder status bar")
				.arg(OBS_STATS_WARNING_PERCENT, 0, 'f', 0)
				.arg(OBS_STATS_DANGER_PERCENT, 0, 'f', 0),
			QStringLiteral("Warning %1% · danger %2% · OBS Stats")
				.arg(OBS_STATS_WARNING_PERCENT, 0, 'f', 0)
				.arg(OBS_STATS_DANGER_PERCENT, 0, 'f', 0),
			QStringLiteral("Warning %1% · danger %2% · OBS wizard congestion")
				.arg(OBS_CONGESTION_WARNING_PERCENT, 0, 'f', 2)
				.arg(OBS_CONGESTION_DANGER_PERCENT, 0, 'f', 2),
			QObject::tr("Fail when the encoder emits no frames in the sample window."),
		};
		for (size_t index = 0; index < metric_rows.size(); ++index) {
			metric_rows[index].value->setText(values[index]);
			metric_rows[index].suffix->setText(suffixes[index]);
			metric_rows[index].value->setToolTip(tooltip_limits[index]);
			metric_rows[index].suffix->setToolTip(tooltip_limits[index]);
			metric_rows[index].swatch->setToolTip(tooltip_limits[index]);
			setClasses(metric_rows[index].value, classes[index]);
		}
		if (point.encoder_frames == 0) {
			metric_rows[7].swatch->setStyleSheet(
				QStringLiteral("color: %1").arg(theme_class_color(dialog, "text-danger").name()));
		} else {
			metric_rows[7].swatch->setStyleSheet(QStringLiteral("color: palette(mid)"));
		}
		const QString bitrate = QStringLiteral("%1 kb/s").arg(point.bitrate_kbps, 0, 'f', 0);
		const QString connect_time =
			point.connect_time_ms <= 0
				? QObject::tr("connect waiting")
				: QStringLiteral("connect %1 ms").arg(point.connect_time_ms, 0, 'f', 0);
		throughput_summary_label->setText(QStringLiteral("%1 · %2").arg(bitrate, connect_time));
		ResizeMetricTableToContents();
		combined_chart->SetSeries(NormalizedChartSeries());

		const std::array<QColor, 7> series_colors = {
			QColor("#56B4E9"), QColor("#0072B2"), QColor("#E69F00"), QColor("#CC79A7"),
			QColor("#F0E442"), QColor("#D55E00"), QColor("#009E73"),
		};
		std::vector<ChartHoverSample> hover_samples;
		hover_samples.reserve(points.size());
		for (const BenchPoint &sample_point : points) {
			const double sample_interval = sample_point.frame_interval_ms;
			const double sample_render_ratio =
				sample_interval > 0.0 ? sample_point.average_frame_time_ms / sample_interval : 0.0;
			auto sample_band = [&band](double value, double warning, double danger) {
				return band(value, warning, danger);
			};
			auto add_line = [&sample_band, &marker, &status_color,
					 &series_colors](ChartHoverSample &sample, const QString &name,
							 const QString &value, const QString &limits,
							 size_t series_index, std::pair<QString, const char *> state) {
				const QString label =
					name + QLatin1Char(' ') + marker(state.second) + value +
					(limits.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(limits));
				sample.lines.push_back(
					{label, series_colors[series_index], status_color(state.second)});
			};
			ChartHoverSample sample;
			const double relative_seconds =
				(sample_point.end_ms <= point.end_ms
					 ? static_cast<double>(point.end_ms - sample_point.end_ms)
					 : 0.0) /
				1000.0;
			sample.time = QStringLiteral("t −%1 s").arg(relative_seconds, 0, 'f', 1);
			sample.lines.push_back({QStringLiteral("CPU %1 %").arg(sample_point.cpu_percent, 0, 'f', 1),
						series_colors[0], dialog->palette().color(QPalette::Text)});
			if (sample_point.gpu_percent) {
				sample.lines.push_back(
					{QStringLiteral("GPU %1 %").arg(*sample_point.gpu_percent, 0, 'f', 1),
					 series_colors[1], dialog->palette().color(QPalette::Text)});
			} else {
				sample.lines.push_back({QObject::tr("GPU unavailable"), series_colors[1],
							dialog->palette().color(QPalette::Text)});
			}
			add_line(sample, QObject::tr("Render"),
				 QStringLiteral("%1 ms · %2% (w%3/d%4 ms)")
					 .arg(sample_point.average_frame_time_ms, 0, 'f', 2)
					 .arg(sample_render_ratio * 100.0, 0, 'f', 0)
					 .arg(sample_interval * OBS_RENDER_WARNING_FRACTION, 0, 'f', 2)
					 .arg(sample_interval * OBS_RENDER_DANGER_FRACTION, 0, 'f', 2),
				 QString(), 2,
				 sample_band(sample_render_ratio, OBS_RENDER_WARNING_FRACTION,
					     OBS_RENDER_DANGER_FRACTION));
			const auto missed_status = sample_band(sample_point.render_lag_percent,
							       OBS_STATS_WARNING_PERCENT, OBS_STATS_DANGER_PERCENT);
			const auto sample_skip = sample_band(sample_point.encoder_skipped_percent,
							     OBS_STATS_WARNING_PERCENT, OBS_STATS_DANGER_PERCENT);
			const char *missed_skip_class = missed_status.second;
			if (strcmp(sample_skip.second, "text-danger") == 0 ||
			    (!missed_skip_class[0] && sample_skip.second[0])) {
				missed_skip_class = sample_skip.second;
			}
			const QString missed_skipped = QStringLiteral("Missed %1%2% · Skipped %3%4% (w%5%/d%6%)")
							       .arg(marker(missed_status.second))
							       .arg(sample_point.render_lag_percent, 0, 'f', 2)
							       .arg(marker(sample_skip.second))
							       .arg(sample_point.encoder_skipped_percent, 0, 'f', 2)
							       .arg(OBS_STATS_WARNING_PERCENT, 0, 'f', 0)
							       .arg(OBS_STATS_DANGER_PERCENT, 0, 'f', 0);
			sample.lines.push_back({missed_skipped, series_colors[3], status_color(missed_skip_class)});
			add_line(sample, QObject::tr("Dropped"),
				 QStringLiteral("%1%").arg(sample_point.dropped_percent, 0, 'f', 2),
				 QStringLiteral("w%1%/d%2%")
					 .arg(OBS_STATS_WARNING_PERCENT, 0, 'f', 0)
					 .arg(OBS_STATS_DANGER_PERCENT, 0, 'f', 0),
				 5,
				 sample_band(sample_point.dropped_percent, OBS_STATS_WARNING_PERCENT,
					     OBS_STATS_DANGER_PERCENT));
			add_line(sample, QObject::tr("Congestion"),
				 QStringLiteral("%1%").arg(sample_point.congestion_percent, 0, 'f', 1),
				 QStringLiteral("w%1%/d%2%")
					 .arg(OBS_CONGESTION_WARNING_PERCENT, 0, 'f', 2)
					 .arg(OBS_CONGESTION_DANGER_PERCENT, 0, 'f', 2),
				 6,
				 sample_band(sample_point.congestion_percent, OBS_CONGESTION_WARNING_PERCENT,
					     OBS_CONGESTION_DANGER_PERCENT));
			const bool no_encoder_frames = sample_point.encoder_frames == 0;
			const QColor encoder_status_color = no_encoder_frames ? status_color("text-danger")
									      : dialog->palette().color(QPalette::Mid);
			sample.lines.push_back(
				{QStringLiteral("Encoder %1%2 frames")
					 .arg(no_encoder_frames ? QStringLiteral("✕ ") : QString())
					 .arg(sample_point.encoder_frames),
				 encoder_status_color,
				 no_encoder_frames ? encoder_status_color : dialog->palette().color(QPalette::Text)});
			sample.lines.push_back({QStringLiteral("Bitrate %1 kb/s · connect %2 ms")
							.arg(sample_point.bitrate_kbps, 0, 'f', 0)
							.arg(sample_point.connect_time_ms, 0, 'f', 0),
						series_colors[0], dialog->palette().color(QPalette::Text)});
			hover_samples.push_back(std::move(sample));
		}
		combined_chart->SetHoverSamples(std::move(hover_samples));
	}

	void UpdateVerdict()
	{
		if (points.empty()) {
			return;
		}
		const auto &point = points.back();
		const QString verdict = point.verdict == OBS_PREFLIGHT_PASS       ? QObject::tr("Pass")
					: point.verdict == OBS_PREFLIGHT_MARGINAL ? QObject::tr("Marginal")
										  : QObject::tr("Fail");
		verdict_label->setText(verdict);
		reason_label->setText(
			QString::fromStdString(preflight::bench::BuildVerdictReason(metric_history.back().value)));
		const char *verdict_class = point.verdict == OBS_PREFLIGHT_PASS ? "bg-success text-success text-bold"
					    : point.verdict == OBS_PREFLIGHT_MARGINAL
						    ? "bg-warning text-warning text-bold"
						    : "bg-danger text-danger text-bold";
		setClasses(verdict_label, verdict_class);
	}

	QString BakeoffVerdictText(int32_t verdict) const
	{
		if (verdict == OBS_PREFLIGHT_PASS) {
			return QObject::tr("Pass");
		}
		if (verdict == OBS_PREFLIGHT_MARGINAL) {
			return QObject::tr("Marginal");
		}
		return QObject::tr("Fail");
	}

	void SetBakeoffCell(int column, const QString &text)
	{
		if (bakeoff_table && active_bakeoff_row < static_cast<size_t>(bakeoff_table->rowCount())) {
			bakeoff_table->item(static_cast<int>(active_bakeoff_row), column)->setText(text);
			ResizeBakeoffTableToContents();
		}
	}

	void SetCandidateMetrics(const obs_preflight_window &window)
	{
		candidate_render_headroom_ns = window.render_headroom_ns;
		candidate_encoder_skip_percent = window.encoder_skip_percent;
		bakeoff_candidate_reason = QString::fromStdString(preflight::bench::BuildVerdictReason(window));
		SetBakeoffCell(1, QStringLiteral("%1 · %2 ms")
					  .arg(BakeoffVerdictText(candidate_verdict))
					  .arg(candidate_render_headroom_ns / 1'000'000.0, 0, 'f', 2));
		if (bakeoff_table && active_bakeoff_row < static_cast<size_t>(bakeoff_table->rowCount())) {
			bakeoff_table->item(static_cast<int>(active_bakeoff_row), 1)
				->setToolTip(QStringLiteral("%1 ms headroom · %2% skipped · %3")
						     .arg(candidate_render_headroom_ns / 1'000'000.0, 0, 'f', 2)
						     .arg(candidate_encoder_skip_percent, 0, 'f', 2)
						     .arg(bakeoff_candidate_reason));
		}
	}

	void CompleteBakeoffCandidate(int32_t verdict, const QString &reason)
	{
		if (!bakeoff_candidate_active || !bakeoff_planner) {
			return;
		}
		candidate_verdict = std::max(candidate_verdict, verdict);
		const QString final_reason = reason.isEmpty() ? bakeoff_candidate_reason : reason;
		const int32_t status = obs_preflight_bakeoff_complete(bakeoff_planner, candidate_verdict);
		if (status != OBS_PREFLIGHT_OK) {
			SetBakeoffStatus(QObject::tr("Bake-off result: could not record"));
		}
		bakeoff_candidate_active = false;
		SetBakeoffCell(1, QStringLiteral("%1 · %2 ms")
					  .arg(BakeoffVerdictText(candidate_verdict))
					  .arg(candidate_render_headroom_ns / 1'000'000.0, 0, 'f', 2));
		bakeoff_table->item(static_cast<int>(active_bakeoff_row), 1)
			->setToolTip(
				QStringLiteral("%1 ms headroom · %2% skipped\n%3")
					.arg(candidate_render_headroom_ns / 1'000'000.0, 0, 'f', 2)
					.arg(candidate_encoder_skip_percent, 0, 'f', 2)
					.arg(final_reason.isEmpty() ? QObject::tr("Not available") : final_reason));
		bakeoff_table->scrollToItem(bakeoff_table->item(static_cast<int>(active_bakeoff_row), 0));
		bakeoff_waiting_for_stop = true;
		StopLoupe();
		if (stream_output && obs_output_active(stream_output)) {
			obs_output_stop(stream_output);
			sample_timer->start(100);
			SetBakeoffStatus(QObject::tr("Candidate %1 / %2 · stopping")
						 .arg(active_candidate.order)
						 .arg(bakeoff_candidates.size()));
		} else {
			QTimer::singleShot(0, dialog, [this]() { ContinueBakeoffAfterStop(); });
		}
	}

	void FinishBakeoff(bool cancelled)
	{
		StopLoupe();
		if (stream_output && !obs_output_active(stream_output)) {
			obs_output_set_video_encoder(stream_output, stream_video_encoder);
		}
		bakeoff_encoder = nullptr;
		bakeoff_running = false;
		bakeoff_waiting_for_stop = false;
		bakeoff_candidate_active = false;
		bakeoff_candidate_has_samples = false;
		bakeoff_cancelled = false;
		sample_timer->stop();
		obs_preflight_engine_destroy(bakeoff_engine);
		bakeoff_engine = nullptr;
		obs_preflight_bakeoff_destroy(bakeoff_planner);
		bakeoff_planner = nullptr;
		bakeoff_button->setEnabled(setup_error.isEmpty());
		bakeoff_button->setText(QObject::tr("Run bake-off"));
		run_button->setEnabled(setup_error.isEmpty());
		if (encoder_properties) {
			encoder_properties->setEnabled(true);
		}
		SetBakeoffUseActionsEnabled(true);
		SetBakeoffStatus(cancelled ? QObject::tr("Stopped · completed rows kept")
					   : QObject::tr("Complete · %1 candidates").arg(bakeoff_candidates.size()));
		SetStatus(cancelled ? QObject::tr("Stopped") : QObject::tr("Complete"));
		if (close_when_stopped) {
			close_when_stopped = false;
			dialog->close();
		}
	}

	void ContinueBakeoffAfterStop()
	{
		if (!bakeoff_waiting_for_stop) {
			return;
		}
		bakeoff_waiting_for_stop = false;
		if (stream_output && !obs_output_active(stream_output)) {
			obs_output_set_video_encoder(stream_output, stream_video_encoder);
		}
		bakeoff_encoder = nullptr;
		if (bakeoff_engine) {
			obs_preflight_engine_destroy(bakeoff_engine);
		}
		bakeoff_engine = obs_preflight_engine_create_with_window(SAMPLE_WINDOW_MS);
		if (bakeoff_cancelled) {
			FinishBakeoff(true);
			return;
		}
		if (!bakeoff_engine) {
			SetBakeoffStatus(QObject::tr("Bake-off window: could not create"));
			FinishBakeoff(true);
			return;
		}
		StartNextBakeoffCandidate();
	}

	void StartNextBakeoffCandidate()
	{
		if (!bakeoff_running || !bakeoff_planner) {
			return;
		}
		active_candidate = {};
		const int32_t next_status = obs_preflight_bakeoff_next(bakeoff_planner, &active_candidate);
		if (next_status == OBS_PREFLIGHT_COMPLETE) {
			FinishBakeoff(false);
			return;
		}
		if (next_status != OBS_PREFLIGHT_OK || active_candidate.order == 0 ||
		    active_candidate.order > bakeoff_candidates.size()) {
			SetBakeoffStatus(QObject::tr("Bake-off candidate: could not read"));
			FinishBakeoff(true);
			return;
		}

		active_bakeoff_row = active_candidate.order - 1;
		bakeoff_candidate_active = true;
		bakeoff_candidate_has_samples = false;
		candidate_verdict = OBS_PREFLIGHT_PASS;
		candidate_render_headroom_ns = 0;
		candidate_encoder_skip_percent = 0.0;
		candidate_last_window_end_ms = 0;
		bakeoff_candidate_reason.clear();
		SetBakeoffCell(1, QObject::tr("Running"));
		if (!PrepareYouTubeTestKey()) {
			bakeoff_cancelled = true;
			CompleteBakeoffCandidate(OBS_PREFLIGHT_FAIL, youtube_test_error);
			return;
		}

		OBSDataAutoRelease settings = obs_encoder_defaults(active_candidate.encoder);
		if (!settings) {
			CompleteBakeoffCandidate(OBS_PREFLIGHT_FAIL, QObject::tr("Encoder settings: could not create"));
			return;
		}
		if (stream_bitrate_kbps > 0) {
			obs_data_set_int(settings, "bitrate", stream_bitrate_kbps);
		}
		obs_service_apply_encoder_settings(test_service, settings, bakeoff_audio_settings);
		bakeoff_encoder = obs_video_encoder_create(active_candidate.encoder, "preflight_bakeoff_video",
							   settings, nullptr);
		if (!bakeoff_encoder) {
			CompleteBakeoffCandidate(OBS_PREFLIGHT_FAIL,
						 QObject::tr("Encoder: could not create on this device"));
			return;
		}

		video_t *video = obs_get_video();
		if (!video || active_candidate.fps_num != current_fps_num || current_fps_den == 0 ||
		    active_candidate.fps_den % current_fps_den != 0) {
			CompleteBakeoffCandidate(OBS_PREFLIGHT_FAIL,
						 QObject::tr("Video mode: FPS candidate unsupported"));
			return;
		}
		obs_encoder_set_video(bakeoff_encoder, video);
		if (active_candidate.width != current_output_width ||
		    active_candidate.height != current_output_height) {
			obs_encoder_set_scaled_size(bakeoff_encoder, active_candidate.width, active_candidate.height);
		}
		const uint32_t divisor = active_candidate.fps_den / current_fps_den;
		if (divisor == 0 || !obs_encoder_set_frame_rate_divisor(bakeoff_encoder, divisor)) {
			CompleteBakeoffCandidate(OBS_PREFLIGHT_FAIL, QObject::tr("Encoder: FPS divisor unsupported"));
			return;
		}

		obs_output_set_video_encoder(stream_output, bakeoff_encoder);
		StartLoupe(bakeoff_encoder);
		if (!obs_output_start(stream_output)) {
			CompleteBakeoffCandidate(OBS_PREFLIGHT_FAIL,
						 output_error(stream_output,
							      QObject::tr("Test output: could not start")));
			return;
		}
		bakeoff_elapsed.start();
		sample_timer->start(static_cast<int>(SAMPLE_WINDOW_MS));
		SetBakeoffStatus(QObject::tr("Candidate %1 / %2 · %3")
					 .arg(active_candidate.order)
					 .arg(bakeoff_candidates.size())
					 .arg(QString::fromUtf8(active_candidate.encoder)));
	}

	void StopBakeoff()
	{
		if (!bakeoff_running) {
			return;
		}
		bakeoff_cancelled = true;
		bakeoff_button->setEnabled(false);
		bakeoff_button->setText(QObject::tr("Stopping…"));
		if (bakeoff_candidate_active) {
			SetBakeoffCell(1, QObject::tr("Stopped\nCandidate: incomplete"));
			bakeoff_candidate_active = false;
		}
		bakeoff_waiting_for_stop = true;
		StopLoupe();
		if (stream_output && obs_output_active(stream_output)) {
			obs_output_stop(stream_output);
			sample_timer->start(100);
		} else {
			QTimer::singleShot(0, dialog, [this]() { ContinueBakeoffAfterStop(); });
		}
		SetBakeoffStatus(QObject::tr("Stopping"));
	}

	void SampleBakeoff()
	{
		if (!bakeoff_running || !bakeoff_candidate_active) {
			return;
		}
		if (!stream_output || !obs_output_active(stream_output)) {
			CompleteBakeoffCandidate(OBS_PREFLIGHT_FAIL,
						 output_error(stream_output,
							      QObject::tr("Test output: stopped unexpectedly")));
			return;
		}
		video_t *video = obs_encoder_video(bakeoff_encoder);
		if (obs_preflight_collect_sample(collector, bakeoff_engine, stream_output, video) == OBS_PREFLIGHT_OK) {
			size_t count = 0;
			if (obs_preflight_engine_get_window_count(bakeoff_engine, &count) == OBS_PREFLIGHT_OK &&
			    count > 0) {
				obs_preflight_window window = {};
				if (obs_preflight_engine_get_window(bakeoff_engine, count - 1, &window) ==
					    OBS_PREFLIGHT_OK &&
				    window.end_ms > candidate_last_window_end_ms) {
					candidate_last_window_end_ms = window.end_ms;
					bakeoff_candidate_has_samples = true;
					SetCandidateMetrics(window);
					PushPoint(window);
					if (window.verdict >= candidate_verdict) {
						candidate_verdict = window.verdict;
						bakeoff_candidate_reason = QString::fromStdString(
							preflight::bench::BuildVerdictReason(window));
					}
					if (candidate_verdict == OBS_PREFLIGHT_FAIL) {
						CompleteBakeoffCandidate(candidate_verdict, bakeoff_candidate_reason);
						return;
					}
				}
			}
		}

		if (bakeoff_elapsed.elapsed() >= static_cast<qint64>(active_candidate.duration_ms)) {
			if (!bakeoff_candidate_has_samples) {
				CompleteBakeoffCandidate(OBS_PREFLIGHT_FAIL, QObject::tr("Encoder: no usable samples"));
			} else {
				CompleteBakeoffCandidate(candidate_verdict, bakeoff_candidate_reason);
			}
		}
	}

	void RecordEditorChanges(preflight::bench::EncoderRole role)
	{
		OBSPropertiesView *view = role == preflight::bench::EncoderRole::Stream ? encoder_properties
											: recording_encoder_properties;
		QJsonObject &previous = role == preflight::bench::EncoderRole::Stream ? last_stream_editor_settings
										      : last_recording_editor_settings;
		if (!view) {
			return;
		}
		const QJsonObject current = json_object_from_settings(view->GetSettings());
		QStringList keys;
		for (auto it = previous.constBegin(); it != previous.constEnd(); ++it) {
			if (current.value(it.key()) != it.value()) {
				keys.push_back(it.key());
			}
		}
		for (auto it = current.constBegin(); it != current.constEnd(); ++it) {
			if (!previous.contains(it.key())) {
				keys.push_back(it.key());
			}
		}
		std::sort(keys.begin(), keys.end());
		std::vector<preflight::bench::SettingChange> changes;
		const std::string description = role == preflight::bench::EncoderRole::Stream ? "Stream encoder"
											      : "Recording encoder";
		for (const QString &key : keys) {
			const QString old_value = preflight::apply::ValueForDisplay(previous.value(key));
			const QString new_value = preflight::apply::ValueForDisplay(current.value(key));
			if (old_value != new_value) {
				changes.push_back({description, key.toStdString(), old_value.toStdString(),
						   new_value.toStdString()});
			}
		}
		previous = current;
		if (!changes.empty()) {
			impact_tracker.RecordChanges(changes);
			RenderImpactEntries(impact_tracker.Entries());
		}
	}

	void ApplyVideoControls()
	{
		if (!resolution_combo || !fps_combo || recorded_apply_preview || resolution_combo->currentIndex() < 0 ||
		    fps_combo->currentIndex() < 0) {
			return;
		}
		const QSize output_size = resolution_combo->currentData().toSize();
		const uint32_t requested_divisor = fps_combo->currentData().toUInt();
		if (output_size.width() <= 0 || output_size.height() <= 0 || requested_divisor == 0 ||
		    video_settings.fps_divisor == 0) {
			return;
		}
		const uint32_t base_den = video_settings.fps_den / video_settings.fps_divisor;
		preflight::bench::VideoSettings requested = video_settings;
		requested.output_width = static_cast<uint32_t>(output_size.width());
		requested.output_height = static_cast<uint32_t>(output_size.height());
		requested.fps_divisor = requested_divisor;
		requested.fps_den = base_den * requested_divisor;
		requested.fps_divisor_supported = true;
		std::vector<preflight::bench::SettingChange> changes;
		const QString old_resolution =
			QStringLiteral("%1 × %2").arg(video_settings.output_width).arg(video_settings.output_height);
		const QString new_resolution =
			QStringLiteral("%1 × %2").arg(requested.output_width).arg(requested.output_height);
		if (old_resolution != new_resolution) {
			changes.push_back({"Video", "Output resolution", old_resolution.toStdString(),
					   new_resolution.toStdString()});
		}
		const QString old_fps =
			QString::number(static_cast<double>(video_settings.fps_num) / video_settings.fps_den, 'f', 2);
		const QString new_fps =
			QString::number(static_cast<double>(requested.fps_num) / requested.fps_den, 'f', 2);
		if (requested_divisor != video_settings.fps_divisor) {
			changes.push_back({"Video", "FPS", old_fps.toStdString(), new_fps.toStdString()});
		}
		if (requested.output_width != video_settings.output_width ||
		    requested.output_height != video_settings.output_height ||
		    requested.fps_den != video_settings.fps_den) {
			SetVideoSettings(requested, changes);
			settings_timer->start(600);
		}
	}

	bool RecreateVideoEncoders()
	{
		if (!video_settings_pending || !encoder_properties || !recording_encoder_properties ||
		    !video_settings.fps_divisor_supported) {
			return !video_settings_pending;
		}
		video_t *video = obs_get_video();
		if (!video) {
			SetStatus(QObject::tr("OBS video: unavailable"));
			return false;
		}
		OBSDataAutoRelease stream_settings = clone_settings(encoder_properties->GetSettings());
		OBSDataAutoRelease recording_settings = clone_settings(recording_encoder_properties->GetSettings());
		if (!stream_settings || !recording_settings) {
			SetStatus(QObject::tr("Bench encoder settings: unreadable"));
			return false;
		}
		if (test_service) {
			obs_service_apply_encoder_settings(test_service, stream_settings, bakeoff_audio_settings);
		}
		const QByteArray stream_id = stream_video_encoder_id.toUtf8();
		const QByteArray recording_id = recording_video_encoder_id.toUtf8();
		OBSEncoderAutoRelease new_stream(obs_video_encoder_create(
			stream_id.constData(), "preflight_stream_video", stream_settings, nullptr));
		OBSEncoderAutoRelease new_recording(obs_video_encoder_create(
			recording_id.constData(), "preflight_recording_video", recording_settings, nullptr));
		if (!new_stream || !new_recording) {
			SetStatus(QObject::tr("Video encoders: could not recreate"));
			return false;
		}
		obs_encoder_set_video(new_stream, video);
		obs_encoder_set_video(new_recording, video);
		obs_encoder_set_scaled_size(new_stream, video_settings.output_width, video_settings.output_height);
		obs_encoder_set_scaled_size(new_recording, video_settings.output_width, video_settings.output_height);
		if (!obs_encoder_set_frame_rate_divisor(new_stream, video_settings.fps_divisor) ||
		    !obs_encoder_set_frame_rate_divisor(new_recording, video_settings.fps_divisor)) {
			SetStatus(QObject::tr("Video FPS: encoder divisor unsupported"));
			return false;
		}
		if (stream_output) {
			obs_output_set_video_encoder(stream_output, nullptr);
		}
		if (recording_output) {
			obs_output_set_video_encoder(recording_output, nullptr);
		}
		stream_video_encoder = obs_encoder_get_ref(new_stream);
		recording_video_encoder = obs_encoder_get_ref(new_recording);
		if (stream_output) {
			obs_output_set_video_encoder(stream_output, stream_video_encoder);
		}
		if (recording_output) {
			obs_output_set_video_encoder(recording_output, recording_video_encoder);
		}
		current_output_width = video_settings.output_width;
		current_output_height = video_settings.output_height;
		current_fps_num = video_settings.fps_num;
		current_fps_den = video_settings.fps_den;
		video_settings_pending = false;
		if (engine) {
			obs_preflight_engine_destroy(engine);
			engine = obs_preflight_engine_create_with_window(SAMPLE_WINDOW_MS);
			last_window_end_ms = 0;
		}
		return engine != nullptr;
	}

	void RestartRequestedOutputs()
	{
		if (!restart_stream_when_stopped && !restart_recording_when_stopped) {
			return;
		}
		if (restart_stream_when_stopped && !PrepareYouTubeTestKey()) {
			StopOutputs(QObject::tr("Stopping"), youtube_test_error);
			return;
		}
		if (restart_recording_when_stopped) {
			StartRecordingLoupe();
			if (!obs_output_start(recording_output)) {
				restart_recording_when_stopped = false;
				StopOutputs(
					output_error(recording_output, QObject::tr("Recording: could not restart")));
				return;
			}
			restart_recording_when_stopped = false;
		}
		if (restart_stream_when_stopped) {
			StartLoupe(stream_video_encoder);
			if (!obs_output_start(stream_output)) {
				restart_stream_when_stopped = false;
				StopOutputs(output_error(stream_output, QObject::tr("Stream test: could not restart")));
				return;
			}
			restart_stream_when_stopped = false;
		}
		bench_running = true;
		StartGpuSampling();
		sample_timer->start(static_cast<int>(SAMPLE_WINDOW_MS));
		SetStatus(QObject::tr("Running"));
	}

	void UpdateEncoderSettings()
	{
		RefreshApplyPreview();
		if (video_settings_pending) {
			const bool stream_active = stream_output && obs_output_active(stream_output);
			const bool recording_active = recording_output && obs_output_active(recording_output);
			if (stream_active || recording_active) {
				restart_stream_when_stopped = stream_active;
				restart_recording_when_stopped = recording_active;
				StopGpuSampling();
				StopLoupe();
				if (stream_active) {
					obs_output_stop(stream_output);
				}
				if (recording_active) {
					obs_output_stop(recording_output);
				}
				sample_timer->start(100);
				SetStatus(QObject::tr("Restarting · video settings"));
				return;
			}
			if (!RecreateVideoEncoders()) {
				return;
			}
			RestartRequestedOutputs();
		}

		if (stream_settings_pending && stream_video_encoder && encoder_properties) {
			if (stream_output && obs_output_active(stream_output)) {
				if (!restart_stream_when_stopped) {
					restart_stream_when_stopped = true;
					StopGpuSampling();
					loupe_capture.Stop();
					obs_output_stop(stream_output);
				}
				SetStatus(QObject::tr("Restarting · stream encoder"));
				sample_timer->start(100);
				return;
			}
			obs_encoder_update(stream_video_encoder, encoder_properties->GetSettings());
			stream_settings_pending = false;
			if (restart_stream_when_stopped) {
				RestartRequestedOutputs();
			}
		}

		if (recording_settings_pending && recording_video_encoder && recording_encoder_properties) {
			if (recording_output && obs_output_active(recording_output)) {
				if (!restart_recording_when_stopped) {
					restart_recording_when_stopped = true;
					StopGpuSampling();
					recording_loupe_capture.Stop();
					obs_output_stop(recording_output);
				}
				SetStatus(QObject::tr("Restarting · recording encoder"));
				sample_timer->start(100);
				return;
			}
			obs_encoder_update(recording_video_encoder, recording_encoder_properties->GetSettings());
			recording_settings_pending = false;
			if (restart_recording_when_stopped) {
				RestartRequestedOutputs();
			}
		}
	}
};

PreflightBenchWindow::PreflightBenchWindow(QWidget *parent) : PreflightBenchWindow(parent, nullptr) {}

PreflightBenchWindow::PreflightBenchWindow(const PreflightBenchWindowRecordedData &recorded_data, QWidget *parent)
	: PreflightBenchWindow(parent, &recorded_data)
{
}

PreflightBenchWindow::PreflightBenchWindow(QWidget *parent, const PreflightBenchWindowRecordedData *recorded_data)
	: QDialog(parent),
	  impl(std::make_unique<Impl>(this))
{
	setObjectName(QStringLiteral("preflight-bench-window"));
	setWindowTitle(tr("Preflight"));
	impl->sample_timer = new QTimer(this);
	impl->sample_timer->setInterval(static_cast<int>(SAMPLE_WINDOW_MS));
	connect(impl->sample_timer, &QTimer::timeout, this, &PreflightBenchWindow::Sample);
	impl->gpu_sample_timer = new QTimer(this);
	connect(impl->gpu_sample_timer, &QTimer::timeout, this, [this]() { impl->SampleGpu(); });
	impl->loupe_timer = new QTimer(this);
	impl->loupe_timer->setInterval(100);
	connect(impl->loupe_timer, &QTimer::timeout, this, [this]() { impl->UpdateLoupe(); });
	impl->settings_timer = new QTimer(this);
	impl->settings_timer->setSingleShot(true);
	connect(impl->settings_timer, &QTimer::timeout, this, &PreflightBenchWindow::ApplyEncoderSettings);
	impl->uplink_timer = new QTimer(this);
	impl->uplink_timer->setInterval(100);
	connect(impl->uplink_timer, &QTimer::timeout, this, [this]() { impl->PollCloudflareSpeedTest(); });
	if (recorded_data) {
		impl->setup_error = tr("Recorded preview");
		impl->recorded_apply_preview = recorded_data->apply_preview;
		impl->enable_recorded_apply_controls_for_test = recorded_data->enable_recorded_apply_controls_for_test;
	} else {
		impl->BuildOutputs();
	}
	impl->BuildUi();
	impl->RefreshApplyPreview();
	if (recorded_data) {
		impl->LoadRecordedData(*recorded_data);
	}
	connect(impl->run_button, &QPushButton::clicked, this, &PreflightBenchWindow::ToggleBench);
	connect(impl->bakeoff_button, &QPushButton::clicked, this, &PreflightBenchWindow::ToggleBakeoff);
	if (impl->apply_button) {
		connect(impl->apply_button, &QPushButton::clicked, this, [this]() { impl->ApplyConfigChanges(); });
	}
	impl->ResizeBakeoffTableToContents();
	impl->ResizeApplyTableToContents();
	// The settings column gets its final width only after the dialog is shown. Recompute wrapped row heights
	// after that first layout so the bake-off table fits its visible rows without blank space below them.
	QTimer::singleShot(0, this, [this]() { impl->ResizeBakeoffTableToContents(); });
	const QSize preferred_size(1040, 720);
	const QScreen *screen = QGuiApplication::primaryScreen();
	resize(screen ? preferred_size.boundedTo(screen->availableGeometry().size()) : preferred_size);
}

PreflightBenchWindow::~PreflightBenchWindow() = default;

preflight::bench::DataApi *PreflightBenchWindow::GetDataApi()
{
	return impl.get();
}

void PreflightBenchWindow::closeEvent(QCloseEvent *event)
{
	if (impl->cloudflare_speed_test.IsRunning()) {
		impl->close_when_stopped = true;
		impl->cloudflare_speed_test.Cancel();
		impl->run_button->setEnabled(false);
		impl->run_button->setText(tr("Stopping…"));
		impl->SetStatus(tr("Stopping"));
		impl->uplink_timer->start(100);
		event->ignore();
		return;
	}
	if (impl->bakeoff_running || impl->bakeoff_waiting_for_stop) {
		impl->close_when_stopped = true;
		if (impl->bakeoff_running && !impl->bakeoff_cancelled) {
			impl->StopBakeoff();
		}
		event->ignore();
		return;
	}
	if (impl->bench_running || impl->stopping) {
		impl->close_when_stopped = true;
		if (!impl->stopping) {
			impl->StopOutputs(tr("Stopping"), tr("Stopped"));
		}
		event->ignore();
		return;
	}
	event->accept();
}

void PreflightBenchWindow::resizeEvent(QResizeEvent *event)
{
	QDialog::resizeEvent(event);
	if (impl) {
		impl->ResizeImpactPanelForWindow(event->size().height());
	}
}

void PreflightBenchWindow::ToggleBench()
{
	if (impl->cloudflare_speed_only) {
		impl->ToggleCloudflareSpeedTest();
		return;
	}
	if (impl->bench_running) {
		impl->StopOutputs(tr("Stopping"), tr("Stopped"));
		return;
	}
	if (impl->bakeoff_running || impl->stopping) {
		impl->SetStatus(tr("Preflight: stop before starting"));
		return;
	}

	if (!impl->setup_error.isEmpty()) {
		impl->SetStatus(impl->setup_error);
		return;
	}
	if (!impl->PrepareYouTubeTestKey()) {
		impl->SetStatus(impl->youtube_test_error);
		return;
	}
	if (!impl->PrepareRecordingPath()) {
		return;
	}
	if (!obs_output_start(impl->recording_output)) {
		impl->SetStatus(output_error(impl->recording_output, tr("Recording: could not start")));
		return;
	}
	impl->StartRecordingLoupe();
	impl->StartLoupe(impl->stream_video_encoder);
	if (!obs_output_start(impl->stream_output)) {
		const QString error = output_error(impl->stream_output, tr("Stream test: could not start"));
		impl->StopOutputs(tr("Stream test failed · stopping recording"), error);
		return;
	}
	impl->bench_running = true;
	impl->StartGpuSampling();
	impl->run_button->setText(tr("Stop bench"));
	impl->SetStatus(tr("Running · private stream + local recording"));
	impl->sample_timer->start();
}

void PreflightBenchWindow::ToggleBakeoff()
{
	if (impl->cloudflare_speed_only) {
		impl->SetBakeoffStatus(tr("Bake-off: service test mode required"));
		return;
	}
	if (impl->bakeoff_running) {
		impl->StopBakeoff();
		return;
	}
	if (!impl->setup_error.isEmpty()) {
		impl->SetBakeoffStatus(impl->setup_error);
		return;
	}
	if (impl->bench_running || impl->stopping || (impl->stream_output && obs_output_active(impl->stream_output))) {
		impl->SetBakeoffStatus(tr("Bench: stop before bake-off"));
		return;
	}
	if (!impl->BuildBakeoffPlan()) {
		return;
	}
	impl->bakeoff_engine = obs_preflight_engine_create_with_window(SAMPLE_WINDOW_MS);
	if (!impl->bakeoff_engine) {
		impl->SetBakeoffStatus(tr("Bake-off engine: could not create"));
		obs_preflight_bakeoff_destroy(impl->bakeoff_planner);
		impl->bakeoff_planner = nullptr;
		return;
	}
	impl->bakeoff_running = true;
	impl->bakeoff_cancelled = false;
	impl->SetBakeoffUseActionsEnabled(false);
	impl->bakeoff_button->setText(tr("Stop bake-off"));
	impl->run_button->setEnabled(false);
	if (impl->encoder_properties) {
		impl->encoder_properties->setEnabled(false);
	}
	impl->SetStatus(tr("Running · private test output"));
	impl->StartNextBakeoffCandidate();
}

void PreflightBenchWindow::Sample()
{
	if (impl->stopping) {
		const bool stream_stopped = !impl->stream_output || !obs_output_active(impl->stream_output);
		const bool recording_stopped = !impl->recording_output || !obs_output_active(impl->recording_output);
		if (stream_stopped && recording_stopped) {
			impl->FinishStopping();
		}
		return;
	}
	if (impl->bakeoff_waiting_for_stop) {
		if (!impl->stream_output || !obs_output_active(impl->stream_output)) {
			impl->ContinueBakeoffAfterStop();
		}
		return;
	}
	if (impl->bakeoff_running) {
		impl->SampleBakeoff();
		return;
	}
	const bool stream_active = impl->stream_output && obs_output_active(impl->stream_output);
	const bool recording_active = impl->recording_output && obs_output_active(impl->recording_output);
	if ((impl->restart_stream_when_stopped && !stream_active) ||
	    (impl->restart_recording_when_stopped && !recording_active) ||
	    (impl->video_settings_pending && !stream_active && !recording_active)) {
		impl->UpdateEncoderSettings();
		return;
	}

	if (!impl->stream_output || !obs_output_active(impl->stream_output)) {
		if (impl->restart_stream_when_stopped) {
			impl->UpdateEncoderSettings();
			return;
		}
		if (impl->bench_running) {
			impl->StopOutputs(tr("Stopping recording · stream ended"),
					  output_error(impl->stream_output, tr("Stream test: stopped")));
		}
		return;
	}

	if (impl->recording_output && !obs_output_active(impl->recording_output)) {
		impl->StopOutputs(tr("Stopping stream · recording ended"),
				  output_error(impl->recording_output, tr("Recording: stopped")));
		return;
	}

	video_t *video = obs_encoder_video(impl->stream_video_encoder);
	const int32_t status = obs_preflight_collect_sample(impl->collector, impl->engine, impl->stream_output, video);
	if (status != OBS_PREFLIGHT_OK) {
		return;
	}

	size_t count = 0;
	if (obs_preflight_engine_get_window_count(impl->engine, &count) != OBS_PREFLIGHT_OK) {
		return;
	}
	if (count >= MAX_ENGINE_WINDOWS) {
		obs_preflight_engine_destroy(impl->engine);
		impl->engine = obs_preflight_engine_create_with_window(SAMPLE_WINDOW_MS);
		impl->last_window_end_ms = 0;
		impl->gpu_window_average.TakeAverage();
		return;
	}
	if (count == 0) {
		return;
	}
	obs_preflight_window window = {};
	if (obs_preflight_engine_get_window(impl->engine, count - 1, &window) == OBS_PREFLIGHT_OK) {
		impl->PushPoint(window, impl->gpu_window_average.TakeAverage());
	}
}

void PreflightBenchWindow::ApplyEncoderSettings()
{
	impl->UpdateEncoderSettings();
}
