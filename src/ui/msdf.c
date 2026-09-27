#include "msdf.h"

#include FT_OUTLINE_H

#include <math.h>
#include <string.h>

/* Everything below is msdfgen's arithmetic, in pixels with y up as FreeType
 * has it. Doubles: the cubic solver loses too much in float, and the TV's VFP
 * does them at nearly the same speed. */

typedef struct {
  double x, y;
} vec2;

static vec2 add(vec2 a, vec2 b) { return (vec2){a.x + b.x, a.y + b.y}; }
static vec2 sub(vec2 a, vec2 b) { return (vec2){a.x - b.x, a.y - b.y}; }
static vec2 mul(vec2 a, double s) { return (vec2){a.x * s, a.y * s}; }
static vec2 mix(vec2 a, vec2 b, double t) { return add(a, mul(sub(b, a), t)); }
static double dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
static double cross(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }
static double length(vec2 a) { return sqrt(dot(a, a)); }
static bool zero(vec2 a) { return a.x == 0 && a.y == 0; }
static vec2 normalize(vec2 a) {
  const double l = length(a);
  return l == 0 ? (vec2){0, 1} : mul(a, 1 / l);
}
static double sign_nz(double x) { return x > 0 ? 1 : -1; }
/* Not fmin/fmax, which keep NaN semantics and so stay library calls. */
static double lesser(double a, double b) { return a < b ? a : b; }
static double greater(double a, double b) { return a > b ? a : b; }

enum { BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE };

typedef struct {
  vec2 p[4];
  int degree; /* 1 line, 2 quadratic, 3 cubic */
  int color;
  double box[4]; /* the control points' bounds, which contain the curve */
} edge;

typedef struct {
  double distance, dot;
} sdist;

static bool closer(sdist a, sdist b) {
  return fabs(a.distance) < fabs(b.distance) ||
         (fabs(a.distance) == fabs(b.distance) && a.dot < b.dot);
}

/* The tangent at 0 or 1, never zero where a control point coincides with an
 * end. */
static vec2 direction(const edge *e, int end) {
  const vec2 *p = e->p;
  const int d = e->degree;
  vec2 t = end == 0 ? sub(p[1], p[0]) : sub(p[d], p[d - 1]);
  if (zero(t) && d >= 2)
    t = end == 0 ? sub(p[2], p[0]) : sub(p[d], p[d - 2]);
  if (zero(t) && d == 3)
    t = sub(p[3], p[0]);
  return t;
}

/* de Casteljau: `left` is [0, t] and `right` [t, 1]. */
static void split(const edge *e, double t, edge *left, edge *right) {
  vec2 q[4];
  memcpy(q, e->p, sizeof(q));
  *left = *right = *e;
  const int d = e->degree;
  left->p[0] = q[0];
  right->p[d] = q[d];
  for (int n = d, k = 1; n > 0; n--, k++) {
    for (int i = 0; i < n; i++)
      q[i] = mix(q[i], q[i + 1], t);
    left->p[k] = q[0];
    right->p[d - k] = q[n - 1];
  }
}

/* ------------------------------------------------------------------ solvers */

static int solve_quadratic(double x[2], double a, double b, double c) {
  if (a == 0 || fabs(b) > 1e12 * fabs(a)) {
    if (b == 0)
      return 0;
    x[0] = -c / b;
    return 1;
  }
  double dscr = b * b - 4 * a * c;
  if (dscr > 0) {
    dscr = sqrt(dscr);
    x[0] = (-b + dscr) / (2 * a);
    x[1] = (-b - dscr) / (2 * a);
    return 2;
  }
  if (dscr == 0) {
    x[0] = -b / (2 * a);
    return 1;
  }
  return 0;
}

static int solve_cubic_normed(double x[3], double a, double b, double c) {
  const double a2 = a * a;
  double q = (a2 - 3 * b) / 9;
  const double r = (a * (2 * a2 - 9 * b) + 27 * c) / 54;
  const double r2 = r * r, q3 = q * q * q;
  a /= 3;
  if (r2 < q3) {
    double t = r / sqrt(q3);
    t = acos(t < -1 ? -1 : t > 1 ? 1 : t);
    q = -2 * sqrt(q);
    /* cos(t/3 +- 2pi/3) from one cos and one sin. */
    const double c3 = cos(t / 3), s3 = sin(t / 3) * 0.86602540378443864676;
    x[0] = q * c3 - a;
    x[1] = q * (-0.5 * c3 - s3) - a;
    x[2] = q * (-0.5 * c3 + s3) - a;
    return 3;
  }
  const double u = (r < 0 ? 1 : -1) * cbrt(fabs(r) + sqrt(r2 - q3));
  const double v = u == 0 ? 0 : q / u;
  x[0] = (u + v) - a;
  if (u == v || fabs(u - v) < 1e-12 * fabs(u + v)) {
    x[1] = -0.5 * (u + v) - a;
    return 2;
  }
  return 1;
}

static int solve_cubic(double x[3], double a, double b, double c, double d) {
  if (a != 0) {
    const double bn = b / a;
    if (fabs(bn) < 1e6)
      return solve_cubic_normed(x, bn, c / a, d / a);
  }
  return solve_quadratic(x, b, c, d);
}

/* ---------------------------------------------------------------- distances */

static sdist end_distance(const edge *e, double min, double param, vec2 qa,
                          vec2 to_end) {
  if (param >= 0 && param <= 1)
    return (sdist){min, 0};
  if (param < .5)
    return (sdist){min, fabs(dot(normalize(direction(e, 0)), normalize(qa)))};
  return (sdist){min, fabs(dot(normalize(direction(e, 1)), normalize(to_end)))};
}

static sdist linear_distance(const edge *e, vec2 origin, double *param) {
  const vec2 aq = sub(origin, e->p[0]), ab = sub(e->p[1], e->p[0]);
  *param = dot(aq, ab) / dot(ab, ab);
  const vec2 eq = sub(e->p[*param > .5], origin);
  const double endpoint = length(eq);
  if (*param > 0 && *param < 1) {
    const double ortho = cross(aq, ab) / length(ab);
    if (fabs(ortho) < endpoint)
      return (sdist){ortho, 0};
  }
  return (sdist){sign_nz(cross(aq, ab)) * endpoint,
                 fabs(dot(normalize(ab), normalize(eq)))};
}

static sdist quadratic_distance(const edge *e, vec2 origin, double *param) {
  const vec2 *p = e->p;
  const vec2 qa = sub(p[0], origin), ab = sub(p[1], p[0]);
  const vec2 br = sub(sub(p[2], p[1]), ab);
  double t[3];
  const int n = solve_cubic(t, dot(br, br), 3 * dot(ab, br),
                            2 * dot(ab, ab) + dot(qa, br), dot(qa, ab));

  vec2 dir = direction(e, 0);
  double min = sign_nz(cross(dir, qa)) * length(qa);
  *param = -dot(qa, dir) / dot(dir, dir);
  const vec2 to_end = sub(p[2], origin);
  dir = direction(e, 1);
  const double distance = length(to_end);
  if (distance < fabs(min)) {
    min = sign_nz(cross(dir, to_end)) * distance;
    *param = dot(sub(origin, p[1]), dir) / dot(dir, dir);
  }
  for (int i = 0; i < n; i++) {
    if (t[i] > 0 && t[i] < 1) {
      const vec2 qe = add(add(qa, mul(ab, 2 * t[i])), mul(br, t[i] * t[i]));
      const double d = length(qe);
      if (d <= fabs(min)) {
        min = sign_nz(cross(add(ab, mul(br, t[i])), qe)) * d;
        *param = t[i];
      }
    }
  }
  return end_distance(e, min, *param, qa, to_end);
}

#define CUBIC_STARTS 4
#define CUBIC_STEPS 4

static sdist cubic_distance(const edge *e, vec2 origin, double *param) {
  const vec2 *p = e->p;
  const vec2 qa = sub(p[0], origin), ab = sub(p[1], p[0]);
  const vec2 br = sub(sub(p[2], p[1]), ab);
  const vec2 as = sub(sub(sub(p[3], p[2]), sub(p[2], p[1])), br);

  vec2 dir = direction(e, 0);
  double min = sign_nz(cross(dir, qa)) * length(qa);
  *param = -dot(qa, dir) / dot(dir, dir);
  const vec2 to_end = sub(p[3], origin);
  dir = direction(e, 1);
  const double distance = length(to_end);
  if (distance < fabs(min)) {
    min = sign_nz(cross(dir, to_end)) * distance;
    *param = dot(sub(dir, to_end), dir) / dot(dir, dir);
  }
  for (int i = 0; i <= CUBIC_STARTS; i++) {
    double t = (double)i / CUBIC_STARTS;
    vec2 qe = add(add(qa, mul(ab, 3 * t)),
                  add(mul(br, 3 * t * t), mul(as, t * t * t)));
    vec2 d1 = add(add(mul(ab, 3), mul(br, 6 * t)), mul(as, 3 * t * t));
    vec2 d2 = add(mul(br, 6), mul(as, 6 * t));
    double improved = t - dot(qe, d1) / (dot(d1, d1) + dot(qe, d2));
    if (!(improved > 0 && improved < 1))
      continue;
    for (int steps = CUBIC_STEPS;;) {
      t = improved;
      qe = add(add(qa, mul(ab, 3 * t)),
               add(mul(br, 3 * t * t), mul(as, t * t * t)));
      d1 = add(add(mul(ab, 3), mul(br, 6 * t)), mul(as, 3 * t * t));
      if (--steps == 0)
        break;
      d2 = add(mul(br, 6), mul(as, 6 * t));
      improved = t - dot(qe, d1) / (dot(d1, d1) + dot(qe, d2));
      if (!(improved > 0 && improved < 1))
        break;
    }
    const double d = length(qe);
    if (d < fabs(min)) {
      min = sign_nz(cross(d1, qe)) * d;
      *param = t;
    }
  }
  return end_distance(e, min, *param, qa, to_end);
}

static sdist edge_distance(const edge *e, vec2 origin, double *param) {
  switch (e->degree) {
  case 1:
    return linear_distance(e, origin, param);
  case 2:
    return quadratic_distance(e, origin, param);
  default:
    return cubic_distance(e, origin, param);
  }
}

/* Past an end, the distance to the edge's extension: what keeps two channels
 * meeting at a corner from rounding it. */
static void to_pseudo(const edge *e, sdist *d, vec2 origin, double param) {
  if (param < 0) {
    const vec2 dir = normalize(direction(e, 0)), aq = sub(origin, e->p[0]);
    if (dot(aq, dir) < 0) {
      const double pseudo = cross(aq, dir);
      if (fabs(pseudo) <= fabs(d->distance))
        *d = (sdist){pseudo, 0};
    }
  } else if (param > 1) {
    const vec2 dir = normalize(direction(e, 1));
    const vec2 bq = sub(origin, e->p[e->degree]);
    if (dot(bq, dir) > 0) {
      const double pseudo = cross(bq, dir);
      if (fabs(pseudo) <= fabs(d->distance))
        *d = (sdist){pseudo, 0};
    }
  }
}

/* ------------------------------------------------------------------- shape */

/* `edges` is sized from the outline up front, so reading it never grows. */
typedef struct {
  edge *edges;
  size_t count, capacity;
  size_t contour; /* where the contour being read starts */
  vec2 pen;
  unsigned long long seed;
  bool failed;
} shape;

static edge *push(shape *s) {
  if (s->count == s->capacity) {
    s->failed = true;
    return NULL;
  }
  return &s->edges[s->count++];
}

static void switch_color(int *color, unsigned long long *seed, int banned) {
  const int combined = *color & banned;
  if (combined == RED || combined == GREEN || combined == BLUE) {
    *color = combined ^ WHITE;
    return;
  }
  if (*color == BLACK || *color == WHITE) {
    static const int start[3] = {CYAN, MAGENTA, YELLOW};
    *color = start[*seed % 3];
    *seed /= 3;
    return;
  }
  const int shifted = *color << (1 + (*seed & 1));
  *color = (shifted | shifted >> 3) & WHITE;
  *seed >>= 1;
}

static bool is_corner(vec2 a, vec2 b) {
  /* sin(3 rad): anything sharper than about 8 degrees off straight. */
  return dot(a, b) <= 0 || fabs(cross(a, b)) > 0.14112000805986721;
}

/* msdfgen's edgeColoringSimple, for the contour just read: smooth runs share
 * a colour and every corner switches it, so no two edges meeting at a corner
 * have a channel in common. */
static void color_contour(shape *s) {
  edge *edges = s->edges + s->contour;
  const size_t m = s->count - s->contour;
  if (m == 0)
    return;
  size_t corners[64], corner_count = 0;
  vec2 previous = direction(&edges[m - 1], 1);
  for (size_t i = 0; i < m; i++) {
    if (is_corner(normalize(previous), normalize(direction(&edges[i], 0))) &&
        corner_count < 64)
      corners[corner_count++] = i;
    previous = direction(&edges[i], 1);
  }

  if (corner_count == 0) {
    for (size_t i = 0; i < m; i++)
      edges[i].color = WHITE;
    return;
  }
  if (corner_count > 1) {
    int color = WHITE;
    switch_color(&color, &s->seed, BLACK);
    const int initial = color;
    size_t spline = 0;
    for (size_t i = 0; i < m; i++) {
      const size_t index = (corners[0] + i) % m;
      if (spline + 1 < corner_count && corners[spline + 1] == index) {
        spline++;
        switch_color(&color, &s->seed,
                     spline == corner_count - 1 ? initial : BLACK);
      }
      edges[index].color = color;
    }
    return;
  }

  /* A teardrop: one corner, three colours around it. */
  int colors[3] = {WHITE, WHITE, WHITE};
  switch_color(&colors[0], &s->seed, BLACK);
  colors[2] = colors[0];
  switch_color(&colors[2], &s->seed, BLACK);
  const size_t corner = corners[0];
  if (m >= 3) {
    for (size_t i = 0; i < m; i++) {
      const int third =
          (int)(3 + 2.875 * (double)i / (double)(m - 1) - 1.4375 + .5) - 3;
      edges[(corner + i) % m].color = colors[1 + third];
    }
    return;
  }
  /* Too few edges to colour three ways: cut each into thirds. */
  edge parts[6];
  size_t n = 0;
  for (size_t i = 0; i < m; i++) {
    edge a, rest, b, c;
    split(&edges[(corner + i) % m], 1.0 / 3, &a, &rest);
    split(&rest, 0.5, &b, &c);
    parts[n++] = a;
    parts[n++] = b;
    parts[n++] = c;
  }
  s->count = s->contour;
  for (size_t i = 0; i < n; i++) {
    edge *e = push(s);
    if (e == NULL)
      return;
    *e = parts[i];
    e->color = colors[i * 3 / n];
  }
}

static void end_contour(shape *s) {
  color_contour(s);
  s->contour = s->count;
}

static vec2 from_ft(const FT_Vector *v) {
  return (vec2){v->x / 64.0, v->y / 64.0};
}

static void add_edge(shape *s, int degree, const vec2 *points) {
  edge *e = push(s);
  if (e == NULL)
    return;
  e->degree = degree;
  e->color = WHITE;
  e->p[0] = s->pen;
  for (int i = 0; i < degree; i++)
    e->p[i + 1] = points[i];
  for (int i = degree + 1; i < 4; i++)
    e->p[i] = e->p[degree];
  e->box[0] = e->box[2] = e->p[0].x;
  e->box[1] = e->box[3] = e->p[0].y;
  for (int i = 1; i <= degree; i++) {
    e->box[0] = lesser(e->box[0], e->p[i].x);
    e->box[1] = lesser(e->box[1], e->p[i].y);
    e->box[2] = greater(e->box[2], e->p[i].x);
    e->box[3] = greater(e->box[3], e->p[i].y);
  }
  s->pen = points[degree - 1];
}

static int move_to(const FT_Vector *to, void *user) {
  shape *s = user;
  end_contour(s);
  s->pen = from_ft(to);
  return 0;
}

static int line_to(const FT_Vector *to, void *user) {
  shape *s = user;
  const vec2 p = from_ft(to);
  if (p.x != s->pen.x || p.y != s->pen.y)
    add_edge(s, 1, &p);
  return 0;
}

static int conic_to(const FT_Vector *control, const FT_Vector *to, void *user) {
  const vec2 p[2] = {from_ft(control), from_ft(to)};
  add_edge(user, 2, p);
  return 0;
}

static int cubic_to(const FT_Vector *c1, const FT_Vector *c2,
                    const FT_Vector *to, void *user) {
  const vec2 p[3] = {from_ft(c1), from_ft(c2), from_ft(to)};
  add_edge(user, 3, p);
  return 0;
}

/* --------------------------------------------------------------- generation */

static float median(float a, float b, float c) {
  return fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
}

/* msdfgen's legacy clash test: neighbours whose channels disagree about which
 * side of an edge they are on by more than a texel's worth of distance. */
static bool clash(const float *a, const float *b, float threshold) {
  float a0 = a[0], a1 = a[1], a2 = a[2], b0 = b[0], b1 = b[1], b2 = b[2], t;
#define SWAP(x, y) (t = x, x = y, y = t)
  if (fabsf(b0 - a0) < fabsf(b1 - a1)) {
    SWAP(a0, a1);
    SWAP(b0, b1);
  }
  if (fabsf(b1 - a1) < fabsf(b2 - a2)) {
    SWAP(a1, a2);
    SWAP(b1, b2);
    if (fabsf(b0 - a0) < fabsf(b1 - a1)) {
      SWAP(a0, a1);
      SWAP(b0, b1);
    }
  }
#undef SWAP
  return fabsf(b1 - a1) >= threshold && !(b0 == b1 && b0 == b2) &&
         fabsf(a2 - .5f) >= fabsf(b2 - .5f);
}

static void correct_errors(jf_arena *scratch, float *field, int w, int h,
                           float threshold) {
  uint8_t *marked = jf_arena_alloc(scratch, (size_t)w * h);
  if (marked == NULL)
    return;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const float *p = field + ((size_t)y * w + x) * 4;
      marked[y * w + x] = (x > 0 && clash(p, p - 4, threshold)) ||
                          (x < w - 1 && clash(p, p + 4, threshold)) ||
                          (y > 0 && clash(p, p - (size_t)w * 4, threshold)) ||
                          (y < h - 1 && clash(p, p + (size_t)w * 4, threshold));
    }
  for (size_t i = 0; i < (size_t)w * h; i++)
    if (marked[i]) {
      float *p = field + i * 4;
      p[0] = p[1] = p[2] = median(p[0], p[1], p[2]);
    }
}

/* A texel more than a pixel from any edge whose sign FreeType's own
 * rasteriser contradicts is flipped: what fixes a face wound the other way,
 * or a stray inversion where contours overlap. Nearer the edge, where
 * coverage and a sampled distance legitimately disagree, the field wins. */
static void correct_signs(FT_Library library, jf_arena *scratch,
                          FT_Outline *outline, int left, int bottom,
                          float *field, int w, int h, float range) {
  uint8_t *coverage = jf_arena_alloc(scratch, (size_t)w * h);
  if (coverage == NULL)
    return;
  memset(coverage, 0, (size_t)w * h);
  FT_Outline_Translate(outline, -left * 64, -bottom * 64);
  FT_Bitmap bitmap = {0};
  bitmap.rows = (unsigned)h;
  bitmap.width = (unsigned)w;
  bitmap.pitch = w;
  bitmap.buffer = coverage;
  bitmap.num_grays = 256;
  bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
  if (FT_Outline_Get_Bitmap(library, outline, &bitmap) == 0) {
    const float margin = 1.0f / range;
    for (size_t i = 0; i < (size_t)w * h; i++) {
      float *p = field + i * 4;
      const float m = median(p[0], p[1], p[2]) - .5f;
      if ((m > margin && coverage[i] == 0) ||
          (m < -margin && coverage[i] == 255))
        p[0] = 1 - p[0], p[1] = 1 - p[1], p[2] = 1 - p[2];
      const float a = p[3] - .5f;
      if ((a > margin && coverage[i] == 0) ||
          (a < -margin && coverage[i] == 255))
        p[3] = 1 - p[3];
    }
  }
}

bool jf_msdf_generate(FT_Library library, FT_Outline *outline, float range,
                      jf_arena *scratch, jf_msdf *out) {
  *out = (jf_msdf){0};
  /* One edge per point at most, a closing line per contour, and a teardrop
   * contour of one or two edges cut into six. */
  shape s = {0};
  s.capacity = (size_t)outline->n_points + 7 * (size_t)outline->n_contours;
  s.edges = jf_arena_alloc(scratch, s.capacity * sizeof(*s.edges));
  if (s.edges == NULL)
    return false;
  static const FT_Outline_Funcs funcs = {move_to,  line_to, conic_to,
                                         cubic_to, 0,       0};
  if (FT_Outline_Decompose(outline, &funcs, &s) != 0)
    s.count = 0;
  end_contour(&s);
  if (s.failed)
    return false;
  if (s.count == 0)
    return true;

  FT_BBox box;
  FT_Outline_Get_CBox(outline, &box);
  const double pad = range / 2;
  const int left = (int)floor(box.xMin / 64.0 - pad);
  const int right = (int)ceil(box.xMax / 64.0 + pad);
  const int bottom = (int)floor(box.yMin / 64.0 - pad);
  const int top = (int)ceil(box.yMax / 64.0 + pad);
  const int w = right - left, h = top - bottom;
  float *field = jf_arena_alloc(scratch, (size_t)w * h * 4 * sizeof(*field));
  uint8_t *rgba = jf_arena_alloc(scratch, (size_t)w * h * 4);
  if (field == NULL || rgba == NULL)
    return false;

  /* TrueType winds outer contours clockwise, which is what makes inside
   * positive here; PostScript the other way. */
  const double orient =
      FT_Outline_Get_Orientation(outline) == FT_ORIENTATION_POSTSCRIPT ? -1 : 1;
  /* Neighbouring texels mostly share their nearest edge; trying it first
   * tightens the cull below before most edges are looked at. */
  size_t warm = 0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const vec2 p = {left + x + .5, top - (y + .5)};
      sdist best[3], nearest = {-1e240, 1};
      const edge *near[3] = {NULL, NULL, NULL};
      double near_param[3] = {0, 0, 0};
      best[0] = best[1] = best[2] = nearest;
      size_t found = warm;
      for (size_t k = 0; k < s.count; k++) {
        const size_t i = warm + k < s.count ? warm + k : warm + k - s.count;
        const edge *e = &s.edges[i];
        /* The box contains the edge, so an edge whose box is further than
         * every one of its channels' nearest edge so far cannot improve any
         * of them - nor the true distance, which is never further than a
         * channel's. Per channel: an edge beyond the nearest edge overall can
         * still be the nearest one carrying green. */
        const double dx = greater(greater(e->box[0] - p.x, p.x - e->box[2]), 0);
        const double dy = greater(greater(e->box[1] - p.y, p.y - e->box[3]), 0);
        const double reach = (dx * dx + dy * dy) / 1.000001;
        bool useful = false;
        for (int c = 0; c < 3 && !useful; c++)
          useful = (e->color & (1 << c)) &&
                   reach <= best[c].distance * best[c].distance;
        if (!useful)
          continue;
        double param;
        const sdist d = edge_distance(e, p, &param);
        if (closer(d, nearest)) {
          nearest = d;
          found = i;
        }
        for (int c = 0; c < 3; c++)
          if ((e->color & (1 << c)) && closer(d, best[c])) {
            best[c] = d;
            near[c] = e;
            near_param[c] = param;
          }
      }
      warm = found;
      float *texel = field + ((size_t)y * w + x) * 4;
      for (int c = 0; c < 3; c++) {
        if (near[c] != NULL)
          to_pseudo(near[c], &best[c], p, near_param[c]);
        texel[c] = (float)(orient * best[c].distance / range + .5);
      }
      texel[3] = (float)(orient * nearest.distance / range + .5);
    }
  }

  correct_errors(scratch, field, w, h, 1.001f / range);
  correct_signs(library, scratch, outline, left, bottom, field, w, h, range);
  for (size_t i = 0; i < (size_t)w * h * 4; i++) {
    const float value = field[i] * 255 + .5f;
    rgba[i] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
  }
  *out = (jf_msdf){w, h, left, -top, rgba};
  return true;
}
