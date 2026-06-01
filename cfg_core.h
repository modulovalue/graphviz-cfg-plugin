/// @file
/// @ingroup engines
/*************************************************************************
 * Pure, domain-agnostic grid-based control-flow-graph layout.
 *
 * This is a C port of Compiler Explorer's `static/graph-layout-core.ts`
 * (itself adapted from Cutter's `GraphGridLayout`), by way of the Dart port in
 * dart-il-explorer (`packages/cfg_layout/lib/src/graph_layout_core.dart`).
 *
 * It has NO graphviz dependency: feed it sized nodes and directed edges and it
 * returns absolute top-left node positions plus an orthogonal polyline for
 * every edge. Coordinates use a y-down, top-left origin (like the original);
 * the graphviz glue (cfg_layout.c) flips y and centers nodes.
 *************************************************************************/

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Tie-break preference for an edge's vertical routing lane when the left and
/// right candidate columns are equidistant. Matches Dart's EdgeBias.
typedef enum { CFG_BIAS_LEFT = 0, CFG_BIAS_RIGHT = 1 } cfg_bias_t;

/// An input node. width/height are the measured box size (caller's units, e.g.
/// points). x/y are filled in on output (top-left corner, y-down).
typedef struct {
  double width;
  double height;
  double x;
  double y;
} cfg_node_t;

/// A directed input edge by node index.
typedef struct {
  int from;
  int to;
  cfg_bias_t bias;
} cfg_edge_in_t;

/// A routed output edge: a polyline of `npoints` points (y-down). The points
/// alternate orthogonal moves. `from`/`to` echo the input for caller mapping.
typedef struct {
  int from;
  int to;
  size_t npoints;
  double *xs;
  double *ys;
} cfg_edge_out_t;

/// Layout result. `nodes` is the same array the caller passed in (positions
/// filled). `edges` are routed, in the same order as the input edges.
typedef struct {
  cfg_node_t *nodes; /* not owned: points at caller's array */
  int nnodes;
  cfg_edge_out_t *edges; /* owned */
  int nedges;
  double width;
  double height;
} cfg_result_t;

/// Lay out `nodes` (sizes filled in) connected by `edges`. Node 0 is treated as
/// the entry block. `center_parents` and `narrow_layout` mirror the upstream
/// toggles (both default off). `edge_spacing` sets the routing-lane spacing in
/// the caller's units (<= 0 uses the upstream default of 10); larger values
/// keep every edge's final approach longer than a rendered arrowhead. Returns a
/// heap result; free with cfg_free().
cfg_result_t *cfg_run(cfg_node_t *nodes, int nnodes, const cfg_edge_in_t *edges,
                      int nedges, int center_parents, int narrow_layout,
                      int edge_spacing);

void cfg_free(cfg_result_t *r);

#ifdef __cplusplus
}
#endif
