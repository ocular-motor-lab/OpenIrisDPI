// gaze.dpi (PLUGINS-SPEC 8.6, 8.10, DPI-SPEC 8): gaze from P1 and P4, by regression on targets. A
// thin plugin over the core's GazeRegression (gaze_regression.hpp), which does the fit, λ by
// left-out targets, one target and apply(). P1 and P4 are two points (decision 13), not P4 − P1:
// the fit finds their weights, near +1 and −1, and can make them a little different where the
// system is slightly nonlinear; the shift rule (decision 12) keeps both moving together from
// being gaze.
#include "dpi.hpp"
#include "openiris/gaze_regression.hpp"
#include "openiris/outputs.hpp"
#include "dpi/outputs.hpp"  // the plugin's own outputs (PLUGINS-SPEC 18.10)

namespace oi {

namespace {

GazeRegressionSetup dpi_setup() {
  GazeRegressionSetup s;
  s.model_id = "dpi/model/gaze";
  s.inputs = {{"p1", &out::dpi::P1::def(), false, true}, {"p4", &out::dpi::P4::def(), false, true}};
  s.amplitude = 8;  // P4 hides behind the iris beyond about ±10°
  s.invariance_mm = 3;  // P1 and P4 moving together (a slip, a head drifting) isn't gaze
  // Before a grid, an average eye (PLUGINS-SPEC 8.8): in Le Grand's eye P4 − P1 moves 0.114 mm per
  // degree, the same way as the pupil (0.182 mm per degree, 10.4 mm): a sphere of 6.5 mm. Measured
  // on the rendered eyes, which trace that eye; their test checks it.
  s.average_radius_mm = dpi::kP4P1AverageRadiusMm;
  s.average_weights = {-1.0, 1.0};  // the average eye's point is P4 − P1
  return s;
}

class GazeDpi : public GazeRegression {
 public:
  GazeDpi() : GazeRegression(dpi_setup()) {}
  [[nodiscard]] std::string name() const override { return "gaze.dpi"; }

 protected:
  void declare_inputs(Declarations& d) override {
    d.describe("Regression (DPI)",
               "Where the subject looks, from P1 and P4 (dual Purkinje imaging), after they look at targets. Linear "
               "unless you ask for more terms; it reports its error on targets left out of the fit. Before a grid, one "
               "target gives an average eye's map (Le Grand's eye); after one, it re-zeroes the map and says how far "
               "off it was.",
               "PLUGINS-SPEC 8.6, 8.10, DPI-SPEC 8. Ridge regression per eye from dpi/p1 and dpi/p4 as two points, "
               "unpenalized intercept, standardized; linear, with quadratic, cubic and interaction terms as options; "
               "lambda chosen by left-out targets. A rule in the fit (invariance mm): P1 and P4 moving together isn't "
               "gaze, a penalty on the fitted gaze changing under such shifts. 9 targets at ±8°.");
    p1_ = d.require<out::dpi::P1>("Per eye.");
    p4_ = d.require<out::dpi::P4>("Per eye.");
  }

  bool read(const SampleBlock& s, int group, int k, std::vector<double>& x, double& quality) const override {
    const int view = tracked_view(s, group);
    if (view < 0) return false;
    if (k == 0) {
      const auto p = s.get(p1_, view);
      if (!good(p, quality)) return false;
      x.push_back(p->p1.x);
      x.push_back(p->p1.y);
      return true;
    }
    const auto p = s.get(p4_, view);
    if (!good(p, quality)) return false;
    x.push_back(p->p4.x);
    x.push_back(p->p4.y);
    return true;
  }

 private:
  In<out::dpi::P1> p1_;
  In<out::dpi::P4> p4_;
};

}  // namespace

void register_dpi_calibration() {
  CalibrationRegistry::instance().add(
      {"gaze.dpi", "Gaze from P4 − P1 (DPI), by regression on targets.", [] { return std::make_unique<GazeDpi>(); }});
}

}  // namespace oi
