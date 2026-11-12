// The dpi plugin (DPI-SPEC.md): dual Purkinje imaging. The algorithms of its stages
// ("pupil.hull", "glints.p1", "dpi.p4"), exposed for its tests, and its registration.
//
// The method is Ryan Ressmeyer's, from his OpenIrisDPI plugin for OpenIris
// (github.com/ryan-ressmeyer/OpenIrisDPI; Ressmeyer et al., J. Neurosci. Methods 2026, 110693).
// This is a new implementation written from a reading of that plugin and its wiki; no code was
// copied. DPI-SPEC 7 lists what differs.
#pragma once

#include <vector>

#include "openiris/output_types.hpp"
#include "openiris/pipeline.hpp"
#include "openiris/smoothing.hpp"

namespace oi {
/// Registers the three DPI stages (plugin.cpp, once, when the plugin loads).
void register_dpi_stages();
/// Registers gaze.dpi, gaze from P4 − P1 (calibration_dpi.cpp).
void register_dpi_calibration();
}  // namespace oi

namespace oi::dpi {

/// The sphere P4 − P1 turns on in an average eye (Le Grand's), mm: gaze.dpi's map before a grid
/// (PLUGINS-SPEC 8.8).
inline constexpr double kP4P1AverageRadiusMm = 6.5;

/// A reflection measured by its brightness above a background level (DPI-SPEC 4).
struct Spot {
  Status status = Status::NotMeasured;
  double x = kNaN;  ///< Brightness-weighted centroid, full-sensor px.
  double y = kNaN;
  double major = kNaN;  ///< Full axes of the moment ellipse (4√λ), px.
  double minor = kNaN;
  double angle = kNaN;  ///< Of the major axis, deg, from +x toward +y.
  double size = kNaN;   ///< √(major × minor): a uniform disk's diameter, 4σ for a Gaussian.
  double mass = 0;      ///< Summed brightness above the background (gray levels × px).
  [[nodiscard]] bool good() const { return status == Status::Good; }
};

struct HullParams {
  double threshold = 30;       ///< Gray level; darker is pupil (pipeline.pupil.threshold).
  double search_radius = 200;  ///< px around the rough center (pipeline.dpi.search_radius).
  int blur = 2;                ///< Smoothing radius, px (pipeline.dpi.blur).
};

struct HullPupil {
  Status status = Status::NotMeasured;
  double quality = kNaN;  ///< Fraction of the edge points on the hull's outline (not cut into).
  Point center;           ///< Centroid of the hull.
  double area = kNaN;     ///< px²
  double diameter = kNaN; ///< Of the circle with the same area.
  Ellipse ellipse;        ///< With the hull's first and second moments.
  std::vector<Point> hull;  ///< The hull's corners, in order.
  int dark_pixels = 0;
};

struct P1Params {
  double threshold = 245;      ///< Gray level; brighter can be P1 (pipeline.glints.threshold).
  double search_radius = 200;  ///< px around the rough pupil center.
  double roi_radius = 40;      ///< Half-width of the square where P1 is measured, px.
  double min_diameter = 5;     ///< Narrowest width of a real P1, px.
  int blur = 2;
  /// Where the rough position is (pipeline.glints.p1.rough). true: the largest bright region, so
  /// another bright reflection (a saturated P4, a second light) can't pull it off P1. false: the
  /// centroid of every bright pixel, as Ryan's plugin does.
  bool largest = true;
};

struct P4Params {
  double background = 30;    ///< Brightness above this counts (the pupil threshold).
  double roi_radius = 15;    ///< Half-width of the square where P4 is measured, px.
  double min_diameter = 0.5; ///< Narrowest width of a real P4, px.
  /// Widest P4, px (0: no limit, Ryan's rule). A brighter spot that's wider (P3, the reflection
  /// from the front of the lens, is larger than P4) is set aside and the next brightest tried.
  double max_diameter = 0;
  double edge_margin = 3;    ///< The pupil is shrunk by this much, px.
  double p1_exclusion = 40;  ///< Pixels this close to P1 are ignored, px.
  int blur = 2;
};

/// Working memory of one stage instance: nothing is allocated per frame once warmed up.
struct Workspace {
  Smoothed smoothed;
  std::vector<Point> points;
  std::vector<int> top;
  std::vector<int> bottom;
  std::vector<double> angles;
  std::vector<int> cells;  ///< P1's coarse grid: which bright region each bright block is in.
  std::vector<int> stack;
};

/// The pupil (DPI-SPEC 2, step 3): within the search circle around `rough`, each row's and column's
/// outermost pixels darker than the threshold, the edge placed where the smoothed gray level
/// crosses it; their convex hull, and the hull polygon's exact moments. NoPupil with fewer than a
/// few dark pixels.
void find_pupil(const EyeImage& image, Point rough, const HullParams& p, Workspace& w, HullPupil& out);

/// P1 (DPI-SPEC 2, step 4): the bright blocks within the search circle around `around`
/// (anywhere in the eye's region when `around` is NaN); the centroid of the largest connected
/// group of them (or of all, as Ryan's), then measured in a square around it. Failed when nothing is bright enough or its narrowest width is below min_diameter.
Spot find_p1(const EyeImage& image, Point around, const P1Params& p, Workspace& w);

/// P4 (DPI-SPEC 2, step 5): the brightest smoothed pixel inside `pupil` shrunk by the edge margin
/// and away from `p1` (ignored when NaN), then measured in a square around it on the pixels of
/// that same mask. With a maximum diameter, a spot wider than it is set aside and the next brightest
/// tried (a few times). Failed when the mask is empty, nothing is brighter than the background, or its
/// narrowest width is below min_diameter.
Spot find_p4(const EyeImage& image, const Ellipse& pupil, Point p1, const P4Params& p, Workspace& w);

}  // namespace oi::dpi
