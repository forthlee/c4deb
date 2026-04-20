// c4deb.c - c4 with interactive debugger
// Based on c4.c by Robert Swierczek
// Debugger features added on top

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#define int int64_t

char *p, *lp, // current position in source code
     *data,   // data/bss pointer
     *data_org; // start of data segment

int *e, *le,  // current position in emitted code
    *e_base,  // start of code segment
    *id,      // currently parsed identifier
    *sym,     // symbol table (simple list of identifiers)
    tk,       // current token
    ival,     // current token value
    ty,       // current expression type
    loc,      // local variable offset
    line,     // current line number
    src,      // print source and assembly flag
    debug,    // print executed instructions (non-interactive)
    trace;    // interactive trace/debug mode
char *op_code;
char _val[255], _cls[16], _tk[16], _ty[16]; 

// tokens and classes (operators last and in precedence order)
enum {
  Num = 128, Fun, Sys, Glo, Loc, Id,
  Char, Else, Enum, If, Int, Return, Sizeof, While,
  Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak
};

// opcodes
enum { LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
       OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT };

// types
enum { CHAR, INT, PTR };

// identifier offsets (since we can't create an ident struct)
enum { Tk, Hash, Name, Class, Type, Val, HClass, HType, HVal, Idsz };

// ---- Debug info structures ----

#define MAX_LNTAB   65536
#define MAX_FNTAB   4096
#define MAX_SRCLINES 65536
#define MAX_WATCHES  64
#define MAX_BRKPTS   64

// line number table: maps pc address -> source line
int  *lntab_pc[MAX_LNTAB];
int   lntab_ln[MAX_LNTAB];
int   lntab_cnt = 0;

// function table: maps entry pc -> id pointer
int  *fntab_pc[MAX_FNTAB];
int  *fntab_id[MAX_FNTAB];
int   fntab_cnt = 0;

// source line index
char *src_lines[MAX_SRCLINES];
int   src_line_cnt = 0;

// local variable debug info (persists after sym table unwind)
#define MAX_DBG_LOCALS 4096
typedef struct {
    char name[32];      // variable name (null-terminated)
    int  fn_entry_pc;   // function entry PC (cast of int*)
    int  frame_off;     // LEA offset = loc - id[Val]  (pos=param, neg=local)
    int  var_type;      // CHAR=0 INT=1 PTR=2+
} DbgLocal;
DbgLocal dbg_locals[MAX_DBG_LOCALS];
int dbg_local_cnt = 0;

// watch list
typedef struct {
    char name[64];
    int *sym_entry;     // non-NULL: global var sym entry
    int  is_local;      // 1: local var watch
    int  frame_off;     // local: LEA offset from bp
    int  var_type;      // local: CHAR/INT/PTR
    int  fn_entry_pc;   // local: owning function entry pc (scope check)
    int  is_indexed;    // 1: watch *(base+index), display as value or string
    int  index;         // index n in name[n]
} Watch;
Watch watchlist[MAX_WATCHES];
int   watch_cnt = 0;

// breakpoints (source line numbers)
int brkpt_lines[MAX_BRKPTS];
int brkpt_cnt = 0;

// function call breakpoints: pause after the named function returns
int *fn_brkpt_pcs[MAX_BRKPTS];  // entry PC of each watched function
char fn_brkpt_names[MAX_BRKPTS][64];
int  fn_brkpt_cnt = 0;

// debugger state
int deb_last_line = -1;
int deb_stepping  = 1;  // 1 = step mode, 0 = continue
int deb_stepover  = 0;  // step-over depth counter

// function-call breakpoint tracking (separate from step-over)
int deb_fn_active = 0;  // waiting for a fn_brkpt call to return
int deb_fn_depth  = 0;  // nested call depth inside that call

// original source buffer pointer (for src_lines)
char *src_buf = NULL;

// stack bounds for safe bp-chain walking
int *stk_lo = NULL;  // lowest valid stack address (malloc base)
int *stk_hi = NULL;  // highest valid stack address (sp initial top)

// heap tracking: record MALC allocations so ptr_in_safe_range can recognise them
#define MAX_HEAP_REGIONS 1024
typedef struct { char *base; int64_t size; } HeapRegion;
static HeapRegion heap_regions[MAX_HEAP_REGIONS];
static int        heap_region_cnt = 0;

static void heap_track_add(char *base, int64_t size) {
    if (heap_region_cnt < MAX_HEAP_REGIONS) {
        heap_regions[heap_region_cnt].base = base;
        heap_regions[heap_region_cnt].size = size;
        heap_region_cnt++;
    }
}
static void heap_track_remove(char *base) {
    int i;
    for (i = 0; i < heap_region_cnt; i++) {
        if (heap_regions[i].base == base) {
            heap_regions[i] = heap_regions[--heap_region_cnt];
            return;
        }
    }
}

// Write at most maxlen escaped chars of s into out[]; return pointer past last byte.
static char *escape_to_buf(char *out, const char *s, int32_t maxlen)
{
    int32_t i;
    for (i = 0; i < maxlen && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '\n') { *out++ = '\\'; *out++ = 'n';  }
        else if (c == '\t') { *out++ = '\\'; *out++ = 't';  }
        else if (c == '\r') { *out++ = '\\'; *out++ = 'r';  }
        else if (c == '\\') { *out++ = '\\'; *out++ = '\\'; }
        else if (c == '"')  { *out++ = '\\'; *out++ = '"';  }
        else                  *out++ = c;
    }
    return out;
}

char* fmt_ty(int ty) {
  sprintf(_ty, "%s%.*s",
            (ty % PTR) == CHAR ? "char" : "int",
            (int32_t)((ty - (ty % PTR))/PTR), "***");
  return _ty;
}

char* fmt_tk(int64_t tk_v)
{
    if      (tk_v == Num) strcpy(_tk, "Num");
    else if (tk_v == Fun) strcpy(_tk, "Fun");
    else if (tk_v == Sys) strcpy(_tk, "Sys");
    else if (tk_v == Glo) strcpy(_tk, "Glo");
    else if (tk_v == Loc) strcpy(_tk, "Loc");
    else if (tk_v == Id)  strcpy(_tk, "Id");
    else sprintf(_tk, "%lld", tk_v);
    return _tk;
}

char* fmt_cls(int64_t cls_v)
{
    if      (cls_v == Num) strcpy(_cls, "Num");
    else if (cls_v == Fun) strcpy(_cls, "Fun");
    else if (cls_v == Sys) strcpy(_cls, "Sys");
    else if (cls_v == Glo) strcpy(_cls, "Glo");
    else if (cls_v == Loc) strcpy(_cls, "Loc");
    else sprintf(_cls, "%lld", cls_v);
    return _cls;
}

// Check if a char* value is safe to dereference as a string.
// Accept: data segment or VM stack (covers string literals and char arrays).
static int ptr_in_safe_range(char *p) {
    if (data_org && data && p >= data_org && p < data) return 1;
    if (stk_lo && stk_hi && p >= (char *)stk_lo && p < (char *)stk_hi) return 1;
    // also accept heap regions allocated by the interpreted program via MALC
    int i;
    for (i = 0; i < heap_region_cnt; i++) {
        if (p >= heap_regions[i].base &&
            p <  heap_regions[i].base + heap_regions[i].size) return 1;
    }
    return 0;
}

char* fmt_val(int64_t gval, int ty_v) {
    if (ty_v == CHAR + PTR) {
        if (!gval) {
            sprintf(_val, "null");
        } else if (ptr_in_safe_range((char *)gval)) {
            char *out = _val;
            *out++ = '"';
            out = escape_to_buf(out, (char *)gval, 64);
            *out++ = '"';
            *out   = '\0';
        } else if (31 < gval && gval < 127) {
            sprintf(_val, "%lld'%c'", gval, (char)gval); 
        } else {
            sprintf(_val, "%lld", gval);
        }
    } else if (31 < gval && gval < 127) {
        sprintf(_val, "%lld'%c'", gval, (char)gval);  
    } else {
        sprintf(_val, "%lld", gval);
    }
    return _val;
}

// ---- lntab / fntab helpers ----

void lntab_add(int *pc_addr, int ln)
{
    if (lntab_cnt < MAX_LNTAB) {
        lntab_pc[lntab_cnt] = pc_addr;
        lntab_ln[lntab_cnt] = ln;
        lntab_cnt++;
    }
}

int lntab_lookup(int *pc_addr)
{
    int best = -1, i;
    for (i = 0; i < lntab_cnt; i++) {
        if (lntab_pc[i] <= pc_addr) best = i;
    }
    if (best < 0) return -1;
    return lntab_ln[best];
}

void fntab_add(int *pc_addr, int *id_ptr)
{
    if (fntab_cnt < MAX_FNTAB) {
        fntab_pc[fntab_cnt] = pc_addr;
        fntab_id[fntab_cnt] = id_ptr;
        fntab_cnt++;
    }
}

static int fntab_best_idx(int *pc_addr)
{
    int best = -1, i;
    for (i = 0; i < fntab_cnt; i++)
        if (fntab_pc[i] <= pc_addr) best = i;
    return best;
}

int *fntab_lookup_id(int *pc_addr) { int b = fntab_best_idx(pc_addr); return b < 0 ? NULL : fntab_id[b]; }
int *fntab_lookup_pc(int *pc_addr) { int b = fntab_best_idx(pc_addr); return b < 0 ? NULL : fntab_pc[b]; }

// ---- dbg_locals helpers ----

void dbg_local_add(int fn_entry_pc, int *id_entry, int frame_off)
{
    if (dbg_local_cnt >= MAX_DBG_LOCALS) return;
    DbgLocal *dl = &dbg_locals[dbg_local_cnt++];
    char *nm = (char *)id_entry[Name];
    int len = id_entry[Hash] & 0x3F;
    if (len > 31) len = 31;
    memcpy(dl->name, nm, len);
    dl->name[len] = 0;
    dl->fn_entry_pc  = fn_entry_pc;
    dl->frame_off    = frame_off;
    dl->var_type     = id_entry[Type]; // CHAR=0 INT=1 PTR>=2
}

DbgLocal *find_dbg_local(char *name, int fn_entry_pc)
{
    int i;
    for (i = 0; i < dbg_local_cnt; i++) {
        if (dbg_locals[i].fn_entry_pc == fn_entry_pc &&
            !strcmp(dbg_locals[i].name, name))
            return &dbg_locals[i];
    }
    return NULL;
}

// ---- source lines index ----

void build_src_lines(char *buf)
{
    char *q = buf;
    src_line_cnt = 0;
    src_lines[src_line_cnt++] = q; // line 1
    while (*q && src_line_cnt < MAX_SRCLINES) {
        if (*q == '\n' && *(q+1)) {
            src_lines[src_line_cnt++] = q + 1;
        }
        q++;
    }
}

static char *src_line_end(char *s) { while (*s && *s != '\n') s++; return s; }

void show_src_context(int cur_line, int ctx)
{
    int start = cur_line - ctx;
    int end   = cur_line + ctx;
    int i;
    if (start < 1) start = 1;
    if (end > src_line_cnt) end = src_line_cnt;
    for (i = start; i <= end; i++) {
        char *ls  = src_lines[i-1];
        char *le2 = src_line_end(ls);
        if (i == cur_line)
            printf("  >>%4lld:  %.*s\n", (int)i, (int32_t)(le2 - ls), ls);
        else
            printf("    %4lld:  %.*s\n", (int)i, (int32_t)(le2 - ls), ls);
    }
}

void show_src_line(int ln)
{
    if (ln < 1 || ln > src_line_cnt) return;
    char *ls  = src_lines[ln-1];
    char *le2 = src_line_end(ls);
    printf("  %lld:  %.*s\n", (int)ln, (int32_t)(le2 - ls), ls);
}

// ---- frame stack builder ----

// Safe bp-chain predicate: walk is a valid frame pointer only when it
// points inside the stack arena AND is strictly higher than cur (grows down).
#define VALID_BP(walk, cur) \
    ((walk) > (cur) && (walk) >= stk_lo && (walk) < stk_hi)

#define MAX_FRAMES 64

// Populate out_bp[]/out_fn[] for all live call frames visible from cur_pc.
// cur_ins: the opcode being dispatched (ENT, LEV, or other).
// Returns the number of frames populated.
static int build_frame_stack(int *cur_pc, int cur_ins, int *bp, int *sp,
                              int **out_bp, int *out_fn)
{
    int cnt = 0;
    int *walk, *wret, *next, *fn;

    if (cur_ins == ENT) {
        // JSR already pushed ret-addr; bp is still the caller's bp.
        fn = fntab_lookup_pc((int *)sp[0]);
        out_bp[cnt] = bp;
        out_fn[cnt] = fn ? (int)fn : 0;
        cnt++;
        walk = VALID_BP((int *)bp[0], bp) ? (int *)bp[0] : NULL;
        wret = (int *)bp[1];
    } else {
        fn = fntab_lookup_pc(cur_pc);
        out_bp[cnt] = bp;
        out_fn[cnt] = fn ? (int)fn : 0;
        cnt++;
        if (cur_ins == LEV) {
            // Current frame still alive; also include the caller frame.
            int *caller_bp = VALID_BP((int *)bp[0], bp) ? (int *)bp[0] : NULL;
            if (caller_bp) {
                fn = fntab_lookup_pc((int *)bp[1]);
                out_bp[cnt] = caller_bp;
                out_fn[cnt] = fn ? (int)fn : 0;
                cnt++;
                bp = caller_bp;
            }
        }
        walk = VALID_BP((int *)bp[0], bp) ? (int *)bp[0] : NULL;
        wret = (int *)bp[1];
    }

    while (walk && cnt < MAX_FRAMES) {
        fn = fntab_lookup_pc(wret);
        out_bp[cnt] = walk;
        out_fn[cnt] = fn ? (int)fn : 0;
        cnt++;
        wret = (int *)walk[1];
        next = (int *)walk[0];
        walk = VALID_BP(next, walk) ? next : NULL;
    }
    return cnt;
}

// ---- watch helpers ----

// Print a watch value; if is_indexed, dereference base as int* and show elem as value/string.
#define PRINT_WATCH_VAL(w, val, ty) do { \
    if ((w)->is_indexed) { \
        int64_t *_ptr = (int64_t *)(int64_t)(val); \
        int64_t _elem = _ptr ? _ptr[(w)->index] : 0; \
        printf(" %s[%lld]=%s ", (w)->name, (int64_t)(w)->index, \
               _ptr ? fmt_val(_elem, CHAR+PTR) : "null"); \
    } else { \
        printf(" %s=%s ", (w)->name, fmt_val((val), (ty))); \
    } \
} while(0)

int *find_sym_by_name(char *name)
{
    int len = strlen(name);
    int *sid = sym;
    while (sid[Tk]) {
        if (sid[Name]) {
            char *sname = (char *)sid[Name];
            // check length from hash low 6 bits
            int slen = sid[Hash] & 0x3F;
            if (slen == len && !memcmp(sname, name, len)) {
                return sid;
            }
        }
        sid = sid + Idsz;
    }
    return NULL;
}

void show_watches(int *bp, int *sp, int *cur_pc)
{
    int i;
    if (watch_cnt == 0) return;

    int  frame_fn[MAX_FRAMES];
    int *frame_bp[MAX_FRAMES];
    int  frame_cnt = build_frame_stack(cur_pc, *cur_pc, bp, sp,
                                       frame_bp, frame_fn);

    printf("  WCH:");
    for (i = 0; i < watch_cnt; i++) {
        Watch *w = &watchlist[i];
        if (w->is_local) {
            // Find home frame (the frame whose fn_entry == w->fn_entry_pc).
            // Then check any INNER frame (j < home_j) for a local (frame_off<0)
            // with the same name — that local shadows the home variable.
            int home_j = -1, j;
            for (j = 0; j < frame_cnt; j++) {
                if (frame_fn[j] == w->fn_entry_pc) { home_j = j; break; }
            }
            if (home_j < 0) {
                printf(" %s=<scope?> ", w->name);  // home function not on stack
            } else {
                int found = 0;
                // Check inner frames for shadowing (local or param both shadow)
                for (j = 0; j < home_j; j++) {
                    if (!frame_bp[j]) continue;
                    DbgLocal *dl = find_dbg_local(w->name, frame_fn[j]);
                    if (dl) {
                        int *addr = frame_bp[j] + dl->frame_off;
                        int val = (dl->var_type == CHAR) ? (int)*(char *)addr : *(int *)addr;
                        PRINT_WATCH_VAL(w, val, dl->var_type);
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    // Use home frame variable
                    DbgLocal *dl = find_dbg_local(w->name, w->fn_entry_pc);
                    if (dl && frame_bp[home_j]) {
                        int *addr = frame_bp[home_j] + dl->frame_off;
                        int val = (dl->var_type == CHAR) ? (int)*(char *)addr : *(int *)addr;
                        PRINT_WATCH_VAL(w, val, dl->var_type);
                    } else {
                        printf(" %s=? ", w->name);
                    }
                }
            }
        } else if (w->sym_entry && w->sym_entry[Class] == Glo) {
            // Check if any local/param shadows the global in any current frame
            int is_loc = 0, j;
            for (j = 0; j < frame_cnt; j++) {
                if (!frame_bp[j]) continue;
                DbgLocal *dl = find_dbg_local(w->name, frame_fn[j]);
                if (dl) {
                    int *addr = frame_bp[j] + dl->frame_off;
                    int val = *addr;
                    PRINT_WATCH_VAL(w, val, dl->var_type);
                    is_loc = 1;
                    break;
                }
            }
            if (!is_loc) {
                int val = *(int *)w->sym_entry[Val];
                PRINT_WATCH_VAL(w, val, w->sym_entry[Type]);
            }
        } else {
            printf(" %s=?", w->name);
        }
    }
    printf("\n");
}

// ---- breakpoint helpers ----

int is_breakpoint(int ln)
{
    int i;
    for (i = 0; i < brkpt_cnt; i++)
        if (brkpt_lines[i] == ln) return 1;
    return 0;
}

// ---- print string with escape sequences instead of raw control chars ----
void print_escaped(const char *s, int32_t len)
{
    int32_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '\n') printf("\\n");
        else if (c == '\t') printf("\\t");
        else if (c == '\r') printf("\\r");
        else if (c == '\\') printf("\\\\");
        else if (c == '"')  printf("\\\"");
        else if (c < 32 || c == 127) printf("\\x%02x", c);
        else putchar(c);
    }
}

// ---- interactive debug prompt ----

void debug_prompt(int *pc, int *sp, int *bp, int a, int cur_ins, int cycle)
{
    char buf[256];
    char qbuf[32][256]; // command queue for comma-separated input
    int  qhead = 0, qtail = 0;
    int handled = 0;

    while (!handled) {
        if (qhead < qtail) {
            // pop next queued sub-command
            strncpy(buf, qbuf[qhead % 32], 255); buf[255] = 0;
            qhead++;
        } else {
            printf("(deb) > ");
            fflush(stdout);
            if (!fgets(buf, sizeof(buf), stdin)) {
                // EOF
                deb_stepping = 0;
                trace = 0;
                handled = 1;
                break;
            }
            // trim newline
            int blen = strlen(buf);
            if (blen > 0 && buf[blen-1] == '\n') buf[--blen] = 0;

            // split by ',' into queue; first token stays in buf
            char *rest = buf;
            char *comma = strchr(rest, ',');
            if (comma) {
                *comma = 0;
                rest = comma + 1;
                while (rest && *rest && qtail - qhead < 32) {
                    while (*rest == ' ') rest++;          // trim leading space
                    char *nc = strchr(rest, ',');
                    if (nc) *nc = 0;
                    strncpy(qbuf[qtail % 32], rest, 255);
                    qbuf[qtail % 32][255] = 0;
                    qtail++;
                    rest = nc ? nc + 1 : NULL;
                }
            }
            // trim trailing spaces from first token (buf)
            blen = strlen(buf);
            while (blen > 0 && buf[blen-1] == ' ') buf[--blen] = 0;
        }

        if (buf[0] == 0 || (buf[0] == 's' && buf[1] == 0)) {
            // step
            deb_stepping = 1;
            deb_stepover = 0;
            handled = 1;
        }
        else if (buf[0] == 'n' && buf[1] == 0) {
            // step over: run until call depth is back
            deb_stepping = 0;
            deb_stepover = 1; // signal to VM loop
            handled = 1;
        }
        else if (buf[0] == 'n' && buf[1] == ' ') {
            // n <func>: break after func() returns
            char *fname = buf + 2;
            int flen = strlen(fname);
            int found = 0, fi;
            for (fi = 0; fi < fntab_cnt; fi++) {
                int *fid = fntab_id[fi];
                if (!fid) continue;
                int nlen = fid[Hash] & 0x3F;
                if (nlen == flen && !memcmp((char *)fid[Name], fname, flen)) {
                    if (fn_brkpt_cnt < MAX_BRKPTS) {
                        fn_brkpt_pcs[fn_brkpt_cnt] = fntab_pc[fi];
                        strncpy(fn_brkpt_names[fn_brkpt_cnt], fname, 63);
                        fn_brkpt_names[fn_brkpt_cnt][63] = 0;
                        fn_brkpt_cnt++;
                        printf("  Function breakpoint %lld set on %s()\n",
                               (int)(fn_brkpt_cnt - 1), fname);
                    } else {
                        printf("  Function breakpoint table full\n");
                    }
                    found = 1;
                    break;
                }
            }
            if (!found) printf("  Function '%s' not found\n", fname);
        }
        else if (buf[0] == 'n' && buf[1] == 'f' && buf[2] == 'l' && buf[3] == 0) {
            int fi;
            printf("  Function breakpoints:\n");
            for (fi = 0; fi < fn_brkpt_cnt; fi++)
                printf("    [%lld] %s()\n", (int)fi, fn_brkpt_names[fi]);
        }
        else if (buf[0] == 'n' && buf[1] == 'f' && buf[2] == 'd' && buf[3] == ' ') {
            int idx = atoi(buf + 4);
            if (idx >= 0 && idx < fn_brkpt_cnt) {
                int fi;
                for (fi = idx; fi < fn_brkpt_cnt - 1; fi++) {
                    fn_brkpt_pcs[fi] = fn_brkpt_pcs[fi + 1];
                    memcpy(fn_brkpt_names[fi], fn_brkpt_names[fi + 1], 64);
                }
                fn_brkpt_cnt--;
                printf("  Function breakpoint %lld deleted\n", (int)idx);
            } else {
                printf("  Invalid function breakpoint index\n");
            }
        }
        else if (buf[0] == 'c' && buf[1] == 0) {
            // continue
            deb_stepping = 0;
            deb_stepover = 0;
            handled = 1;
        }
        else if (buf[0] == 'b' && buf[1] == ' ') {
            int ln = atoi(buf + 2);
            if (brkpt_cnt < MAX_BRKPTS) {
                brkpt_lines[brkpt_cnt++] = ln;
                printf("  Breakpoint %lld set at line %lld\n", (int)(brkpt_cnt-1), (int)ln);
            } else {
                printf("  Breakpoint table full\n");
            }
        }
        else if (buf[0] == 'b' && buf[1] == 'd' && buf[2] == ' ') {
            int idx = atoi(buf + 3);
            if (idx >= 0 && idx < brkpt_cnt) {
                int i;
                for (i = idx; i < brkpt_cnt - 1; i++) brkpt_lines[i] = brkpt_lines[i+1];
                brkpt_cnt--;
                printf("  Breakpoint %lld deleted\n", (int)idx);
            } else {
                printf("  Invalid breakpoint index\n");
            }
        }
        else if (buf[0] == 'b' && buf[1] == 'l' && buf[2] == 0) {
            int i;
            printf("  Breakpoints:\n");
            for (i = 0; i < brkpt_cnt; i++)
                printf("    [%lld] line %lld\n", (int)i, (int)brkpt_lines[i]);
        }
        else if (buf[0] == 'w' && buf[1] == ' ') {
            char *wname = buf + 2;
            if (watch_cnt >= MAX_WATCHES) { printf("  Watch table full\n"); }
            else {
                // Parse optional [n] index: "name[n]" -> base name + index
                char base_name[64];
                int  w_is_indexed = 0, w_index = 0;
                char *bracket = strchr(wname, '[');
                if (bracket) {
                    int blen = (int)(bracket - wname);
                    if (blen > 63) blen = 63;
                    memcpy(base_name, wname, blen); base_name[blen] = 0;
                    w_index = atoi(bracket + 1);
                    w_is_indexed = 1;
                    wname = base_name;
                }
                Watch *w = &watchlist[watch_cnt];
                strncpy(w->name, wname, 63); w->name[63] = 0;
                w->sym_entry = NULL; w->is_local = 0; w->fn_entry_pc = 0;
                w->is_indexed = w_is_indexed; w->index = w_index;
                // Priority: current-scope local > global > cross-function local
                int *fn_pc = fntab_lookup_pc(pc - 1);
                int fn_entry = fn_pc ? (int)fn_pc : 0;
                DbgLocal *dl = find_dbg_local(wname, fn_entry);
                if (dl) {
                    // Found in current scope as local
                    w->is_local    = 1;
                    w->frame_off   = dl->frame_off;
                    w->var_type    = dl->var_type;
                    w->fn_entry_pc = fn_entry;
                    watch_cnt++;
                    printf("  Watch added %s\n", wname);
                } else {
                    // Try global before cross-function locals
                    int *se = find_sym_by_name(wname);
                    if (se && se[Class] == Glo) {
                        w->sym_entry = se;
                        watch_cnt++;
                        printf("  Watch added %s\n", wname);
                    } else {
                        // Last resort: search all functions for this local name
                        int ki;
                        for (ki = 0; ki < dbg_local_cnt; ki++) {
                            if (!strcmp(dbg_locals[ki].name, wname)) {
                                dl = &dbg_locals[ki];
                                fn_entry = dbg_locals[ki].fn_entry_pc;
                                break;
                            }
                        }
                        if (dl) {
                            w->is_local    = 1;
                            w->frame_off   = dl->frame_off;
                            w->var_type    = dl->var_type;
                            w->fn_entry_pc = fn_entry;
                            watch_cnt++;
                            printf("  Watch added %s\n", wname);
                        } else {
                            printf("  Symbol '%s' not found (local or global)\n", wname);
                        }
                    }
                }
            }
        }
        else if (buf[0] == 'w' && buf[1] == 'd' && buf[2] == ' ') {
            int idx = atoi(buf + 3);
            if (idx >= 0 && idx < watch_cnt) {
                int i;
                for (i = idx; i < watch_cnt - 1; i++) watchlist[i] = watchlist[i+1];
                watch_cnt--;
                printf("  Watch %lld deleted\n", (int)idx);
            } else {
                printf("  Invalid watch index\n");
            }
        }
        else if (buf[0] == 'w' && buf[1] == 'l' && buf[2] == 0) {
            int i;
            printf("  Watches:\n");
            for (i = 0; i < watch_cnt; i++) {
                if (watchlist[i].is_indexed)
                    printf("    [%lld] %s[%lld]\n", (int)i, watchlist[i].name, (int64_t)watchlist[i].index);
                else
                    printf("    [%lld] %s\n", (int)i, watchlist[i].name);
            }
        }
        else if (buf[0] == 'p' && buf[1] == ' ') {
            char *pname = buf + 2;
            // Try local first
            int *fn_pc2 = fntab_lookup_pc(pc - 1);
            int fn_entry2 = fn_pc2 ? (int)fn_pc2 : 0;
            DbgLocal *dl2 = find_dbg_local(pname, fn_entry2);
            int *se = find_sym_by_name(pname);
            // Helper: print sym table line for se
            #define PRN_SYM(s) do { \
                printf("  %s : Tk=%s Class=%s Type=%s Val=%lld\n", \
                    pname, \
                    fmt_tk((s)[Tk]), fmt_cls((s)[Class]), fmt_ty((s)[Type]), \
                    (int64_t)(s)[Val]); } while(0)

            if (dl2) {
                if (se) PRN_SYM(se);
                int *addr = bp + dl2->frame_off;
                int val = (dl2->var_type == CHAR) ? (int)*(char *)addr : *(int *)addr;
                printf("   -> %s  (local type=%s val=bp[%lld])\n",
                    fmt_val(val, dl2->var_type),
                    fmt_ty(dl2->var_type),
                    (int)dl2->frame_off);
            } else if (se && se[Class] == Glo) {
                PRN_SYM(se);
                int val = *(int *)se[Val];
                printf("   -> %s  (global type=%s val=[%lld])\n",
                    fmt_val(val, se[Type]),
                    fmt_ty(se[Type]),
                    (int64_t)se[Val]);
            } else if (se && se[Class] == Num) {
                printf("  %s = %lld  [enum/const]\n", pname, (int)se[Val]);
            } else if (se) {
                PRN_SYM(se);
            } else {
                printf("  Symbol '%s' not found\n", pname);
            }
            #undef PRN_SYM
        }
        else if (buf[0] == 'r' && buf[1] == 0) {
            printf("  REG: a=%lld  sp=%lld  bp=%lld  pc=%lld\n",
                   a, (int)sp, (int)bp, (int)pc);
            printf("  Stack:\n");
            int i;
            for (i = 0; i < 8; i++) {
                printf("    %lld:[%2lld]%c= %lld\n",
                       (int)(sp + i), (int)(sp -bp + i), (bp && sp + i == bp) ? 'B' : ' ', sp[i]);
            }
        }
        else if (!strncmp(buf, "src", 3)) {
            int ctx = 5;
            if (buf[3] == ' ') ctx = atoi(buf + 4);
            int cur_ln = lntab_lookup(pc - 1);
            if (cur_ln < 0) cur_ln = 1;
            show_src_context(cur_ln, ctx);
        }
        else if (buf[0] == 'i' && buf[1] == ' ') {
            int64_t nnn = (int64_t)strtoll(buf + 2, NULL, 0);
            int *ptr = (int *)nnn;
            int matched = 0;

            // --- sym range ---
            // Find sym table end
            int *sym_end = sym;
            while (sym_end[Tk]) sym_end += Idsz;
            sym_end += Idsz;

            if (!matched && ptr >= sym && ptr < sym_end) {
                matched = 1;
                // Find which entry contains this address
                int *sid = sym;
                int found = 0;
                while (sid[Tk]) {
                    if (ptr >= sid && ptr < sid + Idsz) {
                        int slen = sid[Hash] & 0x3F;
                        char sname[64] = "?";
                        if (sid[Name]) {
                            if (slen > 63) slen = 63;
                            memcpy(sname, (char *)sid[Name], slen);
                            sname[slen] = 0;
                        }
                        printf("  SYM[%lld]: Tk=%s Name=\"%s\" Class=%s Type=%s Val=%lld\n",
                            (int64_t)(sid - sym) / Idsz,
                            fmt_tk(sid[Tk]),
                            sname,
                            fmt_cls(sid[Class]),
                            fmt_ty(sid[Type]),
                            (int64_t)sid[Val]);
                        found = 1;
                        break;
                    }
                    sid += Idsz;
                }
                if (!found) printf("  addr %lld is in sym range but no matching entry\n", nnn);
            }

            // --- code (e) range ---
            if (!matched && ptr >= e_base && ptr <= e) {
                matched = 1;
                int64_t op = *ptr;
                if (op >= LEA && op <= EXIT) {
                    printf("  CODE[%lld]: %.4s", nnn, &op_code[op * 5]);
                    if (op <= ADJ && ptr + 1 <= e) printf(" %lld", (int64_t)ptr[1]);
                    printf("\n");
                } else {
                    printf("  CODE[%lld]: val=%lld (not a valid opcode)\n", nnn, op);
                }
            }

            // --- data range ---
            if (!matched && (char *)nnn >= data_org && (char *)nnn < data) {
                matched = 1;
                // Look for a sym entry whose Val matches this address
                int *sid = sym;
                int found = 0;
                while (sid[Tk]) {
                    if (sid[Class] == Glo && sid[Val] == nnn) {
                        int64_t ty_v = sid[Type];
                        int slen = sid[Hash] & 0x3F;
                        char sname[64] = "?";
                        if (sid[Name]) {
                            if (slen > 63) slen = 63;
                            memcpy(sname, (char *)sid[Name], slen);
                            sname[slen] = 0;
                        }
                        int64_t cls_v = sid[Class];
                        printf("  SYM: Tk=%lld Name=\"%s\" Class=%s Type=%s Val=%lld\n",
                            (int64_t)sid[Tk],
                            sname,
                            fmt_cls(cls_v),
                            fmt_ty(ty_v),
                            (int64_t)sid[Val]);
                        // For Glo: also show current value stored at Val
                        if (cls_v == Glo) {
                            int64_t gval = *(int64_t *)sid[Val];
                            if (ty_v == CHAR + PTR) {
                                // pointer — show numeric and string content if non-null
                                printf("       [[Val]]=[%lld]", gval);
                                if (gval) printf("=%s\n", fmt_val(gval, ty_v));
                            } else printf("       [Val]=%lld\n", gval);
                        }
                        found = 1;
                        break;
                    }
                    sid += Idsz;
                }
                if (!found) {
                    // Unknown — show raw int and also try as string
                    char *s = (char *)nnn;
                    int32_t plen = 0;
                    while (s[plen] && plen < 32) plen++;
                    printf("  DATA[%lld]: %lld or \"", nnn, *(int64_t *)nnn);
                    print_escaped(s, plen);
                    printf("\"\n");
                }
            }

            // --- stack range ---
            if (!matched && ptr >= stk_lo && ptr < stk_hi) {
                matched = 1;
                printf("  STK[%lld]: %lld  (offset from sp: %lld  from bp: %lld)\n",
                    nnn, (int64_t)*ptr,
                    (int64_t)(ptr - sp), (int64_t)(ptr - bp));
            }

            // --- src range: treat nnn as line number ---
            if (!matched) {
                int64_t ln = nnn;
                if (ln >= 1 && ln <= src_line_cnt) {
                    matched = 1;
                    char *ls  = src_lines[ln - 1];
                    char *le2 = src_line_end(ls);
                    printf("  SRC[%lld]:  ", ln);
                    print_escaped(ls, (int32_t)(le2 - ls));
                    printf("\n");
                }
            }

            if (!matched) {
                printf("  %lld: not in sym/code/data/stack range; as line: out of range (1..%lld)\n",
                    nnn, (int64_t)src_line_cnt);
            }
        }
        else if (buf[0] == 'q' && buf[1] == 0) {
            printf("Quit.\n");
            exit(0);
        }
        else if (buf[0] != 0) {
            printf("  Commands: s/Enter=step  n=step-over  c=continue\n");
            printf("            n <func>=break after func() returns  nfl=list  nfd N=delete\n");
            printf("            b N=break  bd N=del-break  bl=list-breaks\n");
            printf("            w name=watch  w name[n]=watch*(name+n)  wd N=del-watch  wl=list-watches\n");
            printf("            p name=print  r=registers  src [N]=source  q=quit\n");
            printf("            i nnn=inspect address (sym/code/data/stack/src-line)\n");
        }
    }
}

// ---- lexer ----

void next()
{
  char *pp;

  while ((tk = *p)) {
    ++p;
    if (tk == '\n') {
      if (src) {
        printf("%lld: %.*s", line, (int32_t)(p - lp), lp);
        lp = p;
        while (le < e) {
          printf("%8.4s", &op_code[*++le * 5]);
          if (*le <= ADJ) printf(" %lld\n", *++le); else printf("\n");
        }
      }
      ++line;
    }
    else if (tk == '#') {
      while (*p != 0 && *p != '\n') ++p;
    }
    else if ((tk >= 'a' && tk <= 'z') || (tk >= 'A' && tk <= 'Z') || tk == '_') {
      pp = p - 1;
      while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')
        tk = tk * 147 + *p++;
      tk = (tk << 6) + (p - pp);
      id = sym;
      while (id[Tk]) {
        if (tk == id[Hash] && !memcmp((char *)id[Name], pp, p - pp)) { tk = id[Tk]; return; }
        id = id + Idsz;
      }
      id[Name] = (int)pp;
      id[Hash] = tk;
      tk = id[Tk] = Id;
      return;
    }
    else if (tk >= '0' && tk <= '9') {
      if ((ival = tk - '0')) { while (*p >= '0' && *p <= '9') ival = ival * 10 + *p++ - '0'; }
      else if (*p == 'x' || *p == 'X') {
        while ((tk = *++p) && ((tk >= '0' && tk <= '9') || (tk >= 'a' && tk <= 'f') || (tk >= 'A' && tk <= 'F')))
          ival = ival * 16 + (tk & 15) + (tk >= 'A' ? 9 : 0);
      }
      else { while (*p >= '0' && *p <= '7') ival = ival * 8 + *p++ - '0'; }
      tk = Num;
      return;
    }
    else if (tk == '/') {
      if (*p == '/') {
        ++p;
        while (*p != 0 && *p != '\n') ++p;
      }
      else {
        tk = Div;
        return;
      }
    }
    else if (tk == '\'' || tk == '"') {
      pp = data;
      while (*p != 0 && *p != tk) {
        if ((ival = *p++) == '\\') {
          if ((ival = *p++) == 'n') ival = '\n';
        }
        if (tk == '"') *data++ = ival;
      }
      ++p;
      if (tk == '"') ival = (int)pp; else tk = Num;
      return;
    }
    else if (tk == '=') { if (*p == '=') { ++p; tk = Eq; } else tk = Assign; return; }
    else if (tk == '+') { if (*p == '+') { ++p; tk = Inc; } else tk = Add; return; }
    else if (tk == '-') { if (*p == '-') { ++p; tk = Dec; } else tk = Sub; return; }
    else if (tk == '!') { if (*p == '=') { ++p; tk = Ne; } return; }
    else if (tk == '<') { if (*p == '=') { ++p; tk = Le; } else if (*p == '<') { ++p; tk = Shl; } else tk = Lt; return; }
    else if (tk == '>') { if (*p == '=') { ++p; tk = Ge; } else if (*p == '>') { ++p; tk = Shr; } else tk = Gt; return; }
    else if (tk == '|') { if (*p == '|') { ++p; tk = Lor; } else tk = Or; return; }
    else if (tk == '&') { if (*p == '&') { ++p; tk = Lan; } else tk = And; return; }
    else if (tk == '^') { tk = Xor; return; }
    else if (tk == '%') { tk = Mod; return; }
    else if (tk == '*') { tk = Mul; return; }
    else if (tk == '[') { tk = Brak; return; }
    else if (tk == '?') { tk = Cond; return; }
    else if (tk == '~' || tk == ';' || tk == '{' || tk == '}' || tk == '(' || tk == ')' || tk == ']' || tk == ',' || tk == ':') return;
  }
}

void expr(int lev)
{
  int t, *d;

  if (!tk) { printf("%lld: unexpected eof in expression\n", line); exit(-1); }
  else if (tk == Num) { *++e = IMM; *++e = ival; next(); ty = INT; }
  else if (tk == '"') {
    *++e = IMM; *++e = ival; next();
    while (tk == '"') next();
    data = (char *)((int)data + sizeof(int) & -sizeof(int)); ty = PTR;
  }
  else if (tk == Sizeof) {
    next(); if (tk == '(') next(); else { printf("%lld: open paren expected in sizeof\n", line); exit(-1); }
    ty = INT; if (tk == Int) next(); else if (tk == Char) { next(); ty = CHAR; }
    while (tk == Mul) { next(); ty = ty + PTR; }
    if (tk == ')') next(); else { printf("%lld: close paren expected in sizeof\n", line); exit(-1); }
    *++e = IMM; *++e = (ty == CHAR) ? sizeof(char) : sizeof(int);
    ty = INT;
  }
  else if (tk == Id) {
    d = id; next();
    if (tk == '(') {
      next();
      t = 0;
      while (tk != ')') { expr(Assign); *++e = PSH; ++t; if (tk == ',') next(); }
      next();
      if (d[Class] == Sys) *++e = d[Val];
      else if (d[Class] == Fun) { *++e = JSR; *++e = d[Val]; }
      else { printf("%lld: bad function call\n", line); exit(-1); }
      if (t) { *++e = ADJ; *++e = t; }
      ty = d[Type];
    }
    else if (d[Class] == Num) { *++e = IMM; *++e = d[Val]; ty = INT; }
    else {
      if (d[Class] == Loc) { *++e = LEA; *++e = loc - d[Val]; }
      else if (d[Class] == Glo) { *++e = IMM; *++e = d[Val]; }
      else { printf("%lld: undefined variable\n", line); exit(-1); }
      *++e = ((ty = d[Type]) == CHAR) ? LC : LI;
    }
  }
  else if (tk == '(') {
    next();
    if (tk == Int || tk == Char) {
      t = (tk == Int) ? INT : CHAR; next();
      while (tk == Mul) { next(); t = t + PTR; }
      if (tk == ')') next(); else { printf("%lld: bad cast\n", line); exit(-1); }
      expr(Inc);
      ty = t;
    }
    else {
      expr(Assign);
      if (tk == ')') next(); else { printf("%lld: close paren expected\n", line); exit(-1); }
    }
  }
  else if (tk == Mul) {
    next(); expr(Inc);
    if (ty > INT) ty = ty - PTR; else { printf("%lld: bad dereference\n", line); exit(-1); }
    *++e = (ty == CHAR) ? LC : LI;
  }
  else if (tk == And) {
    next(); expr(Inc);
    if (*e == LC || *e == LI) --e; else { printf("%lld: bad address-of\n", line); exit(-1); }
    ty = ty + PTR;
  }
  else if (tk == '!') { next(); expr(Inc); *++e = PSH; *++e = IMM; *++e = 0; *++e = EQ; ty = INT; }
  else if (tk == '~') { next(); expr(Inc); *++e = PSH; *++e = IMM; *++e = -1; *++e = XOR; ty = INT; }
  else if (tk == Add) { next(); expr(Inc); ty = INT; }
  else if (tk == Sub) {
    next(); *++e = IMM;
    if (tk == Num) { *++e = -ival; next(); } else { *++e = -1; *++e = PSH; expr(Inc); *++e = MUL; }
    ty = INT;
  }
  else if (tk == Inc || tk == Dec) {
    t = tk; next(); expr(Inc);
    if (*e == LC) { *e = PSH; *++e = LC; }
    else if (*e == LI) { *e = PSH; *++e = LI; }
    else { printf("%lld: bad lvalue in pre-increment\n", line); exit(-1); }
    *++e = PSH;
    *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
    *++e = (t == Inc) ? ADD : SUB;
    *++e = (ty == CHAR) ? SC : SI;
  }
  else { printf("%lld: bad expression\n", line); exit(-1); }

  while (tk >= lev) {
    t = ty;
    if (tk == Assign) {
      next();
      if (*e == LC || *e == LI) *e = PSH; else { printf("%lld: bad lvalue in assignment\n", line); exit(-1); }
      expr(Assign); *++e = ((ty = t) == CHAR) ? SC : SI;
    }
    else if (tk == Cond) {
      next();
      *++e = BZ; d = ++e;
      expr(Assign);
      if (tk == ':') next(); else { printf("%lld: conditional missing colon\n", line); exit(-1); }
      *d = (int)(e + 3); *++e = JMP; d = ++e;
      expr(Cond);
      *d = (int)(e + 1);
    }
    else if (tk == Lor) { next(); *++e = BNZ; d = ++e; expr(Lan); *d = (int)(e + 1); ty = INT; }
    else if (tk == Lan) { next(); *++e = BZ;  d = ++e; expr(Or);  *d = (int)(e + 1); ty = INT; }
    else if (tk == Or)  { next(); *++e = PSH; expr(Xor); *++e = OR;  ty = INT; }
    else if (tk == Xor) { next(); *++e = PSH; expr(And); *++e = XOR; ty = INT; }
    else if (tk == And) { next(); *++e = PSH; expr(Eq);  *++e = AND; ty = INT; }
    else if (tk == Eq)  { next(); *++e = PSH; expr(Lt);  *++e = EQ;  ty = INT; }
    else if (tk == Ne)  { next(); *++e = PSH; expr(Lt);  *++e = NE;  ty = INT; }
    else if (tk == Lt)  { next(); *++e = PSH; expr(Shl); *++e = LT;  ty = INT; }
    else if (tk == Gt)  { next(); *++e = PSH; expr(Shl); *++e = GT;  ty = INT; }
    else if (tk == Le)  { next(); *++e = PSH; expr(Shl); *++e = LE;  ty = INT; }
    else if (tk == Ge)  { next(); *++e = PSH; expr(Shl); *++e = GE;  ty = INT; }
    else if (tk == Shl) { next(); *++e = PSH; expr(Add); *++e = SHL; ty = INT; }
    else if (tk == Shr) { next(); *++e = PSH; expr(Add); *++e = SHR; ty = INT; }
    else if (tk == Add) {
      next(); *++e = PSH; expr(Mul);
      if ((ty = t) > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = MUL;  }
      *++e = ADD;
    }
    else if (tk == Sub) {
      next(); *++e = PSH; expr(Mul);
      if (t > PTR && t == ty) { *++e = SUB; *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = DIV; ty = INT; }
      else if ((ty = t) > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = MUL; *++e = SUB; }
      else *++e = SUB;
    }
    else if (tk == Mul) { next(); *++e = PSH; expr(Inc); *++e = MUL; ty = INT; }
    else if (tk == Div) { next(); *++e = PSH; expr(Inc); *++e = DIV; ty = INT; }
    else if (tk == Mod) { next(); *++e = PSH; expr(Inc); *++e = MOD; ty = INT; }
    else if (tk == Inc || tk == Dec) {
      if (*e == LC) { *e = PSH; *++e = LC; }
      else if (*e == LI) { *e = PSH; *++e = LI; }
      else { printf("%lld: bad lvalue in post-increment\n", line); exit(-1); }
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
      *++e = (tk == Inc) ? ADD : SUB;
      *++e = (ty == CHAR) ? SC : SI;
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
      *++e = (tk == Inc) ? SUB : ADD;
      next();
    }
    else if (tk == Brak) {
      next(); *++e = PSH; expr(Assign);
      if (tk == ']') next(); else { printf("%lld: close bracket expected\n", line); exit(-1); }
      if (t > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = MUL;  }
      else if (t < PTR) { printf("%lld: pointer type expected\n", line); exit(-1); }
      *++e = ADD;
      *++e = ((ty = t - PTR) == CHAR) ? LC : LI;
    }
    else { printf("%lld: compiler error tk=%lld\n", line, tk); exit(-1); }
  }
}

void stmt()
{
  int *a, *b;

  // Record line number for debug info
  lntab_add(e + 1, line);

  if (tk == If) {
    next();
    if (tk == '(') next(); else { printf("%lld: open paren expected\n", line); exit(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%lld: close paren expected\n", line); exit(-1); }
    *++e = BZ; b = ++e;
    stmt();
    if (tk == Else) {
      *b = (int)(e + 3); *++e = JMP; b = ++e;
      next();
      stmt();
    }
    *b = (int)(e + 1);
  }
  else if (tk == While) {
    next();
    a = e + 1;
    if (tk == '(') next(); else { printf("%lld: open paren expected\n", line); exit(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%lld: close paren expected\n", line); exit(-1); }
    *++e = BZ; b = ++e;
    stmt();
    *++e = JMP; *++e = (int)a;
    *b = (int)(e + 1);
  }
  else if (tk == Return) {
    next();
    if (tk != ';') expr(Assign);
    *++e = LEV;
    if (tk == ';') next(); else { printf("%lld: semicolon expected\n", line); exit(-1); }
  }
  else if (tk == '{') {
    next();
    while (tk != '}') stmt();
    next();
  }
  else if (tk == ';') {
    next();
  }
  else {
    expr(Assign);
    if (tk == ';') next(); else { printf("%lld: semicolon expected\n", line); exit(-1); }
  }
}

int32_t main(int32_t argc, char **argv)
{
  int fd, bt, ty, poolsz, *idmain;
  int *pc, *sp, *bp, a, cycle; // vm registers
  int i, *t; // temps
  int cur_fn_entry_pc = 0; // track current function entry for debug info

  --argc; ++argv;
  if (argc > 0 && **argv == '-' && (*argv)[1] == 's') { src = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 'd') { debug = 1; --argc; ++argv; }
  if (argc > 0 && **argv == '-' && (*argv)[1] == 't') { trace = 1; --argc; ++argv; }
  if (argc < 1) { printf("usage: c4deb [-s] [-d] [-t] file ...\n"); return -1; }

  if ((fd = open(*argv, 0)) < 0) { printf("could not open(%s)\n", *argv); return -1; }

  poolsz = 256*1024; // arbitrary size
  if (!(sym = malloc(poolsz))) { printf("could not malloc(%lld) symbol area\n", poolsz); return -1; }
  if (!(le = e = malloc(poolsz))) { printf("could not malloc(%lld) text area\n", poolsz); return -1; }
  e_base = e;
  if (!(data = malloc(poolsz))) { printf("could not malloc(%lld) data area\n", poolsz); return -1; }
  data_org = data;
  if (!(sp = malloc(poolsz))) { printf("could not malloc(%lld) stack area\n", poolsz); return -1; }

  memset(sym,  0, poolsz);
  memset(e,    0, poolsz);
  memset(data, 0, poolsz);

  p = "char else enum if int return sizeof while "
      "open read close printf malloc free memset memcmp exit int32_t int64_t void main";
  i = Char; while (i <= While) { next(); id[Tk] = i++; }
  i = OPEN; while (i <= EXIT) { next(); id[Class] = Sys; id[Type] = INT; id[Val] = i++; }
  next(); id[Tk] = Int;  // handle int32_t type
  next(); id[Tk] = Int;  // handle int64_t type
  next(); id[Tk] = Char; // handle void type
  next(); idmain = id;   // keep track of main

  if (!(lp = p = malloc(poolsz))) { printf("could not malloc(%lld) source area\n", poolsz); return -1; }
  if ((i = read(fd, p, poolsz-1)) <= 0) { printf("read() returned %lld\n", i); return -1; }
  p[i] = 0;
  close(fd);

  // Build source line index for debugger
  src_buf = p;
  if (trace) build_src_lines(src_buf);

  op_code = "LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
            "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
            "OPEN,READ,CLOS,PRTF,MALC,FREE,MSET,MCMP,EXIT,";
            
  line = 1;
  next();
  while (tk) {
    bt = INT;
    if (tk == Int) next();
    else if (tk == Char) { next(); bt = CHAR; }
    else if (tk == Enum) {
      next();
      if (tk != '{') next();
      if (tk == '{') {
        next();
        i = 0;
        while (tk != '}') {
          if (tk != Id) { printf("%lld: bad enum identifier %lld\n", line, tk); return -1; }
          next();
          if (tk == Assign) {
            next();
            if (tk != Num) { printf("%lld: bad enum initializer\n", line); return -1; }
            i = ival;
            next();
          }
          id[Class] = Num; id[Type] = INT; id[Val] = i++;
          if (tk == ',') next();
        }
        next();
      }
    }
    while (tk != ';' && tk != '}') {
      ty = bt;
      while (tk == Mul) { next(); ty = ty + PTR; }
      if (tk != Id) { printf("%lld: bad global declaration\n", line); return -1; }
      if (id[Class]) { printf("%lld: duplicate global definition\n", line); return -1; }
      next();
      id[Type] = ty;
      if (tk == '(') { // function
        id[Class] = Fun;
        id[Val] = (int)(e + 1);
        cur_fn_entry_pc = (int)(e + 1);
        // Record in function/line tables for debugger
        if (trace) {
          fntab_add((int *)(e + 1), id);
          lntab_add((int *)(e + 1), line); // ENT → function entry line
        }
        next(); i = 0;
        while (tk != ')') {
          ty = INT;
          if (tk == Int) next();
          else if (tk == Char) { next(); ty = CHAR; }
          while (tk == Mul) { next(); ty = ty + PTR; }
          if (tk != Id) { printf("%lld: bad parameter declaration\n", line); return -1; }
          if (id[Class] == Loc) { printf("%lld: duplicate parameter definition\n", line); return -1; }
          id[HClass] = id[Class]; id[Class] = Loc;
          id[HType]  = id[Type];  id[Type] = ty;
          id[HVal]   = id[Val];   id[Val] = i++;
          next();
          if (tk == ',') next();
        }
        next();
        if (tk != '{') { printf("%lld: bad function definition\n", line); return -1; }
        loc = ++i;
        // Record params now that loc is known: params have Val < loc
        if (trace) {
          int *pid = sym;
          while (pid[Tk]) {
            if (pid[Class] == Loc && pid[Val] < loc)
              dbg_local_add(cur_fn_entry_pc, pid, (int)(loc - pid[Val]));
            pid = pid + Idsz;
          }
        }
        next();
        while (tk == Int || tk == Char) {
          bt = (tk == Int) ? INT : CHAR;
          next();
          while (tk != ';') {
            ty = bt;
            while (tk == Mul) { next(); ty = ty + PTR; }
            if (tk != Id) { printf("%lld: bad local declaration\n", line); return -1; }
            if (id[Class] == Loc) { printf("%lld: duplicate local definition\n", line); return -1; }
            id[HClass] = id[Class]; id[Class] = Loc;
            id[HType]  = id[Type];  id[Type] = ty;
            id[HVal]   = id[Val];   id[Val] = ++i;
            // Record local variable: frame_off = loc - id[Val] (negative)
            if (trace) dbg_local_add(cur_fn_entry_pc, id, (int)(loc - id[Val]));
            next();
            if (tk == ',') next();
          }
          next();
        }
        *++e = ENT; *++e = i - loc;
        while (tk != '}') stmt();
        *++e = LEV;
        id = sym; // unwind symbol table locals
        while (id[Tk]) {
          if (id[Class] == Loc) {
            id[Class] = id[HClass];
            id[Type] = id[HType];
            id[Val] = id[HVal];
          }
          id = id + Idsz;
        }
      }
      else {
        id[Class] = Glo;
        id[Val] = (int)data;
        data = data + sizeof(int);
      }
      if (tk == ',') next();
    }
    next();
  }

  if (!(pc = (int *)idmain[Val])) { printf("main() not defined\n"); return -1; }
  if (src) return 0;

  // setup stack
  bp = sp = (int *)((int)sp + poolsz);
  stk_hi = bp;  // top of stack (initial bp)
  stk_lo = (int *)((int)bp - poolsz);  // bottom of stack
  *--sp = EXIT;
  *--sp = PSH; t = sp;
  *--sp = argc;
  *--sp = (int)argv;
  *--sp = (int)t;

  // Debugger init
  deb_stepping = trace ? 1 : 0;
  deb_stepover = 0;
  deb_last_line = -1;
  int deb_stepover_depth = 0; // for step-over tracking

  // run...
  cycle = 0;
  while (1) {
    i = *pc++; ++cycle;

    // Non-interactive debug trace
    if (debug) {
      printf("%lld> %.4s", cycle, &op_code[i * 5]);
      if (i <= ADJ) printf(" %lld\n", *pc); else printf("\n");
    }

    // Interactive trace
    if (trace) {
      int cur_ln = lntab_lookup(pc - 1);

      // Function-call breakpoint: independent depth tracking.
      // When JSR to a watched function is seen, start tracking.
      // depth counts nested JSR/LEV inside that call; foo's own LEV hits 0 → stop.
      if (deb_fn_active) {
        if (i == JSR) deb_fn_depth++;
        else if (i == LEV) {
          if (deb_fn_depth > 0) deb_fn_depth--;
          else {
            // foo() has returned — pause here (before LEV executes)
            deb_stepping  = 1;
            deb_fn_active = 0;
          }
        }
      }
      if (!deb_fn_active && i == JSR && fn_brkpt_cnt > 0) {
        int *target = (int *)*pc;
        int fi;
        for (fi = 0; fi < fn_brkpt_cnt; fi++) {
          if (fn_brkpt_pcs[fi] == target) {
            deb_fn_active = 1;
            deb_fn_depth  = 0;  // JSR itself is the entry; nested calls add depth
            printf("  [Function breakpoint: will pause after %s() returns]\n",
                   fn_brkpt_names[fi]);
            break;
          }
        }
      }

      // Handle step-over depth tracking
      if (deb_stepover) {
        if (i == JSR) deb_stepover_depth++;
        else if (i == LEV) {
          if (deb_stepover_depth > 0) deb_stepover_depth--;
          else {
            // returned to original depth, stop
            deb_stepping = 1;
            deb_stepover = 0;
          }
        }
      }

      // Check breakpoint
      int at_break = 0;
      if (!deb_stepping && cur_ln > 0 && is_breakpoint(cur_ln)) {
        at_break = 1;
        deb_stepping = 1;
      }

      if (deb_stepping || at_break) {
        // Print context header if line changed
        if (cur_ln != deb_last_line && cur_ln > 0) {
          // Find function name
          int *fn_id = fntab_lookup_id(pc - 1);
          char fn_name[64] = "?";
          if (fn_id) {
            int fn_len = fn_id[Hash] & 0x3F;
            if (fn_len > 63) fn_len = 63;
            memcpy(fn_name, (char *)fn_id[Name], fn_len);
            fn_name[fn_len] = 0;
          }
          printf("\n=== [cycle=%lld] line=%lld in %s() ===\n",
                 (int)cycle, (int)cur_ln, fn_name);
          show_src_line(cur_ln);
          deb_last_line = cur_ln;
        }

        // Show current instruction
        printf("   OP: %.4s", &op_code[i * 5]);
        if (i <= ADJ) printf(" %lld", (int)*pc);
        printf("\n");

        // Show registers
        printf("  REG: a=%lld  sp=%lld  bp=%lld  pc=%lld\n",
                (int)a, (int)sp, (int)bp, (int)(pc-1));
        printf("  STK:");
        for(int i=0; i<4; i++) {
          printf(" %s%lld ", (i == bp-sp) ? "B=" : "", sp[i]);
        }
        printf("\n");

        show_watches(bp, sp, pc - 1);

        if (at_break) printf("  [Breakpoint at line %lld]\n", (int)cur_ln);

        // Show prompt
        debug_prompt(pc, sp, bp, a, i, cycle);
      }
    }

    if      (i == LEA) a = (int)(bp + *pc++);
    else if (i == IMM) a = *pc++;
    else if (i == JMP) pc = (int *)*pc;
    else if (i == JSR) { *--sp = (int)(pc + 1); pc = (int *)*pc; }
    else if (i == BZ)  pc = a ? pc + 1 : (int *)*pc;
    else if (i == BNZ) pc = a ? (int *)*pc : pc + 1;
    else if (i == ENT) { *--sp = (int)bp; bp = sp; sp = sp - *pc++; }
    else if (i == ADJ) sp = sp + *pc++;
    else if (i == LEV) { sp = bp; bp = (int *)*sp++; pc = (int *)*sp++; }
    else if (i == LI)  { if (!a) { printf("null pointer dereference (LI) cycle=%lld\n", cycle); return -1; } a = *(int *)a; }
    else if (i == LC)  { if (!a) { printf("null pointer dereference (LC) cycle=%lld\n", cycle); return -1; } a = *(char *)a; }
    else if (i == SI)  { if (!*sp) { printf("null pointer dereference (SI) cycle=%lld\n", cycle); return -1; } *(int *)*sp++ = a; }
    else if (i == SC)  { if (!*sp) { printf("null pointer dereference (SC) cycle=%lld\n", cycle); return -1; } a = *(char *)*sp++ = a; }
    else if (i == PSH) *--sp = a;
    else if (i == OR)  a = *sp++ |  a;
    else if (i == XOR) a = *sp++ ^  a;
    else if (i == AND) a = *sp++ &  a;
    else if (i == EQ)  a = *sp++ == a;
    else if (i == NE)  a = *sp++ != a;
    else if (i == LT)  a = *sp++ <  a;
    else if (i == GT)  a = *sp++ >  a;
    else if (i == LE)  a = *sp++ <= a;
    else if (i == GE)  a = *sp++ >= a;
    else if (i == SHL) a = *sp++ << a;
    else if (i == SHR) a = *sp++ >> a;
    else if (i == ADD) a = *sp++ +  a;
    else if (i == SUB) a = *sp++ -  a;
    else if (i == MUL) a = *sp++ *  a;
    else if (i == DIV) a = *sp++ /  a;
    else if (i == MOD) a = *sp++ %  a;
    else if (i == OPEN) a = open((char *)sp[1], *sp);
    else if (i == READ) a = read(sp[2], (char *)sp[1], *sp);
    else if (i == CLOS) a = close(*sp);
    else if (i == PRTF) { t = sp + pc[1]; a = printf((char *)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6]); }
    else if (i == MALC) { a = (int)malloc(*sp); if (a) heap_track_add((char *)a, *sp); }
    else if (i == FREE) { heap_track_remove((char *)*sp); free((void *)*sp); }
    else if (i == MSET) a = (int)memset((char *)sp[2], sp[1], *sp);
    else if (i == MCMP) a = memcmp((char *)sp[2], (char *)sp[1], *sp);
    else if (i == EXIT) { printf("exit(%lld) cycle = %lld\n", *sp, cycle); return *sp; }
    else { printf("unknown instruction = %lld! cycle = %lld\n", i, cycle); return -1; }
  }
}
