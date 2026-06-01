/// Out-of-tree graphviz layout engine `cfg`: a grid-based, layered
/// control-flow-graph layout with orthogonal edge routing.
///
/// This is the SAME engine as the in-tree lib/cfggen version, but built against
/// a STOCK graphviz install (no fork). The difference is purely in headers:
/// graphviz installs its plugin API (gvplugin_layout.h, types.h, geom.h,
/// gvcjob.h) but NOT its internal common/render.h and common/utils.h. The
/// helper functions we need are nonetheless *exported* from libgvc, so we
/// declare their prototypes ourselves below and link against -lgvc.
///
/// Selected with `-Kcfg` / `layout=cfg`. Edge routing bias is read from the
/// `cfgbias=left|right` attribute (default right; left = classic "true branch").

#include <graphviz/types.h>            // ND_*/GD_* macros, splineInfo, pointf
#include <graphviz/geom.h>             // PS2INCH, POINTS_PER_INCH
#include <graphviz/gvcjob.h>           // LAYOUT_USES_RANKDIR
#include <graphviz/gvplugin_layout.h>  // gvlayout_engine_t, graph_t, cgraph

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "cfg_core.h"

/* --- graphviz internals: exported from libgvc but not in installed headers ---
 * These declarations mirror lib/common/{render,utils}.h and lib/common/const.h.
 * They are stable enough in practice but are NOT part of graphviz's documented
 * public API, so a plugin built this way is coupled to the graphviz version. */
extern void common_init_node(node_t *n);
extern void common_init_edge(edge_t *e);
extern void gv_nodesize(node_t *n, bool flip);
extern void setEdgeType(graph_t *g, int defaultValue);
extern void compute_bb(graph_t *g);
extern void clip_and_install(edge_t *fe, node_t *hn, pointf *ps, size_t pn,
                             splineInfo *info);
extern void dotneato_postprocess(graph_t *g);
extern void gv_cleanup_node(node_t *n);
extern void gv_cleanup_edge(edge_t *e);
extern int State;            // last finished phase (lib/common/globals.h)
extern unsigned short Ndim;  // number of layout dimensions

#define GVSPLINES 1                            // const.h: phase after routing
#define EDGETYPE_NONE (0 << 1)                 // const.h
#define EDGETYPE_LINE (1 << 1)                 // const.h
#define EDGE_TYPE(g) (GD_flags(g) & (7 << 1))  // macros.h

static bool spline_merge(node_t *n) {
  (void)n;
  return false;
}
static bool swap_ends_p(edge_t *e) {
  (void)e;
  return false;
}
static splineInfo cfg_sinfo = {.swapEnds = swap_ends_p,
                               .splineMerge = spline_merge,
                               .ignoreSwap = false,
                               .isOrtho = false};

static void cfg_init_node(node_t *n) {
  agbindrec(n, "Agnodeinfo_t", sizeof(Agnodeinfo_t), true);
  common_init_node(n);
  ND_pos(n) = calloc(GD_ndim(agraphof(n)), sizeof(double));
  gv_nodesize(n, GD_flip(agraphof(n)));
}

static void cfg_init_edge(edge_t *e) {
  agbindrec(e, "Agedgeinfo_t", sizeof(Agedgeinfo_t), true);
  common_init_edge(e);
}

static void cfg_init_graph(graph_t *g) {
  setEdgeType(g, EDGETYPE_LINE);
  Ndim = GD_ndim(agroot(g)) = 2;
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n)) cfg_init_node(n);
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n))
    for (edge_t *e = agfstout(g, n); e; e = agnxtout(g, e)) cfg_init_edge(e);
}

/* Read an edge's routing bias. Default RIGHT; `cfgbias="left"` forces a left
 * lane (the classic "true branch goes left"). */
static cfg_bias_t edge_bias(edge_t *e) {
  char *s = agget(e, "cfgbias");
  if (s && (s[0] == 'l' || s[0] == 'L')) return CFG_BIAS_LEFT;
  return CFG_BIAS_RIGHT;
}

/* Install an orthogonal polyline (nv vertices, first at the source-box
 * boundary, last at the target-box boundary) as edge e's spline. Endpoints are
 * doubled and corners tripled so each cubic is a straight line meeting at a
 * sharp right angle (mirrors graphviz's own orthogonal router). */
static void install_polyline(edge_t *e, const pointf *verts, size_t nv) {
  if (nv < 2) return;
  size_t segs = nv - 1;
  size_t pn = 1 + 3 * segs;
  pointf *ps = calloc(pn, sizeof(pointf));
  ps[0] = ps[1] = verts[0];
  size_t ipt = 2;
  for (size_t i = 1; i < nv - 1; i++) {
    ps[ipt] = ps[ipt + 1] = ps[ipt + 2] = verts[i];
    ipt += 3;
  }
  ps[ipt] = ps[ipt + 1] = verts[nv - 1];
  clip_and_install(e, aghead(e), ps, pn, &cfg_sinfo);
  free(ps);
}

void cfg_layout(graph_t *g) {
  if (agnnodes(g) == 0) {
    cfg_init_graph(g);
    compute_bb(g);
    dotneato_postprocess(g);
    return;
  }
  cfg_init_graph(g);

  int nnodes = agnnodes(g);
  int nedges = agnedges(g);

  cfg_node_t *cnodes = calloc((size_t)nnodes, sizeof(cfg_node_t));
  cfg_edge_in_t *cedges = calloc((size_t)(nedges > 0 ? nedges : 1), sizeof(cfg_edge_in_t));

  /* Map nodes to dense indices (node 0 = first node = entry block). */
  node_t **index2node = calloc((size_t)nnodes, sizeof(node_t *));
  int ni = 0;
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n)) {
    cnodes[ni].width = ND_lw(n) + ND_rw(n);
    cnodes[ni].height = ND_ht(n);
    index2node[ni] = n;
    ND_alg(n) = (void *)(ptrdiff_t)ni;
    ni++;
  }

  /* Edges, preserving graphviz iteration order so output maps back 1:1. */
  edge_t **index2edge = calloc((size_t)(nedges > 0 ? nedges : 1), sizeof(edge_t *));
  int ei = 0;
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n)) {
    for (edge_t *e = agfstout(g, n); e; e = agnxtout(g, e)) {
      cedges[ei].from = (int)(ptrdiff_t)ND_alg(agtail(e));
      cedges[ei].to = (int)(ptrdiff_t)ND_alg(aghead(e));
      cedges[ei].bias = edge_bias(e);
      index2edge[ei] = e;
      ei++;
    }
  }

  /* 18pt routing lanes: longer than graphviz's ~10pt arrowheads, so the final
   * vertical approach into each node stays straight. */
  cfg_result_t *r = cfg_run(cnodes, nnodes, cedges, ei, 0, 0, 18);

  double H = r->height;
  for (int i = 0; i < nnodes; i++) {
    node_t *n = index2node[i];
    ND_coord(n).x = cnodes[i].x + cnodes[i].width / 2.0;
    ND_coord(n).y = H - (cnodes[i].y + cnodes[i].height / 2.0);
    ND_pos(n)[0] = PS2INCH(ND_coord(n).x);
    ND_pos(n)[1] = PS2INCH(ND_coord(n).y);
  }

  if (EDGE_TYPE(g) != EDGETYPE_NONE) {
    for (int i = 0; i < ei; i++) {
      cfg_edge_out_t *oe = &r->edges[i];
      edge_t *e = index2edge[i];
      if (oe->npoints < 2) continue;
      size_t nv = oe->npoints;
      pointf *verts = calloc(nv, sizeof(pointf));
      for (size_t k = 0; k < nv; k++) {
        verts[k].x = oe->xs[k];
        verts[k].y = H - oe->ys[k];
      }
      install_polyline(e, verts, nv);
      free(verts);
    }
  }
  State = GVSPLINES;  // so translate_drawing shifts edges and the renderer emits them

  cfg_free(r);
  free(cnodes);
  free(cedges);
  free(index2node);
  free(index2edge);

  compute_bb(g);  // gv_postprocess consumes GD_bb but does not compute it
  dotneato_postprocess(g);
}

void cfg_cleanup(graph_t *g) {
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n)) {
    for (edge_t *e = agfstout(g, n); e; e = agnxtout(g, e)) gv_cleanup_edge(e);
    free(ND_pos(n));
    gv_cleanup_node(n);
  }
}
