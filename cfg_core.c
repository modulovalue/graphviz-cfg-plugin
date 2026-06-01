/// @file
/// @ingroup engines
/*************************************************************************
 * Pure grid-based CFG layout. See cfg_core.h.
 *
 * A faithful, function-by-function C port of the Dart
 * `graph_layout_core.dart` (which ports Compiler Explorer's
 * `graph-layout-core.ts`). Structure and naming are kept close to the original
 * so it can be audited against upstream. The pipeline:
 *
 *   countEdges -> computeDag -> assignRows -> computeTree ->
 *   assignBlockColumns -> setupRowsAndColumns -> computeEdgeMainColumns ->
 *   routeEdgePaths -> assignEdgeSegments -> computeCoordinates
 *************************************************************************/

#include "cfg_core.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Upstream uses a fixed edge spacing of 10. It is a parameter here (stored on
 * core_t, defaulted by cfg_run) so the graphviz engine can widen the routing
 * lanes: graphviz draws real ~10pt arrowheads, and a 10pt final approach lets
 * arrow clipping bend the last segment. A larger value keeps every approach
 * longer than the arrow. */
#define CFG_DEFAULT_EDGE_SPACING 10

/* Edge-segment classification (rotated 90 deg right for horizontal segments).
 * Ints because the sort compares & subtracts them. */
#define K_LEFT_U (-2)
#define K_LEFT_CORNER (-1)
#define K_VERTICAL (0)
#define K_RIGHT_CORNER (1)
#define K_RIGHT_U (2)
#define K_NULL (1 << 30)

typedef enum { SEG_HORIZONTAL, SEG_VERTICAL } seg_type_t;

/* LayoutEventType: numbering matters; edges sort before blocks on a tied row. */
typedef enum { EV_EDGE = 0, EV_BLOCK = 1 } event_type_t;

typedef enum { DFS_NOTVISITED, DFS_PENDING, DFS_VISITED } dfs_state_t;

/* ------------------------------------------------------------------ */
/* tiny growable vectors                                              */
/* ------------------------------------------------------------------ */

typedef struct {
  int *data;
  size_t len, cap;
} ivec_t;

static void ivec_push(ivec_t *v, int x) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 8;
    v->data = (int *)realloc(v->data, v->cap * sizeof(int));
  }
  v->data[v->len++] = x;
}
static void ivec_free(ivec_t *v) { free(v->data); v->data = NULL; v->len = v->cap = 0; }

typedef struct {
  void **data;
  size_t len, cap;
} pvec_t;

static void pvec_push(pvec_t *v, void *x) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 8;
    v->data = (void **)realloc(v->data, v->cap * sizeof(void *));
  }
  v->data[v->len++] = x;
}
static void pvec_removeAt(pvec_t *v, size_t i) {
  memmove(&v->data[i], &v->data[i + 1], (v->len - i - 1) * sizeof(void *));
  v->len--;
}
static void pvec_free(pvec_t *v) { free(v->data); v->data = NULL; v->len = v->cap = 0; }

/* ------------------------------------------------------------------ */
/* model                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
  int start, end;
} row_bound_t;

typedef struct {
  row_bound_t *data;
  size_t len, cap;
} rbvec_t;

static void rbvec_push(rbvec_t *v, row_bound_t b) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 8;
    v->data = (row_bound_t *)realloc(v->data, v->cap * sizeof(row_bound_t));
  }
  v->data[v->len++] = b;
}
static void rbvec_insert0(rbvec_t *v, row_bound_t b) {
  rbvec_push(v, b);
  memmove(&v->data[1], &v->data[0], (v->len - 1) * sizeof(row_bound_t));
  v->data[0] = b;
}
static void rbvec_free(rbvec_t *v) { free(v->data); v->data = NULL; v->len = v->cap = 0; }

typedef struct {
  int width, height;
  rbvec_t rows;
} bbox_t;

typedef struct {
  int row, col;
  double x, y;
} edge_coord_t;

typedef struct {
  edge_coord_t start, end;
  double horizontalOffset;
  double verticalOffset;
  seg_type_t type;
} edge_segment_t;

typedef struct {
  cfg_bias_t bias;
  int dest;
  int orig_index; /* index into the caller's input edge array */
  int mainColumn;
  pvec_t path; /* edge_segment_t* */
} edge_t;

typedef struct {
  cfg_node_t *data; /* points into caller array */
  pvec_t edges;     /* edge_t* */
  ivec_t dagEdges;
  ivec_t treeEdges;
  int treeParent; /* -1 == none */
  int row, col;
  bbox_t boundingBox;
  double cx, cy; /* coordinates */
  int incidentEdgeCount;
} block_t;

/* interval set: linear list of closed intervals, each carrying a segment. */
typedef struct {
  int lo, hi;
  edge_segment_t *value;
} iset_entry_t;

typedef struct {
  iset_entry_t *data;
  size_t len, cap;
} iset_t;

static int iset_intersectAny(iset_t *s, int a, int b) {
  int lo = a < b ? a : b, hi = a < b ? b : a;
  for (size_t i = 0; i < s->len; i++)
    if (lo <= s->data[i].hi && s->data[i].lo <= hi) return 1;
  return 0;
}
static void iset_insert(iset_t *s, int a, int b, edge_segment_t *v) {
  int lo = a < b ? a : b, hi = a < b ? b : a;
  if (s->len == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 8;
    s->data = (iset_entry_t *)realloc(s->data, s->cap * sizeof(iset_entry_t));
  }
  s->data[s->len].lo = lo;
  s->data[s->len].hi = hi;
  s->data[s->len].value = v;
  s->len++;
}

typedef struct {
  double width_or_height; /* width for columns, height for rows */
  double totalOffset;
  iset_t *intervals; /* array of iset_t */
  size_t nintervals, cap_intervals;
} grid_desc_t; /* used for both block and edge row/column descriptors */

static iset_t *grid_desc_add_interval(grid_desc_t *d) {
  if (d->nintervals == d->cap_intervals) {
    d->cap_intervals = d->cap_intervals ? d->cap_intervals * 2 : 4;
    d->intervals = (iset_t *)realloc(d->intervals, d->cap_intervals * sizeof(iset_t));
  }
  iset_t *t = &d->intervals[d->nintervals++];
  memset(t, 0, sizeof(*t));
  return t;
}

typedef struct {
  int blockIndex;
  int edgeIndex;
  int row;
  event_type_t type;
} layout_event_t;

typedef struct {
  edge_segment_t *segment;
  int length;
  int kind;
  int tiebreaker;
} segment_info_t;

/* ------------------------------------------------------------------ */
/* the layout core                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
  block_t *blocks;
  int nblocks;
  int columnCount;
  int rowCount;
  grid_desc_t *blockColumns;
  grid_desc_t *blockRows;
  grid_desc_t *edgeColumns; /* length columnCount+1 */
  grid_desc_t *edgeRows;    /* length rowCount+1 */
  int centerParents;
  int narrowLayout;
  int edge_spacing;
} core_t;

static edge_segment_t *seg_new(int sr, int sc, int er, int ec, seg_type_t t) {
  edge_segment_t *s = (edge_segment_t *)calloc(1, sizeof(edge_segment_t));
  s->start.row = sr;
  s->start.col = sc;
  s->end.row = er;
  s->end.col = ec;
  s->type = t;
  return s;
}

static void core_countEdges(core_t *c) {
  for (int i = 0; i < c->nblocks; i++) {
    block_t *b = &c->blocks[i];
    for (size_t j = 0; j < b->edges.len; j++) {
      edge_t *e = (edge_t *)b->edges.data[j];
      c->blocks[e->dest].incidentEdgeCount++;
    }
  }
}

static void core_postorderDFS(core_t *c, dfs_state_t *visited, int node, ivec_t *order) {
  if (visited[node] == DFS_VISITED) return;
  if (visited[node] == DFS_NOTVISITED) {
    visited[node] = DFS_PENDING;
    block_t *b = &c->blocks[node];
    for (size_t j = 0; j < b->edges.len; j++) {
      edge_t *e = (edge_t *)b->edges.data[j];
      /* reaching a pending node => loop edge (skip); else DAG edge. */
      if (visited[e->dest] != DFS_PENDING) ivec_push(&b->dagEdges, e->dest);
      core_postorderDFS(c, visited, e->dest, order);
    }
    visited[node] = DFS_VISITED;
    ivec_push(order, node);
  } else {
    assert(visited[node] == DFS_PENDING);
  }
}

/* topological order; breaks loop edges. Returns malloc'd array of length n. */
static int *core_computeDag(core_t *c, int *out_len) {
  dfs_state_t *visited = (dfs_state_t *)calloc((size_t)c->nblocks, sizeof(dfs_state_t));
  ivec_t order = {0};
  if (c->nblocks > 0) core_postorderDFS(c, visited, 0, &order);
  for (int i = 0; i < c->nblocks; i++) core_postorderDFS(c, visited, i, &order);
  free(visited);
  /* reverse */
  int n = (int)order.len;
  int *res = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
  for (int i = 0; i < n; i++) res[i] = order.data[n - 1 - i];
  ivec_free(&order);
  *out_len = n;
  return res;
}

static void core_assignRows(core_t *c, const int *topo, int n) {
  for (int k = 0; k < n; k++) {
    block_t *b = &c->blocks[topo[k]];
    for (size_t j = 0; j < b->dagEdges.len; j++) {
      block_t *t = &c->blocks[b->dagEdges.data[j]];
      if (t->row <= b->row + 1) t->row = b->row + 1;
    }
  }
}

static void core_computeTree(core_t *c, const int *topo, int n) {
  for (int k = 0; k < n; k++) {
    int i = topo[k];
    block_t *b = &c->blocks[i];
    for (size_t j = 0; j < b->dagEdges.len; j++) {
      int dj = b->dagEdges.data[j];
      block_t *t = &c->blocks[dj];
      if (t->treeParent == -1 && t->row == b->row + 1) {
        ivec_push(&b->treeEdges, dj);
        t->treeParent = i;
      }
    }
  }
}

static void core_adjustSubtree(core_t *c, int root, int rowShift, int columnShift) {
  block_t *b = &c->blocks[root];
  b->row += rowShift;
  b->col += columnShift;
  for (size_t i = 0; i < b->boundingBox.rows.len; i++) {
    b->boundingBox.rows.data[i].start += columnShift;
    b->boundingBox.rows.data[i].end += columnShift;
  }
  for (size_t i = 0; i < b->treeEdges.len; i++)
    core_adjustSubtree(c, b->treeEdges.data[i], rowShift, columnShift);
}

static int calculateTreePacking(const bbox_t *left, const bbox_t *right, int narrow) {
  if (!narrow) return 0;
  size_t n = left->rows.len < right->rows.len ? left->rows.len : right->rows.len;
  int best = 0;
  int have = 0;
  for (size_t i = 0; i < n; i++) {
    int leftBound = left->rows.data[i].end;
    int rightBound = right->rows.data[i].start;
    int offset = 0;
    offset -= left->width - leftBound;
    offset -= rightBound;
    if (!have || offset < best) {
      best = offset;
      have = 1;
    }
  }
  return have ? best : 0;
}

/* mutates `left` in place, returns nothing; left becomes the combined bounds.
 * Mirrors Dart _combineRowBounds (which returns left, possibly extended). */
static void combineRowBounds(rbvec_t *left, const rbvec_t *right) {
  size_t n = left->len < right->len ? left->len : right->len;
  for (size_t i = 0; i < n; i++) {
    if (right->data[i].start < left->data[i].start) left->data[i].start = right->data[i].start;
    if (right->data[i].end > left->data[i].end) left->data[i].end = right->data[i].end;
  }
  for (size_t i = left->len; i < right->len; i++) rbvec_push(left, right->data[i]);
}

static void bbox_set(bbox_t *bb, int w, int h) {
  rbvec_free(&bb->rows);
  bb->width = w;
  bb->height = h;
}

static void core_computeTreeColumnPositions(core_t *c, int node) {
  block_t *b = &c->blocks[node];
  if (b->treeEdges.len == 0) {
    b->row = 0;
    b->col = 0;
    bbox_set(&b->boundingBox, 2, 1);
    rbvec_push(&b->boundingBox.rows, (row_bound_t){0, 2});
  } else if (b->treeEdges.len == 1) {
    int childIndex = b->treeEdges.data[0];
    block_t *child = &c->blocks[childIndex];
    b->row = 0;
    b->col = child->col;
    bbox_t nb = {0};
    nb.width = child->boundingBox.width;
    nb.height = child->boundingBox.height + 1;
    rbvec_push(&nb.rows, (row_bound_t){child->col, child->col + 2});
    for (size_t i = 0; i < child->boundingBox.rows.len; i++)
      rbvec_push(&nb.rows, child->boundingBox.rows.data[i]);
    rbvec_free(&b->boundingBox.rows);
    b->boundingBox = nb;
    core_adjustSubtree(c, childIndex, 1, 0);
  } else {
    bbox_t bb = {0};
    for (size_t k = 0; k < b->treeEdges.len; k++) {
      int i = b->treeEdges.data[k];
      block_t *child = &c->blocks[i];
      int offset = calculateTreePacking(&bb, &child->boundingBox, c->narrowLayout);
      core_adjustSubtree(c, i, 1, bb.width + offset);
      bb.width += child->boundingBox.width + offset;
      if (child->boundingBox.height > bb.height) bb.height = child->boundingBox.height;
      combineRowBounds(&bb.rows, &child->boundingBox.rows);
    }
    bb.height++;
    rbvec_free(&b->boundingBox.rows);
    b->boundingBox = bb;
    b->row = 0;
    if (c->centerParents) {
      int w = b->boundingBox.width - 2;
      b->col = (w > 0 ? w : 0) / 2;
    } else {
      block_t *left = &c->blocks[b->treeEdges.data[0]];
      block_t *right = &c->blocks[b->treeEdges.data[1]];
      b->col = (left->col + right->col) / 2;
    }
    rbvec_insert0(&b->boundingBox.rows, (row_bound_t){b->col, b->col + 2});
  }
}

static void core_assignBlockColumns(core_t *c, const int *topo, int n) {
  for (int k = n - 1; k >= 0; k--) core_computeTreeColumnPositions(c, topo[k]);
  int offset = 0;
  for (int i = 0; i < c->nblocks; i++) {
    if (c->blocks[i].treeParent == -1) {
      core_adjustSubtree(c, i, 0, offset);
      offset += c->blocks[i].boundingBox.width;
    }
  }
}

static grid_desc_t *make_descs(int count, double initial_size) {
  grid_desc_t *d = (grid_desc_t *)calloc((size_t)(count > 0 ? count : 1), sizeof(grid_desc_t));
  for (int i = 0; i < count; i++) d[i].width_or_height = initial_size;
  return d;
}

static void core_setupRowsAndColumns(core_t *c) {
  if (c->nblocks == 0) {
    c->rowCount = 0;
    c->columnCount = 0;
  } else {
    int maxRow = 0, maxCol = 0;
    for (int i = 0; i < c->nblocks; i++) {
      if (c->blocks[i].row > maxRow) maxRow = c->blocks[i].row;
      if (c->blocks[i].col > maxCol) maxCol = c->blocks[i].col;
    }
    c->rowCount = maxRow + 1;
    c->columnCount = maxCol + 2;
  }
  c->blockRows = make_descs(c->rowCount, 0);
  c->blockColumns = make_descs(c->columnCount, 0);
  c->edgeRows = make_descs(c->rowCount + 1, 2 * c->edge_spacing);
  c->edgeColumns = make_descs(c->columnCount + 1, 2 * c->edge_spacing);
}

/* layout events ---------------------------------------------------- */

static int event_cmp(const void *pa, const void *pb) {
  const layout_event_t *a = (const layout_event_t *)pa;
  const layout_event_t *b = (const layout_event_t *)pb;
  if (a->row != b->row) return a->row - b->row;
  return (int)a->type - (int)b->type;
}

static layout_event_t *core_getLayoutEvents(core_t *c, size_t *out_n) {
  pvec_t tmp = {0}; /* not used; we count then fill */
  (void)tmp;
  size_t cap = 0;
  for (int i = 0; i < c->nblocks; i++) cap += 1 + c->blocks[i].edges.len;
  layout_event_t *ev = (layout_event_t *)malloc((cap ? cap : 1) * sizeof(layout_event_t));
  size_t k = 0;
  for (int i = 0; i < c->nblocks; i++) {
    block_t *b = &c->blocks[i];
    ev[k++] = (layout_event_t){i, -1, b->row, EV_BLOCK};
    for (size_t j = 0; j < b->edges.len; j++) {
      edge_t *e = (edge_t *)b->edges.data[j];
      int destRow = c->blocks[e->dest].row;
      int r = (b->row + 1) > destRow ? (b->row + 1) : destRow;
      ev[k++] = (layout_event_t){i, (int)j, r, EV_EDGE};
    }
  }
  *out_n = k;
  return ev;
}

static void closestUnblockedColumn(int sourceColumn, int topRow, const int *blocked,
                                    int blockedLen, int *leftOut, int *rightOut) {
  int leftFound = -1;
  for (int k = 0; k < sourceColumn; k++) {
    if (blocked[sourceColumn - 1 - k] < topRow) {
      leftFound = k;
      break;
    }
  }
  *leftOut = sourceColumn - 1 - leftFound;
  int rightFound = -1;
  for (int k = 0; k < blockedLen - sourceColumn; k++) {
    if (blocked[sourceColumn + k] < topRow) {
      rightFound = k;
      break;
    }
  }
  *rightOut = sourceColumn + rightFound;
}

static void core_assignMainColumn(block_t *source, block_t *target, edge_t *edge,
                                  const int *blocked, int blockedLen) {
  int sourceColumn = source->col + 1;
  int targetColumn = target->col + 1;
  int topRow = (source->row + 1) < target->row ? (source->row + 1) : target->row;
  if (blocked[sourceColumn] < topRow) {
    edge->mainColumn = sourceColumn;
  } else if (blocked[targetColumn] < topRow) {
    edge->mainColumn = targetColumn;
  } else {
    int leftCandidate, rightCandidate;
    closestUnblockedColumn(sourceColumn, topRow, blocked, blockedLen, &leftCandidate,
                           &rightCandidate);
    int distanceLeft = abs(sourceColumn - leftCandidate) + abs(targetColumn - leftCandidate);
    int distanceRight = abs(sourceColumn - rightCandidate) + abs(targetColumn - rightCandidate);
    if (target->row < source->row) {
      if (targetColumn < sourceColumn && blocked[sourceColumn + 1] < topRow &&
          sourceColumn - targetColumn <= distanceLeft + 2) {
        edge->mainColumn = sourceColumn + 1;
        return;
      }
      if (targetColumn > sourceColumn && blocked[sourceColumn - 1] < topRow &&
          targetColumn - sourceColumn <= distanceRight + 2) {
        edge->mainColumn = sourceColumn - 1;
        return;
      }
    }
    if (distanceLeft == distanceRight) {
      edge->mainColumn = edge->bias == CFG_BIAS_LEFT ? leftCandidate : rightCandidate;
    } else if (distanceLeft < distanceRight) {
      edge->mainColumn = leftCandidate;
    } else {
      edge->mainColumn = rightCandidate;
    }
  }
}

static void core_computeEdgeMainColumns(core_t *c) {
  size_t nev = 0;
  layout_event_t *ev = core_getLayoutEvents(c, &nev);
  qsort(ev, nev, sizeof(layout_event_t), event_cmp);
  int blockedLen = c->columnCount + 1;
  int *blocked = (int *)malloc((size_t)blockedLen * sizeof(int));
  for (int i = 0; i < blockedLen; i++) blocked[i] = -1;
  for (size_t i = 0; i < nev; i++) {
    layout_event_t *e = &ev[i];
    if (e->type == EV_BLOCK) {
      block_t *b = &c->blocks[e->blockIndex];
      blocked[b->col + 1] = b->row;
    } else {
      block_t *source = &c->blocks[e->blockIndex];
      edge_t *edge = (edge_t *)source->edges.data[e->edgeIndex];
      block_t *target = &c->blocks[edge->dest];
      core_assignMainColumn(source, target, edge, blocked, blockedLen);
    }
  }
  free(blocked);
  free(ev);
}

/* edge routing ----------------------------------------------------- */

static edge_segment_t *makeSegment(int sr, int sc, int er, int ec) {
  seg_type_t t = (sc == ec) ? SEG_VERTICAL : SEG_HORIZONTAL;
  return seg_new(sr, sc, er, ec, t);
}

static void core_addEdgeSegments(core_t *c, block_t *block, edge_t *edge) {
  block_t *target = &c->blocks[edge->dest];
  pvec_push(&edge->path, makeSegment(block->row + 1, block->col + 1, block->row + 1, block->col + 1));
  pvec_push(&edge->path, makeSegment(block->row + 1, block->col + 1, block->row + 1, edge->mainColumn));
  pvec_push(&edge->path, makeSegment(block->row + 1, edge->mainColumn, target->row, edge->mainColumn));
  pvec_push(&edge->path, makeSegment(target->row, edge->mainColumn, target->row, target->col + 1));
  pvec_push(&edge->path, makeSegment(target->row, target->col + 1, target->row, target->col + 1));
}

static void core_simplifyEdgePaths(edge_t *edge) {
  int movement;
  do {
    movement = 0;
    for (size_t i = 1; i < edge->path.len; i++) {
      edge_segment_t *prev = (edge_segment_t *)edge->path.data[i - 1];
      edge_segment_t *seg = (edge_segment_t *)edge->path.data[i];
      if (seg->start.col == seg->end.col && seg->start.row == seg->end.row &&
          i != edge->path.len - 1) {
        free(seg);
        pvec_removeAt(&edge->path, i);
        movement = 1;
        continue;
      }
      if (prev->type == seg->type) {
        prev->end = seg->end;
        free(seg);
        pvec_removeAt(&edge->path, i);
        movement = 1;
      }
    }
  } while (movement);
}

static void core_routeEdgePaths(core_t *c) {
  for (int i = 0; i < c->nblocks; i++) {
    block_t *b = &c->blocks[i];
    for (size_t j = 0; j < b->edges.len; j++) {
      edge_t *e = (edge_t *)b->edges.data[j];
      core_addEdgeSegments(c, b, e);
      core_simplifyEdgePaths(e);
    }
  }
}

static int classifyEdgeSegment(size_t i, pvec_t *path) {
  edge_segment_t *segment = (edge_segment_t *)path->data[i];
  int kind = K_NULL;
  if (i == 0) {
    if (path->len == 1) {
      kind = K_VERTICAL;
    } else {
      edge_segment_t *next = (edge_segment_t *)path->data[i + 1];
      kind = next->end.col > segment->end.col ? K_RIGHT_CORNER : K_LEFT_CORNER;
    }
  } else if (i == path->len - 1) {
    edge_segment_t *prev = (edge_segment_t *)path->data[i - 1];
    kind = prev->start.col > segment->end.col ? K_RIGHT_CORNER : K_LEFT_CORNER;
  } else {
    edge_segment_t *next = (edge_segment_t *)path->data[i + 1];
    edge_segment_t *prev = (edge_segment_t *)path->data[i - 1];
    if (segment->type == SEG_VERTICAL) {
      if (prev->start.col < segment->start.col && next->end.col < segment->start.col)
        kind = K_LEFT_U;
      else if (prev->start.col > segment->start.col && next->end.col > segment->start.col)
        kind = K_RIGHT_U;
      else if (prev->start.col > segment->end.col)
        kind = K_RIGHT_CORNER;
      else
        kind = K_LEFT_CORNER;
    } else {
      if (prev->start.row <= segment->start.row && next->end.row < segment->start.row)
        kind = K_LEFT_U;
      else if (prev->start.row > segment->start.row && next->end.row > segment->start.row)
        kind = K_RIGHT_U;
      else if (prev->start.row > segment->end.row)
        kind = K_RIGHT_CORNER;
      else
        kind = K_LEFT_CORNER;
    }
  }
  return kind;
}

static segment_info_t *core_getEdgeSegmentInfo(core_t *c, size_t *out_n) {
  pvec_t infos = {0};
  for (int bi = 0; bi < c->nblocks; bi++) {
    block_t *block = &c->blocks[bi];
    for (size_t j = 0; j < block->edges.len; j++) {
      edge_t *edge = (edge_t *)block->edges.data[j];
      int edgeLength = 0;
      for (size_t k = 0; k < edge->path.len; k++) {
        edge_segment_t *s = (edge_segment_t *)edge->path.data[k];
        edgeLength += abs(s->start.col - s->end.col) + abs(s->start.row - s->end.row);
      }
      block_t *target = &c->blocks[edge->dest];
      for (size_t k = 0; k < edge->path.len; k++) {
        edge_segment_t *seg = (edge_segment_t *)edge->path.data[k];
        int kind = classifyEdgeSegment(k, &edge->path);
        assert(kind != K_NULL);
        segment_info_t *si = (segment_info_t *)malloc(sizeof(segment_info_t));
        si->segment = seg;
        si->length = abs(seg->start.col - seg->end.col) + abs(seg->start.row - seg->end.row);
        si->kind = kind;
        si->tiebreaker = 2 * edgeLength + (target->row >= block->row ? 1 : 0);
        pvec_push(&infos, si);
      }
    }
  }
  /* flatten to array */
  size_t n = infos.len;
  segment_info_t *arr = (segment_info_t *)malloc((n ? n : 1) * sizeof(segment_info_t));
  for (size_t i = 0; i < n; i++) {
    arr[i] = *(segment_info_t *)infos.data[i];
    free(infos.data[i]);
  }
  pvec_free(&infos);
  *out_n = n;
  return arr;
}

static int segment_cmp(const void *pa, const void *pb) {
  const segment_info_t *a = (const segment_info_t *)pa;
  const segment_info_t *b = (const segment_info_t *)pb;
  if (a->kind != b->kind) return a->kind - b->kind;
  int kind = a->kind;
  if (a->length != b->length) {
    if (kind <= 0) return a->length - b->length;
    return b->length - a->length;
  }
  if (kind <= 0) return a->tiebreaker - b->tiebreaker;
  return b->tiebreaker - a->tiebreaker;
}

static void core_computeEdgeSegmentIntervals(core_t *c) {
  size_t n = 0;
  segment_info_t *segs = core_getEdgeSegmentInfo(c, &n);
  qsort(segs, n, sizeof(segment_info_t), segment_cmp);
  for (size_t i = 0; i < n; i++) {
    edge_segment_t *segment = segs[i].segment;
    if (segment->type == SEG_VERTICAL) {
      grid_desc_t *col = &c->edgeColumns[segment->start.col];
      int inserted = 0;
      for (size_t t = 0; t < col->nintervals; t++) {
        if (!iset_intersectAny(&col->intervals[t], segment->start.row, segment->end.row)) {
          iset_insert(&col->intervals[t], segment->start.row, segment->end.row, segment);
          inserted = 1;
          break;
        }
      }
      if (!inserted) {
        iset_t *t = grid_desc_add_interval(col);
        iset_insert(t, segment->start.row, segment->end.row, segment);
      }
    } else {
      grid_desc_t *row = &c->edgeRows[segment->start.row];
      int inserted = 0;
      for (size_t t = 0; t < row->nintervals; t++) {
        if (!iset_intersectAny(&row->intervals[t], segment->start.col, segment->end.col)) {
          iset_insert(&row->intervals[t], segment->start.col, segment->end.col, segment);
          inserted = 1;
          break;
        }
      }
      if (!inserted) {
        iset_t *t = grid_desc_add_interval(row);
        iset_insert(t, segment->start.col, segment->end.col, segment);
      }
    }
  }
  free(segs);
}

static void core_assignEdgeSegments(core_t *c) {
  core_computeEdgeSegmentIntervals(c);
  for (int i = 0; i < c->columnCount + 1; i++) {
    grid_desc_t *ec = &c->edgeColumns[i];
    double w = c->edge_spacing + (double)ec->nintervals * c->edge_spacing;
    ec->width_or_height = w > 2 * c->edge_spacing ? w : 2 * c->edge_spacing;
    for (size_t t = 0; t < ec->nintervals; t++)
      for (size_t v = 0; v < ec->intervals[t].len; v++)
        ec->intervals[t].data[v].value->horizontalOffset = (double)(c->edge_spacing * (t + 1));
  }
  for (int i = 0; i < c->rowCount + 1; i++) {
    grid_desc_t *er = &c->edgeRows[i];
    double h = c->edge_spacing + (double)er->nintervals * c->edge_spacing;
    er->width_or_height = h > 2 * c->edge_spacing ? h : 2 * c->edge_spacing;
    for (size_t t = 0; t < er->nintervals; t++)
      for (size_t v = 0; v < er->intervals[t].len; v++)
        er->intervals[t].data[v].value->verticalOffset = (double)(c->edge_spacing * (t + 1));
  }
}

/* coordinates ------------------------------------------------------ */

static void core_updateBlockDimensions(core_t *c) {
  for (int i = 0; i < c->nblocks; i++) {
    block_t *b = &c->blocks[i];
    double extra = (double)(b->incidentEdgeCount - 1) * c->edge_spacing;
    if (b->data->width < extra) b->data->width = extra;
  }
}

static void core_computeGridDimensions(core_t *c) {
  for (int i = 0; i < c->nblocks; i++) {
    block_t *b = &c->blocks[i];
    double halfWidth = (b->data->width - c->edgeColumns[b->col + 1].width_or_height) / 2;
    if (c->blockRows[b->row].width_or_height < b->data->height)
      c->blockRows[b->row].width_or_height = b->data->height;
    if (c->blockColumns[b->col].width_or_height < halfWidth)
      c->blockColumns[b->col].width_or_height = halfWidth;
    if (c->blockColumns[b->col + 1].width_or_height < halfWidth)
      c->blockColumns[b->col + 1].width_or_height = halfWidth;
  }
}

static void core_computeGridOffsets(core_t *c) {
  for (int i = 0; i < c->rowCount; i++) {
    c->blockRows[i].totalOffset = c->edgeRows[i].totalOffset + c->edgeRows[i].width_or_height;
    c->edgeRows[i + 1].totalOffset = c->blockRows[i].totalOffset + c->blockRows[i].width_or_height;
  }
  for (int i = 0; i < c->columnCount; i++) {
    c->blockColumns[i].totalOffset =
        c->edgeColumns[i].totalOffset + c->edgeColumns[i].width_or_height;
    c->edgeColumns[i + 1].totalOffset =
        c->blockColumns[i].totalOffset + c->blockColumns[i].width_or_height;
  }
}

static void core_computeBlockCoordinates(core_t *c, block_t *b) {
  b->cx = c->edgeColumns[b->col + 1].totalOffset -
          (b->data->width - c->edgeColumns[b->col + 1].width_or_height) / 2;
  b->cy = c->blockRows[b->row].totalOffset;
}

static void core_computeEdgeCoordinates(core_t *c, block_t *block, edge_t *edge) {
  pvec_t *path = &edge->path;
  if (path->len == 1) {
    edge_segment_t *segment = (edge_segment_t *)path->data[0];
    block_t *target = &c->blocks[edge->dest];
    segment->start.x = c->edgeColumns[segment->start.col].totalOffset + segment->horizontalOffset;
    segment->start.y = block->cy + block->data->height;
    segment->end.x = c->edgeColumns[segment->end.col].totalOffset + segment->horizontalOffset;
    segment->end.y = c->edgeRows[target->row].totalOffset + c->edgeRows[target->row].width_or_height;
  } else {
    {
      edge_segment_t *segment = (edge_segment_t *)path->data[0];
      segment->start.x = c->edgeColumns[segment->start.col].totalOffset + segment->horizontalOffset;
      segment->start.y = block->cy + block->data->height;
      segment->end.x = c->edgeColumns[segment->end.col].totalOffset + segment->horizontalOffset;
      segment->end.y = 0;
    }
    for (size_t k = 1; k < path->len - 1; k++) {
      edge_segment_t *segment = (edge_segment_t *)path->data[k];
      segment->start.x = c->edgeColumns[segment->start.col].totalOffset + segment->horizontalOffset;
      segment->start.y = c->edgeRows[segment->start.row].totalOffset + segment->verticalOffset;
      segment->end.x = c->edgeColumns[segment->end.col].totalOffset + segment->horizontalOffset;
      segment->end.y = c->edgeRows[segment->end.row].totalOffset + segment->verticalOffset;
    }
    {
      block_t *target = &c->blocks[edge->dest];
      edge_segment_t *segment = (edge_segment_t *)path->data[path->len - 1];
      segment->start.x = c->edgeColumns[segment->start.col].totalOffset + segment->horizontalOffset;
      segment->start.y = 0;
      segment->end.x = c->edgeColumns[segment->start.col].totalOffset + segment->horizontalOffset;
      segment->end.y =
          c->edgeRows[target->row].totalOffset + c->edgeRows[target->row].width_or_height;
    }
    for (size_t i = 0; i < path->len; i++) {
      edge_segment_t *segment = (edge_segment_t *)path->data[i];
      if (segment->type == SEG_VERTICAL) {
        if (i > 0) ((edge_segment_t *)path->data[i - 1])->end.x = segment->start.x;
        if (i < path->len - 1) ((edge_segment_t *)path->data[i + 1])->start.x = segment->end.x;
      } else {
        if (i > 0) ((edge_segment_t *)path->data[i - 1])->end.y = segment->start.y;
        if (i < path->len - 1) ((edge_segment_t *)path->data[i + 1])->start.y = segment->end.y;
      }
    }
  }
}

static void core_computeCoordinates(core_t *c) {
  core_updateBlockDimensions(c);
  core_computeGridDimensions(c);
  core_computeGridOffsets(c);
  for (int i = 0; i < c->nblocks; i++) {
    block_t *b = &c->blocks[i];
    core_computeBlockCoordinates(c, b);
    for (size_t j = 0; j < b->edges.len; j++)
      core_computeEdgeCoordinates(c, b, (edge_t *)b->edges.data[j]);
  }
}

static double core_getWidth(core_t *c) {
  grid_desc_t *last = &c->edgeColumns[c->columnCount];
  return last->totalOffset + last->width_or_height;
}
static double core_getHeight(core_t *c) {
  grid_desc_t *last = &c->edgeRows[c->rowCount];
  return last->totalOffset + last->width_or_height;
}

/* ------------------------------------------------------------------ */
/* public entry point                                                 */
/* ------------------------------------------------------------------ */

cfg_result_t *cfg_run(cfg_node_t *nodes, int nnodes, const cfg_edge_in_t *edges, int nedges,
                      int center_parents, int narrow_layout, int edge_spacing) {
  core_t c;
  memset(&c, 0, sizeof(c));
  c.nblocks = nnodes;
  c.centerParents = center_parents;
  c.narrowLayout = narrow_layout;
  c.edge_spacing = edge_spacing > 0 ? edge_spacing : CFG_DEFAULT_EDGE_SPACING;
  c.blocks = (block_t *)calloc((size_t)(nnodes > 0 ? nnodes : 1), sizeof(block_t));
  for (int i = 0; i < nnodes; i++) {
    c.blocks[i].data = &nodes[i];
    c.blocks[i].treeParent = -1;
  }
  /* populate edges grouped by source, preserving input order. */
  for (int i = 0; i < nedges; i++) {
    if (edges[i].from < 0 || edges[i].from >= nnodes) continue;
    if (edges[i].to < 0 || edges[i].to >= nnodes) continue;
    edge_t *e = (edge_t *)calloc(1, sizeof(edge_t));
    e->bias = edges[i].bias;
    e->dest = edges[i].to;
    e->orig_index = i;
    e->mainColumn = -1;
    pvec_push(&c.blocks[edges[i].from].edges, e);
  }

  if (nnodes > 0) {
    core_countEdges(&c);
    int topoLen = 0;
    int *topo = core_computeDag(&c, &topoLen);
    core_assignRows(&c, topo, topoLen);
    core_computeTree(&c, topo, topoLen);
    core_assignBlockColumns(&c, topo, topoLen);
    core_setupRowsAndColumns(&c);
    core_computeEdgeMainColumns(&c);
    core_routeEdgePaths(&c);
    core_assignEdgeSegments(&c);
    core_computeCoordinates(&c);
    free(topo);
  } else {
    core_setupRowsAndColumns(&c);
  }

  /* build result */
  cfg_result_t *r = (cfg_result_t *)calloc(1, sizeof(cfg_result_t));
  r->nodes = nodes;
  r->nnodes = nnodes;
  r->width = nnodes > 0 ? core_getWidth(&c) : 0;
  r->height = nnodes > 0 ? core_getHeight(&c) : 0;
  for (int i = 0; i < nnodes; i++) {
    nodes[i].x = c.blocks[i].cx;
    nodes[i].y = c.blocks[i].cy;
  }
  r->edges = (cfg_edge_out_t *)calloc((size_t)(nedges > 0 ? nedges : 1), sizeof(cfg_edge_out_t));
  r->nedges = nedges;
  /* default-init each output edge so unmapped ones are empty but valid */
  for (int i = 0; i < nedges; i++) {
    r->edges[i].from = edges[i].from;
    r->edges[i].to = edges[i].to;
    r->edges[i].npoints = 0;
    r->edges[i].xs = NULL;
    r->edges[i].ys = NULL;
  }
  for (int bi = 0; bi < nnodes; bi++) {
    block_t *b = &c.blocks[bi];
    for (size_t j = 0; j < b->edges.len; j++) {
      edge_t *e = (edge_t *)b->edges.data[j];
      cfg_edge_out_t *out = &r->edges[e->orig_index];
      size_t np = e->path.len == 0 ? 0 : e->path.len + 1;
      out->npoints = np;
      if (np > 0) {
        out->xs = (double *)malloc(np * sizeof(double));
        out->ys = (double *)malloc(np * sizeof(double));
        edge_segment_t *first = (edge_segment_t *)e->path.data[0];
        out->xs[0] = first->start.x;
        out->ys[0] = first->start.y;
        for (size_t k = 0; k < e->path.len; k++) {
          edge_segment_t *s = (edge_segment_t *)e->path.data[k];
          out->xs[k + 1] = s->end.x;
          out->ys[k + 1] = s->end.y;
        }
      }
    }
  }

  /* free working state */
  for (int i = 0; i < nnodes; i++) {
    block_t *b = &c.blocks[i];
    for (size_t j = 0; j < b->edges.len; j++) {
      edge_t *e = (edge_t *)b->edges.data[j];
      for (size_t k = 0; k < e->path.len; k++) free(e->path.data[k]);
      pvec_free(&e->path);
      free(e);
    }
    pvec_free(&b->edges);
    ivec_free(&b->dagEdges);
    ivec_free(&b->treeEdges);
    rbvec_free(&b->boundingBox.rows);
  }
  free(c.blocks);
  /* free grid descriptors */
  grid_desc_t *descs[4] = {c.blockRows, c.blockColumns, c.edgeRows, c.edgeColumns};
  int counts[4] = {c.rowCount, c.columnCount, c.rowCount + 1, c.columnCount + 1};
  for (int d = 0; d < 4; d++) {
    if (!descs[d]) continue;
    for (int i = 0; i < counts[d]; i++) {
      for (size_t t = 0; t < descs[d][i].nintervals; t++) free(descs[d][i].intervals[t].data);
      free(descs[d][i].intervals);
    }
    free(descs[d]);
  }
  return r;
}

void cfg_free(cfg_result_t *r) {
  if (!r) return;
  for (int i = 0; i < r->nedges; i++) {
    free(r->edges[i].xs);
    free(r->edges[i].ys);
  }
  free(r->edges);
  free(r);
}
