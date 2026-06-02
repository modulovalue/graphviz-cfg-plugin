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

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg_core.h"

/* --- graphviz internals: exported from libgvc but not in installed headers ---
 * These declarations mirror lib/common/{render,utils}.h and lib/common/const.h.
 * They are stable enough in practice but are NOT part of graphviz's documented
 * public API, so a plugin built this way is coupled to the graphviz version. */
extern Agsym_t *N_width;  // node "width" attr symbol gv_nodesize reads
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
/* Cluster (subgraph) box support: more libgvc internals (see header note). */
extern bool is_a_cluster(graph_t *g);     // name starts "cluster" or cluster=true
extern void do_graph_label(graph_t *sg);  // makes GD_label + GD_border for sg
extern void free_label(textlabel_t *p);

#define GVSPLINES 1                            // const.h: phase after routing
#define EDGETYPE_NONE (0 << 1)                 // const.h
#define EDGETYPE_LINE (1 << 1)                 // const.h
#define EDGE_TYPE(g) (GD_flags(g) & (7 << 1))  // macros.h

#define CL_OFFSET 8  // const.h: cluster box margin in points
/* GD_border[] side indices (const.h). */
#define BOTTOM_IX 0
#define RIGHT_IX 1
#define TOP_IX 2
#define LEFT_IX 3

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

/* Routing-lane spacing fed to cfg_core (see cfg_run calls below). */
#define CFG_SPACING 18.0

static void cfg_init_node(node_t *n) {
  agbindrec(n, "Agnodeinfo_t", sizeof(Agnodeinfo_t), true);
  /* Widen a high-degree node so its edges attach INSIDE the box rather than on
   * (or past) the corners: edges fan across ~(degree-1)*spacing, so the box
   * needs to be at least that plus a one-spacing margin each side. We raise
   * graphviz's minimum `width` BEFORE common_init_node so the box outline is
   * built wide (it can't be resized afterward); the attr is a minimum, so a
   * wider label still wins. Works because cfg_init_graph calls graph_init,
   * which wires up the N_width attr symbol gv_nodesize reads. */
  graph_t *g = agraphof(n);
  int od = agdegree(g, n, false, true), id = agdegree(g, n, true, false);
  int deg = od > id ? od : id;
  if (deg > 2) {
    double want = (double)(deg + 1) * CFG_SPACING;  /* span + margin, points */
    char buf[32];
    snprintf(buf, sizeof buf, "%.4f", want / (double)POINTS_PER_INCH);
    char *cur = agget(n, (char *)"width");
    if (!cur || !*cur || atof(cur) * POINTS_PER_INCH < want)
      agsafeset(n, (char *)"width", buf, (char *)"");
  }
  common_init_node(n);
  ND_pos(n) = calloc(GD_ndim(g), sizeof(double));
  gv_nodesize(n, GD_flip(g));
}

static void cfg_init_edge(edge_t *e) {
  agbindrec(e, "Agedgeinfo_t", sizeof(Agedgeinfo_t), true);
  common_init_edge(e);
}

static void cfg_init_graph(graph_t *g) {
  /* gvLayoutJobs already ran graph_init (GD_drawing + attr globals), but the
   * N_width symbol gv_nodesize reads is NULL unless the source declared a
   * `width` node attr. Point it at the attr (declaring it default "" if
   * missing, which leaves label-sized nodes untouched) so the per-node width we
   * set on high-degree nodes in cfg_init_node actually takes effect. */
  N_width = agattr(agroot(g), AGNODE, (char *)"width", NULL);
  if (!N_width) N_width = agattr(agroot(g), AGNODE, (char *)"width", (char *)"");
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

static bool cfg_is_compass(const char *s) {
  static const char *k[] = {"n", "ne", "e",  "se", "s",
                            "sw", "w", "nw", "c",  "_", NULL};
  for (int i = 0; k[i]; i++)
    if (strcmp(s, k[i]) == 0) return true;
  return false;
}

/* Resolve a node:port spec ("field", "field:compass", or a bare compass) to a
 * horizontal offset from the node's center, in points. Honors only the x
 * position — the CFG convention keeps edges leaving the bottom / entering the
 * top, so compass n/s and record cell heights don't move the attach side.
 * Returns false when there is no usable port. */
static bool cfg_port_dx(node_t *n, const char *spec, double *dx) {
  if (!spec || !*spec) return false;
  if (!ND_shape(n) || !ND_shape(n)->fns || !ND_shape(n)->fns->portfn) return false;
  char buf[256];
  strncpy(buf, spec, sizeof buf - 1);
  buf[sizeof buf - 1] = '\0';
  char *name = buf, *compass = NULL;
  char *colon = strchr(buf, ':');
  if (colon) {
    *colon = '\0';
    compass = colon + 1;
  } else if (cfg_is_compass(buf)) {
    name = (char *)"";  /* a bare compass keyword has no field name */
    compass = buf;
  }
  port p = ND_shape(n)->fns->portfn(n, name, compass);
  if (!p.defined) return false;
  *dx = p.p.x;
  return true;
}

/* Install an orthogonal polyline (nv vertices, first at the source-box
 * boundary, last at the target-box boundary) as edge e's spline. Endpoints are
 * doubled and corners tripled so each cubic is a straight line meeting at a
 * sharp right angle (mirrors graphviz's own orthogonal router). If the edge
 * carries tail/head ports, the endpoints are jogged along the node edge to the
 * port's x before installation. */
static void install_polyline(edge_t *e, const pointf *verts0, size_t nv0) {
  if (nv0 < 2) return;
  pointf *verts = calloc(nv0 + 2, sizeof(pointf));
  size_t nv = 0;
  double tdx, hdx;
  bool tport = cfg_port_dx(agtail(e), agget(e, "tailport"), &tdx);
  bool hport = cfg_port_dx(aghead(e), agget(e, "headport"), &hdx);
  if (tport) {
    pointf p = {ND_coord(agtail(e)).x + tdx, verts0[0].y};
    if (p.x != verts0[0].x) verts[nv++] = p;
  }
  for (size_t i = 0; i < nv0; i++)
    if (nv == 0 || verts[nv - 1].x != verts0[i].x || verts[nv - 1].y != verts0[i].y)
      verts[nv++] = verts0[i];
  if (hport) {
    pointf p = {ND_coord(aghead(e)).x + hdx, verts0[nv0 - 1].y};
    if (p.x != verts[nv - 1].x) verts[nv++] = p;
  }
  if (nv < 2) {
    free(verts);
    return;
  }
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
  free(verts);
}

/* --- cluster (subgraph) boxes -------------------------------------------
 * The whole graph is laid out by ONE cfg_run (see cfg_layout below), so every
 * edge shares the same lane assignment and edges never overlap or overdraw.
 * Clusters are then drawn as boxes around their members: we build graphviz's
 * GD_clust tree (so its renderer emits the boxes + labels) and set each
 * cluster's GD_bb to the bounding box of its member nodes, plus a margin and
 * the reserved label band. Keeping a cluster's members spatially together is
 * cfg_core's job (cluster-aware ordering); here we just wrap the region they
 * occupy. */

static int cfg_count_clusters(graph_t *g) {
  int n = 0;
  for (graph_t *sg = agfstsubg(g); sg; sg = agnxtsubg(sg)) {
    if (is_a_cluster(sg) && agfstnode(sg)) n++;
    n += cfg_count_clusters(sg);
  }
  return n;
}

/* Bind graphinfo + label and register each cluster in its parent's GD_clust
 * array; recurse through non-cluster subgraphs. Empty clusters are skipped. */
static void cfg_register_clusters(graph_t *g, graph_t *parent) {
  for (graph_t *sg = agfstsubg(g); sg; sg = agnxtsubg(sg)) {
    if (is_a_cluster(sg) && agfstnode(sg)) {
      agbindrec(sg, "Agraphinfo_t", sizeof(Agraphinfo_t), true);
      do_graph_label(sg);  // makes GD_label + GD_border (no-op if unlabeled)
      int cno = ++GD_n_cluster(parent);
      GD_clust(parent) =
          realloc(GD_clust(parent), (size_t)(cno + 1) * sizeof(graph_t *));
      GD_clust(parent)[cno] = sg;
      cfg_register_clusters(sg, sg);
    } else {
      cfg_register_clusters(sg, parent);
    }
  }
}

/* Bottom-up AABB of a cluster: its member nodes' boxes, its child cluster
 * boxes, a CL_OFFSET margin, and the reserved label band. Final y-up coords. */
static boxf cfg_cluster_bb(graph_t *sg) {
  for (int c = 1; c <= GD_n_cluster(sg); c++) cfg_cluster_bb(GD_clust(sg)[c]);
  boxf bb = {.LL = {DBL_MAX, DBL_MAX}, .UR = {-DBL_MAX, -DBL_MAX}};
  for (node_t *n = agfstnode(sg); n; n = agnxtnode(sg, n)) {
    pointf c = ND_coord(n);
    double hw = (ND_lw(n) + ND_rw(n)) / 2.0, hh = ND_ht(n) / 2.0;
    if (c.x - hw < bb.LL.x) bb.LL.x = c.x - hw;
    if (c.y - hh < bb.LL.y) bb.LL.y = c.y - hh;
    if (c.x + hw > bb.UR.x) bb.UR.x = c.x + hw;
    if (c.y + hh > bb.UR.y) bb.UR.y = c.y + hh;
  }
  for (int c = 1; c <= GD_n_cluster(sg); c++) {
    boxf cb = GD_bb(GD_clust(sg)[c]);
    if (cb.LL.x < bb.LL.x) bb.LL.x = cb.LL.x;
    if (cb.LL.y < bb.LL.y) bb.LL.y = cb.LL.y;
    if (cb.UR.x > bb.UR.x) bb.UR.x = cb.UR.x;
    if (cb.UR.y > bb.UR.y) bb.UR.y = cb.UR.y;
  }
  bb.LL.x -= CL_OFFSET + GD_border(sg)[LEFT_IX].x;
  bb.UR.x += CL_OFFSET + GD_border(sg)[RIGHT_IX].x;
  bb.LL.y -= CL_OFFSET + GD_border(sg)[BOTTOM_IX].y;
  bb.UR.y += CL_OFFSET + GD_border(sg)[TOP_IX].y;
  GD_bb(sg) = bb;
  return bb;
}

static void cfg_free_clusters(graph_t *g) {
  for (int c = 1; c <= GD_n_cluster(g); c++) {
    graph_t *sg = GD_clust(g)[c];
    cfg_free_clusters(sg);
    if (GD_label(sg)) free_label(GD_label(sg));
  }
  free(GD_clust(g));
  GD_clust(g) = NULL;
  GD_n_cluster(g) = 0;
}

/* Nudge unjustified cluster labels to the top-left so a centered edge entering
 * the cluster doesn't run through the label. Runs after postprocess (bb final).
 * An explicit labeljust is respected. */
static void cfg_leftalign_labels(graph_t *g) {
  for (int c = 1; c <= GD_n_cluster(g); c++) {
    graph_t *sg = GD_clust(g)[c];
    cfg_leftalign_labels(sg);
    char *lj = agget(sg, "labeljust");
    if (GD_label(sg) && (!lj || !*lj))
      GD_label(sg)->pos.x = GD_bb(sg).LL.x + GD_label(sg)->dimen.x / 2.0 + CL_OFFSET;
  }
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

  /* Map nodes/edges to dense indices (node 0 = first node = entry block);
   * ND_alg(n) carries the index, edge order preserves graphviz iteration. */
  node_t **index2node = calloc((size_t)nnodes, sizeof(node_t *));
  edge_t **index2edge = calloc((size_t)(nedges > 0 ? nedges : 1), sizeof(edge_t *));
  int ni = 0;
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n)) {
    index2node[ni] = n;
    ND_alg(n) = (void *)(ptrdiff_t)ni;
    ni++;
  }
  int ei = 0;
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n))
    for (edge_t *e = agfstout(g, n); e; e = agnxtout(g, e)) index2edge[ei++] = e;

  /* ONE cfg_core pass over ALL nodes and edges, whether or not there are
   * clusters. Routing everything together means edges share lane assignment,
   * so they never overlap or overdraw -- the failure of the old recursive
   * (per-cluster) layout. Cluster boxes are added afterward (below). */
  {
    cfg_node_t *cnodes = calloc((size_t)nnodes, sizeof(cfg_node_t));
    cfg_edge_in_t *cedges = calloc((size_t)(ei > 0 ? ei : 1), sizeof(cfg_edge_in_t));
    for (int i = 0; i < nnodes; i++) {
      cnodes[i].width = ND_lw(index2node[i]) + ND_rw(index2node[i]);
      cnodes[i].height = ND_ht(index2node[i]);
    }
    for (int i = 0; i < ei; i++) {
      edge_t *e = index2edge[i];
      cedges[i].from = (int)(ptrdiff_t)ND_alg(agtail(e));
      cedges[i].to = (int)(ptrdiff_t)ND_alg(aghead(e));
      cedges[i].bias = edge_bias(e);
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
    State = GVSPLINES;  // so translate_drawing shifts edges and emits them
    cfg_free(r);
    free(cnodes);
    free(cedges);
  }

  free(index2node);
  free(index2edge);

  /* Draw subgraph boxes: build the GD_clust tree and wrap each cluster's
   * members in a box. Must run after ND_coord is final and before compute_bb
   * (which folds the cluster boxes into the root bounding box). */
  if (cfg_count_clusters(g) > 0) {
    cfg_register_clusters(g, g);
    for (int c = 1; c <= GD_n_cluster(g); c++) cfg_cluster_bb(GD_clust(g)[c]);
  }

  compute_bb(g);  // gv_postprocess consumes GD_bb but does not compute it
  dotneato_postprocess(g);
  cfg_leftalign_labels(g);  // keep cluster labels clear of entering edges
}

void cfg_cleanup(graph_t *g) {
  cfg_free_clusters(g);
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n)) {
    for (edge_t *e = agfstout(g, n); e; e = agnxtout(g, e)) gv_cleanup_edge(e);
    free(ND_pos(n));
    gv_cleanup_node(n);
  }
}
