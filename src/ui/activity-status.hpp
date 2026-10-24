// Clip Atlas — a compact native activity pill with a floating, keyboard-accessible step panel.
#pragma once

#include "activity-model.hpp"

#include <QWidget>

#include <memory>

namespace clip_atlas {

class ActivityStatus : public QWidget {
public:
	explicit ActivityStatus(QWidget *parent = nullptr);
	~ActivityStatus() override;
	void on_activity(const ActivityEvent &event);
	void stop();
	void set_expanded(bool expanded);
	bool is_expanded() const;
	// Also useful for hosts with their own reduced-motion setting.
	void set_reduced_motion(bool reduced);

protected:
	bool eventFilter(QObject *object, QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace clip_atlas
