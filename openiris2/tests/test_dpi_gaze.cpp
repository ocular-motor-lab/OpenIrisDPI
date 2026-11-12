// gaze.dpi on the synthetic eyes that show P4 (DPI-SPEC 8, OpenIris 2's PLUGINS-SPEC 8.6): calibrated in
// the tracker with the plugin's profile "Synthetic eyes, DPI". From OpenIris 2's tests of the
// regression calibrations (tests/test_gaze_regression.cpp), with the plugin.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

#include "openiris/calibration.hpp"
#include "openiris/clock.hpp"
#include "openiris/outputs.hpp"
#include "openiris/tracker.hpp"

using namespace oi;
using Catch::Matchers::WithinAbs;

namespace {

bool wait_for(const std::function<bool()>& done, double seconds) {
  const double end = now() + seconds;
  while (now() < end) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return done();
}

void set(Tracker& t, const std::string& path, const Json& value) {
  t.commands().invoke("settings.set", {{"path", path}, {"value", value}});
}

// Runs a procedure to the end and returns its result.
Json run_procedure(Tracker& t, const std::string& plugin, const std::string& procedure) {
  t.commands().invoke("calibration.start", {{"plugin", plugin}, {"procedure", procedure}});
  REQUIRE(wait_for([&] { return t.calibration_status()["active"] == false; }, 40.0));
  return t.calibration_status()["result"];
}

}  // namespace

TEST_CASE("gaze.dpi: P1 and P4 on the DPI synthetic eyes") {
  Tracker t;
  REQUIRE(t.load_profile("Synthetic eyes, DPI").empty());  // calibrated by gaze.dpi
  REQUIRE(t.plugin_problems().empty());
  set(t, "calibration.gaze.dpi.settle", 0.3);
  set(t, "calibration.gaze.dpi.collect", 0.3);
  t.commands().invoke("tracking.start");
  REQUIRE(wait_for([&] { return t.stats().live_processed > 100; }, 5.0));
  const Json result = run_procedure(t, "gaze.dpi", "grid");
  INFO(result.dump());
  REQUIRE(result["ok"] == true);
  WARN("gaze.dpi: " << result["message"].get<std::string>());
  const auto model = t.models().get("dpi/model/gaze");
  REQUIRE(model);
  // P1 and P4 as two points (PLUGINS-SPEC 8.10, decision 13), with the shift rule.
  CHECK(model->values["inputs"][0]["output"] == "dpi/p1");
  CHECK(model->values["inputs"][1]["output"] == "dpi/p4");
  CHECK(model->values["invariance"]["applied"] == true);
  t.source()->show_target(true, 4.0, -3.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const auto s = t.latest_sample();
  REQUIRE(s);
  const auto gaze = s->read<out::GazeAngles>(Eye::Left);
  REQUIRE(gaze.good());
  CHECK_THAT(gaze->h, WithinAbs(4.0, 0.5));
  CHECK_THAT(gaze->v, WithinAbs(-3.0, 0.5));
  t.commands().invoke("tracking.stop");
}
