#pragma once

#include "obs-preflight.h"
#include "preflight-apply-plan.hpp"
#include "preflight-bench-data.hpp"
#include "preflight-gpu-sampler.hpp"
#include "preflight-loupe-core.hpp"

#include <QDialog>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

class QCloseEvent;
class QResizeEvent;

struct PreflightBenchWindowRecordedBakeoffRow {
	QString encoder;
	QString resolution;
	QString fps;
	obs_preflight_window result{};
};

struct PreflightBenchWindowRecordedData {
	std::vector<obs_preflight_window> windows;
	std::vector<preflight::gpu::Sample> gpu_samples;
	std::vector<PreflightBenchWindowRecordedBakeoffRow> bakeoff_rows;
	std::shared_ptr<const preflight::loupe::FramePair> loupe_pair;
	std::shared_ptr<const preflight::loupe::FramePair> recording_loupe_pair;
	std::vector<preflight::bench::ImpactEntry> impacts;
	std::vector<preflight::bench::FeatureToggle> toggles;
	std::optional<preflight::apply::Plan> apply_preview;
	bool enable_recorded_apply_controls_for_test = false;
};

class PreflightBenchWindow : public QDialog {
	Q_OBJECT

	struct Impl;
	std::unique_ptr<Impl> impl;
	PreflightBenchWindow(QWidget *parent, const PreflightBenchWindowRecordedData *recorded_data);

public:
	explicit PreflightBenchWindow(QWidget *parent = nullptr);
	PreflightBenchWindow(const PreflightBenchWindowRecordedData &recorded_data, QWidget *parent = nullptr);
	~PreflightBenchWindow() override;
	preflight::bench::DataApi *GetDataApi();

protected:
	void closeEvent(QCloseEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private slots:
	void ToggleBench();
	void ToggleBakeoff();
	void Sample();
	void ApplyEncoderSettings();
};
