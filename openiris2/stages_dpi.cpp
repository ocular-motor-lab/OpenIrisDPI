// Dual Purkinje imaging (DPI-SPEC.md): the pupil by convex hull (slot 4), P1 (slot 3)
// and P4 (extras). The method is Ryan Ressmeyer's, from his OpenIrisDPI plugin for OpenIris
// (github.com/ryan-ressmeyer/OpenIrisDPI; Ressmeyer et al., J. Neurosci. Methods 2026, 110693);
// this is a new implementation written from a reading of it, with no code copied. DPI-SPEC 7 says
// what differs and why.
#include "dpi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "openiris/outputs.hpp"
#include "dpi/outputs.hpp"  // the plugin's own outputs (PLUGINS-SPEC 18.10)
#include "openiris/pupil.hpp"

namespace oi {

namespace dpi {

namespace {

/// Fewer dark pixels than this in the search circle: no pupil.
constexpr int kMinDarkPixels = 10;
/// An edge point more than this (px) inside the hull is the edge of something that cuts into the
/// pupil (a reflection on its border, an eyelash): the pupil's own edge is unseen there. Points on
/// a seen edge are within a fraction of a pixel of the hull.
constexpr double kNotch = 1.0;
/// How many spots P4's search sets aside as too wide before giving up.
constexpr int kMaxP4Tries = 4;

Rect clip(const Rect& r, const Rect& bounds) {
  const int x0 = std::max(r.x, bounds.x);
  const int y0 = std::max(r.y, bounds.y);
  const int x1 = std::min(r.x + r.width, bounds.x + bounds.width);
  const int y1 = std::min(r.y + r.height, bounds.y + bounds.height);
  return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

/// A square of half-width r around (x, y), clipped.
Rect square(double x, double y, int r, const Rect& bounds) {
  const int cx = static_cast<int>(std::lround(x));
  const int cy = static_cast<int>(std::lround(y));
  return clip(Rect{cx - r, cy - r, 2 * r + 1, 2 * r + 1}, bounds);
}

bool finite(const Point& p) { return std::isfinite(p.x) && std::isfinite(p.y); }

/// The ellipse with central second moments mu20, mu02, mu11 (per unit mass): full axes 4√λ of the
/// covariance's eigenvalues (a uniform ellipse's own axes), angle of the major axis.
Ellipse moment_ellipse(double cx, double cy, double mu20, double mu02, double mu11) {
  const double mean = 0.5 * (mu20 + mu02);
  const double spread = std::sqrt(0.25 * (mu20 - mu02) * (mu20 - mu02) + mu11 * mu11);
  Ellipse e;
  e.center = {cx, cy};
  e.major = 4.0 * std::sqrt(std::max(0.0, mean + spread));
  e.minor = 4.0 * std::sqrt(std::max(0.0, mean - spread));
  e.angle = 0.5 * std::atan2(2.0 * mu11, mu20 - mu02) * 180.0 / std::numbers::pi;
  return e;
}

/// Brightness above `level` in `rect` of the smoothed image, where `use` says so: centroid and
/// second moments (Ryan's spot measure). Failed when nothing is above the level or the spot's
/// narrowest width is below `min_diameter`.
template <class Use>
Spot measure_spot(const Smoothed& s, const Rect& rect, double level, double min_diameter, Use use) {
  Spot spot;
  // Sums about the rectangle's center, so the second moments don't lose precision.
  const double ox = rect.x + 0.5 * (rect.width - 1);
  const double oy = rect.y + 0.5 * (rect.height - 1);
  double m = 0, mx = 0, my = 0, mxx = 0, myy = 0, mxy = 0;
  for (int y = rect.y; y < rect.y + rect.height; ++y) {
    const double dy = y - oy;
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
      const double w = s.at(x, y) - level;
      if (w <= 0 || !use(x, y)) continue;
      const double dx = x - ox;
      m += w;
      mx += w * dx;
      my += w * dy;
      mxx += w * dx * dx;
      myy += w * dy * dy;
      mxy += w * dx * dy;
    }
  }
  if (m <= 0) {
    spot.status = Status::Failed;
    return spot;
  }
  const double cx = mx / m;
  const double cy = my / m;
  const Ellipse e = moment_ellipse(ox + cx, oy + cy, mxx / m - cx * cx, myy / m - cy * cy, mxy / m - cx * cy);
  spot.x = e.center.x;
  spot.y = e.center.y;
  spot.major = e.major;
  spot.minor = e.minor;
  spot.angle = e.angle;
  spot.size = std::sqrt(e.major * e.minor);
  spot.mass = m;
  spot.status = e.minor >= min_diameter ? Status::Good : Status::Failed;
  return spot;
}

/// Andrew's monotone chain. Sorts `pts`; `hull` gets the corners, counterclockwise in a y-up frame.
void convex_hull(std::vector<Point>& pts, std::vector<Point>& hull) {
  hull.clear();
  const std::size_t n = pts.size();
  if (n < 3) {
    hull = pts;
    return;
  }
  std::sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
  auto cross = [](const Point& o, const Point& a, const Point& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  };
  hull.resize(2 * n);
  std::size_t k = 0;
  for (std::size_t i = 0; i < n; ++i) {
    while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
    hull[k++] = pts[i];
  }
  for (std::size_t i = n - 1, lower = k + 1; i > 0; --i) {
    while (k >= lower && cross(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0) --k;
    hull[k++] = pts[i - 1];
  }
  hull.resize(k - 1);  // the last point is the first
}

/// The fraction of `points` on the outline of the convex polygon `hull` (within kNotch of it,
/// along the ray from `c`, a point inside the hull). Each point's ray meets the hull edge between
/// the two corners around its direction, found by bisection on the corners' angles.
double fraction_on_hull(const std::vector<Point>& points, const std::vector<Point>& hull, Point c,
                        std::vector<double>& angles) {
  const std::size_t n = hull.size();
  if (points.empty() || n < 3) return 0.0;
  // The corners' angles, increasing from the smallest (the hull is convex around c, so they go
  // around once, in one direction).
  angles.resize(n);
  std::size_t first = 0;
  for (std::size_t i = 0; i < n; ++i) {
    angles[i] = std::atan2(hull[i].y - c.y, hull[i].x - c.x);
    if (angles[i] < angles[first]) first = i;
  }
  const bool increasing = std::remainder(angles[(first + 1) % n] - angles[first], 2 * std::numbers::pi) > 0;
  auto corner = [&](std::size_t k) {  // k-th corner in increasing angle
    return increasing ? (first + k) % n : (first + n - k) % n;
  };
  auto angle_of = [&](std::size_t k) { return angles[corner(k)]; };
  std::size_t on = 0;
  for (const Point& p : points) {
    const double dx = p.x - c.x;
    const double dy = p.y - c.y;
    const double rho = std::hypot(dx, dy);
    const double theta = std::atan2(dy, dx);
    // The last corner at or before theta (cyclic: before the first, it's the last one).
    std::size_t lo = 0, hi = n;
    while (hi - lo > 1) {
      const std::size_t mid = (lo + hi) / 2;
      if (angle_of(mid) <= theta) lo = mid;
      else hi = mid;
    }
    const std::size_t k = angle_of(0) <= theta ? lo : n - 1;
    const Point& a = hull[corner(k)];
    const Point& b = hull[corner((k + 1) % n)];
    // Where the ray from c through p meets the edge a-b.
    const double ex = b.x - a.x;
    const double ey = b.y - a.y;
    const double denom = dx * ey - dy * ex;
    if (rho <= 0 || std::abs(denom) < 1e-12) {
      ++on;
      continue;
    }
    const double t = ((a.x - c.x) * ey - (a.y - c.y) * ex) / denom;  // in units of (dx, dy)
    if ((t - 1.0) * rho <= kNotch) ++on;
  }
  return static_cast<double>(on) / static_cast<double>(points.size());
}

}  // namespace

void find_pupil(const EyeImage& image, Point rough, const HullParams& p, Workspace& w, HullPupil& out) {
  out.status = Status::NoPupil;
  out.quality = kNaN;
  out.center = {};
  out.area = out.diameter = kNaN;
  out.ellipse = {};
  out.hull.clear();
  out.dark_pixels = 0;
  if (!finite(rough)) return;
  const Rect region = eye_region(image);
  const double R = std::max(1.0, p.search_radius);
  const int half = static_cast<int>(std::ceil(R)) + 1;
  const Rect window = square(rough.x, rough.y, half, region);
  if (window.empty()) {
    out.status = Status::OutOfRoi;
    return;
  }
  w.smoothed.compute(image, window, p.blur);
  const Smoothed& s = w.smoothed;
  const double T = p.threshold;
  const double R2 = R * R;
  auto in_circle = [&](int x, int y) {
    const double dx = x - rough.x;
    const double dy = y - rough.y;
    return window.contains(x, y) && dx * dx + dy * dy <= R2;
  };
  // Where the edge crosses the threshold, between the outermost dark pixel (x, y) and its
  // neighbor outward (dx, dy): interpolated on the smoothed gray levels. Half a pixel out when the
  // neighbor is beyond the search circle.
  auto edge = [&](int x, int y, int dx, int dy) {
    double t = 0.5;
    if (in_circle(x + dx, y + dy)) {
      const double v0 = s.at(x, y);
      const double v1 = s.at(x + dx, y + dy);
      if (v1 > v0) t = std::clamp(1.0 - (v1 - T) / (v1 - v0), 0.0, 1.0);
    }
    return Point{x + dx * t, y + dy * t};
  };

  // Each row's outermost dark pixels, and each column's (collected on the way).
  w.points.clear();
  w.top.assign(static_cast<std::size_t>(window.width), -1);
  w.bottom.assign(static_cast<std::size_t>(window.width), -1);
  int dark = 0;
  for (int y = window.y; y < window.y + window.height; ++y) {
    const double dy = y - rough.y;
    const double span2 = R2 - dy * dy;
    if (span2 < 0) continue;
    const double span = std::sqrt(span2);
    const int xa = std::max(window.x, static_cast<int>(std::ceil(rough.x - span)));
    const int xb = std::min(window.x + window.width - 1, static_cast<int>(std::floor(rough.x + span)));
    int first = -1;
    int last = -1;
    for (int x = xa; x <= xb; ++x) {
      if (s.at(x, y) >= T) continue;
      if (first < 0) first = x;
      last = x;
      ++dark;
      const auto c = static_cast<std::size_t>(x - window.x);
      if (w.top[c] < 0) w.top[c] = y;
      w.bottom[c] = y;
    }
    if (first < 0) continue;
    w.points.push_back(edge(first, y, -1, 0));
    w.points.push_back(edge(last, y, 1, 0));
  }
  for (int c = 0; c < window.width; ++c) {
    if (w.top[static_cast<std::size_t>(c)] < 0) continue;
    const int x = window.x + c;
    w.points.push_back(edge(x, w.top[static_cast<std::size_t>(c)], 0, -1));
    w.points.push_back(edge(x, w.bottom[static_cast<std::size_t>(c)], 0, 1));
  }
  out.dark_pixels = dark;
  if (dark < kMinDarkPixels) return;

  convex_hull(w.points, out.hull);
  const std::size_t n = out.hull.size();
  if (n < 3) return;

  // The polygon's exact area, centroid and second moments (Green's theorem), about the rough
  // center so the sums keep their precision.
  double a2 = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Point& p0 = out.hull[i];
    const Point& p1 = out.hull[(i + 1) % n];
    const double x0 = p0.x - rough.x, y0 = p0.y - rough.y;
    const double x1 = p1.x - rough.x, y1 = p1.y - rough.y;
    const double a = x0 * y1 - x1 * y0;
    a2 += a;
    sx += (x0 + x1) * a;
    sy += (y0 + y1) * a;
    sxx += (x0 * x0 + x0 * x1 + x1 * x1) * a;
    syy += (y0 * y0 + y0 * y1 + y1 * y1) * a;
    sxy += (x0 * y1 + 2.0 * x0 * y0 + 2.0 * x1 * y1 + x1 * y0) * a;
  }
  const double area = 0.5 * a2;  // signed: the moments below keep the same sign
  if (std::abs(area) < 1.0) return;
  const double cx = sx / (6.0 * area);
  const double cy = sy / (6.0 * area);
  const double mu20 = sxx / (12.0 * area) - cx * cx;
  const double mu02 = syy / (12.0 * area) - cy * cy;
  const double mu11 = sxy / (24.0 * area) - cx * cy;
  out.center = {rough.x + cx, rough.y + cy};
  out.area = std::abs(area);
  out.diameter = 2.0 * std::sqrt(out.area / std::numbers::pi);
  out.ellipse = moment_ellipse(out.center.x, out.center.y, mu20, mu02, mu11);
  out.quality = fraction_on_hull(w.points, out.hull, out.center, w.angles);
  out.status = Status::Good;
}

Spot find_p1(const EyeImage& image, Point around, const P1Params& p, Workspace& w) {
  Spot failed;
  failed.status = Status::Failed;
  const Rect region = eye_region(image);
  const bool circle = finite(around);
  const double R = std::max(1.0, p.search_radius);
  const Rect window = circle ? square(around.x, around.y, static_cast<int>(std::ceil(R)) + 1, region) : region;
  if (window.empty()) return failed;

  // Rough position, from the bright blocks of the smoothed image (Ryan's downsampling: blocks
  // R / 100 px wide, 2 × 2 at the default radius of 200). Smoothing first, as his does, keeps a
  // single noisy pixel from counting.
  w.smoothed.compute(image, window, p.blur);
  const Smoothed& sm = w.smoothed;
  const Rect& area = sm.area();
  const int step = std::max(1, static_cast<int>(std::lround(R / 100.0)));
  const double block_threshold = p.threshold * step * step;
  const int gw = area.width / step;
  const int gh = area.height / step;
  if (gw <= 0 || gh <= 0) return failed;
  // Which blocks are bright: 0 dark, -1 bright (not yet in a region).
  w.cells.assign(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh), 0);
  auto cell = [&](int gx, int gy) -> int& {
    return w.cells[static_cast<std::size_t>(gy) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(gx)];
  };
  auto center_x = [&](int gx) { return area.x + gx * step + 0.5 * (step - 1); };
  auto center_y = [&](int gy) { return area.y + gy * step + 0.5 * (step - 1); };
  double sx = 0, sy = 0, n = 0;
  for (int gy = 0; gy < gh; ++gy) {
    const int by = area.y + gy * step;
    const double cy = center_y(gy);
    for (int gx = 0; gx < gw; ++gx) {
      const int bx = area.x + gx * step;
      const double cx = center_x(gx);
      if (circle && (cx - around.x) * (cx - around.x) + (cy - around.y) * (cy - around.y) > R * R) continue;
      double sum = 0;
      for (int yy = by; yy < by + step; ++yy) {
        for (int xx = bx; xx < bx + step; ++xx) sum += sm.at(xx, yy);
      }
      if (sum < block_threshold) continue;
      cell(gx, gy) = -1;
      sx += cx;
      sy += cy;
      n += 1;
    }
  }
  if (n == 0) return failed;
  if (p.largest) {
    // The largest group of touching bright blocks (8-connected): P1 is the biggest reflection.
    double best = 0, best_x = 0, best_y = 0;
    int label = 0;
    for (int gy = 0; gy < gh; ++gy) {
      for (int gx = 0; gx < gw; ++gx) {
        if (cell(gx, gy) != -1) continue;
        ++label;
        double count = 0, rx = 0, ry = 0;
        w.stack.clear();
        w.stack.push_back(gy * gw + gx);
        cell(gx, gy) = label;
        while (!w.stack.empty()) {
          const int k = w.stack.back();
          w.stack.pop_back();
          const int kx = k % gw, ky = k / gw;
          count += 1;
          rx += center_x(kx);
          ry += center_y(ky);
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              const int nx = kx + dx, ny = ky + dy;
              if (nx < 0 || ny < 0 || nx >= gw || ny >= gh || cell(nx, ny) != -1) continue;
              cell(nx, ny) = label;
              w.stack.push_back(ny * gw + nx);
            }
          }
        }
        if (count > best) {
          best = count;
          best_x = rx / count;
          best_y = ry / count;
        }
      }
    }
    sx = best_x;
    sy = best_y;
    n = 1;
  }

  // Measured in a square around it, on the smoothed image.
  const int r = std::max(1, static_cast<int>(std::lround(p.roi_radius)));
  const Rect box = square(sx / n, sy / n, r, region);
  w.smoothed.compute(image, box, p.blur);
  return measure_spot(w.smoothed, w.smoothed.area(), p.threshold, p.min_diameter, [](int, int) { return true; });
}

Spot find_p4(const EyeImage& image, const Ellipse& pupil, Point p1, const P4Params& p, Workspace& w) {
  Spot failed;
  failed.status = Status::Failed;
  const double a = 0.5 * pupil.major - p.edge_margin;
  const double b = 0.5 * pupil.minor - p.edge_margin;
  if (!finite(pupil.center) || !(a > 0) || !(b > 0)) return failed;
  const double theta = std::isfinite(pupil.angle) ? pupil.angle * std::numbers::pi / 180.0 : 0.0;
  const double c = std::cos(theta);
  const double s = std::sin(theta);
  // The shrunken pupil's bounding box.
  const double ex = std::sqrt(a * a * c * c + b * b * s * s);
  const double ey = std::sqrt(a * a * s * s + b * b * c * c);
  const int x0 = static_cast<int>(std::floor(pupil.center.x - ex));
  const int y0 = static_cast<int>(std::floor(pupil.center.y - ey));
  const int x1 = static_cast<int>(std::ceil(pupil.center.x + ex));
  const int y1 = static_cast<int>(std::ceil(pupil.center.y + ey));
  const Rect box = clip(Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1}, eye_region(image));
  if (box.empty()) return failed;
  w.smoothed.compute(image, box, p.blur);
  const Smoothed& sm = w.smoothed;

  const bool has_p1 = finite(p1);
  const double exclusion2 = p.p1_exclusion * p.p1_exclusion;
  // Spots set aside as too wide (P3): discs that are left out like P1's.
  std::array<std::array<double, 3>, kMaxP4Tries> rejected{};
  int n_rejected = 0;
  auto in_mask = [&](int x, int y) {
    const double dx = x - pupil.center.x;
    const double dy = y - pupil.center.y;
    const double u = (dx * c + dy * s) / a;
    const double v = (-dx * s + dy * c) / b;
    if (u * u + v * v > 1.0) return false;
    if (has_p1) {
      const double px = x - p1.x;
      const double py = y - p1.y;
      if (px * px + py * py <= exclusion2) return false;
    }
    for (int k = 0; k < n_rejected; ++k) {
      const auto& d = rejected[static_cast<std::size_t>(k)];
      if ((x - d[0]) * (x - d[0]) + (y - d[1]) * (y - d[1]) <= d[2] * d[2]) return false;
    }
    return true;
  };

  const int r = std::max(1, static_cast<int>(std::lround(p.roi_radius)));
  for (int attempt = 0; attempt < kMaxP4Tries; ++attempt) {
    // Rough position: the brightest pixel left.
    float best = -1;
    int bx = 0, by = 0;
    for (int y = box.y; y < box.y + box.height; ++y) {
      for (int x = box.x; x < box.x + box.width; ++x) {
        const float v = sm.at(x, y);
        if (v > best && in_mask(x, y)) {
          best = v;
          bx = x;
          by = y;
        }
      }
    }
    if (best < 0) return failed;
    Spot spot = measure_spot(sm, square(bx, by, r, box), p.background, p.min_diameter, in_mask);
    if (!spot.good() || p.max_diameter <= 0 || spot.major <= p.max_diameter) return spot;
    // Too wide for P4: leave it out, with a margin, and look again.
    rejected[static_cast<std::size_t>(n_rejected++)] = {spot.x, spot.y, 0.5 * spot.major + 2.0};
  }
  return failed;
}

}  // namespace dpi

namespace {

// ---- Settings the DPI stages share (the same declaration from each: the first one wins) ----

EyeParam<double> declare_search_radius(Settings& s) {
  ParamDesc d;
  d.path = "pipeline.dpi.search_radius";
  d.type = ParamType::Double;
  d.default_value = 200.0;
  d.min = 5;
  d.max = 1000;
  d.units = "px";
  d.help =
      "The pupil and P1 are searched within this distance of the rough pupil center: larger than the most "
      "dilated pupil, small enough to leave out eyelashes and other dark patches.";
  d.role = ParamRole::Subject;
  d.preview = Preview::circle_of(Preview::Size::Radius, "oi/pupil_approx/center");
  return s.declare_per_eye_param<double>(std::move(d));
}

Param<int> declare_blur(Settings& s) {
  return Param<int>{s.declare_int("pipeline.dpi.blur", 2, 0, kMaxBlur, "px",
                                  "Smoothing before the thresholds: the radius of a binomial kernel (about a Gaussian "
                                  "of σ = √(radius / 2) px). 0: none.")};
}

EyeParam<double> declare_roi_radius(Settings& s, const std::string& path, double def, const char* help, const char* center) {
  ParamDesc d;
  d.path = path;
  d.type = ParamType::Double;
  d.default_value = def;
  d.min = 1;
  d.max = 200;
  d.units = "px";
  d.help = help;
  d.role = ParamRole::Subject;
  d.preview = Preview::circle_of(Preview::Size::Radius, center);
  return s.declare_per_eye_param<double>(std::move(d));
}

constexpr const char* kCredit = "After Ryan Ressmeyer's OpenIrisDPI (DPI-SPEC).";

// ---- Slot 4: the pupil ----

class PupilHullStage : public Stage {
 public:
  [[nodiscard]] std::string name() const override { return "pupil.hull"; }

  void declare(Declarations& d) override {
    d.describe("Pupil (convex hull, DPI)",
               "Finds the pupil as the smallest convex shape around its dark pixels, so reflections inside it or on "
               "its edge don't dent it. Made for dual Purkinje (DPI) trackers, where P1 and P4 sit in the pupil. "
               "Ryan Ressmeyer's method (OpenIrisDPI).",
               std::string("On the smoothed image (pipeline.dpi.blur), within pipeline.dpi.search_radius of the rough "
                           "center: each row's and column's outermost pixels darker than the pupil threshold, the edge "
                           "interpolated to where the gray level crosses it; their convex hull; center, area and "
                           "ellipse from the hull polygon's exact moments. Quality: the fraction of those edge points "
                           "on the hull's outline (more than 1 px inside it, something cuts into the pupil there). ") +
                   kCredit);
    Settings& s = d.settings();
    threshold_ = declare_pupil_threshold(s);
    search_ = declare_search_radius(s);
    blur_ = declare_blur(s);
    d.uses("image", "The eye's image.");
    approx_ = d.can_use<out::PupilApproxCenter>("Where to search; without it, the stage makes its own first pass.");
    center_ = d.provide<out::PupilCenter>("Centroid of the hull.");
    size_ = d.provide<out::PupilSize>("Area of the hull, and the diameter of a circle as large.");
    ellipse_ = d.provide<out::PupilEllipse>("The ellipse with the hull's first and second moments.");
  }

  void process(ViewContext& v) override {
    const int e = index(v.eye());
    dpi::HullParams p;
    p.threshold = v.settings.get(threshold_[e]);
    p.search_radius = v.settings.get(search_[e]);
    p.blur = v.settings.get(blur_);
    Point rough;
    Status rough_status = Status::Good;
    if (const auto a = v.get(approx_); a && a.status() != Status::NotMeasured) {
      rough_status = a.status();
      rough = a->center;
    } else {
      PupilParams pp;
      pp.threshold = p.threshold;
      const PupilMeasure m = approximate_pupil(v.image, pp);
      rough_status = m.status;
      rough = {m.cx, m.cy};
    }
    if (rough_status != Status::Good) {
      write_status(v, rough_status);
      return;
    }
    dpi::find_pupil(v.image, rough, p, work_, pupil_);
    if (pupil_.status != Status::Good) {
      write_status(v, pupil_.status);
      return;
    }
    v.set(center_, {.center = pupil_.center}, Status::Good, pupil_.quality);
    v.set(size_, {.area = pupil_.area, .diameter = pupil_.diameter}, Status::Good, pupil_.quality);
    v.set(ellipse_, {.ellipse = pupil_.ellipse}, Status::Good, pupil_.quality);
  }

 private:
  void write_status(ViewContext& v, Status status) {
    v.write(center_, status);
    v.write(size_, status);
    v.write(ellipse_, status);
  }

  EyeParam<double> threshold_;
  EyeParam<double> search_;
  Param<int> blur_;
  In<out::PupilApproxCenter> approx_;
  Out<out::PupilCenter> center_;
  Out<out::PupilSize> size_;
  Out<out::PupilEllipse> ellipse_;
  dpi::Workspace work_;
  dpi::HullPupil pupil_;
};

// ---- Slot 3: P1, the corneal reflection ----

class GlintP1Stage : public Stage {
 public:
  [[nodiscard]] std::string name() const override { return "glints.p1"; }

  void declare(Declarations& d) override {
    d.describe("P1 (corneal reflection, DPI)",
               "Finds P1, the bright reflection of the light on the cornea, for a dual Purkinje (DPI) tracker, and "
               "gives it as the eye's glint too. Ryan Ressmeyer's method (OpenIrisDPI).",
               std::string("Rough position: on the smoothed image, the blocks at least as bright as the glint threshold "
                           "within pipeline.dpi.search_radius of the rough pupil center (anywhere in the eye's region "
                           "without one; blocks search_radius / 100 px wide), and the centroid of the largest group of "
                           "touching ones (rough = largest) or of all of them (rough = all, Ryan's rule). Then, in a square of half-width "
                           "roi_radius around it, the centroid and second moments of the smoothed brightness above "
                           "the threshold. Failed when the spot's narrowest width is below min_diameter. ") +
                   kCredit);
    Settings& s = d.settings();
    threshold_ = declare_glint_threshold(s);
    search_ = declare_search_radius(s);
    blur_ = declare_blur(s);
    roi_ = declare_roi_radius(s, d.prefix() + "roi_radius", 40,
                              "P1 is measured in a square of this half-width around its rough position: larger than "
                              "P1 and its halo.",
                              "dpi/p1");
    min_diameter_ = Param<double>{s.declare_double(d.prefix() + "min_diameter", 5, 0, 100, "px",
                                                   "A bright spot narrower than this isn't P1 (0: any).")};
    rough_ = Param<std::string>{s.declare_choice(
        d.prefix() + "rough", "largest", {"largest", "all"},
        "Where P1's rough position is among the bright pixels near the pupil. largest: in the largest bright "
        "region, so another bright reflection (a saturated P4, a second light) can't pull it off P1. all: the "
        "centroid of all of them, as Ryan's plugin does.")};
    d.uses("image", "The eye's image.");
    approx_ = d.can_use<out::PupilApproxCenter>("Where to search; without it, anywhere in the eye's region.");
    p1_ = d.provide<out::dpi::P1>("Position and size of P1.");
    glints_ = d.provide<out::Glints>("P1 as the eye's one glint, labeled P1.");
  }

  void process(ViewContext& v) override {
    const int e = index(v.eye());
    dpi::P1Params p;
    p.threshold = v.settings.get(threshold_[e]);
    p.search_radius = v.settings.get(search_[e]);
    p.roi_radius = v.settings.get(roi_[e]);
    p.min_diameter = v.settings.get(min_diameter_);
    p.blur = v.settings.get(blur_);
    p.largest = v.settings.get(rough_) != "all";
    Point around;  // NaN: anywhere
    if (const auto a = v.get(approx_); a.good()) around = a->center;
    const dpi::Spot spot = dpi::find_p1(v.image, around, p, work_);
    auto& g = v.write(glints_, Status::Good);
    if (!spot.good()) {
      v.write(p1_, spot.status);
      g.glints.count = 0;
      return;
    }
    v.set(p1_, {.p1 = {spot.x, spot.y}, .size = spot.size}, Status::Good);
    g.glints.count = 1;
    auto& item = g.glints.items[0];
    item.position = {spot.x, spot.y};
    item.size = spot.size;
    item.source = kNoLabel;
    item.purkinje = 0;  // "P1", the first of the definition's values
  }

 private:
  EyeParam<double> threshold_;
  EyeParam<double> search_;
  Param<int> blur_;
  EyeParam<double> roi_;
  Param<double> min_diameter_;
  Param<std::string> rough_;
  In<out::PupilApproxCenter> approx_;
  Out<out::dpi::P1> p1_;
  Out<out::Glints> glints_;
  dpi::Workspace work_;
};

// ---- Extras: P4, and the DPI signal ----

class DpiP4Stage : public Stage {
 public:
  [[nodiscard]] std::string name() const override { return "dpi.p4"; }

  void declare(Declarations& d) override {
    d.describe("P4 (lens reflection, DPI)",
               "Finds P4, the dim reflection of the light on the back of the lens, inside the pupil, and P4 − P1: the "
               "dual Purkinje (DPI) signal, which follows the eye's rotation and ignores small shifts. Ryan "
               "Ressmeyer's method (OpenIrisDPI).",
               std::string("Inside the pupil (its ellipse, or a circle of its diameter) shrunk by edge_margin, leaving "
                           "out p1_exclusion around P1: the brightest smoothed pixel, then the centroid and second "
                           "moments of the brightness above the pupil threshold in a square of half-width roi_radius "
                           "around it. Failed when the spot's narrowest width is below min_diameter, or without P1 "
                           "(P1 could be taken for P4). With max_diameter, a wider spot (P3) is set aside and the "
                           "next brightest tried. ") +
                   kCredit);
    Settings& s = d.settings();
    background_ = declare_pupil_threshold(s);
    blur_ = declare_blur(s);
    roi_ = declare_roi_radius(s, d.prefix() + "roi_radius", 15,
                              "P4 is measured in a square of this half-width around its brightest pixel: larger than "
                              "P4.",
                              "dpi/p4");
    min_diameter_ = Param<double>{s.declare_double(d.prefix() + "min_diameter", 0.5, 0, 50, "px",
                                                   "A spot narrower than this isn't P4 (0: any).")};
    max_diameter_ = Param<double>{s.declare_double(
        d.prefix() + "max_diameter", 0, 0, 100, "px",
        "A brighter spot wider than this isn't P4 (P3, the reflection from the front of the lens, is larger): it's "
        "set aside and the next brightest tried. 0: no limit, the brightest is P4 (Ryan's rule).")};
    edge_margin_ = Param<double>{
        s.declare_double(d.prefix() + "edge_margin", 3, 0, 50, "px",
                         "P4 is searched this far inside the pupil's edge, so bright iris pixels there don't count.")};
    p1_exclusion_ = Param<double>{s.declare_double(d.prefix() + "p1_exclusion", 40, 0, 200, "px",
                                                   "Pixels this close to P1 are left out, so P1 and its halo aren't "
                                                   "taken for P4.")};
    d.uses("image", "The eye's image.");
    p1_ = d.require<out::dpi::P1>("What to leave out; without P1, no P4.");
    center_ = d.require<out::PupilCenter>("Where P4 can be: inside the pupil.");
    size_ = d.require<out::PupilSize>("The pupil's extent, when there's no ellipse.");
    ellipse_ = d.can_use<out::PupilEllipse>("The pupil's shape, when a pupil stage gives one.");
    p4_ = d.provide<out::dpi::P4>("Position and size of P4.");
    signal_ = d.provide<out::dpi::P4P1>("P4 minus P1.");
  }

  void process(ViewContext& v) override {
    const int e = index(v.eye());
    dpi::P4Params p;
    p.background = v.settings.get(background_[e]);
    p.roi_radius = v.settings.get(roi_[e]);
    p.min_diameter = v.settings.get(min_diameter_);
    p.max_diameter = v.settings.get(max_diameter_);
    p.edge_margin = v.settings.get(edge_margin_);
    p.p1_exclusion = v.settings.get(p1_exclusion_);
    p.blur = v.settings.get(blur_);

    const auto p1 = v.get(p1_);
    const auto center = v.get(center_);
    dpi::Spot spot;
    if (!center.good()) {
      spot.status = center.status() == Status::NotMeasured ? Status::NoPupil : center.status();
    } else if (!p1.good()) {
      spot.status = Status::Failed;  // Ryan's rule: P1 not found could be taken for P4
    } else {
      Ellipse pupil;
      if (const auto el = v.get(ellipse_); el.good()) {
        pupil = el->ellipse;
      } else {
        const auto size = v.get(size_);
        pupil.center = center->center;
        pupil.major = pupil.minor = size.good() ? size->diameter : kNaN;
        pupil.angle = 0;
      }
      spot = dpi::find_p4(v.image, pupil, p1->p1, p, work_);
    }
    if (!spot.good()) {
      v.write(p4_, spot.status);
      v.write(signal_, p1.good() ? spot.status : (p1.status() == Status::NotMeasured ? Status::Failed : p1.status()));
      return;
    }
    v.set(p4_, {.p4 = {spot.x, spot.y}, .size = spot.size}, Status::Good);
    v.set(signal_, {.p4_p1 = {spot.x - p1->p1.x, spot.y - p1->p1.y}}, Status::Good);
  }

 private:
  EyeParam<double> background_;
  Param<int> blur_;
  EyeParam<double> roi_;
  Param<double> min_diameter_;
  Param<double> max_diameter_;
  Param<double> edge_margin_;
  Param<double> p1_exclusion_;
  In<out::dpi::P1> p1_;
  In<out::PupilCenter> center_;
  In<out::PupilSize> size_;
  In<out::PupilEllipse> ellipse_;
  Out<out::dpi::P4> p4_;
  Out<out::dpi::P4P1> signal_;
  dpi::Workspace work_;
};

}  // namespace

void register_dpi_stages() {
  auto& r = StageRegistry::instance();
  r.add({"glints.p1", "P1 for dual Purkinje (DPI) tracking (Ryan Ressmeyer's method).",
         [] { return std::make_unique<GlintP1Stage>(); }});
  r.add({"pupil.hull", "Convex hull of the dark pupil, for DPI (Ryan Ressmeyer's method).",
         [] { return std::make_unique<PupilHullStage>(); }});
  r.add({"dpi.p4", "P4 and P4 − P1 for DPI tracking (Ryan Ressmeyer's method).",
         [] { return std::make_unique<DpiP4Stage>(); }});
}

}  // namespace oi
