#include "plot3d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using giac::gen;

namespace {

struct P3 {
	double x, y, z;
};

struct Facet {
	P3 v[4];
	double depth;
};

struct Curve {
	std::vector<P3> pts;
};

const int SURFACE_N = 48;   // grid steps per parameter
const int CURVE_N = 400;    // samples along a curve
const double W = 560, H = 440;  // picture size in px

bool to_double(const gen &g, double &out, const giac::context *ctx)
{
	gen e = giac::evalf_double(g, 1, ctx);
	if (e.type != giac::_DOUBLE_)
		return false;
	out = e._DOUBLE_val;
	return std::isfinite(out);
}

// Evaluate the parametrization at the given values of its variables.
bool eval_point(const gen &param, const gen &vars, const giac::vecteur &vals, P3 &p,
                const giac::context *ctx)
{
	gen r = giac::subst(param, vars, gen(vals), false, ctx);
	if (r.type != giac::_VECT || r._VECTptr->size() != 3)
		return false;
	const giac::vecteur &v = *r._VECTptr;
	return to_double(v[0], p.x, ctx) && to_double(v[1], p.y, ctx) && to_double(v[2], p.z, ctx);
}

// hypersurface([[x(u,v),y(u,v),z(u,v)], [u,v], [umin,vmin], [umax,vmax], ...], ...)
bool sample_surface(const gen &hs, std::vector<std::vector<P3>> &grid,
                    std::vector<std::vector<char>> &ok, const giac::context *ctx)
{
	const gen &f = hs._SYMBptr->feuille;
	if (f.type != giac::_VECT || f._VECTptr->empty())
		return false;
	const gen &d = f._VECTptr->front();
	if (d.type != giac::_VECT || d._VECTptr->size() < 4)
		return false;
	const giac::vecteur &dv = *d._VECTptr;
	const gen &param = dv[0], &vars = dv[1], &mins = dv[2], &maxs = dv[3];
	if (param.type != giac::_VECT || vars.type != giac::_VECT || vars._VECTptr->size() != 2 ||
	    mins.type != giac::_VECT || maxs.type != giac::_VECT)
		return false;
	double u0, v0, u1, v1;
	if (!to_double(mins[0], u0, ctx) || !to_double(mins[1], v0, ctx) || !to_double(maxs[0], u1, ctx) ||
	    !to_double(maxs[1], v1, ctx))
		return false;
	const int N = SURFACE_N;
	grid.assign(N + 1, std::vector<P3>(N + 1));
	ok.assign(N + 1, std::vector<char>(N + 1, 0));
	for (int i = 0; i <= N; ++i)
		for (int j = 0; j <= N; ++j) {
			if (giac::ctrl_c)  // interrupted
				return false;
			double u = u0 + (u1 - u0) * i / N, v = v0 + (v1 - v0) * j / N;
			giac::vecteur uv;
			uv.push_back(gen(u));
			uv.push_back(gen(v));
			ok[i][j] = eval_point(param, vars, uv, grid[i][j], ctx);
		}
	return true;
}

// curve([[x(t),y(t),z(t)], t, tmin, tmax, ...], ...)
bool sample_curve(const gen &cv, Curve &c, const giac::context *ctx)
{
	const gen &f = cv._SYMBptr->feuille;
	if (f.type != giac::_VECT || f._VECTptr->empty())
		return false;
	const gen &d = f._VECTptr->front();
	if (d.type != giac::_VECT || d._VECTptr->size() < 4)
		return false;
	const giac::vecteur &dv = *d._VECTptr;
	double t0, t1;
	if (dv[0].type != giac::_VECT || dv[0]._VECTptr->size() != 3 || !to_double(dv[2], t0, ctx) ||
	    !to_double(dv[3], t1, ctx))
		return false;
	for (int i = 0; i <= CURVE_N; ++i) {
		P3 p;
		giac::vecteur var(1, dv[1]), t(1, gen(t0 + (t1 - t0) * i / CURVE_N));
		if (eval_point(dv[0], gen(var), t, p, ctx))
			c.pts.push_back(p);
	}
	return c.pts.size() > 1;
}

// Viridis-like colour ramp, t in [0,1]
void ramp(double t, double &r, double &g, double &b)
{
	static const double stops[][3] = {{68, 1, 84}, {59, 82, 139}, {33, 145, 140}, {94, 201, 98}, {253, 231, 37}};
	t = std::clamp(t, 0.0, 1.0) * 4;
	int i = std::min(3, (int)t);
	double f = t - i;
	r = stops[i][0] + (stops[i + 1][0] - stops[i][0]) * f;
	g = stops[i][1] + (stops[i + 1][1] - stops[i][1]) * f;
	b = stops[i][2] + (stops[i + 1][2] - stops[i][2]) * f;
}

std::string fmt(double v)
{
	char s[32];
	std::snprintf(s, sizeof s, "%.3g", std::fabs(v) < 1e-12 ? 0.0 : v);
	return s;
}

}  // namespace

std::string plot3d_svg(const gen &g, const giac::context *ctx)
{
	// collect the objects
	std::vector<gen> objs;
	auto add = [&](const gen &p) {
		if (p.is_symb_of_sommet(giac::at_pnt) && p._SYMBptr->feuille.type == giac::_VECT &&
		    !p._SYMBptr->feuille._VECTptr->empty())
			objs.push_back(p._SYMBptr->feuille._VECTptr->front());
	};
	if (g.type == giac::_VECT)
		for (const gen &e : *g._VECTptr)
			add(e);
	else
		add(g);

	std::vector<std::vector<std::vector<P3>>> surfaces;
	std::vector<std::vector<std::vector<char>>> surface_ok;
	std::vector<Curve> curves;
	for (const gen &o : objs) {
		if (o.is_symb_of_sommet(giac::at_hypersurface)) {
			std::vector<std::vector<P3>> grid;
			std::vector<std::vector<char>> ok;
			if (sample_surface(o, grid, ok, ctx)) {
				surfaces.push_back(std::move(grid));
				surface_ok.push_back(std::move(ok));
			}
		} else if (o.is_symb_of_sommet(giac::at_curve)) {
			Curve c;
			if (sample_curve(o, c, ctx))
				curves.push_back(std::move(c));
		}
	}
	if (surfaces.empty() && curves.empty())
		return "";

	// bounding box
	P3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
	auto grow = [&](const P3 &p) {
		lo.x = std::min(lo.x, p.x), lo.y = std::min(lo.y, p.y), lo.z = std::min(lo.z, p.z);
		hi.x = std::max(hi.x, p.x), hi.y = std::max(hi.y, p.y), hi.z = std::max(hi.z, p.z);
	};
	for (size_t s = 0; s < surfaces.size(); ++s)
		for (size_t i = 0; i < surfaces[s].size(); ++i)
			for (size_t j = 0; j < surfaces[s][i].size(); ++j)
				if (surface_ok[s][i][j])
					grow(surfaces[s][i][j]);
	for (const Curve &c : curves)
		for (const P3 &p : c.pts)
			grow(p);
	if (lo.x > hi.x)
		return "";
	auto widen = [](double &a, double &b) {
		if (b - a < 1e-12) {
			a -= 0.5;
			b += 0.5;
		}
	};
	widen(lo.x, hi.x), widen(lo.y, hi.y), widen(lo.z, hi.z);

	// Normalise to a box [-1,1] x [-1,1] x [-0.8,0.8], then view it from
	// azimuth -60 degrees, elevation 25 degrees (matplotlib's default angle).
	const double az = -60 * M_PI / 180, el = 25 * M_PI / 180;
	const double ca = std::cos(az), sa = std::sin(az), ce = std::cos(el), se = std::sin(el);
	struct Q {
		double sx, sy, depth;
	};
	auto norm = [&](const P3 &p) {
		return P3{2 * (p.x - lo.x) / (hi.x - lo.x) - 1, 2 * (p.y - lo.y) / (hi.y - lo.y) - 1,
		          1.6 * (p.z - lo.z) / (hi.z - lo.z) - 0.8};
	};
	auto project = [&](const P3 &p) {
		P3 n = norm(p);
		double xr = n.x * ca - n.y * sa;
		double yr = n.x * sa + n.y * ca;
		// screen: right = xr, up = z*cos(el) + yr*sin(el); depth grows away from the viewer
		return Q{xr, n.z * ce + yr * se, yr * ce - n.z * se};
	};
	// fit the projected unit box into the picture
	double pminx = 1e300, pmaxx = -1e300, pminy = 1e300, pmaxy = -1e300;
	for (int c = 0; c < 8; ++c) {
		Q q = project(P3{c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z});
		pminx = std::min(pminx, q.sx), pmaxx = std::max(pmaxx, q.sx);
		pminy = std::min(pminy, q.sy), pmaxy = std::max(pmaxy, q.sy);
	}
	const double margin = 46;
	double scale = std::min((W - 2 * margin) / (pmaxx - pminx), (H - 2 * margin) / (pmaxy - pminy));
	double ox = W / 2 - scale * (pminx + pmaxx) / 2, oy = H / 2 + scale * (pminy + pmaxy) / 2;
	auto px = [&](const Q &q) { return ox + scale * q.sx; };
	auto py = [&](const Q &q) { return oy - scale * q.sy; };

	std::string svg;
	char buf[400];
	std::snprintf(buf, sizeof buf,
	              "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%g\" height=\"%g\" viewBox=\"0 0 %g %g\">\n"
	              "<rect width=\"%g\" height=\"%g\" fill=\"white\" />\n",
	              W, H, W, H, W, H);
	svg += buf;

	// box: the back edges first (behind the surface), labelled bottom edges
	P3 corner[8];
	Q cq[8];
	for (int c = 0; c < 8; ++c) {
		corner[c] = P3{c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z};
		cq[c] = project(corner[c]);
	}
	int far = 0;  // the corner farthest from the viewer: its three edges are at the back
	for (int c = 1; c < 8; ++c)
		if (cq[c].depth > cq[far].depth)
			far = c;
	auto line = [&](const Q &a, const Q &b, const char *colour, double width) {
		std::snprintf(buf, sizeof buf,
		              "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"%g\" />\n",
		              px(a), py(a), px(b), py(b), colour, width);
		svg += buf;
	};
	for (int a = 0; a < 8; ++a)
		for (int bit = 1; bit < 8; bit <<= 1) {
			int b = a | bit;
			if (b == a)
				continue;
			// back faces: the three faces through the far corner
			int shared = ~(a ^ b) & 7;  // axes on which the edge is constant
			bool on_back_face = false;
			for (int ax = 1; ax < 8; ax <<= 1)
				if ((shared & ax) && (a & ax) == (far & ax))
					on_back_face = true;
			if (on_back_face)
				line(cq[a], cq[b], "#b8b8b8", 1);
		}

	// facets, painted back to front
	std::vector<Facet> facets;
	for (size_t s = 0; s < surfaces.size(); ++s) {
		const auto &gr = surfaces[s];
		const auto &ok = surface_ok[s];
		for (size_t i = 0; i + 1 < gr.size(); ++i)
			for (size_t j = 0; j + 1 < gr[i].size(); ++j) {
				if (!ok[i][j] || !ok[i + 1][j] || !ok[i + 1][j + 1] || !ok[i][j + 1])
					continue;
				Facet f{{gr[i][j], gr[i + 1][j], gr[i + 1][j + 1], gr[i][j + 1]}, 0};
				for (const P3 &p : f.v)
					f.depth += project(p).depth / 4;
				facets.push_back(f);
			}
	}
	std::sort(facets.begin(), facets.end(), [](const Facet &a, const Facet &b) { return a.depth > b.depth; });
	// light from the upper left, slightly towards the viewer
	const P3 light = [] {
		P3 l{-0.4, -0.6, 0.7};
		double n = std::sqrt(l.x * l.x + l.y * l.y + l.z * l.z);
		return P3{l.x / n, l.y / n, l.z / n};
	}();
	for (const Facet &f : facets) {
		P3 a = norm(f.v[0]), b = norm(f.v[1]), c = norm(f.v[2]), d = norm(f.v[3]);
		P3 u{c.x - a.x, c.y - a.y, c.z - a.z}, v{d.x - b.x, d.y - b.y, d.z - b.z};
		P3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
		double len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		double shade = len > 0 ? std::fabs(n.x * light.x + n.y * light.y + n.z * light.z) / len : 1;
		double zmid = (f.v[0].z + f.v[1].z + f.v[2].z + f.v[3].z) / 4;
		double r, g2, bl;
		ramp((zmid - lo.z) / (hi.z - lo.z), r, g2, bl);
		double k = 0.45 + 0.55 * shade;
		int R = (int)(r * k), G = (int)(g2 * k), B = (int)(bl * k);
		std::string pts;
		for (const P3 &p : f.v) {
			Q q = project(p);
			char pt[48];
			std::snprintf(pt, sizeof pt, "%.1f,%.1f ", px(q), py(q));
			pts += pt;
		}
		// the stroke in the fill colour hides hairline seams between facets
		std::snprintf(buf, sizeof buf, "<polygon points=\"%s\" fill=\"rgb(%d,%d,%d)\" stroke=\"rgb(%d,%d,%d)\" stroke-width=\"0.6\" />\n",
		              pts.c_str(), R, G, B, R, G, B);
		svg += buf;
	}

	for (const Curve &c : curves) {
		std::string pts;
		for (const P3 &p : c.pts) {
			Q q = project(p);
			char pt[48];
			std::snprintf(pt, sizeof pt, "%.1f,%.1f ", px(q), py(q));
			pts += pt;
		}
		svg += "<polyline points=\"" + pts + "\" fill=\"none\" stroke=\"rgb(0,4,192)\" stroke-width=\"1.6\" />\n";
	}

	// Axis ranges. x and y on the two bottom edges at the corner nearest the
	// viewer; z on the vertical edge furthest left, clear of the surface.
	int near = 0;
	for (int c = 1; c < 4; ++c)
		if (cq[c].depth < cq[near].depth)
			near = c;
	int left = 0;
	for (int c = 1; c < 4; ++c)
		if (px(cq[c]) < px(cq[left]))
			left = c;
	auto label = [&](double x, double y, const std::string &text, const char *anchor) {
		std::snprintf(buf, sizeof buf,
		              "<text x=\"%.1f\" y=\"%.1f\" font-family=\"sans-serif\" font-size=\"13\" fill=\"#333\" text-anchor=\"%s\">%s</text>\n",
		              x, y + 4, anchor, text.c_str());
		svg += buf;
	};
	const char *names[] = {"x", "y", "z"};
	const double lov[] = {lo.x, lo.y, lo.z}, hiv[] = {hi.x, hi.y, hi.z};
	for (int ax = 0; ax < 3; ++ax) {
		int bit = 1 << ax;
		int from = ax == 2 ? left : near;
		int a = from & ~bit, b = from | bit;
		double ax0 = px(cq[a]), ay0 = py(cq[a]), ax1 = px(cq[b]), ay1 = py(cq[b]);
		double ex = ax1 - ax0, ey = ay1 - ay0, len = std::sqrt(ex * ex + ey * ey);
		if (len < 1)
			continue;
		ex /= len, ey /= len;
		// the edge's normal, turned to point away from the picture centre
		double nx = -ey, ny = ex;
		double mx = (ax0 + ax1) / 2, my = (ay0 + ay1) / 2;
		if (nx * (mx - W / 2) + ny * (my - H / 2) < 0)
			nx = -nx, ny = -ny;
		const char *anchor = nx < -0.3 ? "end" : nx > 0.3 ? "start" : "middle";
		const double off = 14, inset = 0.12 * len;  // inset separates labels at a shared corner
		label(ax0 + nx * off + ex * inset, ay0 + ny * off + ey * inset, fmt(lov[ax]), anchor);
		label(ax1 + nx * off - ex * inset, ay1 + ny * off - ey * inset, fmt(hiv[ax]), anchor);
		label(mx + nx * off * 2.2, my + ny * off * 2.2, names[ax], anchor);
		line(cq[a], cq[b], "#808080", 1);
	}
	svg += "</svg>";
	return svg;
}
