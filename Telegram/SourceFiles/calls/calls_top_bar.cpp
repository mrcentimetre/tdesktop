/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/calls_top_bar.h"

#include "ui/effects/cross_line.h"
#include "ui/paint/blobs_linear.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/chat/group_call_bar.h" // Ui::GroupCallBarContent.
#include "ui/layers/generic_box.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/text/format_values.h"
#include "lang/lang_keys.h"
#include "core/application.h"
#include "calls/calls_call.h"
#include "calls/calls_instance.h"
#include "calls/calls_signal_bars.h"
#include "calls/calls_group_panel.h" // LeaveGroupCallBox.
#include "history/view/history_view_group_call_tracker.h" // ContentByCall.
#include "data/data_user.h"
#include "data/data_group_call.h"
#include "data/data_channel.h"
#include "data/data_changes.h"
#include "main/main_session.h"
#include "boxes/abstract_box.h"
#include "base/timer.h"
#include "app.h"
#include "styles/style_calls.h"
#include "styles/style_layers.h"

namespace Calls {
namespace {

constexpr auto kUpdateDebugTimeoutMs = crl::time(500);
constexpr auto kSwitchStateDuration = 120;

constexpr auto kMinorBlobAlpha = 76. / 255.;

constexpr auto kBlobLevelDuration1 = 250;
constexpr auto kBlobLevelDuration2 = 120;

auto LinearBlobs() -> std::array<Ui::Paint::LinearBlobs::BlobData, 3> {
	return { {
		{
			.segmentsCount = 5,
			.minScale = 1.,
			.minRadius = (float)st::groupCallMajorBlobMinRadius,
			.maxRadius = (float)st::groupCallMajorBlobMaxRadius,
			.speedScale = .3,
			.alpha = 1.,
			.topOffset = st::groupCallMajorBlobTopOffset,
		},
		{
			.segmentsCount = 7,
			.minScale = 1.,
			.minRadius = (float)st::groupCallMinorBlobMinRadius,
			.maxRadius = (float)st::groupCallMinorBlobMaxRadius,
			.speedScale = .7,
			.alpha = kMinorBlobAlpha,
			.topOffset = st::groupCallMinorBlobTopOffset,
		},
		{
			.segmentsCount = 8,
			.minScale = 1.,
			.minRadius = (float)st::groupCallMinorBlobMinRadius,
			.maxRadius = (float)st::groupCallMinorBlobMaxRadius,
			.speedScale = .7,
			.alpha = kMinorBlobAlpha,
			.topOffset = st::groupCallMinorBlobTopOffset,
		},
	} };
}

auto Colors() {
	using Vector = std::vector<QColor>;
	return base::flat_map<MuteState, Vector>{
		{
			MuteState::ForceMuted,
			Vector{ st::groupCallMembersBg->c, st::groupCallMembersBg->c }
		},
		{
			MuteState::Active,
			Vector{ st::groupCallLive1->c, st::groupCallLive2->c }
		},
		{
			MuteState::Muted,
			Vector{ st::groupCallMuted1->c, st::groupCallMuted2->c }
		},
	};
}

class DebugInfoBox : public Ui::BoxContent {
public:
	DebugInfoBox(QWidget*, base::weak_ptr<Call> call);

protected:
	void prepare() override;

private:
	void updateText();

	base::weak_ptr<Call> _call;
	QPointer<Ui::FlatLabel> _text;
	base::Timer _updateTextTimer;

};

DebugInfoBox::DebugInfoBox(QWidget*, base::weak_ptr<Call> call)
: _call(call) {
}

void DebugInfoBox::prepare() {
	setTitle(rpl::single(qsl("Call Debug")));

	addButton(tr::lng_close(), [this] { closeBox(); });
	_text = setInnerWidget(
		object_ptr<Ui::PaddingWrap<Ui::FlatLabel>>(
			this,
			object_ptr<Ui::FlatLabel>(this, st::callDebugLabel),
			st::callDebugPadding))->entity();
	_text->setSelectable(true);
	updateText();
	_updateTextTimer.setCallback([this] { updateText(); });
	_updateTextTimer.callEach(kUpdateDebugTimeoutMs);
	setDimensions(st::boxWideWidth, st::boxMaxListHeight);
}

void DebugInfoBox::updateText() {
	if (auto call = _call.get()) {
		_text->setText(call->getDebugLog());
	}
}

} // namespace

class Mute final : public Ui::IconButton {
public:
	Mute(QWidget *parent, const style::IconButton &st)
	: Ui::IconButton(parent, st)
	, _st(st)
	, _crossLineMuteAnimation(st::callTopBarMuteCrossLine) {
		resize(_st.width, _st.height);
		installEventFilter(this);

		style::PaletteChanged(
		) | rpl::start_with_next([=] {
			_crossLineMuteAnimation.invalidate();
		}, lifetime());
	}

	void setProgress(float64 progress) {
		if (_progress == progress) {
			return;
		}
		_progress = progress;
		update();
	}

	void setRippleColorOverride(const style::color *colorOverride) {
		_rippleColorOverride = colorOverride;
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) {
		if (event->type() == QEvent::Paint) {
			Painter p(this);
			paintRipple(
				p,
				_st.rippleAreaPosition.x(),
				_st.rippleAreaPosition.y(),
				_rippleColorOverride ? &(*_rippleColorOverride)->c : nullptr);
			_crossLineMuteAnimation.paint(p, _st.iconPosition, _progress);
			return true;
		}
		return QObject::eventFilter(object, event);
	}

private:
	float64 _progress = 0.;

	const style::IconButton &_st;
	Ui::CrossLineAnimation _crossLineMuteAnimation;
	const style::color *_rippleColorOverride = nullptr;

};

TopBar::TopBar(
	QWidget *parent,
	const base::weak_ptr<Call> &call)
: TopBar(parent, call, nullptr) {
}

TopBar::TopBar(
	QWidget *parent,
	const base::weak_ptr<GroupCall> &call)
: TopBar(parent, nullptr, call) {
}

TopBar::TopBar(
	QWidget *parent,
	const base::weak_ptr<Call> &call,
	const base::weak_ptr<GroupCall> &groupCall)
: RpWidget(parent)
, _call(call)
, _groupCall(groupCall)
, _durationLabel(_call
	? object_ptr<Ui::LabelSimple>(this, st::callBarLabel)
	: object_ptr<Ui::LabelSimple>(nullptr))
, _signalBars(_call
	? object_ptr<SignalBars>(this, _call.get(), st::callBarSignalBars)
	: object_ptr<SignalBars>(nullptr))
, _fullInfoLabel(this, st::callBarInfoLabel)
, _shortInfoLabel(this, st::callBarInfoLabel)
, _hangupLabel(_call
	? object_ptr<Ui::LabelSimple>(
		this,
		st::callBarLabel,
		tr::lng_call_bar_hangup(tr::now).toUpper())
	: object_ptr<Ui::LabelSimple>(nullptr))
, _mute(this, st::callBarMuteToggle)
, _info(this)
, _hangup(this, st::callBarHangup)
, _blobs(base::make_unique_q<Ui::RpWidget>(parent))
, _blobsPaint(std::make_unique<Ui::Paint::LinearBlobs>(
	LinearBlobs() | ranges::to_vector,
	kBlobLevelDuration1,
	kBlobLevelDuration2,
	1.))
, _gradients(Colors(), QPointF(), QPointF()) {
	initControls();
	resize(width(), st::callBarHeight);
}

void TopBar::initControls() {
	_mute->setClickedCallback([=] {
		if (const auto call = _call.get()) {
			call->setMuted(!call->muted());
		} else if (const auto group = _groupCall.get()) {
			if (group->muted() != MuteState::ForceMuted) {
				group->setMuted((group->muted() == MuteState::Muted)
					? MuteState::Active
					: MuteState::Muted);
			}
		}
	});

	const auto mapToState = [](bool muted) {
		return muted ? MuteState::Muted : MuteState::Active;
	};
	const auto fromState = _mute->lifetime().make_state<MuteState>(
		_call ? mapToState(_call->muted()) : _groupCall->muted());
	auto muted = _call
		? _call->mutedValue() | rpl::map(mapToState)
		: (_groupCall->mutedValue()
			| MapPushToTalkToActive()
			| rpl::distinct_until_changed()
			| rpl::type_erased());
	std::move(
		muted
	) | rpl::start_with_next([=](MuteState state) {
		setMuted(state != MuteState::Active);
		update();

		const auto isForceMuted = (state == MuteState::ForceMuted);
		if (isForceMuted) {
			_mute->clearState();
		}
		_mute->setAttribute(
			Qt::WA_TransparentForMouseEvents,
			isForceMuted);

		const auto to = 1.;
		const auto from = _switchStateAnimation.animating()
			? (to - _switchStateAnimation.value(0.))
			: 0.;
		const auto fromMuted = *fromState;
		const auto toMuted = state;
		*fromState = state;

		const auto crossFrom = (fromMuted != MuteState::Active) ? 1. : 0.;
		const auto crossTo = (toMuted != MuteState::Active) ? 1. : 0.;

		auto animationCallback = [=](float64 value) {
			if (_groupCall) {
				_groupBrush = QBrush(
					_gradients.gradient(fromMuted, toMuted, value));
				update();
			}

			const auto crossProgress = (crossFrom == crossTo)
				? crossTo
				: (crossFrom + float64(crossTo - crossFrom) * value);
			_mute->setProgress(crossProgress);
		};

		_switchStateAnimation.stop();
		const auto duration = (to - from) * kSwitchStateDuration;
		_switchStateAnimation.start(
			std::move(animationCallback),
			from,
			to,
			duration);
	}, _mute->lifetime());

	if (const auto group = _groupCall.get()) {
		subscribeToMembersChanges(group);
	}

	if (const auto call = _call.get()) {
		call->user()->session().changes().peerUpdates(
			Data::PeerUpdate::Flag::Name
		) | rpl::filter([=](const Data::PeerUpdate &update) {
			// _user may change for the same Panel.
			return (_call != nullptr) && (update.peer == _call->user());
		}) | rpl::start_with_next([=] {
			updateInfoLabels();
		}, lifetime());
	}

	setInfoLabels();
	_info->setClickedCallback([=] {
		if (const auto call = _call.get()) {
			if (Logs::DebugEnabled()
				&& (_info->clickModifiers() & Qt::ControlModifier)) {
				Ui::show(Box<DebugInfoBox>(_call));
			} else {
				Core::App().calls().showInfoPanel(call);
			}
		} else if (const auto group = _groupCall.get()) {
			Core::App().calls().showInfoPanel(group);
		}
	});
	_hangup->setClickedCallback([this] {
		if (const auto call = _call.get()) {
			call->hangup();
		} else if (const auto group = _groupCall.get()) {
			Ui::show(Box(
				LeaveGroupCallBox,
				group,
				false,
				BoxContext::MainWindow));
		}
	});
	_updateDurationTimer.setCallback([this] { updateDurationText(); });
	updateDurationText();

	initBlobs();
}

void TopBar::initBlobs() {
	const auto &hideDuration = kBlobLevelDuration1 * 2;
	const auto hideLastTime = _blobs->lifetime().make_state<crl::time>(0);
	const auto lastTime = _blobs->lifetime().make_state<crl::time>(0);
	const auto blobsRect =
		_blobs->lifetime().make_state<QRect>(_blobs->rect());

	_blobsAnimation.init([=](crl::time now) {
		if (const auto last = *hideLastTime; (last > 0)
			&& (now - last >= hideDuration)) {
			_blobsAnimation.stop();
			return false;
		}
		_blobsPaint->updateLevel(now - *lastTime);
		*lastTime = now;

		_blobs->update();
		return true;
	});

	_groupCall->stateValue(
	) | rpl::start_with_next([=](Calls::GroupCall::State state) {
		if (state == Calls::GroupCall::State::HangingUp) {
			_blobs->hide();
		}
	}, lifetime());

	auto hideBlobs = rpl::combine(
		rpl::single(anim::Disabled()) | rpl::then(anim::Disables()),
		Core::App().appDeactivatedValue(),
		_groupCall->stateValue(
		) | rpl::map([](Calls::GroupCall::State state) {
			using State = Calls::GroupCall::State;
			if (state != State::Creating
				&& state != State::Joining
				&& state != State::Joined
				&& state != State::Connecting) {
				return true;
			}
			return false;
		}) | rpl::distinct_until_changed()
	) | rpl::map([](bool animDisabled, bool hide, bool isBadState) {
		return isBadState || !(!animDisabled && !hide);
	});

	std::move(
		hideBlobs
	) | rpl::start_with_next([=](bool hide) {
		if (hide) {
			_blobsPaint->setLevel(0.);
		}
		*hideLastTime = hide ? crl::now() : 0;
		if (!hide && !_blobsAnimation.animating()) {
			_blobsAnimation.start();
		}

		const auto from = hide ? 0. : 1.;
		const auto to = hide ? 1. : 0.;
		_blobsHideAnimation.start([=](float64 value) {
			blobsRect->setHeight(anim::interpolate(0, -20, value));
		}, from, to, hideDuration);
	}, lifetime());

	///

	const auto parent = static_cast<Ui::RpWidget*>(parentWidget());
	geometryValue(
	) | rpl::start_with_next([=](QRect rect) {
		_blobs->resize(rect.width(), rect.height());

		{
			const auto &r = _blobs->rect();
			*blobsRect = QRect(
				r.x(),
				r.y(),
				r.width() + st::groupCallBlobWidthAdditional,
				0);
		}

		const auto relativePos = mapTo(parent, rect.topLeft());
		_blobs->moveToLeft(relativePos.x(), relativePos.y() + rect.height());
	}, lifetime());

	shownValue(
	) | rpl::start_with_next([=](bool shown) {
		_blobs->setVisible(shown);
	}, lifetime());

	_blobs->paintRequest(
	) | rpl::start_with_next([=](QRect clip) {
		Painter p(_blobs);

		const auto alpha = 1. - _blobsHideAnimation.value(0.);
		if (alpha < 1.) {
			p.setOpacity(alpha);
		}
		_blobsPaint->paint(p, _groupBrush, *blobsRect, 0, 0);
	}, _blobs->lifetime());

	rpl::single(
	) | rpl::then(
		events(
		) | rpl::filter([](not_null<QEvent*> e) {
			return e->type() == QEvent::ZOrderChange;
		}) | rpl::to_empty
	) | rpl::start_with_next([=] {
		crl::on_main(_blobs.get(), [=] {
			_blobs->raise();
		});
	}, lifetime());

	_groupCall->levelUpdates(
	) | rpl::filter([=](const LevelUpdate &update) {
		return update.self;
	}) | rpl::start_with_next([=](const LevelUpdate &update) {
		if (*hideLastTime) {
			 return;
		}
		_blobsPaint->setLevel(update.value);
	}, _blobs->lifetime());

	_blobs->setAttribute(Qt::WA_TransparentForMouseEvents);
	_blobs->show();
	_blobsAnimation.start();

	crl::on_main([=] {
		const auto r = rect();
		const auto relativePos = mapTo(parent, r.topLeft());
		_blobs->moveToLeft(relativePos.x(), relativePos.y() + r.height());
	});
}

void TopBar::subscribeToMembersChanges(not_null<GroupCall*> call) {
	const auto channel = call->channel();
	channel->session().changes().peerFlagsValue(
		channel,
		Data::PeerUpdate::Flag::GroupCall
	) | rpl::map([=] {
		return channel->call();
	}) | rpl::filter([=](Data::GroupCall *real) {
		const auto call = _groupCall.get();
		return call && real && (real->id() == call->id());
	}) | rpl::take(
		1
	) | rpl::map([=](not_null<Data::GroupCall*> real) {
		return HistoryView::GroupCallTracker::ContentByCall(
			real,
			HistoryView::UserpicsInRowStyle{
				.size = st::groupCallTopBarUserpicSize,
				.shift = st::groupCallTopBarUserpicShift,
				.stroke = st::groupCallTopBarUserpicStroke,
			});
	}) | rpl::flatten_latest(
	) | rpl::start_with_next([=](const Ui::GroupCallBarContent &content) {
		const auto changed = (_userpics.size() != content.userpics.size());
		_userpics = content.userpics;
		if (changed) {
			updateControlsGeometry();
		}
		update();
	}, lifetime());
}

void TopBar::updateInfoLabels() {
	setInfoLabels();
	updateControlsGeometry();
}

void TopBar::setInfoLabels() {
	if (const auto call = _call.get()) {
		const auto user = call->user();
		const auto fullName = user->name;
		const auto shortName = user->firstName;
		_fullInfoLabel->setText(fullName.toUpper());
		_shortInfoLabel->setText(shortName.toUpper());
	} else if (const auto group = _groupCall.get()) {
		const auto channel = group->channel();
		const auto name = channel->name;
		_fullInfoLabel->setText(name.toUpper());
		_shortInfoLabel->setText(name.toUpper());
	}
}

void TopBar::setMuted(bool mute) {
	_mute->setRippleColorOverride(&st::shadowFg);
	_hangup->setRippleColorOverride(&st::shadowFg);
	_muted = mute;
}

void TopBar::updateDurationText() {
	if (!_call || !_durationLabel) {
		return;
	}
	auto wasWidth = _durationLabel->width();
	auto durationMs = _call->getDurationMs();
	auto durationSeconds = durationMs / 1000;
	startDurationUpdateTimer(durationMs);
	_durationLabel->setText(Ui::FormatDurationText(durationSeconds));
	if (_durationLabel->width() != wasWidth) {
		updateControlsGeometry();
	}
}

void TopBar::startDurationUpdateTimer(crl::time currentDuration) {
	auto msTillNextSecond = 1000 - (currentDuration % 1000);
	_updateDurationTimer.callOnce(msTillNextSecond + 5);
}

void TopBar::resizeEvent(QResizeEvent *e) {
	updateControlsGeometry();
}

void TopBar::updateControlsGeometry() {
	auto left = 0;
	_mute->moveToLeft(left, 0);
	left += _mute->width();
	if (_durationLabel) {
		_durationLabel->moveToLeft(left, st::callBarLabelTop);
		left += _durationLabel->width() + st::callBarSkip;
	}
	if (!_userpics.isNull()) {
		left += _userpics.width() / _userpics.devicePixelRatio();
	}
	if (_signalBars) {
		_signalBars->moveToLeft(left, (height() - _signalBars->height()) / 2);
		left += _signalBars->width() + st::callBarSkip;
	}

	auto right = st::callBarRightSkip;
	if (_hangupLabel) {
		_hangupLabel->moveToRight(right, st::callBarLabelTop);
		right += _hangupLabel->width();
	} else {
		//right -= st::callBarRightSkip;
	}
	right += st::callBarHangup.width;
	_hangup->setGeometryToRight(0, 0, right, height());
	_info->setGeometryToLeft(
		_mute->width(),
		0,
		width() - _mute->width() - _hangup->width(),
		height());

	auto fullWidth = _fullInfoLabel->naturalWidth();
	auto showFull = (left + fullWidth + right <= width());
	_fullInfoLabel->setVisible(showFull);
	_shortInfoLabel->setVisible(!showFull);

	auto setInfoLabelGeometry = [this, left, right](auto &&infoLabel) {
		auto minPadding = qMax(left, right);
		auto infoWidth = infoLabel->naturalWidth();
		auto infoLeft = (width() - infoWidth) / 2;
		if (infoLeft < minPadding) {
			infoLeft = left;
			infoWidth = width() - left - right;
		}
		infoLabel->setGeometryToLeft(infoLeft, st::callBarLabelTop, infoWidth, st::callBarInfoLabel.style.font->height);
	};
	setInfoLabelGeometry(_fullInfoLabel);
	setInfoLabelGeometry(_shortInfoLabel);

	_gradients.set_points(
		QPointF(0, st::callBarHeight / 2),
		QPointF(width(), st::callBarHeight / 2));
}

void TopBar::paintEvent(QPaintEvent *e) {
	Painter p(this);
	auto brush = _groupCall
		? _groupBrush
		: (_muted ? st::callBarBgMuted : st::callBarBg);
	p.fillRect(e->rect(), std::move(brush));

	if (!_userpics.isNull()) {
		const auto imageSize = _userpics.size()
			/ _userpics.devicePixelRatio();
		const auto top = (height() - imageSize.height()) / 2;
		p.drawImage(_mute->width(), top, _userpics);
	}
}

TopBar::~TopBar() = default;

} // namespace Calls
