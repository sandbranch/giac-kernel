#include "engine.h"
#include "plot3d.h"

#include <giac/config.h>
#include <giac/giac.h>
#include <giac/input_lexer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <set>

using giac::gen;

struct Engine::Impl {
	giac::context ctx;
};

Engine::Engine() : impl_(new Impl) {}
Engine::~Engine() = default;

void Engine::interrupt() { giac::ctrl_c = true; }

std::string Engine::version() const { return VERSION; }

static std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

static bool is_ident_char(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

namespace {

struct Parsed {
	gen value;
	std::string error;  // Giac's parser message, empty when it parsed
	std::string log;    // warnings the parser printed
};

}  // namespace

static Parsed parse(const std::string &code, const giac::context *ctx)
{
	Parsed p;
	giac::first_error_line(0, ctx);
	giac::parser_error("", ctx);
	{
		giac::log_output_redirect lr(ctx);
		p.value = gen(code, ctx);
		p.log = lr.get_buffer_string();
	}
	if (giac::first_error_line(ctx) > 0)
		p.error = trim(giac::parser_error(ctx));
	return p;
}

// Graphics are pnt(...) objects, alone or in a list (several objects drawn
// together, as plot(...) with several curves returns them).
static bool is_graphic(const gen &g)
{
	if (g.is_symb_of_sommet(giac::at_pnt))
		return true;
	if (g.type != giac::_VECT || g._VECTptr->empty())
		return false;
	for (const gen &e : *g._VECTptr)
		if (!e.is_symb_of_sommet(giac::at_pnt))
			return false;
	return true;
}

static bool is_3d(const gen &g)
{
	if (g.is_symb_of_sommet(giac::at_pnt))
		return giac::is3d(g);
	for (const gen &e : *g._VECTptr)
		if (giac::is3d(e))
			return true;
	return false;
}

// Giac 1.9 writes some attributes without a separating space
// (baseProfile="tiny"width="13cm"), which is not valid XML, and strict SVG
// renderers reject the whole image. Insert the missing spaces.
static std::string fix_attribute_spacing(const std::string &in)
{
	std::string out;
	out.reserve(in.size() + in.size() / 50);
	bool in_tag = false, in_quote = false;
	for (size_t i = 0; i < in.size(); ++i) {
		char c = in[i];
		out += c;
		if (!in_tag) {
			in_tag = c == '<';
		} else if (c == '"') {
			in_quote = !in_quote;
			if (!in_quote && i + 1 < in.size() && std::isalpha((unsigned char)in[i + 1]))
				out += ' ';
		} else if (c == '>' && !in_quote) {
			in_tag = false;
		}
	}
	return out;
}

static double attr_num(const std::string &tag, const char *name, bool &found)
{
	std::string key = std::string(" ") + name + "=\"";
	size_t p = tag.find(key);
	found = p != std::string::npos;
	return found ? std::atof(tag.c_str() + p + key.size()) : 0;
}

// Two fixes to the grid Giac draws:
// - every fifth gridline is drawn in the axes' blue, so at x=2.5 it looks
//   like a second axis. Keep blue for the real axes (x=0, y=0) only.
// - y tick labels are right-aligned just inside the frame and overlap it.
//   Right-align them at the right edge of the picture instead.
static std::string tidy_grid(const std::string &in, double view_right)
{
	std::string out;
	out.reserve(in.size());
	size_t pos = 0;
	while (pos < in.size()) {
		size_t lt = in.find('<', pos);
		if (lt == std::string::npos) {
			out.append(in, pos, std::string::npos);
			break;
		}
		size_t gt = in.find('>', lt);
		if (gt == std::string::npos) {
			out.append(in, pos, std::string::npos);
			break;
		}
		out.append(in, pos, lt - pos);
		std::string tag = in.substr(lt, gt - lt + 1);
		bool f1, f2, f3, f4;
		if (tag.compare(0, 5, "<line") == 0 && tag.find("stroke=\"blue\"") != std::string::npos) {
			double x1 = attr_num(tag, "x1", f1), x2 = attr_num(tag, "x2", f2);
			double y1 = attr_num(tag, "y1", f3), y2 = attr_num(tag, "y2", f4);
			bool axis = (f1 && f2 && x1 == x2 && x1 == 0) || (f3 && f4 && y1 == y2 && y1 == 0);
			if (!axis) {
				tag.replace(tag.find("stroke=\"blue\""), 13, "stroke=\"cyan\"");
				size_t sw = tag.find("stroke-width=\"");
				if (sw != std::string::npos) {
					size_t v = sw + 14, ve = tag.find('"', v);
					char val[40];
					std::snprintf(val, sizeof val, "%g", std::atof(tag.c_str() + v) / 2);
					tag.replace(v, ve - v, val);
				}
			}
		} else if (tag.compare(0, 5, "<text") == 0 && tag.find("text-anchor:end") != std::string::npos) {
			size_t sc = tag.find("scale(");
			double sx = sc != std::string::npos ? std::atof(tag.c_str() + sc + 6) : 1;
			size_t xp = tag.find(" x=\"");
			if (xp != std::string::npos && sx > 0) {
				size_t v = xp + 4, ve = tag.find('"', v);
				char val[40];
				std::snprintf(val, sizeof val, "%g", view_right / sx);
				tag.replace(v, ve - v, val);
			}
		}
		out += tag;
		pos = gt + 1;
	}
	return out;
}

// A complete SVG document for a 2D graphic, its view fitted to the content
// the way Xcas autoscales. gen2svg's own preamble always shows -6..6.
static std::string graphic_svg(const gen &g, const giac::context *ctx)
{
	std::vector<double> vx, vy, vz;
	giac::autoscaleg(g, vx, vy, vz, ctx);
	double xmin = -5, xmax = 5, ymin = -5, ymax = 5;
	if (!vx.empty() && !vy.empty()) {
		// full view: SVG does not clip, so trimming outliers (as Xcas does
		// on screen) would draw curves outside the frame
		giac::autoscaleminmax(vx, xmin, xmax, true);
		giac::autoscaleminmax(vy, ymin, ymax, true);
	}
	// Fill the frame (Xcas' default). Equal x and y scales made wide plots
	// tiny and cut off tick labels.
	double ratio = (ymax - ymin) / (xmax - xmin);
	std::string svg = giac::svg_preamble(13, 8, xmin, xmax, ymin, ymax, false, false, 0);
	svg += giac::gen2svg(g, xmin, xmax, ymin, ymax, ratio, ctx, false);
	svg += giac::svg_grid(xmin, xmax, ymin, ymax, 0);  // closes the plot group, adds tick labels
	svg += giac::svg_end;
	svg = fix_attribute_spacing(svg);

	// Transparent by default: black labels vanish on a dark terminal. Paint
	// the viewBox white, as the first thing drawn.
	size_t vb = svg.find("viewBox=\"");
	size_t open_end = svg.find('>');
	double x, y, w, h;
	if (vb == std::string::npos || open_end == std::string::npos || vb > open_end ||
	    std::sscanf(svg.c_str() + vb + 9, "%lf %lf %lf %lf", &x, &y, &w, &h) != 4 || w <= 0 || h <= 0)
		return svg;
	char rect[200];
	std::snprintf(rect, sizeof rect, "\n<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"white\" />", x,
	              y, w, h);
	svg.insert(open_end + 1, rect);

	// Curves are drawn with vector-effect="non-scaling-stroke" and a width in
	// pixels. resvg (jotter's renderer) ignores vector-effect, so the width is
	// read in plot units and a 1px line becomes a band a unit wide. Convert
	// the width to plot units ourselves and drop the attribute.
	double wcm = 0, hcm = 0;
	size_t ws = svg.find("width=\""), hs = svg.find("height=\"");
	if (ws < open_end && hs < open_end) {
		wcm = std::atof(svg.c_str() + ws + 7);
		hcm = std::atof(svg.c_str() + hs + 8);
	}
	const double px_per_cm = 96 / 2.54;
	double sx = wcm > 0 ? wcm * px_per_cm / w : 1, sy = hcm > 0 ? hcm * px_per_cm / h : 1;
	double unit_per_px = 1 / std::sqrt(sx * sy);  // x and y scales differ; use their mean
	const std::string effect = "vector-effect=\"non-scaling-stroke\"";
	for (size_t p = svg.find(effect); p != std::string::npos; p = svg.find(effect, p)) {
		svg.erase(p, effect.size());
		size_t tag_end = svg.find('>', p);
		size_t tag_start = svg.rfind('<', p);
		size_t sw = svg.find("stroke-width=\"", tag_start);
		if (sw == std::string::npos || sw > tag_end)
			continue;
		size_t v = sw + 14, ve = svg.find('"', v);
		double px = std::atof(svg.c_str() + v);
		char val[40];
		std::snprintf(val, sizeof val, "%g", (px > 0 ? px : 1) * 1.5 * unit_per_px);
		svg.replace(v, ve - v, val);
	}
	svg = tidy_grid(svg, x + w * 0.99);  // labels end 1% inside the right edge
	return svg;
}

// A Giac parser message looks like ":1: syntax error  line 1 col 4 at ; in";
// keep the readable part.
static std::string clean_syntax_error(std::string s)
{
	size_t pos = s.find("syntax error");
	if (pos != std::string::npos)
		s = s.substr(pos);
	std::string out;
	for (char c : s)
		if (!(c == ' ' && !out.empty() && out.back() == ' '))
			out += c;
	if (out.size() > 3 && out.compare(out.size() - 3, 3, " in") == 0)
		out.resize(out.size() - 3);
	return trim(out);
}

EvalResult Engine::execute(const std::string &code)
{
	EvalResult r;
	const giac::context *ctx = &impl_->ctx;
	giac::ctrl_c = false;
	giac::interrupted = false;

	std::string src = trim(code);
	if (src.empty())
		return r;

	if (src[0] == '?') {  // ?factor, as in Xcas
		std::string name = trim(src.substr(1));
		r.output = help(name);
		if (r.output.empty()) {
			r.ok = false;
			r.ename = "NoHelp";
			r.evalue = "no help for " + name;
		}
		return r;
	}

	// Xcas convention: a trailing ":;" evaluates without showing the result
	bool quiet = src.size() >= 2 && src.compare(src.size() - 2, 2, ":;") == 0;

	Parsed p = parse(src, ctx);
	r.output = p.log;
	if (!p.error.empty()) {
		r.ok = false;
		r.ename = "SyntaxError";
		r.evalue = clean_syntax_error(p.error);
		return r;
	}

	gen v;
	{
		giac::log_output_redirect lr(ctx);
		v = giac::protecteval(p.value, 1, ctx);
		r.output += lr.get_buffer_string();
	}
	giac::ctrl_c = false;

	// protecteval turns a Giac exception into a string with subtype -1
	if (v.type == giac::_STRNG && v.subtype == -1) {
		r.ok = false;
		std::string msg = *v._STRNGptr;
		if (msg.find("user interruption") != std::string::npos) {
			r.ename = "Interrupted";
			r.evalue = "Stopped by user interruption";
		} else {
			r.ename = "GiacError";
			r.evalue = trim(msg);
		}
		giac::interrupted = false;
		return r;
	}

	if (quiet)
		return r;
	if (v.type == giac::_STRNG && *v._STRNGptr == "Done")
		return r;

	if (is_graphic(v)) {
		// The printed form of a graphic is thousands of raw points; clients
		// that cannot show the SVG get a short placeholder instead.
		if (is_3d(v)) {
			std::string svg = plot3d_svg(v, ctx);
			if (svg.empty()) {
				r.data.emplace_back("text/plain", "[3D graphic: not shown, only surfaces and curves are drawn]");
			} else {
				r.data.emplace_back("text/plain", "[3D graphic]");
				r.data.emplace_back("image/svg+xml", svg);
			}
		} else {
			r.data.emplace_back("text/plain", "[graphic]");
			r.data.emplace_back("image/svg+xml", graphic_svg(v, ctx));
		}
		return r;
	}
	r.data.emplace_back("text/plain", v.print(ctx));
	if (v.type != giac::_STRNG) {
		std::string tex = giac::gen2tex(v, ctx);
		if (!tex.empty())
			r.data.emplace_back("text/latex", "$\\displaystyle " + tex + "$");
	}
	return r;
}

std::string Engine::is_complete(const std::string &code)
{
	std::string src = trim(code);
	if (src.empty() || src[0] == '?')
		return "complete";
	Parsed p = parse(src, &impl_->ctx);
	if (p.error.empty())
		return "complete";
	// The parser ran out of input: an open bracket, block or string
	if (p.error.find("end of input") != std::string::npos)
		return "incomplete";
	return "invalid";
}

// The identifier ending at byte offset pos: [start, pos)
static size_t word_start(const std::string &code, size_t pos)
{
	size_t s = std::min(pos, code.size());
	while (s > 0 && is_ident_char((unsigned char)code[s - 1]))
		--s;
	return s;
}

Completion Engine::complete(const std::string &code, size_t cursor)
{
	Completion c;
	c.end = std::min(cursor, code.size());
	c.start = word_start(code, c.end);
	std::string prefix = code.substr(c.start, c.end - c.start);
	if (prefix.empty())
		return c;

	std::set<std::string> found;
	for (auto *f = giac::builtin_lexer_functions_begin(); f != giac::builtin_lexer_functions_end(); ++f)
		if (f->first && std::strncmp(f->first, prefix.c_str(), prefix.size()) == 0)
			found.insert(f->first);
	// variables and functions the user has defined in this session
	if (const giac::sym_tab *t = impl_->ctx.tabptr)
		for (const auto &kv : *t)
			if (std::strncmp(kv.first, prefix.c_str(), prefix.size()) == 0)
				found.insert(kv.first);
	c.matches.assign(found.begin(), found.end());
	return c;
}

std::string Engine::help(const std::string &name)
{
	if (name.empty())
		return "";
	const char *cmd = name.c_str();
	const char *howto = nullptr, *syntax = nullptr, *examples = nullptr, *related = nullptr;
	// language 2 is English in Giac's numbering (1 French, 2 English, ...)
	if (!giac::has_static_help(cmd, 2, howto, syntax, examples, related))
		return "";
	std::string s = name;
	if (syntax && *syntax)
		s += "(" + std::string(syntax) + ")";
	s += "\n";
	if (howto && *howto)
		s += std::string(howto) + "\n";
	if (examples && *examples) {
		s += "\nExamples:\n";
		std::string ex = examples;
		size_t a = 0;
		while (a < ex.size()) {
			size_t b = ex.find(';', a);
			if (b == std::string::npos)
				b = ex.size();
			std::string one = trim(ex.substr(a, b - a));
			if (!one.empty())
				s += "  " + one + "\n";
			a = b + 1;
		}
	}
	if (related && *related)
		s += "\nSee also: " + std::string(related) + "\n";
	return s;
}

std::string Engine::help_at(const std::string &code, size_t cursor)
{
	size_t end = std::min(cursor, code.size());
	while (end < code.size() && is_ident_char((unsigned char)code[end]))
		++end;  // cursor may sit inside the word
	size_t start = word_start(code, end);
	return help(code.substr(start, end - start));
}
