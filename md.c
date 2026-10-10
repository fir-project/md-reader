/*
 *
 * simple markdown reader
 * developer and maintainer: emil@firproject.org
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
  char *p;
  size_t len, cap;
} Buf;

static void buf_init(Buf *b) {
  b->p = NULL;
  b->len = b->cap = 0;
}

static void buf_reserve(Buf *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap)
    return;
  size_t nc = b->cap ? b->cap : 64;
  while (nc < b->len + extra + 1)
    nc *= 2;
  char *np = realloc(b->p, nc);
  if (!np) {
    perror("realloc");
    exit(1);
  }
  b->p = np;
  b->cap = nc;
}

static void buf_add(Buf *b, const char *s, size_t n) {
  if (!n)
    return;
  buf_reserve(b, n);
  memcpy(b->p + b->len, s, n);
  b->len += n;
}

static void buf_adds(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_addc(Buf *b, char c) { buf_add(b, &c, 1); }

static char *buf_take(Buf *b) {
  buf_reserve(b, 0);
  b->p[b->len] = '\0';
  char *r = b->p;
  b->p = NULL;
  b->len = b->cap = 0;
  return r;
}

static char *xstrdup(const char *s) {
  char *r = strdup(s);
  if (!r) {
    perror("strdup");
    exit(1);
  }
  return r;
}

static char *subdup(const char *s, size_t n) {
  char *r = malloc(n + 1);
  if (!r) {
    perror("malloc");
    exit(1);
  }
  memcpy(r, s, n);
  r[n] = '\0';
  return r;
}

static int is_blank(const char *s) {
  for (; *s; s++)
    if (!isspace((unsigned char)*s))
      return 0;
  return 1;
}

static int is_hr(const char *s) {
  int cnt = 0;
  char c = 0;
  for (; *s; s++) {
    if (*s == ' ' || *s == '\t')
      continue;
    if (c == 0) {
      if (*s != '-' && *s != '*' && *s != '_')
        return 0;
      c = *s;
    }
    if (*s != c)
      return 0;
    cnt++;
  }
  return cnt >= 3;
}

#define LF_PRE 1u

typedef struct {
  char **v;
  unsigned char *f;
  size_t n, cap;
} Lines;

static void lines_push_f(Lines *L, char *s, unsigned char fl) {
  if (L->n == L->cap) {
    size_t nc = L->cap ? L->cap * 2 : 64;
    char **nv = realloc(L->v, nc * sizeof *nv);
    unsigned char *nf = realloc(L->f, nc * sizeof *nf);
    if (!nv || !nf) {
      perror("realloc");
      exit(1);
    }
    L->v = nv;
    L->f = nf;
    L->cap = nc;
  }
  L->v[L->n] = s;
  L->f[L->n] = fl;
  L->n++;
}

static void lines_push(Lines *L, char *s) { lines_push_f(L, s, 0); }

static void lines_free(Lines *L) {
  for (size_t i = 0; i < L->n; i++)
    free(L->v[i]);
  free(L->v);
  free(L->f);
  L->v = NULL;
  L->f = NULL;
  L->n = L->cap = 0;
}

#define BOLD_ON "\x1b[1m"
#define BOLD_OFF "\x1b[22m"
#define CODE_ON "\x1b[36m"
#define CODE_OFF "\x1b[39m"
#define BLOCK_ON "\x1b[2;36m"
#define BLOCK_OFF "\x1b[22;39m"

static void render_inline(const char *s, Buf *out) {
  size_t n = strlen(s);
  size_t i = 0;

  while (i < n) {
    char c = s[i];

    if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
      buf_addc(out, s[i + 1]);
      i += 2;
      continue;
    }

    if (c == '`') {
      size_t j = i + 1;
      while (j < n && s[j] != '`')
        j++;
      if (j < n) {
        buf_adds(out, CODE_ON);
        buf_add(out, s + i + 1, j - i - 1);
        buf_adds(out, CODE_OFF);
        i = j + 1;
      } else {
        buf_addc(out, c);
        i++;
      }
      continue;
    }

    if (c == '!' && i + 1 < n && s[i + 1] == '[') {
      size_t j = i + 2;
      while (j < n && s[j] != ']')
        j++;
      if (j < n) {
        char *alt = subdup(s + i + 2, j - i - 2);
        buf_addc(out, '[');
        render_inline(alt, out);
        buf_addc(out, ']');
        free(alt);
        i = j + 1;
        if (i < n && s[i] == '(') {
          size_t k = i + 1;
          while (k < n && s[k] != ')')
            k++;
          if (k < n)
            i = k + 1;
        }
        continue;
      }
    }

    if (c == '[') {
      size_t j = i + 1;
      while (j < n && s[j] != ']')
        j++;
      if (j + 1 < n && s[j + 1] == '(') {
        size_t k = j + 2;
        while (k < n && s[k] != ')')
          k++;
        if (k < n) {
          char *text = subdup(s + i + 1, j - i - 1);
          char *url = subdup(s + j + 2, k - j - 2);
          render_inline(text, out);
          if (*url && strcmp(url, text) != 0) {
            buf_adds(out, " (");
            buf_adds(out, url);
            buf_addc(out, ')');
          }
          free(text);
          free(url);
          i = k + 1;
          continue;
        }
      }
    }

    if (c == '*' || c == '_') {

      if (i + 1 < n && s[i + 1] == c) {
        size_t j = i + 2;
        int found = 0;
        while (j + 1 < n) {
          if (s[j] == c && s[j + 1] == c) {
            found = 1;
            break;
          }
          j++;
        }
        if (found) {
          char *inner = subdup(s + i + 2, j - i - 2);
          buf_adds(out, "\x1b[1m");
          render_inline(inner, out);
          buf_adds(out, "\x1b[22m");
          free(inner);
          i = j + 2;
          continue;
        }
      }

      if (c == '_' && i > 0 && isalnum((unsigned char)s[i - 1])) {
        buf_addc(out, c);
        i++;
        continue;
      }

      if (i + 1 < n && s[i + 1] != ' ' && s[i + 1] != c) {
        size_t j = i + 1;
        int found = 0;
        while (j < n) {
          if (s[j] == c) {
            found = 1;
            break;
          }
          j++;
        }
        if (found && j > i + 1) {
          char *inner = subdup(s + i + 1, j - i - 1);
          buf_adds(out, "\x1b[3m");
          render_inline(inner, out);
          buf_adds(out, "\x1b[23m");
          free(inner);
          i = j + 1;
          continue;
        }
      }
    }

    buf_addc(out, c);
    i++;
  }
}

static void add_expanded(Buf *b, const char *s, int col) {
  for (; *s; s++) {
    if (*s == '\t') {
      int adv = 8 - (col % 8);
      for (int i = 0; i < adv; i++)
        buf_addc(b, ' ');
      col += adv;
    } else {
      buf_addc(b, *s);
      if (((unsigned char)*s & 0xC0) != 0x80)
        col++;
    }
  }
}

static void push_code(Lines *out, const char *s, int col) {
  Buf o;
  buf_init(&o);
  buf_adds(&o, "  ");
  buf_adds(&o, BLOCK_ON);
  add_expanded(&o, s, col);
  buf_adds(&o, BLOCK_OFF);
  lines_push_f(out, buf_take(&o), LF_PRE);
}

static int indented_amount(const char *s) {
  if (!strncmp(s, "    ", 4))
    return 4;
  if (s[0] == '\t')
    return 1;
  return 0;
}

static void render_md(const char *src, Lines *out) {
  size_t len = strlen(src);
  size_t pos = 0;
  int in_code = 0;  /* inside a fenced block */
  int in_ind = 0;   /* inside an indented code block */
  int ind_pend = 0; /* blank line held back inside an indented block */
  int list_ctx = 0; /* the current top-level block is a list */
  int prev_blank = 1;

  while (pos < len) {
    size_t e = pos;
    while (e < len && src[e] != '\n')
      e++;

    char *line = subdup(src + pos, e - pos);
    if (*line && line[strlen(line) - 1] == '\r')
      line[strlen(line) - 1] = '\0';

    Buf o;
    buf_init(&o);

    if (in_code) {
      if (!strncmp(line, "```", 3) || !strncmp(line, "~~~", 3)) {
        in_code = 0;
        lines_push(out, xstrdup(""));
        prev_blank = 1;
      } else {
        push_code(out, line, 0);
        prev_blank = 0;
      }
      goto next;
    }

    if (in_ind) {
      if (is_blank(line)) { /* keep, but only if code follows */
        ind_pend = 1;
        goto next;
      }
      int ind = indented_amount(line);
      if (ind) {
        if (ind_pend) {
          lines_push_f(out, xstrdup(""), LF_PRE);
          ind_pend = 0;
        }
        push_code(out, line + ind, ind == 4 ? 4 : 8);
        prev_blank = 0;
        goto next;
      }
      in_ind = 0;
      if (ind_pend) {
        lines_push_f(out, xstrdup(""), LF_PRE);
        ind_pend = 0;
        prev_blank = 1;
      }
      if (!prev_blank)
        lines_push(out, xstrdup(""));
      prev_blank = 1;
    }

    if (!strncmp(line, "```", 3) || !strncmp(line, "~~~", 3)) {
      if (!prev_blank)
        lines_push(out, xstrdup(""));
      in_code = 1;
      list_ctx = 0;
      prev_blank = 1;
      goto next;
    }

    if (is_blank(line)) {
      if (!prev_blank) {
        lines_push(out, xstrdup(""));
        prev_blank = 1;
      }
      goto next;
    }

    {
      int ind = indented_amount(line);
      if (prev_blank && !list_ctx && ind) {
        push_code(out, line + ind, ind == 4 ? 4 : 8);
        in_ind = 1;
        prev_blank = 0;
        goto next;
      }
    }

    {
      int h = 0;
      while (h < 6 && line[h] == '#')
        h++;
      if (h > 0 && (line[h] == ' ' || line[h] == '\t' || line[h] == '\0')) {
        const char *txt = line + h;
        while (*txt == ' ' || *txt == '\t')
          txt++;
        if (!prev_blank)
          lines_push(out, xstrdup(""));
        buf_adds(&o, BOLD_ON);
        render_inline(txt, &o);
        buf_adds(&o, BOLD_OFF);
        lines_push(out, buf_take(&o));
        lines_push(out, xstrdup(""));
        list_ctx = 0;
        prev_blank = 1;
        goto next;
      }
    }

    if (is_hr(line)) {
      if (!prev_blank)
        lines_push(out, xstrdup(""));
      for (int i = 0; i < 32; i++)
        buf_adds(&o, "\u2500");
      lines_push(out, buf_take(&o));
      lines_push(out, xstrdup(""));
      list_ctx = 0;
      prev_blank = 1;
      goto next;
    }

    if (line[0] == '>') {
      const char *txt = line + 1;
      while (*txt == ' ')
        txt++;
      buf_adds(&o, "\u2502 ");
      render_inline(txt, &o);
      lines_push(out, buf_take(&o));
      list_ctx = 0;
      prev_blank = 0;
      goto next;
    }

    if ((line[0] == '-' || line[0] == '*' || line[0] == '+') &&
        (line[1] == ' ' || line[1] == '\t')) {
      const char *txt = line + 2;
      while (*txt == ' ' || *txt == '\t')
        txt++;
      buf_adds(&o, "\u2022 ");
      render_inline(txt, &o);
      lines_push(out, buf_take(&o));
      list_ctx = 1;
      prev_blank = 0;
      goto next;
    }

    {
      size_t k = 0;
      while (isdigit((unsigned char)line[k]))
        k++;
      if (k > 0 && line[k] == '.' &&
          (line[k + 1] == ' ' || line[k + 1] == '\t')) {
        const char *txt = line + k + 2;
        while (*txt == ' ' || *txt == '\t')
          txt++;
        buf_add(&o, line, k);
        buf_adds(&o, ". ");
        render_inline(txt, &o);
        lines_push(out, buf_take(&o));
        list_ctx = 1;
        prev_blank = 0;
        goto next;
      }
    }

    if (line[0] != ' ' && line[0] != '\t')
      list_ctx = 0;
    render_inline(line, &o);
    lines_push(out, buf_take(&o));
    prev_blank = 0;

  next:
    free(line);
    pos = e + 1;
  }
}

static int vis_len(const char *s) {
  int n = 0;
  for (size_t i = 0; s[i];) {
    if (s[i] == '\x1b' && s[i + 1] == '[') {
      i += 2;
      while (s[i] && !(s[i] >= '@' && s[i] <= '~'))
        i++;
      if (s[i])
        i++;
    } else {
      unsigned char c = (unsigned char)s[i++];
      if ((c & 0xC0) != 0x80)
        n++;
    }
  }
  return n;
}

typedef struct {
  const char *p;
  int n;
  int vis;
} Tok;

static void buf_add_lim(Buf *b, const char *s, int max) {
  if (max < 0)
    max = 0;
  int w = 0;
  for (size_t i = 0; s[i];) {
    if (s[i] == '\x1b' && s[i + 1] == '[') {
      size_t st = i;
      i += 2;
      while (s[i] && !(s[i] >= '@' && s[i] <= '~'))
        i++;
      if (s[i])
        i++;
      buf_add(b, s + st, i - st);
      continue;
    }
    size_t st = i;
    unsigned char c = (unsigned char)s[i++];
    if ((c & 0xC0) != 0x80)
      while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80)
        i++;
    if (w >= max)
      continue;
    buf_add(b, s + st, i - st);
    w++;
  }
}

static void compute_pad(const Lines *w, int cols, int *pad) {
  size_t i = 0;
  while (i < w->n) {
    size_t j = i;
    if (w->f[i] & LF_PRE) {
      int maxw = 0;
      while (j < w->n && (w->f[j] & LF_PRE)) {
        int vl = vis_len(w->v[j]);
        if (vl > maxw)
          maxw = vl;
        j++;
      }
      int p = (cols - maxw) / 2;
      if (p < 0)
        p = 0;
      for (size_t k = i; k < j; k++)
        pad[k] = p;
    } else {
      while (j < w->n && !(w->f[j] & LF_PRE)) {
        int p = (cols - vis_len(w->v[j])) / 2;
        if (p < 0)
          p = 0;
        pad[j] = p;
        j++;
      }
    }
    i = j;
  }
}

static Tok *tokenize(const char *s, int *ntok) {
  size_t len = strlen(s);
  Tok *t = malloc(sizeof(Tok) * (len + 1));
  if (!t) {
    perror("malloc");
    exit(1);
  }

  int n = 0;
  for (size_t i = 0; i < len;) {
    size_t start = i;
    if (s[i] == '\x1b' && s[i + 1] == '[') {
      i += 2;
      while (i < len && !(s[i] >= '@' && s[i] <= '~'))
        i++;
      if (i < len)
        i++;
    } else {
      unsigned char c = (unsigned char)s[i];
      i++;
      if ((c & 0xE0) == 0xC0) {
        while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80)
          i++;
      } else if ((c & 0xF0) == 0xE0) {
        while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80)
          i++;
      } else if ((c & 0xF8) == 0xF0) {
        while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80)
          i++;
      }
    }
    t[n].p = s + start;
    t[n].n = (int)(i - start);
    t[n].vis = (s[start] == '\x1b') ? 0 : 1;
    n++;
  }
  *ntok = n;
  return t;
}

static Lines wrap_text(const char *s, int width) {
  Lines out;
  memset(&out, 0, sizeof out);

  if (width < 1)
    width = 1;

  int ntok = 0;
  Tok *t = tokenize(s, &ntok);
  if (ntok == 0) {
    lines_push(&out, xstrdup(""));
    free(t);
    return out;
  }

  int i = 0;
  while (i < ntok) {
    Buf line;
    buf_init(&line);

    int w = 0, j = i, last_space = -1;

    while (j < ntok) {
      int vw = t[j].vis ? 1 : 0;
      if (t[j].vis && w + vw > width)
        break;
      if (t[j].vis && t[j].n == 1 && t[j].p[0] == ' ')
        last_space = j;
      buf_add(&line, t[j].p, (size_t)t[j].n);
      w += vw;
      j++;
    }

    if (j < ntok && last_space >= i) {
      line.len = 0;
      for (int k = i; k < last_space; k++)
        buf_add(&line, t[k].p, (size_t)t[k].n);
      j = last_space + 1;
      while (j < ntok && t[j].vis && t[j].n == 1 && t[j].p[0] == ' ')
        j++;
    }

    lines_push(&out, buf_take(&line));
    i = j;
  }

  free(t);
  return out;
}

static Lines wrap_all(Lines *src, int width) {
  Lines out;
  memset(&out, 0, sizeof out);

  for (size_t i = 0; i < src->n; i++) {
    if (src->f[i] & LF_PRE) { /* code: keep verbatim, never re-wrap */
      lines_push_f(&out, xstrdup(src->v[i]), LF_PRE);
      continue;
    }
    Lines w = wrap_text(src->v[i], width);
    for (size_t j = 0; j < w.n; j++)
      lines_push(&out, w.v[j]);
    free(w.v);
    free(w.f);
  }
  if (out.n == 0)
    lines_push(&out, xstrdup(""));
  return out;
}

#define KEY_UP 0x101
#define KEY_DOWN 0x102
#define KEY_PGUP 0x103
#define KEY_PGDN 0x104
#define KEY_HOME 0x105
#define KEY_END 0x106
#define KEY_RESIZE 0x107

static struct termios g_orig;
static int g_raw = 0;
static int g_alt = 0;
static int g_tty = -1;

static void cleanup_term(void) {
  if (g_alt) {
    ssize_t r = write(STDOUT_FILENO, "\x1b[?1049l", 8);
    (void)r;
    g_alt = 0;
  }
  {
    ssize_t r = write(STDOUT_FILENO, "\x1b[?25h", 6);
    (void)r;
  }
  if (g_raw && g_tty >= 0) {
    tcsetattr(g_tty, TCSAFLUSH, &g_orig);
    g_raw = 0;
  }
}

static void on_signal(int sig) {
  cleanup_term();
  _exit(128 + sig);
}

static void on_winch(int sig) { (void)sig; }

static void get_winsize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
    *rows = ws.ws_row;
    *cols = ws.ws_col;
  } else {
    *rows = 24;
    *cols = 80;
  }
}

static int enter_raw(int fd) {
  if (tcgetattr(fd, &g_orig) < 0)
    return -1;
  struct termios raw = g_orig;
  raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_oflag &= (tcflag_t) ~(OPOST);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSAFLUSH, &raw) < 0)
    return -1;
  g_raw = 1;
  return 0;
}

static int read_key(int fd) {
  unsigned char c;
  ssize_t r = read(fd, &c, 1);
  if (r < 0)
    return (errno == EINTR) ? KEY_RESIZE : -1;
  if (r == 0)
    return -1;
  if (c != 27)
    return c;

  fd_set fds;
  struct timeval tv;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);
  tv.tv_sec = 0;
  tv.tv_usec = 50000;

  if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0)
    return 27;

  unsigned char s1;
  if (read(fd, &s1, 1) != 1)
    return 27;

  if (s1 == '[' || s1 == 'O') {
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = 0;
    tv.tv_usec = 50000;
    if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0)
      return 27;

    unsigned char s2;
    if (read(fd, &s2, 1) != 1)
      return 27;

    switch (s2) {
    case 'A':
      return KEY_UP;
    case 'B':
      return KEY_DOWN;
    case 'C':
      return KEY_RESIZE;
    case 'D':
      return KEY_RESIZE;
    case 'H':
      return KEY_HOME;
    case 'F':
      return KEY_END;
    case '5': {
      unsigned char t;
      if (read(fd, &t, 1) < 0) {
      }
      return KEY_PGUP;
    }
    case '6': {
      unsigned char t;
      if (read(fd, &t, 1) < 0) {
      }
      return KEY_PGDN;
    }
    }
  }
  return 27;
}

static void draw(Lines *w, int top, int rows, int cols, const char *title) {
  Buf b;
  buf_init(&b);

  int view = rows - 1;
  if (view < 0)
    view = 0;

  int *pad = NULL;
  if (w->n) {
    pad = malloc(w->n * sizeof *pad);
    if (!pad) {
      perror("malloc");
      exit(1);
    }
    compute_pad(w, cols, pad);
  }

  buf_adds(&b, "\x1b[H");

  for (int r = 0; r < view; r++) {
    int idx = top + r;
    if (idx >= 0 && idx < (int)w->n) {
      int p = pad[idx];
      for (int i = 0; i < p; i++)
        buf_addc(&b, ' ');
      buf_add_lim(&b, w->v[idx], cols - p);
    }
    buf_adds(&b, "\x1b[K\r\n");
  }

  if (rows > 0) {
    int total = (int)w->n;
    int last = top + view;
    if (last > total)
      last = total;

    int pct = total ? (last * 100) / total : 100;
    if (pct > 100)
      pct = 100;

    char left[512], right[256];
    snprintf(left, sizeof left, " %s", title);
    snprintf(right, sizeof right,
             "%d%%  %d/%d  q quit  j/k scroll  space page ", pct, last, total);

    Buf sb;
    buf_init(&sb);
    buf_adds(&sb, left);
    int ll = (int)strlen(left), rl = (int)strlen(right);
    int gap = cols - ll - rl;
    for (int i = 0; i < gap; i++)
      buf_addc(&sb, ' ');
    buf_adds(&sb, right);
    if ((int)sb.len > cols)
      sb.len = (size_t)cols;

    buf_adds(&b, "\x1b[7m");
    buf_add(&b, sb.p, sb.len);
    for (size_t i = sb.len; i < (size_t)cols; i++)
      buf_addc(&b, ' ');
    buf_adds(&b, "\x1b[0m\x1b[K");

    free(sb.p);
  }

  ssize_t rr = write(STDOUT_FILENO, b.p, b.len);
  (void)rr;
  free(b.p);
  free(pad);
}

static char *read_all(FILE *f, size_t *out_len) {
  Buf b;
  buf_init(&b);
  char tmp[65536];
  size_t n;
  while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
    buf_add(&b, tmp, n);
  *out_len = b.len;
  return buf_take(&b);
}

static void strip_ansi(const char *s, Buf *out) {
  for (size_t i = 0; s[i];) {
    if (s[i] == '\x1b' && s[i + 1] == '[') {
      i += 2;
      while (s[i] && !(s[i] >= '@' && s[i] <= '~'))
        i++;
      if (s[i])
        i++;
    } else {
      buf_addc(out, s[i]);
      i++;
    }
  }
}

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "usage: %s [file]\n"
          "       ... | %s\n\n"
          "A terminal Markdown reader.  Keys:\n"
          "  j / down / enter   scroll down\n"
          "  k / up             scroll up\n"
          "  space / pgdn / f   page down\n"
          "  b / pgup           page up\n"
          "  g / home           go to top\n"
          "  G / end            go to bottom\n"
          "  q / esc            quit\n",
          prog, prog);
}

int main(int argc, char **argv) {
  const char *path = NULL;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage(argv[0]);
      return 0;
    } else if (argv[i][0] == '-' && argv[i][1]) {
      fprintf(stderr, "%s: unknown option '%s'\n", argv[0], argv[i]);
      usage(argv[0]);
      return 1;
    } else {
      path = argv[i];
    }
  }

  if (!path && isatty(STDIN_FILENO)) {
    usage(argv[0]);
    return 2;
  }

  FILE *fp = stdin;
  if (path) {
    fp = fopen(path, "rb");
    if (!fp) {
      perror(path);
      return 1;
    }
  }

  size_t src_len = 0;
  char *src = read_all(fp, &src_len);
  if (fp != stdin)
    fclose(fp);

  Lines md;
  memset(&md, 0, sizeof md);
  render_md(src, &md);
  free(src);

  const char *title = path ? base_name(path) : "stdin";

  if (!isatty(STDOUT_FILENO)) {
    Buf o;
    for (size_t i = 0; i < md.n; i++) {
      buf_init(&o);
      strip_ansi(md.v[i], &o);
      fwrite(o.p, 1, o.len, stdout);
      fputc('\n', stdout);
      free(o.p);
    }
    lines_free(&md);
    return 0;
  }

  int ttyfd;
  if (isatty(STDIN_FILENO)) {
    ttyfd = STDIN_FILENO;
  } else {
    ttyfd = open("/dev/tty", O_RDONLY);
    if (ttyfd < 0) {
      /* no keyboard available: dump and exit */
      Buf o;
      for (size_t i = 0; i < md.n; i++) {
        buf_init(&o);
        strip_ansi(md.v[i], &o);
        fwrite(o.p, 1, o.len, stdout);
        fputc('\n', stdout);
        free(o.p);
      }
      lines_free(&md);
      return 0;
    }
  }
  g_tty = ttyfd;

  if (enter_raw(ttyfd) < 0) {
    perror("tcsetattr");
    if (ttyfd != STDIN_FILENO)
      close(ttyfd);
    lines_free(&md);
    return 1;
  }

  atexit(cleanup_term);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGHUP, on_signal);

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_winch;
  sa.sa_flags = 0; /* no SA_RESTART: interrupt read() */
  sigaction(SIGWINCH, &sa, NULL);

  {
    ssize_t r = write(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l", 14);
    (void)r;
  }
  g_alt = 1;

  int rows, cols;
  get_winsize(&rows, &cols);

  Lines w = wrap_all(&md, cols);
  int cur_cols = cols;
  int top = 0;

  for (;;) {
    get_winsize(&rows, &cols);
    if (cols < 1)
      cols = 1;

    if (cols != cur_cols) {
      lines_free(&w);
      w = wrap_all(&md, cols);
      cur_cols = cols;
    }

    int view = rows - 1;
    if (view < 1)
      view = 1;

    int maxtop = (int)w.n - view;
    if (maxtop < 0)
      maxtop = 0;
    if (top > maxtop)
      top = maxtop;
    if (top < 0)
      top = 0;

    draw(&w, top, rows, cols, title);

    int k = read_key(ttyfd);

    if (k == -1)
      break;

    switch (k) {
    case 'q':
    case 'Q':
    case 3:
      goto done;
    case 27:
      goto done;
    case 'j':
    case '\n':
    case '\r':
      top++;
      break;
    case 'k':
      top--;
      break;
    case ' ':
    case 'f':
    case KEY_PGDN:
      top += view;
      break;
    case 'b':
    case KEY_PGUP:
      top -= view;
      break;
    case KEY_DOWN:
      top++;
      break;
    case KEY_UP:
      top--;
      break;
    case 'g':
    case KEY_HOME:
      top = 0;
      break;
    case 'G':
    case KEY_END:
      top = maxtop;
      break;
    default:
      break;
    }
  }

done:
  lines_free(&w);
  lines_free(&md);
  cleanup_term();
  if (ttyfd != STDIN_FILENO)
    close(ttyfd);
  return 0;
}
