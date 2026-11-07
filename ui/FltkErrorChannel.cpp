// FltkErrorChannel.cpp - see FltkErrorChannel.h
#include "ui/FltkErrorChannel.h"

#include <FL/Fl.H>

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace {

std::function<void(const std::string &)> g_sink;
std::vector<std::string> g_messages;
std::string g_log;
bool g_reporting = false;

void Report(const char *format, va_list aArgs)
{
	char buffer[2048];
	std::vsnprintf(buffer, sizeof(buffer), format, aArgs);
	std::string text(buffer);
	// FLTK's messages often end with a newline; the console wants one line.
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
		text.pop_back();
	if (text.empty())
		return;

	// m084: the sink is the deck's console, and the console is a text
	// display - so a FLTK error that is written to the console can provoke
	// ANOTHER FLTK error. Without this guard that is a loop that ends in a
	// stack overflow (the mcp gate died on it): error -> append -> error ->
	// append. While a message is being reported, the sink stays out - but
	// the message itself is still logged, so nothing goes missing.
	g_messages.push_back(text);
	if (g_messages.size() > 256)
		g_messages.erase(g_messages.begin());
	g_log += text;
	g_log += "\n";
	if (g_log.size() > 64 * 1024)
		g_log.erase(0, g_log.size() - 32 * 1024);

	// stderr first: it works before the window exists, and it is the MCP
	// endpoint's log channel (the gates read it).
	std::fprintf(stderr, "[fltk] %s\n", text.c_str());
	std::fflush(stderr);

	if (g_reporting)
		return;
	g_reporting = true;
	if (g_sink)
		g_sink(text);
	g_reporting = false;
}

void FltkError(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	Report(format, args);
	va_end(args);
}

void FltkWarning(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	Report(format, args);
	va_end(args);
}

} // namespace

namespace ahkdeck
{
namespace ui
{

void InstallFltkErrorChannel()
{
	// Fl::error / Fl::warning are function pointers (src/Fl_abort.cxx), so
	// taking them over needs no FLTK patch. Fl::fatal stays untouched: a
	// fatal FLTK error must still abort the process.
	Fl::error = FltkError;
	Fl::warning = FltkWarning;
}

void SetFltkErrorSink(std::function<void(const std::string &)> aSink)
{
	g_sink = std::move(aSink);
}

const std::string &FltkErrorLog()
{
	return g_log;
}

int FltkErrorCount()
{
	return (int)g_messages.size();
}

} // namespace ui
} // namespace ahkdeck
