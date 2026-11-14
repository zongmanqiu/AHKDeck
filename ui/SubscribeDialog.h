// ui/SubscribeDialog.h - 获取脚本 > 订阅(m180).
//
// m179 put ONE url box on the 获取脚本 window itself, with a 订阅 / 移除 button
// that flipped meaning depending on whether the typed URL happened to be
// already subscribed. That reads as "you can only subscribe": there was no
// list, so no way to see what you had, and no way to drop one without
// re-typing its URL exactly.
//
// This window is the list. It takes a URL, fetches the manifest once to prove
// it is a catalog (and to learn the name the catalog gives itself), and keeps
// the accepted ones in a table with a 移除 per row. The 获取脚本 window only
// shows a 订阅 button now, and re-merges its rows when this returns.
#pragma once

#include "core/ScriptCatalog.h"

#include <functional>
#include <string>
#include <vector>

namespace ahkdeck
{
namespace ui
{

struct SubscribeCallbacks
{
	// Called after every change (added / removed), with the whole list.
	std::function<void(const std::vector<CustomCatalogSource> &)> save;
	std::function<void(const std::string &)> log;
};

// Modal. aSources is the persisted list; aOut receives the final one. Returns
// true when the list changed, so the caller knows to rebuild its merged rows.
bool ShowSubscribeDialog(const std::vector<CustomCatalogSource> &aSources,
	bool aRounded, bool aShadow, const SubscribeCallbacks &aCallbacks,
	std::vector<CustomCatalogSource> &aOut);

} // namespace ui
} // namespace ahkdeck
