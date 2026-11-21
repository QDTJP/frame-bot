// FrameBot (desktop + Android/iOS): macro bot + frame stepper + swift clicks + speedhack,
// with an anti-cheat kill near the end of the level.
//
// Mobile: use the on-screen menu in the level (tap "FB" to collapse/expand it).
// Desktop hotkeys: F6 stepper, F7 step, F8 record, F9 replay, F10 swift clicks.
//
// All four features can run in the same physics frame. Speed stacks with other
// mods' speedhacks because we scale dt and let the scheduler apply its own timescale.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#ifdef GEODE_IS_DESKTOP
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#endif
#include <fstream>
#include <cmath>
#include <functional>
#include <memory>

using namespace geode::prelude;

struct Input {
	uint32_t frame;
	int button;
	bool down;
	bool p1;
};

enum class Mode { Off, Record, Replay };

struct Bot {
	Mode mode = Mode::Off;
	std::vector<Input> macro;
	size_t cursor = 0;
	uint32_t frame = 0;

	bool stepper = false;
	bool stepRequested = false;
	bool swift = false;
	bool swiftDown = false;
	uint32_t swiftReleaseAt = 0;

	bool injecting = false;
	bool tainted = false; // an assist was active this attempt

	static Bot& get() {
		static Bot b;
		return b;
	}
};

static bool inPlay(GJBaseGameLayer* l) {
	auto pl = PlayLayer::get();
	return pl && static_cast<GJBaseGameLayer*>(pl) == l;
}

static void inject(GJBaseGameLayer* l, bool down, int btn, bool p1) {
	auto& b = Bot::get();
	b.injecting = true;
	l->handleButton(down, btn, p1);
	b.injecting = false;
}

static std::filesystem::path macroPath(PlayLayer* pl) {
	return Mod::get()->getSaveDir() / fmt::format("{}.macro", pl->m_level->m_levelID.value());
}

static void saveMacro(PlayLayer* pl) {
	std::ofstream f(macroPath(pl));
	for (auto& i : Bot::get().macro) {
		f << i.frame << ' ' << i.button << ' ' << i.down << ' ' << i.p1 << '\n';
	}
}

static bool loadMacro(PlayLayer* pl) {
	std::ifstream f(macroPath(pl));
	if (!f) return false;
	auto& b = Bot::get();
	b.macro.clear();
	Input i{};
	int down, p1;
	while (f >> i.frame >> i.button >> down >> p1) {
		i.down = down;
		i.p1 = p1;
		b.macro.push_back(i);
	}
	return !b.macro.empty();
}

// Shared by the on-screen buttons (mobile) and the hotkeys (desktop).
namespace actions {
	void stepper() {
		auto& b = Bot::get();
		b.stepper = !b.stepper;
		Notification::create(b.stepper ? "Frame stepper ON" : "Frame stepper OFF")->show();
	}

	void step() {
		auto& b = Bot::get();
		if (b.stepper) b.stepRequested = true;
	}

	void record() {
		auto pl = PlayLayer::get();
		if (!pl) return;
		auto& b = Bot::get();
		if (b.mode == Mode::Record) {
			b.mode = Mode::Off;
			saveMacro(pl);
			Notification::create(fmt::format("Saved {} inputs", b.macro.size()))->show();
		} else {
			b.mode = Mode::Record;
			b.macro.clear();
			Notification::create("Recording (restarting level)")->show();
			pl->resetLevel();
		}
	}

	void replay() {
		auto pl = PlayLayer::get();
		if (!pl) return;
		auto& b = Bot::get();
		if (b.mode == Mode::Replay) {
			b.mode = Mode::Off;
			Notification::create("Replay stopped")->show();
		} else if (loadMacro(pl)) {
			b.mode = Mode::Replay;
			Notification::create("Replaying (restarting level)")->show();
			pl->resetLevel();
		} else {
			Notification::create("No macro for this level")->show();
		}
	}

	void swift() {
		auto& b = Bot::get();
		b.swift = !b.swift;
		Notification::create(b.swift ? "Swift clicks ON" : "Swift clicks OFF")->show();
	}
}

class $modify(BotBaseLayer, GJBaseGameLayer) {
	// Frame stepper: while on, the game only advances when a step is requested.
	void update(float dt) {
		auto& b = Bot::get();
		if (inPlay(this) && b.stepper) {
			if (!b.stepRequested) return;
			b.stepRequested = false;
			float tps = static_cast<float>(Mod::get()->getSettingValue<int64_t>("tps"));
			dt = (1.f / tps) * 1.0001f; // tiny epsilon so exactly one tick runs
		}
		GJBaseGameLayer::update(dt);
	}

	// Runs once per physics tick.
	void processCommands(float dt) {
		if (!inPlay(this)) return GJBaseGameLayer::processCommands(dt);

		auto& b = Bot::get();
		auto mod = Mod::get();

		// Anything that bends the rules marks this attempt as tainted.
		// Another mod's speedhack shows up as a non-1 scheduler timescale.
		float otherScale = CCDirector::get()->getScheduler()->getTimeScale();
		if (b.mode != Mode::Off || b.stepper || b.swift ||
			mod->getSettingValue<double>("speed") != 1.0 || otherScale != 1.f) {
			b.tainted = true;
		}

		// Replay: feed recorded inputs for this frame.
		if (b.mode == Mode::Replay) {
			while (b.cursor < b.macro.size() && b.macro[b.cursor].frame <= b.frame) {
				auto& i = b.macro[b.cursor++];
				inject(this, i.down, i.button, i.p1);
			}
		}

		// Swift clicks: frame-locked autoclicker (recorded like normal input).
		if (b.swift && b.mode != Mode::Replay) {
			double tps = static_cast<double>(mod->getSettingValue<int64_t>("tps"));
			double cps = mod->getSettingValue<double>("swift-cps");
			uint32_t period = std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(tps / cps)));
			if (b.swiftDown && b.frame >= b.swiftReleaseAt) {
				inject(this, false, 1, true);
				b.swiftDown = false;
			}
			if (!b.swiftDown && b.frame % period == 0) {
				inject(this, true, 1, true);
				b.swiftDown = true;
				b.swiftReleaseAt = b.frame + 1;
			}
		}

		GJBaseGameLayer::processCommands(dt);
		b.frame++;
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (inPlay(this)) {
			auto& b = Bot::get();
			// During replay the bot owns the inputs.
			if (b.mode == Mode::Replay && !b.injecting) return;
			if (b.mode == Mode::Record) {
				b.macro.push_back({b.frame, button, down, isPlayer1});
			}
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}
};

class $modify(BotPlayLayer, PlayLayer) {
	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();
		auto mod = Mod::get();
		if (!mod->getSettingValue<bool>("show-menu")) return;

		auto win = CCDirector::get()->getWinSize();
		float px = static_cast<float>(mod->getSettingValue<double>("menu-x")) / 100.f * win.width;
		float py = static_cast<float>(mod->getSettingValue<double>("menu-y")) / 100.f * win.height;
		float scale = static_cast<float>(mod->getSettingValue<double>("menu-scale"));
		GLubyte opacity = static_cast<GLubyte>(mod->getSettingValue<double>("menu-opacity") / 100.0 * 255.0);
		bool horizontal = mod->getSettingValue<bool>("menu-horizontal");

		auto menu = CCMenu::create();
		menu->setID("framebot-menu"_spr);
		menu->setPosition({0.f, 0.f});
		m_uiLayer->addChild(menu, 100);

		auto panel = std::make_shared<std::vector<CCMenuItemSpriteExtra*>>(); // all but the collapse button
		int index = 0;

		auto addBtn = [&](char const* text, std::function<void()> fn, bool inPanel = true) {
			auto spr = ButtonSprite::create(text, 44, true, "bigFont.fnt", "GJ_button_04.png", 24.f, .5f);
			spr->setOpacity(opacity);
			auto item = CCMenuItemExt::createSpriteExtra(spr, [fn = std::move(fn)](CCMenuItemSpriteExtra*) { fn(); });
			item->setScale(scale);
			// Column steps down, row steps right.
			float step = (horizontal ? 52.f : 30.f) * scale;
			item->setPosition(horizontal
				? CCPoint{px + index * step, py}
				: CCPoint{px, py - index * step});
			index++;
			menu->addChild(item);
			if (inPanel) panel->push_back(item);
			return item;
		};

		// Collapse/expand so the menu never blocks the screen while playing.
		addBtn("FB", [panel]() {
			if (panel->empty()) return;
			bool show = !(*panel)[0]->isVisible();
			for (auto p : *panel) p->setVisible(show);
		}, false);

		addBtn("Step", [] { actions::stepper(); });
		addBtn("Next", [] { actions::step(); });
		addBtn("Rec", [] { actions::record(); });
		addBtn("Play", [] { actions::replay(); });
		addBtn("Swift", [] { actions::swift(); });
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		auto& b = Bot::get();
		b.frame = 0;
		b.cursor = 0;
		b.swiftDown = false;
		b.tainted = false;
		if (b.mode == Mode::Record) b.macro.clear();
	}

	// Anti-cheat: die near the end of the level if any assist was used.
	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		auto& b = Bot::get();
		if (!b.tainted || m_player1->m_isDead || m_level->isPlatformer()) return;
		double limit = Mod::get()->getSettingValue<double>("kill-percent");
		if (this->getCurrentPercent() >= limit) {
			this->destroyPlayer(m_player1, nullptr);
		}
	}

	void onQuit() {
		auto& b = Bot::get();
		if (b.mode == Mode::Record) saveMacro(this);
		b.mode = Mode::Off;
		b.stepper = false;
		b.swift = false;
		PlayLayer::onQuit();
	}
};

// Speedhack. Scale dt, then let the scheduler apply any other mod's timescale on top.
class $modify(BotScheduler, CCScheduler) {
	void update(float dt) {
		double s = Mod::get()->getSettingValue<double>("speed");
		if (s != 1.0 && PlayLayer::get()) dt *= static_cast<float>(s);
		CCScheduler::update(dt);
	}
};

#ifdef GEODE_IS_DESKTOP
class $modify(BotKeys, CCKeyboardDispatcher) {
	// NOTE: the signature changed between GD versions; if it fails to compile,
	// check the bindings for your Geode SDK (older: no `timestamp` param).
	bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
		if (PlayLayer::get() && down) {
			switch (key) {
				case KEY_F6:  if (!repeat) actions::stepper(); break;
				case KEY_F7:  actions::step(); break; // hold to keep stepping
				case KEY_F8:  if (!repeat) actions::record(); break;
				case KEY_F9:  if (!repeat) actions::replay(); break;
				case KEY_F10: if (!repeat) actions::swift(); break;
				default: break;
			}
		}
		return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
	}
};
#endif
