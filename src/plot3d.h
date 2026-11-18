// 3D graphics as a static SVG picture.
//
// libgiac has no 3D export: Xcas draws 3D with OpenGL in its GUI. Giac's
// result does carry the parametrization (a surface's x(u,v), y(u,v),
// z(u,v) with the ranges of u and v; a curve's x(t), y(t), z(t)), so we
// sample it ourselves, project it with a fixed view and paint the facets
// back to front.
#pragma once

#include <giac/config.h>
#include <giac/giac.h>

#include <string>

// SVG for a 3D graphic (a pnt object or a list of them). Empty if it holds
// nothing we know how to draw (only surfaces and curves are supported).
std::string plot3d_svg(const giac::gen &g, const giac::context *ctx);
