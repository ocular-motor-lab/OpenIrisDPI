// The dpi plugin (DPI-SPEC.md): the DPI stages on synthetic eye images with known
// pupil, P1 and P4 positions. No real DPI images are available yet (DPI-SPEC 8).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <random>

#include "dpi.hpp"
#include "openiris/monitor.hpp"
#include "openiris/outputs.hpp"
#include "dpi/outputs.hpp"  // the plugin's own outputs (PLUGINS-SPEC 18.10)
#include "openiris/pipeline.hpp"
#include "openiris/plugin_loader.hpp"
#include "openiris/profiles.hpp"
#include "openiris/source.hpp"
#include "openiris/synthetic.hpp"
#include "openiris/tracker.hpp"

using namespace oi;
using Catch::Approx;

namespace {

/// A DPI-like eye image: a dark elliptical pupil on a uniform iris, a saturated P1 and a dim
/// Gaussian P4, all at known sub-pixel positions. Pixel centers are at integers (SPEC 9.1); each
/// pixel is the mean of 4 × 4 samples over its area, so edges are anti-aliased.
struct Scene {
  int width = 480;
  int height = 400;
  double pupil_x = 240.37;
  double pupil_y = 199.62;
  double pupil_a = 105;  // semi-axes
  double pupil_b = 95;
  double pupil_angle = 25;  // deg, from +x toward +y
  double pupil_level = 20;
  double iris_level = 120;
  bool p1 = true;
  double p1_x = 215.3;
  double p1_y = 172.8;
  double p1_amplitude = 600;  // saturates: a flat core about 8 px across
  double p1_sigma = 3;
  bool p4 = true;
  double p4_x = 268.9;
  double p4_y = 221.4;
  double p4_amplitude = 110;
  double p4_sigma = 1.5;
  double noise = 0;  // gray levels
  unsigned seed = 1;

  /// A point on the pupil's edge, at parametric angle t (deg).
  [[nodiscard]] Point edge(double t_deg) const {
    const double t = t_deg * std::numbers::pi / 180.0;
    const double th = pupil_angle * std::numbers::pi / 180.0;
    const double u = pupil_a * std::cos(t);
    const double v = pupil_b * std::sin(t);
    return {pupil_x + u * std::cos(th) - v * std::sin(th), pupil_y + u * std::sin(th) + v * std::cos(th)};
  }
};

double spot(double x, double y, double cx, double cy, double amplitude, double sigma) {
  const double r2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
  if (r2 > 49 * sigma * sigma) return 0;
  return amplitude * std::exp(-r2 / (2 * sigma * sigma));
}

double scene_value(const Scene& s, double x, double y) {
  const double th = s.pupil_angle * std::numbers::pi / 180.0;
  const double dx = x - s.pupil_x;
  const double dy = y - s.pupil_y;
  const double u = (dx * std::cos(th) + dy * std::sin(th)) / s.pupil_a;
  const double v = (-dx * std::sin(th) + dy * std::cos(th)) / s.pupil_b;
  double value = u * u + v * v <= 1 ? s.pupil_level : s.iris_level;
  if (s.p1) value += spot(x, y, s.p1_x, s.p1_y, s.p1_amplitude, s.p1_sigma);
  if (s.p4) value += spot(x, y, s.p4_x, s.p4_y, s.p4_amplitude, s.p4_sigma);
  return std::min(value, 255.0);
}

struct Rendered {
  FramePool pool{1, 480, 400};
  FrameRef frame;

  void render(const Scene& s) {
    frame.reset();  // the pool has one frame: give it back first
    frame = pool.acquire();
    Frame& f = *frame.writable();
    f.resize(s.width, s.height);
    f.mutable_info().roi = Rect{0, 0, s.width, s.height};
    std::mt19937 rng(s.seed);
    std::normal_distribution<double> noise(0.0, 1.0);
    constexpr int kSub = 4;
    for (int y = 0; y < s.height; ++y) {
      std::uint8_t* row = f.mutable_row(y);
      for (int x = 0; x < s.width; ++x) {
        double sum = 0;
        for (int j = 0; j < kSub; ++j) {
          for (int i = 0; i < kSub; ++i) {
            sum += scene_value(s, x - 0.5 + (i + 0.5) / kSub, y - 0.5 + (j + 0.5) / kSub);
          }
        }
        double v = sum / (kSub * kSub);
        if (s.noise > 0) v += s.noise * noise(rng);
        row[x] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
      }
    }
  }
  [[nodiscard]] EyeImage image() const { return EyeImage{frame.get(), Eye::Left, {}, {}}; }
};

// Thresholds for the scenes: the pupil threshold halfway between pupil and iris (so the edge
// found is the true edge), the P1 threshold as Ryan's default.
dpi::HullParams hull_params() {
  dpi::HullParams p;
  p.threshold = 70;
  return p;
}
dpi::P1Params p1_params() { return {}; }
dpi::P4Params p4_params() {
  dpi::P4Params p;
  p.background = 70;
  return p;
}

double distance(double x0, double y0, double x1, double y1) { return std::hypot(x1 - x0, y1 - y0); }

/// The DPI stages for one view, declared as the tracker does (in the order they run), with a
/// sample laid out from their declarations.
struct Pipeline1 {
  Settings settings;
  std::vector<StagePtr> stages;
  SampleLayoutPtr layout;

  explicit Pipeline1(const std::vector<std::string>& names) {
    load_plugins();  // the DPI stages come from the dpi plugin's module
    std::vector<PluginDeclaration> declared;
    int k = 0;
    for (const auto& n : names) {
      auto s = StageRegistry::instance().create(n);
      REQUIRE(s);
      Declarations d(settings, "pipeline." + n + ".", n, k++);
      s->declare(d);
      declared.push_back(d.take());
      stages.push_back(std::move(s));
    }
    layout = SampleLayout::build(declared, {Eye::Left, Eye::Right});  // throws if they don't fit
    REQUIRE(settings.set("pipeline.pupil.threshold", 70.0).empty());
    REQUIRE(settings.set("pipeline.glints.threshold", 245.0).empty());
  }

  std::unique_ptr<SampleBlock> run(const EyeImage& image) {
    const auto snap = settings.snapshot();
    auto sample = SampleBlock::make(layout);
    const int view = layout->view_of(Eye::Left);
    ViewContext v{image, view, *snap, *sample, sample->info.views[static_cast<std::size_t>(view)]};
    for (auto& s : stages) s->process(v);
    return sample;
  }
};

const std::vector<std::string> kDpiStages = {"pupil.centroid.approx", "glints.p1", "pupil.hull", "dpi.p4"};

}  // namespace

TEST_CASE("DPI smoothing keeps flat areas flat and spots where they are") {
  Scene s;
  s.p4 = false;
  Rendered r;
  r.render(s);
  Smoothed sm;
  // Radius 0 copies the pixels.
  sm.compute(r.image(), Rect{10, 10, 20, 20}, 0);
  REQUIRE(sm.area() == Rect{10, 10, 20, 20});
  for (int y = 10; y < 30; ++y) {
    for (int x = 10; x < 30; ++x) REQUIRE(sm.at(x, y) == r.frame->at(x, y));
  }
  // The iris stays at its level; the corner of the image repeats its edge.
  sm.compute(r.image(), Rect{-5, -5, 30, 30}, 2);
  REQUIRE(sm.area() == Rect{0, 0, 25, 25});
  REQUIRE(sm.at(0, 0) == Approx(s.iris_level).margin(1e-3));
  REQUIRE(sm.at(12, 12) == Approx(s.iris_level).margin(1e-3));
  // A symmetric spot stays centered: its brightness-weighted centroid doesn't move.
  sm.compute(r.image(), Rect{195, 152, 41, 41}, 3);
  double m = 0, mx = 0, my = 0;
  for (int y = 152; y < 193; ++y) {
    for (int x = 195; x < 236; ++x) {
      const double w = std::max(0.0, sm.at(x, y) - 100.0);
      m += w;
      mx += w * x;
      my += w * y;
    }
  }
  REQUIRE(mx / m == Approx(s.p1_x).margin(0.02));
  REQUIRE(my / m == Approx(s.p1_y).margin(0.02));
}

TEST_CASE("the hull pupil matches the true ellipse") {
  Rendered r;
  dpi::Workspace w;
  dpi::HullPupil pupil;
  double max_err = 0;
  for (double angle : {0.0, 25.0, -60.0, 90.0}) {
    for (double shift : {0.0, 0.31, 0.77}) {
      Scene s;
      s.pupil_angle = angle;
      s.pupil_x += shift;
      s.pupil_y -= shift / 2;
      s.p1_x += shift;
      s.p4_x += shift;
      r.render(s);
      // A rough center a few pixels off, as the first pass gives.
      dpi::find_pupil(r.image(), {s.pupil_x + 3.4, s.pupil_y - 2.2}, hull_params(), w, pupil);
      INFO("angle " << angle << ", shift " << shift);
      REQUIRE(pupil.status == Status::Good);
      max_err = std::max(max_err, distance(pupil.center.x, pupil.center.y, s.pupil_x, s.pupil_y));
      REQUIRE(pupil.ellipse.major == Approx(2 * s.pupil_a).margin(1.0));
      REQUIRE(pupil.ellipse.minor == Approx(2 * s.pupil_b).margin(1.0));
      // Same axis, modulo 180°.
      const double da = std::remainder(pupil.ellipse.angle - angle, 180.0);
      REQUIRE(std::abs(da) < 1.0);
      REQUIRE(pupil.area == Approx(std::numbers::pi * s.pupil_a * s.pupil_b).epsilon(0.01));
      // P1 and P4 are inside: the hull's outline is all seen edge.
      REQUIRE(pupil.quality > 0.99);
    }
  }
  INFO("max pupil center error " << max_err << " px");
  REQUIRE(max_err < 0.05);
}

TEST_CASE("a reflection on the pupil's edge doesn't dent the hull, and lowers its quality") {
  Rendered r;
  dpi::Workspace w;
  dpi::HullPupil pupil;
  for (double t : {200.0, 300.0, 45.0}) {
    Scene s;
    const Point at = s.edge(t);
    s.p1_x = at.x;
    s.p1_y = at.y;
    r.render(s);
    dpi::find_pupil(r.image(), {s.pupil_x, s.pupil_y}, hull_params(), w, pupil);
    INFO("P1 on the edge at " << t << " deg");
    REQUIRE(pupil.status == Status::Good);
    REQUIRE(distance(pupil.center.x, pupil.center.y, s.pupil_x, s.pupil_y) < 0.1);
    REQUIRE(pupil.quality < 0.995);
    REQUIRE(pupil.quality > 0.9);
  }
}

TEST_CASE("no dark region: no hull pupil") {
  Scene s;
  s.pupil_level = s.iris_level;  // no pupil
  Rendered r;
  r.render(s);
  dpi::Workspace w;
  dpi::HullPupil pupil;
  dpi::find_pupil(r.image(), {240, 200}, hull_params(), w, pupil);
  REQUIRE(pupil.status == Status::NoPupil);
  // Outside the image altogether.
  dpi::find_pupil(r.image(), {-1000, -1000}, hull_params(), w, pupil);
  REQUIRE(pupil.status == Status::OutOfRoi);
}

TEST_CASE("P1 and P4 are found at their true positions") {
  Rendered r;
  dpi::Workspace w;
  double max_p1 = 0, max_p4 = 0;
  for (double dx : {0.0, 0.23, 0.5, 0.81}) {
    for (double dy : {0.0, 0.37, 0.66}) {
      Scene s;
      s.p1_x += dx;
      s.p1_y += dy;
      s.p4_x -= dy;
      s.p4_y += dx;
      r.render(s);
      const auto p1 = dpi::find_p1(r.image(), {s.pupil_x + 2, s.pupil_y - 3}, p1_params(), w);
      REQUIRE(p1.good());
      max_p1 = std::max(max_p1, distance(p1.x, p1.y, s.p1_x, s.p1_y));
      // The saturated core is about 8.4 px across; the size, from the brightness above the
      // threshold (weights that taper off at the rim), is a little smaller.
      REQUIRE(p1.size > 4);
      REQUIRE(p1.size < 9);
      Ellipse pupil;
      pupil.center = {s.pupil_x, s.pupil_y};
      pupil.major = 2 * s.pupil_a;
      pupil.minor = 2 * s.pupil_b;
      pupil.angle = s.pupil_angle;
      const auto p4 = dpi::find_p4(r.image(), pupil, {p1.x, p1.y}, p4_params(), w);
      REQUIRE(p4.good());
      max_p4 = std::max(max_p4, distance(p4.x, p4.y, s.p4_x, s.p4_y));
      REQUIRE(p4.size > 1);
      REQUIRE(p4.size < 6);
    }
  }
  INFO("max P1 error " << max_p1 << " px, max P4 error " << max_p4 << " px");
  REQUIRE(max_p1 < 0.05);
  REQUIRE(max_p4 < 0.08);
}

TEST_CASE("with sensor noise, the pupil, P1 and P4 stay sub-pixel") {
  Rendered r;
  dpi::Workspace w;
  dpi::HullPupil pupil;
  double sum_pupil = 0, sum_p1 = 0, sum_p4 = 0;
  constexpr int kN = 12;
  for (int i = 0; i < kN; ++i) {
    Scene s;
    s.noise = 3;
    s.seed = static_cast<unsigned>(i + 1);
    s.p1_x += 0.07 * i;
    s.p4_y -= 0.05 * i;
    r.render(s);
    dpi::find_pupil(r.image(), {s.pupil_x - 2, s.pupil_y + 1}, hull_params(), w, pupil);
    REQUIRE(pupil.status == Status::Good);
    sum_pupil += distance(pupil.center.x, pupil.center.y, s.pupil_x, s.pupil_y);
    const auto p1 = dpi::find_p1(r.image(), pupil.center, p1_params(), w);
    REQUIRE(p1.good());
    sum_p1 += distance(p1.x, p1.y, s.p1_x, s.p1_y);
    const auto p4 = dpi::find_p4(r.image(), pupil.ellipse, {p1.x, p1.y}, p4_params(), w);
    REQUIRE(p4.good());
    sum_p4 += distance(p4.x, p4.y, s.p4_x, s.p4_y);
  }
  INFO("mean errors: pupil " << sum_pupil / kN << ", P1 " << sum_p1 / kN << ", P4 " << sum_p4 / kN << " px");
  REQUIRE(sum_pupil / kN < 0.1);
  REQUIRE(sum_p1 / kN < 0.1);
  REQUIRE(sum_p4 / kN < 0.1);
}

TEST_CASE("P1's rough position is the largest bright region: a saturated P4 doesn't pull it away") {
  // A P4 that saturates too, over a smaller area than P1 (an image too bright for DPI, or a small,
  // bright LED): the centroid of every bright pixel (Ryan's rule) lands between them, and the square
  // where P1 is measured misses P1. The largest bright region is P1. (The rendered eyes' P4
  // saturates only in its center at their default exposure, too little to show this.)
  Rendered r;
  dpi::Workspace w;
  Scene s;
  s.p4_amplitude = 600;
  s.p4_sigma = 2.5;  // a saturated core about 6.5 px across, against P1's 8.4
  r.render(s);
  dpi::P1Params p = p1_params();
  p.roi_radius = 15;
  const Point around{s.pupil_x + 2, s.pupil_y - 3};
  p.largest = false;
  const auto all = dpi::find_p1(r.image(), around, p, w);
  INFO("all bright pixels: " << (all.good() ? distance(all.x, all.y, s.p1_x, s.p1_y) : -1.0) << " px from P1");
  CHECK((!all.good() || distance(all.x, all.y, s.p1_x, s.p1_y) > 1));
  p.largest = true;
  const auto largest = dpi::find_p1(r.image(), around, p, w);
  REQUIRE(largest.good());
  CHECK(distance(largest.x, largest.y, s.p1_x, s.p1_y) < 0.05);
}

TEST_CASE("P4 isn't taken near P1, near the pupil's edge, or when it isn't there") {
  Rendered r;
  dpi::Workspace w;
  Scene base;
  Ellipse pupil;
  pupil.center = {base.pupil_x, base.pupil_y};
  pupil.major = 2 * base.pupil_a;
  pupil.minor = 2 * base.pupil_b;
  pupil.angle = base.pupil_angle;
  SECTION("within the P1 exclusion: left out, and nothing else is bright enough") {
    Scene s;
    s.p4_x = s.p1_x + 20;
    s.p4_y = s.p1_y + 15;
    r.render(s);
    const auto p1 = dpi::find_p1(r.image(), pupil.center, p1_params(), w);
    REQUIRE(p1.good());
    REQUIRE(dpi::find_p4(r.image(), pupil, {p1.x, p1.y}, p4_params(), w).status == Status::Failed);
  }
  SECTION("within the edge margin") {
    Scene s;
    const Point at = s.edge(120);
    s.p4_x = at.x;
    s.p4_y = at.y;
    r.render(s);
    // With no margin, iris pixels at the edge count: the brightest is the bright side of the edge.
    dpi::P4Params p = p4_params();
    p.edge_margin = 8;
    REQUIRE(dpi::find_p4(r.image(), pupil, {s.p1_x, s.p1_y}, p, w).status == Status::Failed);
  }
  SECTION("no P4 in the image") {
    Scene s;
    s.p4 = false;
    r.render(s);
    REQUIRE(dpi::find_p4(r.image(), pupil, {s.p1_x, s.p1_y}, p4_params(), w).status == Status::Failed);
  }
  SECTION("a single bright pixel is too narrow") {
    Scene s;
    s.p4 = false;
    r.render(s);
    const int x = static_cast<int>(std::lround(s.p4_x));
    const int y = static_cast<int>(std::lround(s.p4_y));
    r.frame.writable()->mutable_row(y)[x] = 250;
    dpi::P4Params p = p4_params();
    p.blur = 0;
    REQUIRE(dpi::find_p4(r.image(), pupil, {s.p1_x, s.p1_y}, p, w).status == Status::Failed);
  }
  SECTION("an empty mask") {
    Ellipse tiny = pupil;
    tiny.major = tiny.minor = 4;
    r.render(base);
    REQUIRE(dpi::find_p4(r.image(), tiny, {base.p1_x, base.p1_y}, p4_params(), w).status == Status::Failed);
  }
}

TEST_CASE("the DPI stages run as a pipeline and write their outputs") {
  Pipeline1 pl(kDpiStages);
  Rendered r;
  Scene s;
  r.render(s);
  const auto sample = pl.run(r.image());
  const int view = pl.layout->view_of(Eye::Left);

  const auto center = sample->read<out::PupilCenter>(view);
  REQUIRE(center.good());
  REQUIRE(distance(center->center.x, center->center.y, s.pupil_x, s.pupil_y) < 0.05);
  REQUIRE(center.header->quality > 0.99);
  REQUIRE(sample->read<out::PupilEllipse>(view)->ellipse.major == Approx(2 * s.pupil_a).margin(1.0));
  REQUIRE(sample->read<out::PupilSize>(view)->area == Approx(std::numbers::pi * s.pupil_a * s.pupil_b).epsilon(0.01));

  const auto p1 = sample->read<out::dpi::P1>(view);
  REQUIRE(p1.good());
  REQUIRE(distance(p1->p1.x, p1->p1.y, s.p1_x, s.p1_y) < 0.05);
  // P1 is the eye's glint too, labeled P1.
  const auto glints = sample->read<out::Glints>(view);
  REQUIRE(glints.good());
  REQUIRE(glints->glints.count == 1);
  REQUIRE(glints->glints.items[0].position.x == p1->p1.x);
  REQUIRE(glints->glints.items[0].size == p1->size);
  REQUIRE(glints->glints.items[0].purkinje == 0);  // "P1"
  REQUIRE(out::Glints::def().fields[0].items[3].values[0] == "P1");

  const auto p4 = sample->read<out::dpi::P4>(view);
  REQUIRE(p4.good());
  REQUIRE(distance(p4->p4.x, p4->p4.y, s.p4_x, s.p4_y) < 0.08);
  const auto signal = sample->read<out::dpi::P4P1>(view);
  REQUIRE(signal.good());
  REQUIRE(signal->p4_p1.x == p4->p4.x - p1->p1.x);
  REQUIRE(signal->p4_p1.y == p4->p4.y - p1->p1.y);
}

TEST_CASE("without P1 there's no P4 (Ryan's rule), and without a pupil neither") {
  Pipeline1 pl(kDpiStages);
  Rendered r;
  const int view = pl.layout->view_of(Eye::Left);
  {
    Scene s;
    s.p1 = false;
    r.render(s);
    const auto sample = pl.run(r.image());
    REQUIRE(sample->read<out::PupilCenter>(view).good());
    REQUIRE(sample->read<out::dpi::P1>(view).status() == Status::Failed);
    REQUIRE(sample->read<out::Glints>(view)->glints.count == 0);
    REQUIRE(sample->read<out::dpi::P4>(view).status() == Status::Failed);
    REQUIRE(sample->read<out::dpi::P4P1>(view).status() == Status::Failed);
    REQUIRE(std::isnan(sample->read<out::dpi::P4P1>(view)->p4_p1.x));
  }
  {
    Scene s;
    s.pupil_level = s.iris_level;
    r.render(s);
    const auto sample = pl.run(r.image());
    REQUIRE(sample->read<out::PupilCenter>(view).status() == Status::NoPupil);
    REQUIRE(sample->read<out::dpi::P1>(view).good());  // P1 is searched anywhere without a pupil
    REQUIRE(sample->read<out::dpi::P4>(view).status() == Status::NoPupil);
    REQUIRE(sample->read<out::dpi::P4P1>(view).status() == Status::NoPupil);
  }
}

TEST_CASE("P4 works after another pupil stage, from its center and size") {
  // pupil.centroid gives no ellipse: P4 searches a circle of the pupil's diameter.
  Pipeline1 pl({"pupil.centroid.approx", "glints.p1", "pupil.centroid", "dpi.p4"});
  Rendered r;
  Scene s;
  s.pupil_b = s.pupil_a;  // a round pupil, so the circle is the pupil
  r.render(s);
  const auto sample = pl.run(r.image());
  const int view = pl.layout->view_of(Eye::Left);
  REQUIRE(sample->read<out::PupilEllipse>(view).status() == Status::NotMeasured);
  const auto p4 = sample->read<out::dpi::P4>(view);
  REQUIRE(p4.good());
  REQUIRE(distance(p4->p4.x, p4->p4.y, s.p4_x, s.p4_y) < 0.08);
}

TEST_CASE("P4 needs P1 and a pupil before it: the layout check says so") {
  load_plugins();
  Settings settings;
  auto p4 = StageRegistry::instance().create("dpi.p4");
  REQUIRE(p4);
  Declarations d(settings, "pipeline.dpi.p4.", "dpi.p4", 0);
  p4->declare(d);
  try {
    (void)SampleLayout::build({d.take()}, {Eye::Left, Eye::Right});
    FAIL("dpi.p4 alone should not fit");
  } catch (const LayoutError& e) {
    const auto& problems = e.problems();
    auto mentions = [&](const std::string& id) {
      return std::any_of(problems.begin(), problems.end(), [&](const std::string& p) { return p.find(id) != std::string::npos; });
    };
    REQUIRE(mentions("dpi/p1"));
    REQUIRE(mentions("oi/pupil/center"));
  }
}

TEST_CASE("the DPI stages are in plugins.describe, with their outputs and settings") {
  Tracker t;
  const Json all = t.commands().invoke("plugins.describe")["plugins"];
  auto find = [&](const std::string& id) {
    for (const auto& p : all) {
      if (p["id"] == id) return p;
    }
    FAIL("no plugin " << id);
    return Json();
  };
  auto ids = [](const Json& ports) {
    std::vector<std::string> out;
    for (const auto& p : ports) out.push_back(p["id"]);
    return out;
  };
  auto settings = [](const Json& p) {
    std::vector<std::string> out;
    for (const auto& s : p["settings"]) out.push_back(s["path"]);
    return out;
  };
  auto has = [](const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); };

  const Json p1 = find("glints.p1");
  CHECK(ids(p1["outputs"]) == std::vector<std::string>{"dpi/p1", "oi/glints"});
  CHECK(has(settings(p1), "pipeline.glints.threshold"));
  CHECK(has(settings(p1), "pipeline.glints.p1.roi_radius"));
  CHECK(has(settings(p1), "pipeline.dpi.search_radius"));
  CHECK(p1["developer"].get<std::string>().find("Ressmeyer") != std::string::npos);

  const Json hull = find("pupil.hull");
  CHECK(ids(hull["outputs"]) == std::vector<std::string>{"oi/pupil/center", "oi/pupil/size", "oi/pupil/ellipse"});
  CHECK(has(settings(hull), "pipeline.pupil.threshold"));

  const Json p4 = find("dpi.p4");
  CHECK(ids(p4["outputs"]) == std::vector<std::string>{"dpi/p4", "dpi/p4_p1"});
  const auto in = ids(p4["inputs"]);
  CHECK(has(in, "dpi/p1"));
  CHECK(has(in, "oi/pupil/center"));
  CHECK(has(settings(p4), "pipeline.dpi.p4.roi_radius"));
  CHECK(has(settings(p4), "pipeline.dpi.p4.edge_margin"));
  CHECK(has(settings(p4), "pipeline.pupil.threshold"));  // P4's background, shared with the pupil
}

TEST_CASE("the DPI rig profile's stages fit together, with Ryan's settings") {
  load_plugins();  // the profiles come with the dpi plugin's module
  const auto& profiles = built_in_profiles();
  const auto it = std::find_if(profiles.begin(), profiles.end(), [](const Json& p) { return p["name"] == "DPI rig"; });
  REQUIRE(it != profiles.end());
  REQUIRE(validate_profile(*it).empty());
  Tracker t;
  if (SourceRegistry::instance().find("spinnaker").has_value()) {
    REQUIRE(t.load_profile("DPI rig").empty());
    CHECK(t.settings().to_json().at("pipeline.pupil.threshold.left") == 30.0);
    CHECK(t.settings().to_json().at("pipeline.glints.threshold.right") == 245.0);
  } else {
    // Without the Spinnaker SDK, the same pipeline on synthetic eyes.
    t.set_pipeline((*it)["pipeline"]);
    t.set_calibration((*it)["calibration"]);
  }
  // Calibrated by gaze.dpi, which reads P1 and P4.
  CHECK((*it)["calibration"] == Json::array({"gaze.dpi"}));
  CHECK(t.plugin_problems().empty());
  // Its Monitor panel plots P4 − P1, x and y, for both eyes.
  REQUIRE((*it)["monitor"]["panels"][0] == "dpi/p4_p1");
  const Pipeline1 pl(kDpiStages);
  const auto columns = panel_columns("dpi/p4_p1", pl.layout.get(), pl.settings.to_json());
  CHECK(columns.size() == 4);
  CHECK(std::find(columns.begin(), columns.end(), "left_dpi_p4_p1_x") != columns.end());
  CHECK(std::find(columns.begin(), columns.end(), "right_dpi_p4_p1_y") != columns.end());
}

TEST_CASE("synthetic eyes with P4: the DPI profile's stages follow the truth as the eyes move") {
  // The settings of the plugin's profile, so this checks them too.
  load_plugins();  // the profiles come with the dpi plugin's module
  const auto& profiles = built_in_profiles();
  const auto it =
      std::find_if(profiles.begin(), profiles.end(), [](const Json& p) { return p["name"] == "Synthetic eyes, DPI"; });
  REQUIRE(it != profiles.end());
  Pipeline1 pl(kDpiStages);
  SyntheticParams sp;
  for (const auto& [path, value] : (*it)["settings"].items()) {
    if (path.rfind("pipeline.", 0) == 0) REQUIRE(pl.settings.set_json(path, value, ChangeOrigin::Profile).empty());
  }
  sp.p4 = (*it)["settings"].value("source.synthetic.p4", false);
  REQUIRE(sp.p4);
  sp.noise = 2;

  FramePool pool{1, sp.width, sp.height};
  const int view = pl.layout->view_of(Eye::Left);
  double max_pupil = 0, max_p1 = 0, max_p4 = 0, sum_p1 = 0, sum_p4 = 0;
  double min_dx = 1e9, max_dx = -1e9;
  int n = 0;
  for (double t = 0; t < 4.0; t += 0.037) {
    FrameRef frame = pool.acquire();
    frame.writable()->resize(sp.width, sp.height);
    frame.writable()->mutable_info().roi = Rect{0, 0, sp.width, sp.height};
    const SyntheticTruth truth = synthetic_truth(Eye::Left, t, sp);
    REQUIRE(truth.p4);
    render_synthetic_eye(*frame.writable(), truth, sp.noise, static_cast<std::uint64_t>(t * 1000) + 1);
    const auto sample = pl.run(EyeImage{frame.get(), Eye::Left, {}, {}});
    INFO("t = " << t << ", pupil " << truth.cx << "," << truth.cy << " r " << truth.radius << ", glint " << truth.glint_x << "," << truth.glint_y << ", p4 " << truth.p4_x << "," << truth.p4_y);
    const auto pupil = sample->read<out::PupilCenter>(view);
    const auto p1 = sample->read<out::dpi::P1>(view);
    const auto p4 = sample->read<out::dpi::P4>(view);
    const auto signal = sample->read<out::dpi::P4P1>(view);
    REQUIRE(pupil.good());
    REQUIRE(p1.good());
    REQUIRE(p4.good());
    REQUIRE(signal.good());
    max_pupil = std::max(max_pupil, distance(pupil->center.x, pupil->center.y, truth.centroid_x, truth.centroid_y));
    const double e1 = distance(p1->p1.x, p1->p1.y, truth.glint_x, truth.glint_y);
    const double e4 = distance(p4->p4.x, p4->p4.y, truth.p4_x, truth.p4_y);
    max_p1 = std::max(max_p1, e1);
    max_p4 = std::max(max_p4, e4);
    sum_p1 += e1;
    sum_p4 += e4;
    min_dx = std::min(min_dx, signal->p4_p1.x);
    max_dx = std::max(max_dx, signal->p4_p1.x);
    ++n;
    frame.reset();
  }
  INFO("max errors: pupil " << max_pupil << ", P1 " << max_p1 << ", P4 " << max_p4 << " px; mean P1 " << sum_p1 / n
                            << ", P4 " << sum_p4 / n << " px; P4 - P1 x from " << min_dx << " to " << max_dx);
  // Small spots (about 3 px) and noise: a P1 on the pupil's edge, half on the brighter iris, is
  // pulled toward it by a few tenths of a pixel.
  CHECK(max_pupil < 0.2);
  CHECK(max_p1 < 0.5);
  CHECK(max_p4 < 0.4);
  CHECK(sum_p1 / n < 0.15);
  CHECK(sum_p4 / n < 0.15);
  // P4 - P1 follows the eye's rotation: it moves by 0.4 of the pupil's movement.
  CHECK(max_dx - min_dx > 10);
}

// Not run by ctest (hidden): `openiris_tests "[.timing]"` prints the time per eye image.
TEST_CASE("DPI stages: time per eye image", "[.timing]") {
  Pipeline1 pl(kDpiStages);
  REQUIRE(pl.settings.set("pipeline.pupil.centroid.approx.step", 8).empty());
  Rendered r;
  Scene s;
  s.noise = 2;
  r.render(s);
  const EyeImage image = r.image();
  for (int i = 0; i < 20; ++i) (void)pl.run(image);  // warm up
  constexpr int kN = 500;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) (void)pl.run(image);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kN;
  WARN("DPI pipeline on " << s.width << " x " << s.height << ", search radius 200 px: " << ms << " ms per eye image");
}
