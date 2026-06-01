/// Plugin registration for the out-of-tree `cfg` layout engine.
///
/// graphviz discovers a plugin by dlopen'ing the shared library and looking up
/// the symbol `gvplugin_<name>_LTX_library`, where <name> is derived from the
/// file name (libgvplugin_cfg.{so,dylib} -> "cfg"). The library advertises one
/// or more APIs; we provide a single API_layout engine named "cfg".

#include <graphviz/gvplugin.h>
#include <graphviz/gvplugin_layout.h>
#include <graphviz/gvcjob.h>  // LAYOUT_USES_RANKDIR

extern void cfg_layout(graph_t *g);
extern void cfg_cleanup(graph_t *g);

static gvlayout_engine_t cfg_engine = {
    cfg_layout,
    cfg_cleanup,
};

static gvlayout_features_t cfg_features = {
    LAYOUT_USES_RANKDIR,
};

static gvplugin_installed_t cfg_layouts[] = {
    {0, "cfg", 0, &cfg_engine, &cfg_features},
    {0, NULL, 0, NULL, NULL},
};

static gvplugin_api_t apis[] = {
    {API_layout, cfg_layouts},
    {(api_t)0, 0},
};

gvplugin_library_t gvplugin_cfg_LTX_library = {"cfg", apis};
