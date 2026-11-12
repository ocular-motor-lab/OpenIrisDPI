// The dpi plugin for OpenIris 2 (DPI-SPEC.md): dual Purkinje imaging. glints.p1 finds P1, pupil.hull
// the pupil, dpi.p4 finds P4 and the DPI signal P4 − P1, and gaze.dpi turns it into gaze. The method
// is Ryan Ressmeyer's, from his OpenIrisDPI plugin for OpenIris (../OpenIrisDPI), re-implemented.
// Its outputs (outputs/) and profiles (profiles/) are registered by OI_PLUGIN, from what OpenIris 2's
// plugin kit made of them (OpenIris 2's PLUGINS-SPEC 18.10, 22).
#include "dpi.hpp"
#include "openiris/plugin.hpp"

OI_PLUGIN(dpi, OI_PLUGIN_VERSION, OI_PLUGIN_DESCRIPTION) {
  oi::register_dpi_stages();
  oi::register_dpi_calibration();
}
