// The dpi plugin on the rendered eyes' DPI setup (OpenIris 2's rendered plugin): ray-traced eyes seen
// by a DPI camera, with P1 to P4 from the cornea and the lens, and their exact positions. Built when
// OpenIris 2's build has the rendered plugin (its kit's OpenIris2::rendered).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "dpi.hpp"

#include "openiris/calibration.hpp"
#include "openiris/clock.hpp"
#include "openiris/outputs.hpp"
#include "dpi/outputs.hpp"  // the plugin's own outputs (PLUGINS-SPEC 18.10)
#include "openiris/source.hpp"
#include "openiris/tracker.hpp"
#include "rendered_eye.hpp"

using namespace oi;

namespace {

/// The rendered DPI setup, as the profile "Rendered eyes, DPI" has it (the region and the exposure
/// don't change the truth).
RenderedEyeParams dpi_setup() {
  RenderedEyeParams p;
  p.setup = RenderedSetup::Dpi;
  p.lights = RenderedLights::One;
  p.frame_rate = 100;
  return p;
}

/// The truth of a frame the tracker's rendered source made, from its view: the eye at the time it
/// was rendered (as the rendered plugin's own tests do it).
RenderedEyeTruth truth_of(const ViewInfo& vi, const RenderedEyeParams& p) {
  const Eye eye = vi.camera == 0 ? Eye::Left : Eye::Right;
  return rendered_eye_truth(p, vi.camera, rendered_eye_state(eye, vi.hw_timestamp, p));
}

bool wait_for(const std::function<bool()>& done, double seconds) {
  const double end = now() + seconds;
  while (now() < end) {
    if (done()) return true;
    sleep_until(now() + 0.01);
  }
  return done();
}

void set(Tracker& t, const std::string& path, const Json& value) {
  t.commands().invoke("settings.set", {{"path", path}, {"value", value}});
}

}  // namespace

TEST_CASE("dpi on rendered eyes: the tracker with its profile follows P1 and P4", "[rendered]") {
  Tracker t;
  REQUIRE(t.load_profile("Rendered eyes, DPI").empty());
  REQUIRE(t.plugin_problems().empty());
  t.commands().invoke("tracking.start");
  const double start = now();
  while (t.stats().live_processed < 20 && now() - start < 60) sleep_until(now() + 0.05);
  REQUIRE(t.stats().live_processed >= 20);
  REQUIRE(SourceRegistry::instance().plugin_of("rendered") == "rendered");
  const RenderedEyeParams p = dpi_setup();
  int compared = 0;
  double worst_pupil = 0, worst1 = 0, worst4 = 0;
  for (int k = 0; k < 12; ++k) {
    const auto s = t.latest_sample();
    REQUIRE(s);
    for (const Eye eye : kEyes) {
      const int view = s->layout().view_of(eye);
      REQUIRE(view >= 0);
      const ViewInfo& vi = s->info.views[static_cast<std::size_t>(view)];
      const RenderedEyeTruth truth = truth_of(vi, p);
      const PurkinjeImage& t1 = truth.purkinje[0][0];
      const PurkinjeImage& t4 = truth.purkinje[0][3];
      if (truth.pupil_edge_visible < 1.0 || !t1.visible || !t4.visible) continue;  // a blink, a lash
      INFO(to_string(eye) << " eye, frame " << vi.frame_number);
      const auto pupil = s->read<out::PupilCenter>(eye);
      const auto p1 = s->read<out::dpi::P1>(eye);
      const auto p4 = s->read<out::dpi::P4>(eye);
      const auto signal = s->read<out::dpi::P4P1>(eye);
      REQUIRE(pupil.good());
      REQUIRE(p1.good());
      REQUIRE(p4.good());
      REQUIRE(signal.good());
      const double ep = std::hypot(pupil->center.x - truth.pupil_x, pupil->center.y - truth.pupil_y);
      const double e1 = std::hypot(p1->p1.x - t1.x, p1->p1.y - t1.y);
      const double e4 = std::hypot(p4->p4.x - t4.x, p4->p4.y - t4.y);
      INFO("pupil " << ep << " px, P1 " << e1 << " px, P4 " << e4 << " px from the truth");
      CHECK(ep < 1.0);
      CHECK(e1 < 0.5);
      CHECK(e4 < 0.5);
      worst_pupil = std::max(worst_pupil, ep);
      worst1 = std::max(worst1, e1);
      worst4 = std::max(worst4, e4);
      ++compared;
    }
    sleep_until(now() + 0.07);
  }
  t.commands().invoke("tracking.stop");
  REQUIRE(compared >= 12);
  WARN("rendered DPI eyes, tracked with their profile: pupil within " << worst_pupil << " px, P1 within " << worst1
                                                                       << " px, P4 within " << worst4 << " px of the truth");
}

TEST_CASE("dpi on rendered eyes: gaze.dpi calibrates from P1 and P4 with the profile", "[rendered]") {
  // The rendered eyes look at each target (0.15 s after it appears) and hold still on it, so the
  // fit's error on left-out targets and the gaze on a new target say how well P1 and P4 follow the
  // eye's rotation through the profile's stages.
  Tracker t;
  REQUIRE(t.load_profile("Rendered eyes, DPI").empty());  // calibrated by gaze.dpi
  REQUIRE(t.plugin_problems().empty());
  set(t, "calibration.gaze.dpi.settle", 0.3);
  set(t, "calibration.gaze.dpi.collect", 0.3);
  t.commands().invoke("tracking.start");
  REQUIRE(wait_for([&] { return t.stats().live_processed > 20; }, 60.0));
  t.commands().invoke("calibration.start", {{"plugin", "gaze.dpi"}, {"procedure", "grid"}});
  REQUIRE(wait_for([&] { return t.calibration_status()["active"] == false; }, 120.0));
  const Json result = t.calibration_status()["result"];
  INFO(result.dump());
  REQUIRE(result["ok"] == true);
  WARN("gaze.dpi on rendered eyes: " << result["message"].get<std::string>());
  const auto model = t.models().get("dpi/model/gaze");
  REQUIRE(model);
  for (const char* eye : {"left", "right"}) {
    INFO(eye << " eye: " << model->quality.dump());
    CHECK(model->quality[std::string(eye) + "_left_out_deg"].get<double>() < 0.3);
  }
  t.source()->show_target(true, 4.0, -3.0);
  sleep_until(now() + 0.5);
  const auto s = t.latest_sample();
  REQUIRE(s);
  for (const Eye eye : kEyes) {
    INFO(to_string(eye) << " eye");
    const auto gaze = s->read<out::GazeAngles>(eye);
    REQUIRE(gaze.good());
    INFO("gaze " << gaze->h << ", " << gaze->v << " deg");
    CHECK(std::abs(gaze->h - 4.0) < 0.5);
    CHECK(std::abs(gaze->v + 3.0) < 0.5);
  }
  t.commands().invoke("tracking.stop");
}

TEST_CASE("dpi: an average eye's P4 - P1 is Le Grand's, as the rendered eyes trace it (PLUGINS-SPEC 8.8)", "[rendered]") {
  // gaze.dpi's map before a grid turns P4 - P1 on a sphere of kP4P1AverageRadiusMm: the slope of
  // P4 - P1 per degree in the rendered eye, at its pixel size, over ±6°.
  const RenderedEyeParams p = dpi_setup();
  constexpr double kRad = 3.14159265358979323846 / 180.0;
  for (int cam = 0; cam < 2; ++cam) {
    const Eye eye = cam == 0 ? Eye::Left : Eye::Right;
    for (int axis = 0; axis < 2; ++axis) {
      auto p4_p1 = [&](double a) {
        const auto t = rendered_eye_truth(p, cam, rendered_eye_looking(eye, 0.0, axis == 0 ? a : 0.0, axis == 1 ? a : 0.0, p));
        const auto& p1 = t.purkinje[0][0];
        const auto& p4 = t.purkinje[0][3];
        REQUIRE(p1.found);
        REQUIRE(p4.found);
        return std::pair{axis == 0 ? p4.x - p1.x : p4.y - p1.y, t.mm_per_px};
      };
      const auto [lo, mm_lo] = p4_p1(-6.0);
      const auto [hi, mm_hi] = p4_p1(6.0);
      const double mm_per_deg = std::abs(hi - lo) / 12.0 * 0.5 * (mm_lo + mm_hi);
      const double radius = mm_per_deg / kRad;
      INFO("camera " << cam << (axis == 0 ? " horizontal" : " vertical") << ": " << radius << " mm");
      CHECK(std::abs(radius - dpi::kP4P1AverageRadiusMm) < 0.03 * dpi::kP4P1AverageRadiusMm);
    }
  }
}

TEST_CASE("dpi on rendered eyes: before a grid, one target and an average eye (PLUGINS-SPEC 8.8)", "[rendered]") {
  // The profile has the camera's place and pixel size: one target, no grid, gives gaze from Le
  // Grand's eye's P4 - P1, checked against where the rendered eyes look.
  Tracker t;
  REQUIRE(t.load_profile("Rendered eyes, DPI").empty());
  set(t, "calibration.gaze.dpi.settle", 0.3);
  set(t, "calibration.gaze.dpi.collect", 0.3);
  t.commands().invoke("tracking.start");
  REQUIRE(wait_for([&] { return t.stats().live_processed > 20; }, 60.0));
  t.commands().invoke("calibration.start", {{"plugin", "gaze.dpi"}, {"procedure", "one_point"}});
  REQUIRE(wait_for([&] { return t.calibration_status()["active"] == false; }, 60.0));
  const Json result = t.calibration_status()["result"];
  INFO(result.dump());
  REQUIRE(result["ok"] == true);
  const auto model = t.models().get("dpi/model/gaze");
  REQUIRE(model);
  CHECK(model->values["default"] == true);
  for (const auto& [h, v] : std::vector<std::pair<double, double>>{{4.0, -3.0}, {-5.0, 4.0}}) {
    t.source()->show_target(true, h, v);
    sleep_until(now() + 0.5);
    const auto s = t.latest_sample();
    REQUIRE(s);
    for (const Eye eye : kEyes) {
      INFO(to_string(eye) << " eye looking at " << h << ", " << v);
      const auto gaze = s->read<out::GazeAngles>(eye);
      REQUIRE(gaze.good());
      INFO("gaze " << gaze->h << ", " << gaze->v << " deg");
      CHECK(std::abs(gaze->h - h) < 0.5);
      CHECK(std::abs(gaze->v - v) < 0.5);
    }
  }
  t.commands().invoke("tracking.stop");
}
