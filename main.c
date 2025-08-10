#ifdef MEMDBG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <errno.h>

#include "platform.h"
#include "crtio.h"

#define VERSION "0.2"

#define MAX_COLS    26
#define MAX_ROWS    256
#define VIEW_COLS   6     // viewport width 
#define VIEW_ROWS   24    // viewport height
#define CELL_W      11    // cell display width

#define CELL_TBL_SIZE  31 // size of hash table for cells

#define INPUT_LINE_ROW (SCREEN_HEIGHT - 6)
#define STATUS_LINE_ROW (INPUT_LINE_ROW - 1)

#define FLG_DIRTY       1
#define FLG_FORMULA     2
#define FLG_EMPTY       4
#define FLG_VISITING    128

#define MSK_DIRTY       (~FLG_DIRTY)
#define MSK_FORMULA     (~FLG_FORMULA)
#define MSK_EMPTY       (~FLG_EMPTY)
#define MSK_VISITING    (~FLG_VISITING)

#define MAX_FUNC_ARGS 5

typedef enum { REDRAW_ALL, REDRAW_CONTENT } REDRAW_MODE;

char ln[80];
char* e_filename = NULL;
uint8_t is_dirty = 0;
uint8_t was_dirty = 0;
uint8_t has_error = 0; // set if error occurred during evaluation
REDRAW_MODE redraw = REDRAW_ALL;

struct Cell;

typedef enum { TYPE_NULL, TYPE_NUM, TYPE_STR, TYPE_TEXT, TYPE_ERROR } ValType;

/* Generic value returned by evaluator */
typedef struct Value {
    ValType type;
    union {
        float  num;
        char* str;     // allocated if TYPE_STR, not allocated if TYPE_TEXT or TYPE_ERROR
    };
} Value;

Value errInvalidArg = { .type = TYPE_ERROR };
Value errExprInvalid = { .type = TYPE_ERROR };
Value errExprDivZero = { .type = TYPE_ERROR };
Value errExprCyclicRef = { .type = TYPE_ERROR };
Value errExprExpectLParen = { .type = TYPE_ERROR };
Value errExprExpectRParen = { .type = TYPE_ERROR };
Value errExprExpectNumeric = { .type = TYPE_ERROR };
Value errOutOfMemory = { .type = TYPE_ERROR };


// Forward declarations
Value  make_num(float v);
Value  make_str(const char* s);

void   free_val(Value *v);
void error(const char* fmt, ...);
void status(const char* fmt, ...);

typedef struct Dep {
    struct Cell* cell;
    struct Dep* next;
} Dep;

// One spreadsheet cell
typedef struct Cell {
    Dep* deps;          // cells this cell references
    Dep* revdeps;       // cells referemcing this cell

    int col, row;
    char* content;      // raw text
    Value cached;

    uint8_t flags;
} Cell;

typedef struct HNode {
    Cell* cell;
    struct HNode* next;
} HNode;

uint8_t is_str_value(Value v) {
    return v.type == TYPE_STR || v.type == TYPE_TEXT;
}

HNode* hash_table[CELL_TBL_SIZE] = { 0 }; // hash table for cells

Cell* find_cell(int col, int row) {
    int h = (col + (row * 257)) % CELL_TBL_SIZE;
    HNode* node = hash_table[h];
    while (node) {
        if (node->cell->col == col && node->cell->row == row) {
            return node->cell;
        }
        node = node->next;
    }
    return NULL; // not found
}

void add_cell(Cell* cell) {
    int h = (cell->col + (cell->row * 257)) % CELL_TBL_SIZE;
    HNode* node = malloc(sizeof(HNode));
    if (!node) {
        error(errOutOfMemory.str);
        return;
    }
    node->cell = cell;
    node->next = hash_table[h];
    hash_table[h] = node;
}

typedef void  (*PFN_ACCUM)(void* state, Cell* cell);
typedef Value(*PFN_EVAL)(void* state);

typedef enum {
    tokNone,
    tokCellRef, tokRange, tokNumber, tokString, 
    tokEq, tokNe, tokLt, tokLe, tokGt, tokGe, 
    tokPlus, tokMinus, tokMul, tokDiv, tokMod, tokLParen, tokRParen, tokComma, tokEnd,
    tokScalarFunc,
    tokRangeFunc,

    tokError, 
} TokenType;

typedef struct Function {
    const char* name;
    PFN_ACCUM pfn_accum;    // function accumulator
    PFN_EVAL pfn_eval;      // function evaluator
    TokenType tok_type;     // token type for this function
    uint8_t min_args;       // minimum number of arguments
    uint8_t max_args;       // maximum number of arguments
} Function;

void sum_range(void* state, Cell* cell);
Value sum_eval(void* state);
Value avg_eval(void* state);
void count_range(void* state, Cell* cell);
Value count_eval(void* state);
void max_range(void* state, Cell* cell);
void min_range(void* state, Cell* cell);
Value best_eval(void* state);

Value sin_eval(void* state);
Value cos_eval(void* state);
Value tan_eval(void* state);
Value asin_eval(void* state);
Value acos_eval(void* state);
Value atan_eval(void* state);

Value abs_eval(void* state);
Value ceil_eval(void* state);
Value floor_eval(void* state);
Value round_eval(void* state);
Value trunc_eval(void* state);

Value sqrt_eval(void* state);
Value exp_eval(void* state);
Value log_eval(void* state);
Value log10_eval(void* state);
Value log2_eval(void* state);

Value dec2bin_eval(void* state);
Value bin2dec_eval(void* state);
Value dec2hex_eval(void* state);
Value hex2dec_eval(void* state);

Value if_eval(void* state);

Function functions[] = {
    { "SUM", sum_range, sum_eval, tokRangeFunc, 1, 1},
    { "AVG", sum_range, avg_eval, tokRangeFunc, 1, 1 },
    { "COUNT", count_range, count_eval, tokRangeFunc, 1, 1 },
    { "MAX", max_range, best_eval, tokRangeFunc, 1, 1 },
    { "MIN", min_range, best_eval, tokRangeFunc, 1, 1 },
    { "SIN", NULL, sin_eval, tokScalarFunc, 1, 1},
    { "COS", NULL, cos_eval, tokScalarFunc, 1, 1},
    { "TAN", NULL, tan_eval, tokScalarFunc, 1, 1},
    { "ASIN", NULL, asin_eval, tokScalarFunc, 1, 1},
    { "ACOS", NULL, acos_eval, tokScalarFunc, 1, 1},
    { "ATAN", NULL, atan_eval, tokScalarFunc, 1, 1},
    { "ABS", NULL, abs_eval, tokScalarFunc, 1, 1},
    { "CEIL", NULL, ceil_eval, tokScalarFunc, 1, 1},
    { "FLOOR", NULL, floor_eval, tokScalarFunc, 1, 1},
    { "ROUND", NULL, round_eval, tokScalarFunc, 1, 1},
    { "TRUNC", NULL, trunc_eval, tokScalarFunc, 1, 1},
    { "SQRT", NULL, sqrt_eval, tokScalarFunc, 1, 1},
    { "EXP", NULL, exp_eval, tokScalarFunc, 1, 1},
    { "LOG", NULL, log_eval, tokScalarFunc, 1, 1},
    { "LOG10", NULL, log10_eval, tokScalarFunc, 1, 1},
    { "LOG2", NULL, log2_eval, tokScalarFunc, 1, 1},
    { "DEC2BIN", NULL, dec2bin_eval, tokScalarFunc, 1, 1},
    { "BIN2DEC", NULL, bin2dec_eval, tokScalarFunc, 1, 1},
    { "DEC2HEX", NULL, dec2hex_eval, tokScalarFunc, 1, 1},
    { "HEX2DEC", NULL, hex2dec_eval, tokScalarFunc, 1, 1},
    { "IF", NULL, if_eval, tokScalarFunc, 3, 3},

    { NULL, NULL }  /* sentinel */
};

TokenType tok_type;
char token[32];
char* expr;
char ch;
Function* current_function;

void error(const char* fmt, ...) {
    uint8_t ox, oy;

    has_error = 1;
    get_cursor_pos(&ox, &oy);
    va_list args;
    va_start(args, fmt);
    vsprintf(ln, fmt, args);
    va_end(args);
    set_cursor_pos(0, STATUS_LINE_ROW);
    prints(ln); clreol();
    set_cursor_pos(ox, oy);

    if (fmt == errOutOfMemory.str) getch();
}

void status(const char* fmt, ...) {
    uint8_t ox, oy;
    
    get_cursor_pos(&ox, &oy);
    va_list args;
    va_start(args, fmt);
    vsprintf(ln, fmt, args);
    va_end(args);
    set_cursor_pos(0, STATUS_LINE_ROW);
    prints(ln); clreol();
    set_cursor_pos(ox, oy);
}

void next_char(void) {
    ch = *expr;
    if (ch == 0) return;
    ++expr;
}

void parse_cellref(const char** sp, int* col, int* row);
void parse_range(const char** sp, int* from_col, int* from_row, int* to_col, int* to_row);

uint8_t is_cellref(const char* s) {
    int n = 0;
    if (!isalpha(*s)) return 0;
    ++s;
    if (!isdigit(*s)) return 0;
    while (isdigit(*s)) {
        n = n * 10 + (*s - '0');
        ++s;
    }
    if (n < 1 || n > MAX_ROWS) return 0;
    return *s == 0;
}

void get_token(void) {
    tok_type = tokNone;

    while (isspace(ch)) next_char();
    if (ch == 0) {
        tok_type = tokEnd;
        return;
    }
    if (isalpha(ch)) {
        int i = 0;
        while (isalpha(ch) || isdigit(ch)) {
            if (i < sizeof(token) - 1)
                token[i++] = toupper(ch);
            next_char();
        }
        token[i] = 0;
        if (is_cellref(token)) {
            tok_type = tokCellRef;
            if (ch == ':') {
                token[i++] = ch; // include ':' in token
                next_char(); // skip ':'
                char* s = token + i;
                while (isalpha(ch) || isdigit(ch)) {
                    if (i < sizeof(token) - 1)
                        token[i++] = toupper(ch);
                    next_char();
                }
                token[i] = 0;
                if (!is_cellref(s)) {
                    tok_type = tokError;
                    return;
                }
                tok_type = tokRange;
            }
        }
        else {
            for (Function* f = functions; f->name; f++) {
                if (strcmp(f->name, token) == 0) {
                    tok_type = f->tok_type;
                    current_function = f;
                    break;
                }
            }
        }
        if (tok_type == tokNone) tok_type = tokError;
    }
    else if (isdigit(ch)) {
        int i = 0;
        while (isdigit(ch)) {
            if (i < sizeof(token) - 1)
                token[i++] = ch;
            next_char();
        }
        if (ch == '.') {
            if (i < sizeof(token) - 1)
                token[i++] = ch;
            next_char();
            while (isdigit(ch)) {
                if (i < sizeof(token) - 1)
                    token[i++] = ch;
                next_char();
            }
        }
        token[i] = 0;
        tok_type = tokNumber;
    }
    else {
        switch (ch) {
            case '=': tok_type = tokEq; next_char(); break;
            case '<': 
            next_char();
            if (ch == '=') {
                tok_type = tokLe; next_char();
            } else if (ch == '>') {
                tok_type = tokNe; next_char();
            }
            else {
                tok_type = tokLt;
            }
            break;
            case '>':
                next_char();
                if (ch == '=') {
                    tok_type = tokGe; next_char();
                }
                else {
                    tok_type = tokGt;
                }
            break;                
            case '+': tok_type = tokPlus; next_char(); break;
            case '-': tok_type = tokMinus; next_char(); break;
            case '*': tok_type = tokMul; next_char(); break;
            case '/': tok_type = tokDiv; next_char(); break;
            case '%': tok_type = tokMod; next_char(); break;
            case '(': tok_type = tokLParen; next_char(); break;
            case ')': tok_type = tokRParen; next_char(); break;
            case ',': tok_type = tokComma; next_char(); break;
            case '\'': // string literal start
            case '"': 
                {
                    char quote = ch;
                    int i = 0;
                    next_char();
                    while (ch && ch != quote) {
                        if (i < sizeof(token) - 1)
                            token[i++] = ch;
                        next_char();
                    }
                    if (ch == quote) {
                        next_char(); // skip closing quote
                        token[i] = 0;
                        tok_type = tokString;
                    }
                    else {
                        tok_type = tokError; // unterminated string
                    }
                }
                break;
            default:
                tok_type = tokError;
                break;
        }
    }
}

uint8_t expect_token(TokenType expected) {
    if (tok_type != expected) {
        return 0;
    }
    get_token();
    return 1;
}

char* trim(char* s);

/* Globals */
static int view_r = 0, view_c = 0;
static int ccol = 0, crow = 0;

/* Helpers for Value */
Value make_num(float v) {
    Value x; x.type = TYPE_NUM; x.num = v; return x;
}

Value make_str(const char* s) {
    Value x; x.type = TYPE_STR;
    if (s) x.str = strdup(s); else x.str = NULL;
    return x;
}

void free_val(Value *v) {
    if (v->type == TYPE_STR && v->str) {
        free(v->str);        
    }
    v->type = TYPE_NULL;
    v->str = NULL;
}

// Trim leading/trailing space
char* trim(char* s) {
    char* p = s + strlen(s) - 1;
    while (p >= s && isspace(*p)) *p-- = 0;
    p = s;
    while (*p && isspace(*p)) p++;
    return p;
}

Cell* new_cell(int c, int r) {
    Cell* p = calloc(1, sizeof(Cell));
    if (p == NULL) {
        error(errOutOfMemory.str);
        return NULL;
    }
    p->col = c; p->row = r;
    add_cell(p);
    return p;
}

void remove_revdep(Cell* c, Cell* dep) {
    Dep** pp = &c->revdeps;
    while (*pp && (*pp)->cell != dep) pp = &(*pp)->next;
    if (*pp) {
        Dep* t = *pp; *pp = t->next; // unlink
        free(t);
    }
}

void remove_deps(Cell* c) {
    Dep* d = c->deps;
    while (d) {
        Dep* t = d; d = d->next;
        remove_revdep(t->cell, c);
        free(t);
    }
    c->deps = NULL;
}

#ifdef MEMDBG
void free_deplist(Dep *d) {
    while (d) {
        Dep* t = d; d = d->next;
        free(t);
    }        
}

void remove_cell(Cell* cell) {
    int h = (cell->col + (cell->row * 257)) % CELL_TBL_SIZE;
    HNode* node = hash_table[h];
    HNode* prev = NULL;

    free_deplist(cell->deps);
    free_deplist(cell->revdeps);
    while (node) {
        if (node->cell == cell) {
            if (prev) {
                prev->next = node->next;
            }
            else {
                hash_table[h] = node->next;
            }
            free(node);
            return;
        }
        prev = node;
        node = node->next;
    }
}

void free_cell(Cell* c) {
    if (c->content) free(c->content);
    free_val(&c->cached);
    remove_cell(c);
    free(c);
}

void free_cells(void) {
    for (int i = 0; i < CELL_TBL_SIZE; i++) {
        HNode* node = hash_table[i];
        while (node) {
            HNode* next = node->next;
            Cell* c = node->cell;
            free_cell(c);            
            node = next;
        }
        hash_table[i] = NULL; // clear the hash table entry
    }
}
#endif //MEMDBG

/* Add owner→dependency link both ways */
void add_dep(Cell* owner, Cell* dep) {
    Dep* d = malloc(sizeof * d);
    if (d == NULL) {
        error(errOutOfMemory.str);
        return;
    }
    d->cell = dep; d->next = owner->deps; owner->deps = d;
    Dep* r = malloc(sizeof * r);
    if (d == NULL) {
        error(errOutOfMemory.str);
        return;
    }
    r->cell = owner; r->next = dep->revdeps; dep->revdeps = r;
}

/* Mark c and all its dependents dirty */
void propagate_dirty(Cell* c) {
    if (!(c->flags & FLG_DIRTY)) {
        c->flags |= FLG_DIRTY;
        for (Dep* d = c->revdeps; d; d = d->next)
            propagate_dirty(d->cell);
    }
}

/* Evaluate a cell (with caching & cycle detect) */
void eval_cell(Cell* c);
Value eval_expr(void);
Value eval_expr1(void);
Value eval_term(void);
Value eval_factor(void);

void parse_cellref(const char** sp, int* col, int* row) {
    char ref[8] = { 0 };
    int i = 0;
    if (isalpha(**sp)) {
        ref[i++] = toupper(*(*sp)++);
        while (isdigit(**sp) && i < 7) ref[i++] = *(*sp)++;
    }
    ref[i] = 0;
    *col = ref[0] - 'A';
    *row = atoi(ref + 1) - 1;
}

void parse_range(const char** sp, int* from_col, int* from_row, int* to_col, int* to_row) {
    int c1, r1, c2, r2;
    parse_cellref(sp, &c1, &r1);
    if (**sp == ':') (*sp)++;
    parse_cellref(sp, &c2, &r2);
    if (c2 < c1) { int t = c1; c1 = c2; c2 = t; }
    if (r2 < r1) { int t = r1; r1 = r2; r2 = t; }
    *from_col = c1; *from_row = r1;
    *to_col = c2; *to_row = r2;
}

typedef struct {
    float total;
    float best;
    int count;
} AccumState;

void sum_range(void* state, Cell* cell) {
    AccumState* acc = (AccumState*)state;

    Value dv = cell->cached;
    if (dv.type == TYPE_NUM) {
        acc->total += dv.num;
        acc->count++;
    }
}

Value sum_eval(void* state) {
    AccumState* acc = (AccumState*)state;
    Value v = make_num(acc->total);
    return v;
}

Value avg_eval(void* state) {
    AccumState* acc = (AccumState*)state;
    Value v = make_num(acc->count ? acc->total / acc->count : 0);
    return v;
}

void count_range(void* state, Cell* cell) {
    AccumState* acc = (AccumState*)state;
    Value v = cell->cached;
    if (v.type == TYPE_NULL || v.type == TYPE_ERROR) {
        return; // skip null or error values
    }
    if (v.type == TYPE_NUM || (v.str && *v.str)) {
        acc->count++;
    }
}

Value count_eval(void* state) {
    AccumState* acc = (AccumState*)state;
    Value v = make_num((float)acc->count);
    return v;
}

void max_range(void* state, Cell* cell) {
    AccumState* acc = (AccumState*)state;
    Value v = cell->cached;
    if (v.type == TYPE_NUM) {
        if (acc->count == 0 || v.num > acc->best) {
            acc->best = v.num;
        }
        acc->count++;
    }
}

void min_range(void* state, Cell* cell) {
    AccumState* acc = (AccumState*)state;
    Value v = cell->cached;
    if (v.type == TYPE_NUM) {
        if (acc->count == 0 || v.num < acc->best) {
            acc->best = v.num;
        }
        acc->count++;
    }
}

Value best_eval(void* state) {
    AccumState* acc = (AccumState*)state;
    Value v = make_num(acc->best);
    return v;
}

Value sin_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(sinf(arg.num));
    return v;
}

Value cos_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(cosf(arg.num));
    return v;
}
Value tan_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(tanf(arg.num));
    return v;
}
Value asin_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(asinf(arg.num));
    return v;
}
Value acos_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(acosf(arg.num));
    return v;
}
Value atan_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(atanf(arg.num));
    return v;
}

Value abs_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(fabsf(arg.num));
    return v;
}
Value ceil_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(ceilf(arg.num));
    return v;
}
Value floor_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(floorf(arg.num));
    return v;
}
Value round_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num((float)(int)(arg.num + 0.5));
    return v;
}
Value trunc_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(truncf(arg.num));
    return v;
}

Value sqrt_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(sqrtf(arg.num));
    return v;
}
Value exp_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(expf(arg.num));
    return v;
}
Value log_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(logf(arg.num));
    return v;
}
Value log10_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(log10f(arg.num));
    return v;
}
Value log2_eval(void* state) {
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    Value v = make_num(log2f(arg.num));
    return v;
}

Value dec2bin_eval(void* state) {
    char b[32];
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    int n = (int)arg.num;
    itoa(n, b, 2);
    Value v = make_str(b);
    return v;
}

Value bin2dec_eval(void* state) {
    Value arg = *(Value*)state;
    char* endptr;
    if (!is_str_value(arg)) return errInvalidArg;
    int n = (int)strtol(arg.str, &endptr, 2);

    if (*endptr != '\0') return errExprInvalid;

    Value v = make_num((float)n);
    return v;
}

Value dec2hex_eval(void* state) {
    char b[32];
    Value arg = *(Value*)state;
    if (arg.type != TYPE_NUM) return errExprExpectNumeric;
    int n = (int)arg.num;
    itoa(n, b, 16);
    Value v = make_str(b);
    return v;
}

Value hex2dec_eval(void* state) {
    Value arg = *(Value*)state;
    char* endptr;
    if (is_str_value(arg)) return errInvalidArg;
    int n = (int)strtol(arg.str, &endptr, 16);

    if (*endptr != '\0') return errExprInvalid;

    Value v = make_num((float)n);
    return v;
}

Value if_eval(void* state) {
    Value* args = (Value*)state;
    if (args[0].type != TYPE_NUM) {
        return errExprExpectNumeric;
    }
    Value v = args[args[0].num ? 1 : 2]; // return true or false branch
    return v;
}
    

void set_range_dep(void* state, Cell* cell) {
    Cell* root = (Cell*)state;
    if (cell->flags & FLG_EMPTY) {
        cell = new_cell(cell->col, cell->row);
    }
    add_dep(root, cell);
}

Value process_range(
    int c1, int r1, int c2, int r2,
    void* state,
    PFN_ACCUM pfnAccum,
    PFN_EVAL pfnEval) {

    Cell empty = { 0 };
    empty.flags |= FLG_EMPTY;

    for (int cc = c1; cc <= c2; cc++) {
        for (int rr = r1; rr <= r2; rr++) {
            Cell* cell = find_cell(cc, rr);
            if (!cell) {
                cell = &empty;  /* use empty cell if not found */
                empty.col = cc;
                empty.row = rr;
            }
            pfnAccum(state, cell);
        }
    }
    Value x;
    if (pfnEval) {
        x = pfnEval(state);
    }
    else {
        x = make_num(0);  /* default to zero if no final fn */
    }
    return x;
}

/* Assignment: text may be formula, pure-number, or string */
void set_cell(int c, int r, const char* s) {
    Cell* p = find_cell(c, r);
    if (!p && (!s || !*s)) return; // no cell, no text -> nothing to do

    if (!p) {
        p = new_cell(c, r);
        if (!p) {
            error(errOutOfMemory.str);
            return;
        }
    }
    else if (p->content) {
        if (s && strcmp(p->content, s) == 0) return; // No change -> nothing to do
        free(p->content);
    }
    
    char* txt = (char*)s;
    if (txt) txt = trim(txt);
    if (!txt || !*txt) {
        free_val(&p->cached);
        p->content = NULL;
        p->flags = 0;
        
        remove_deps(p);
        goto reevaluate;
    }

    /* detect formula vs numeric vs string */
    if (*txt == '=') {
        p->flags |= FLG_FORMULA;
        p->content = strdup(txt);
    }
    else if (*txt =='\'') {
        /* string literal */
        p->flags &= MSK_FORMULA; // clear formula flag
        p->content = strdup(txt);
        if (p->content == NULL) {
            error(errOutOfMemory.str);
            return;
        }
    }
    else {
        char* end;
        float v = strtof(txt, &end);
        if (end > txt && *end == 0) {
            /* pure number → treat as single-term formula */
            p->flags |= FLG_FORMULA;
            p->content = malloc(strlen(txt) + 2);
            if (!p->content) {
                error(errOutOfMemory.str);
                return;
            }
            sprintf(p->content, "=%s", txt);
        }
        else {
            /* string */
            p->flags &= MSK_FORMULA;
            p->content = strdup(txt);
            if (p->content == NULL) {
                error(errOutOfMemory.str);
                return;
            }            
        }
    }
    /* rebuild dependencies */
    remove_deps(p);
    if (p->flags & FLG_FORMULA) {
        expr = p->content + 1;
        next_char();
        get_token();
        while (tok_type != tokEnd && tok_type != tokError) {
            const char* ref = &token[0];

            if (tok_type == tokCellRef) {
                /* single cell reference */
                int cc, rr;
                parse_cellref(&ref, &cc, &rr);
                Cell* d = find_cell(cc, rr);
                if (!d) d = new_cell(cc, rr);
                add_dep(p, d);
            }
            else if (tok_type == tokRange) {
                /* range reference */
                int c1, r1, c2, r2;
                parse_range(&ref, &c1, &r1, &c2, &r2);
                Value v = process_range(
                    c1, r1, c2, r2,
                    p,
                    set_range_dep,
                    NULL);
                free_val(&v);
            }
            get_token();
        }
    }

reevaluate:
    is_dirty = 1; // mark spreadsheet dirty
    propagate_dirty(p);
    eval_cell(p); // force evaluation
}

Value parse_expr(char* e) {
    expr = e;
    next_char();
    get_token();
    Value v;
    v = eval_expr();    
    return v;
}

/* Grammar: expr = expr {relop expr} */
Value eval_expr(void) {
    Value v = eval_expr1();
    if (v.type == TYPE_ERROR) return v; // propagate error
    Value res = v;
    while (tok_type == tokEq || tok_type == tokNe || tok_type == tokLt || tok_type == tokLe || tok_type == tokGt || tok_type == tokGe) {
        TokenType op = tok_type;
        get_token();  // skip operator
        Value v2 = eval_expr1();

        if (v2.type == TYPE_ERROR) return v2; // propagate error

        if (v.type == TYPE_NULL && v2.type == TYPE_NULL) {
            res = make_num(1); // nulls are equal            
        }
        else {
            int cmp = 0;
            if (is_str_value(v) && is_str_value(v2)) {
                cmp = strcmp(v.str, v2.str);
            }
            else {
                float n1 = (v.type == TYPE_NUM ? v.num : v.str ? strtof(v.str, NULL) : 0);
                float n2 = (v2.type == TYPE_NUM ? v2.num : v2.str ? strtof(v2.str, NULL) : 0);
                cmp = (n1 < n2 ? -1 : (n1 > n2 ? 1 : 0));
            }
            switch (op) {
                case tokEq:
                    res = make_num((float)(cmp == 0));
                    break;
                case tokNe:
                    res = make_num((float)(cmp != 0));
                    break;
                case tokLt:
                    res = make_num((float)(cmp < 0));
                    break;
                case tokLe:
                    res = make_num((float)(cmp <= 0));
                    break;
                case tokGt:
                    res = make_num((float)(cmp > 0));
                    break;
                case tokGe:
                    res = make_num((float)(cmp >= 0));
                    break;
            }
        }
        free_val(&v); free_val(&v2);
        v = res;  // continue with the result
    }
    return res;
}

/* Grammar: expr = term {(+|-) term} */
Value eval_expr1(void) {
    Value v = eval_term();
    if (v.type == TYPE_ERROR) return v; // propagate error
    Value res = v;
    while (tok_type == tokPlus || tok_type == tokMinus) {
        TokenType op = tok_type;
        get_token();  // skip operator
        Value v2 = eval_term();
        if (v2.type == TYPE_ERROR) return v2; // propagate error
        if (op == tokPlus && (is_str_value(v) || is_str_value(v2))) {
            char buf[CELL_W] = { 0 }, tmp1[CELL_W] = { 0 }, tmp2[CELL_W] = { 0 };
            
            if (is_str_value(v)) {
                if (v.str) strncpy(tmp1, v.str, CELL_W);
            }
            else sprintf(tmp1, "%g", v.num);

            if (is_str_value(v2)) {
                if (v2.str) strncpy(tmp2, v2.str, CELL_W);
            }
            else sprintf(tmp2, "%g", v2.num);

            snprintf(buf, sizeof(buf), "%s%s", tmp1, tmp2);
            res = make_str(buf);
        }
        else {
            float n1 = (v.type == TYPE_NUM ? v.num : v.str ? strtof(v.str, NULL) : 0);
            float n2 = (v2.type == TYPE_NUM ? v2.num : v2.str ? strtof(v2.str, NULL) : 0);
            switch (op) {
                case tokPlus:
                    res = make_num(n1 + n2);
                    break;
                case tokMinus:
                    res = make_num(n1 - n2);
                    break;
            }
        }
        free_val(&v); free_val(&v2);
        v = res;  // continue with the result
    }
    return res;
}

/* term = factor {(*|/) factor} */
Value eval_term(void) {
    Value v = eval_factor();
    if (v.type == TYPE_ERROR) return v; // propagate error
    Value res = v;
    while (tok_type == tokMul || tok_type == tokDiv || tok_type == tokMod) {
        TokenType op = tok_type;
        get_token();  // skip operator
        Value v2 = eval_factor();
        if (v2.type == TYPE_ERROR) return v2; // propagate error
        float n1 = (v.type == TYPE_NUM ? v.num : v.str ? strtof(v.str, NULL) : 0);
        float n2 = (v2.type == TYPE_NUM ? v2.num : v2.str ? strtof(v2.str, NULL) : 0);
        switch (op) {
            case tokMul:
                res = make_num(n1 * n2);
                break;
            case tokDiv:
                if (n2 == 0)
                    res = errExprDivZero;
                else
                    res = make_num(n1 / n2);
                break;
            case tokMod:
                if (n2 == 0)
                    res = errExprDivZero;
                else
                    res = make_num(fmodf(n1, n2));
                break;
        }
        free_val(&v); free_val(&v2);
        v = res;  // continue with the result
    }
    return res;
}

/* factor = num | ref | AVG(range) | '('expr')' */
Value eval_factor(void) {
    Value v = { 0 };
    uint8_t negative = 0;
    while (tok_type == tokMinus || tok_type == tokPlus) {
        if (tok_type == tokMinus) {
            negative = !negative;  // toggle sign
        }
        get_token();  // skip sign
    }

    switch (tok_type) {
        case tokError:
            v = errExprInvalid;
            break;
        case tokLParen: {
            get_token();  // skip '('
            v = eval_expr();
            if (v.type == TYPE_ERROR) break;

            if (!expect_token(tokRParen)) {
                v = errExprExpectRParen;
                break;
            }
        }
                      break;

        case tokNumber: {
            float num = strtof(token, NULL);
            get_token();  // skip number
            v = make_num(num);
        }
                      break;

        case tokCellRef: {
            int cc, rr;
            const char* ref = &token[0];
            parse_cellref(&ref, &cc, &rr);
            Cell* c = find_cell(cc, rr);
            if (c) v = c->cached;
            get_token();  // skip cell reference            
        }
                       break;

        case tokRangeFunc: {
            get_token();

            if (!expect_token(tokLParen)) {
                v = errExprExpectLParen;
                break;
            }

            int c1, r1, c2, r2;
            char* ref = &token[0];
            parse_range(&ref, &c1, &r1, &c2, &r2);            
            get_token();  // skip range

            if (!expect_token(tokRParen)) {
                v = errExprExpectRParen;
                break;
            }

            AccumState acc = { .total = 0, .count = 0 };
            v = process_range(c1, r1, c2, r2, &acc, current_function->pfn_accum, current_function->pfn_eval);
        }
                         break;

        case tokScalarFunc: {
            get_token();  // skip function name
            Function* local_function = current_function;

            if (!expect_token(tokLParen)) {
                v = errExprExpectLParen;
                break;
            }

            Value args[MAX_FUNC_ARGS];
            memset(args, 0, sizeof(args));

            int arg_count = 0;
            while (arg_count < MAX_FUNC_ARGS) {
                Value arg = eval_expr();
                if (arg.type == TYPE_ERROR) {
                    v = arg;  // error to propagate
                    break;
                }
                args[arg_count++] = arg;

                if (tok_type != tokComma) break;
                get_token(); // skip comma                
            }

            if (arg_count < local_function->min_args || arg_count > local_function->max_args) {
                v = errInvalidArg; // invalid number of arguments
                break;
            }


            if (v.type == TYPE_ERROR) {
                break; // propagate error
            }

            v = local_function->pfn_eval(args);

            if (!expect_token(tokRParen)) {
                v = v = errExprExpectRParen;
                break;
            }
        } 
        break;
        
        case tokString:
            v = make_str(token);
            if (v.str == NULL) {
                v = errOutOfMemory; // handle memory allocation failure
            }
            get_token();  // skip string
        break;

        default:
            v = errExprInvalid; // unexpected token
            break;
    }

    if (negative) {
        if (v.type == TYPE_NUM) {
            v.num = -v.num;
        }
    }
    return v;
}

// Evaluate cell and its dependencies, caching the result
void eval_cell(Cell* c) {
    if (!c) {
        return;
    }

    if (!(c->flags & FLG_DIRTY)) {
        return;
    }

    free_val(&c->cached);
    if (!(c->flags & FLG_FORMULA)) {
        if (c->content) {
            c->cached.type = TYPE_TEXT;
            if (*c->content == '\'')
                c->cached.str = c->content + 1; // skip leading quote
            else                            
                c->cached.str = c->content;
        }
        else {
            c->cached.type = TYPE_NULL;
        }
    }
    else if (c->flags & FLG_VISITING) {
        c->cached = errExprCyclicRef;
    }
    else {
        c->flags |= FLG_VISITING;
        if (c->deps) {
            // ensure all dependencies are evaluated
            for (Dep* d = c->deps; d; d = d->next) {                
                eval_cell(d->cell);
            }
        }

        if (c->flags & FLG_DIRTY) {
            Value v = parse_expr(c->content + 1);

            c->cached.type = v.type;
          
            if (v.type == TYPE_STR) { // DO NOT USE is_str_value, it checks for TYPE_TEXT
                c->cached.str = v.str ? strdup(v.str) : NULL;
            }
            else {
                c->cached = v; // propagate non-allocated value
            }
        }
        c->flags &= MSK_VISITING; // reset visiting flag
    }
    c->flags &= (MSK_DIRTY & MSK_VISITING);

    if (c->revdeps) {
        // ensure all reverse dependencies are evaluated
        for (Dep* d = c->revdeps; d; d = d->next) {
            eval_cell(d->cell);
        }
    }
}

typedef enum CommandAction {
    COMMAND_ACTION_NONE,
    COMMAND_ACTION_QUIT,
    COMMAND_ACTION_FAILED,
    COMMAND_ACTION_CANCEL,
    COMMAND_ACTION_YES,
    COMMAND_ACTION_NO,
} CommandAction;

typedef struct {
    const char* short_cut_key;
    const char* description;
    char key;
    CommandAction(*action)(void) MYCC;
} Command;

CommandAction sheet_save(void) MYCC;
CommandAction sheet_goto(void) MYCC;
CommandAction sheet_quit(void) MYCC;

Command commands[] = {
    {"^S", "Save", KEY_SAVE, sheet_save},
    {"^G", "Goto", KEY_GOTO, sheet_goto},
    {"^Q", "Quit", KEY_QUIT, sheet_quit},
    {NULL, NULL, 0, NULL}
};

CommandAction confirm(const char *prompt) MYCC {
    CommandAction retval;

    set_cursor_pos(0, INPUT_LINE_ROW);
    clreol();
    print("%s (y/n)", prompt);
    char ch = getch();
    
    switch (ch) {
        case 'Y':
        case 'y': retval = COMMAND_ACTION_YES; break;
        case KEY_ESC: retval = COMMAND_ACTION_CANCEL; break;
        default: retval = COMMAND_ACTION_NO; break;
    }
    set_cursor_pos(0, INPUT_LINE_ROW);
    clreol();
    return retval;
}

void clrcell(void) {
    for (uint8_t i = 0; i < CELL_W; i++) {
        putch(' ');
    }
}

void print_cell(int col, int row) MYCC {
    int cx, cy;
    cx = 4 + (col - view_c) * (CELL_W + 1);
    cy = 1 + (row - view_r);
    set_cursor_pos(cx, cy);
    if (col == ccol && row == crow) {
        highlight();
    }
    else {
        standard();
    }
    Cell* c = find_cell(col, row);
    if (c && c->content) {
        Value v = c->cached;
        if (v.type == TYPE_ERROR) {
            print("%*s", CELL_W, "<error>");
            if (col == ccol && row == crow) {
                error("Error: %s", v.str);
            }
        }
        else if (v.type == TYPE_NUM) {
            int i = sprintf(ln, "%*g", CELL_W, v.num);
            if (i > CELL_W) {
                sprintf(ln, "%*.5g", CELL_W, v.num);
            }
            prints(ln);
        }
        else if (v.str) {
            int skip_first = 0;
            if (*v.str == '\'') skip_first = 1;
            int i = sprintf(ln, "%*s", CELL_W, v.str+skip_first);
            if (i >= CELL_W) {
                ln[CELL_W] = 0; // truncate to fit
            }
            prints(ln);         
        }
        else
            clrcell();
    }
    else
        clrcell();
    standard();
}

#define HOTKEY_ITEM_WIDTH 12
#define HOTKEY_ITEMS_PER_LINE (int)(SCREEN_WIDTH / HOTKEY_ITEM_WIDTH)

void sheet_print_hotkey(const char* short_cut_key, const char* description) MYCC {
    int len = HOTKEY_ITEM_WIDTH - (strlen(short_cut_key) + strlen(description));
    highlight(); prints(short_cut_key); standard();
    print(" %s", description);
    while (len-- > 0) putch(' ');
}

void sheet_show_hotkeys(void) MYCC {
    set_cursor_pos(0, SCREEN_HEIGHT - 4);
    int i = 0;
    for (Command* cmd = &commands[0]; cmd->short_cut_key != NULL; ++cmd) {
        sheet_print_hotkey(cmd->short_cut_key, cmd->description);
        if (++i % HOTKEY_ITEMS_PER_LINE == 0) putch(NL);
    }
    print("Version: %s", VERSION);
}

void sheet_update_filename(void) MYCC {
    set_cursor_pos(0, SCREEN_HEIGHT - 1);
    highlight();
    char offs = 0;
    if (e_filename && strlen(e_filename) >= SCREEN_WIDTH) {
        offs = strlen(e_filename) - SCREEN_WIDTH;
    }
    print("Filename:%s%s%c", offs ? "..." : "", e_filename ? e_filename + offs : "Untitled", is_dirty ? '*' : ' ');
    standard();
    clreol();
}

const char* get_filename(const char* path) MYCC {
    const char* s = &path[0];
    const char* p = s + strlen(path);
    while (p > s && *(p - 1) != '/' && *(p - 1) != '\\') --p;
    return p;
}

int do_save(void) MYCC {
    status("Saving...");
    strcpy(tmpbuffer, e_filename);
    strcat(tmpbuffer, ".tmp");

    errno = 0;
    void* f = create_file(tmpbuffer);
    if (errno) return errno;

    for (int i = 0; i < CELL_TBL_SIZE; i++) {
        HNode* node = hash_table[i];
        while (node) {
            Cell* c = node->cell;
            if (c->content && *c->content) {
                sprintf(ln, "%c%d:%s\r\n", 'A' + c->col, c->row + 1, c->content);
                write_file(f, ln, strlen(ln));
                if (errno) {
                    close_file(f);
                    return errno;
                }
            }
            node = node->next;
        }
    }
    close_file(f);

    rename_file(tmpbuffer, filename);
    return errno;
}

void do_load(const char* filepath) MYCC {
    status("Loading...");
    errno = 0;
    void* f = open_file(filepath);
    
    if (!errno) {
        int bytes = read_file(f, tmpbuffer, sizeof(tmpbuffer));
        int pos = 0;
        while (bytes > 0) {
            for (int i = 0; i < bytes; i++) {
                if (tmpbuffer[i] == '\n') continue;
                if (tmpbuffer[i] == '\r') {
                    if (pos > 0) {
                        ln[pos] = 0; // terminate line
                        pos = 0; // reset position for next line
                        char* p = ln;
                        char col_ref;
                        int col = -1, row = -1;
                        char* content = NULL;
                        if (sscanf(p, "%c%d:%s", &col_ref, &row, ln) == 3) {
                            col = col_ref - 'A'; // convert to zero-based index
                            row--;               // convert to zero-based index
                            if (col >= 0 && col < MAX_COLS && row >= 0 && row < MAX_ROWS) {
                                set_cell(col, row, ln);
                            }
                        }
                    }
                }
                else if (pos < sizeof(ln) - 1) // prevent overflow
                    ln[pos++] = tmpbuffer[i];
            }
            bytes = read_file(f, tmpbuffer, sizeof(tmpbuffer));
        }
        close_file(f);
    }
    is_dirty = 0; // clear dirty flag
    strcpy(filename, filepath);
    e_filename = &filename[0];
}

CommandAction sheet_save(void) MYCC {
    set_cursor_pos(0, INPUT_LINE_ROW);
    
    const char* p = get_filename(filename);
    if (!edit_line("File name", NULL, p, 250))
        return COMMAND_ACTION_CANCEL;

    e_filename = &filename[0];
    int status = do_save();
    if (status) {
        error("Error saving file: %s", strerror(status));
        return COMMAND_ACTION_FAILED;
    }
    
    is_dirty = 0; // clear dirty flag
    sheet_update_filename();
    return COMMAND_ACTION_NONE;    
}

CommandAction sheet_goto(void) MYCC {
    char input[5] = { 0 };
    set_cursor_pos(0, INPUT_LINE_ROW);
    if (edit_line("Goto cell", NULL, input, sizeof(input) - 1)) {
        int col;
        int row;
        const char* s = input;
        parse_cellref(&s, &col, &row);

        if (col >= 0 && col < MAX_COLS && row >= 0 && row < MAX_ROWS) {
            ccol = col;
            crow = row;
            view_c = ccol < VIEW_COLS ? 0 : ccol - VIEW_COLS + 1;
            view_r = crow < VIEW_ROWS ? 0 : crow - VIEW_ROWS + 1;
            redraw = REDRAW_ALL;
        }
    }
    return COMMAND_ACTION_NONE;
}

CommandAction sheet_quit(void) MYCC {
    if (is_dirty) {
        CommandAction action = confirm("File modified. Save?");
        switch (action) {
            case COMMAND_ACTION_YES:
                if (sheet_save() != COMMAND_ACTION_NONE) {
                    return COMMAND_ACTION_NONE;
                }
                break;
            case COMMAND_ACTION_CANCEL:
                return COMMAND_ACTION_NONE;
        }
        return COMMAND_ACTION_QUIT;
    }
    else if (confirm("Quit?") == COMMAND_ACTION_YES) {
        return COMMAND_ACTION_QUIT;
    }

    return COMMAND_ACTION_NONE;
}

void print_col_headers(void) {
    highlight();
    set_cursor_pos(0, 0);
    prints("    "); 
    for (int cc = 0; cc < VIEW_COLS; cc++) {
        char hdr = 'A' + view_c + cc;
        print(" %*c %*c", CELL_W / 2, hdr, (int)(CELL_W / 2.0f + 0.5) - 1, '|');
    }
    prints("    ");
    standard();
}

void print_row_headers(void) {
    highlight();
    for (int rr = 0; rr < VIEW_ROWS; rr++) {
        set_cursor_pos(0, rr + 1);
        int realr = view_r + rr;
        print("%3d", realr + 1);                
    }
    standard();
}


/* Print viewport */
void print_view(void) {
    has_error = 0; // reset error flag
    set_cursor_pos(0, 0);

    if (redraw == REDRAW_ALL) {
        print_col_headers();
        print_row_headers();
        redraw = REDRAW_CONTENT;
    }

    for (int rr = 0; rr < VIEW_ROWS; rr++) {
        set_cursor_pos(3, rr + 1);
        int realr = view_r + rr;
        for (int cc = 0; cc < VIEW_COLS; cc++) {
            int realc = view_c + cc;
            print_cell(realc, realr);
        }
    }
    
    if (!has_error) {
        set_cursor_pos(0, STATUS_LINE_ROW);
        clreol();
    }
    
    if (is_dirty != was_dirty) {
        was_dirty = is_dirty;
        sheet_update_filename();
    }

    set_cursor_pos(0, INPUT_LINE_ROW);
    highlight();
    print("%c%d:", 'A' + ccol, crow + 1);
    Cell* c = find_cell(ccol, crow);
    if (c && c->content) {
        prints(c->content);
    }
    standard();
    clreol();
}

void move_left(void) {
    if (ccol > 0) ccol--;
    if (ccol < view_c) {
        view_c--;
        redraw = REDRAW_ALL;
    }
}

void move_right(void) {
    if (ccol < MAX_COLS - 1) ccol++;
    if (ccol >= view_c + VIEW_COLS) {
        view_c++;
        redraw = REDRAW_ALL;
    }
}

void move_up(void) {
    if (crow > 0) crow--;
    if (crow < view_r) {
        view_r--;
        redraw = REDRAW_ALL;
    }
}

void move_down(void) {
    if (crow < MAX_ROWS - 1) crow++;
    if (crow >= view_r + VIEW_ROWS) {
        view_r++; 
        redraw = REDRAW_ALL;
    }
}


int main(int argc, char* argv[]) {
    errInvalidArg.str = "Invalid argument";
    errExprInvalid.str = "Invalid expression";
    errExprDivZero.str = "Division by zero";
    errExprCyclicRef.str = "Cyclic reference";
    errExprExpectLParen.str = "Expected '('";
    errExprExpectRParen.str = "Expected ')'";
    errExprExpectNumeric.str = "Expected numeric value";
    errOutOfMemory.str = "Out of memory";

    init();
    screen_init();

    print_view();
    if (argc > 1) {
        do_load(get_lfn(argv[1]));
    }
    else {
        filename[0] = 0;
    }

    sheet_show_hotkeys();
    sheet_update_filename();
    
    for (;;) {
        print_view();
        char ch = getch();
        switch (ch) {
            case KEY_BACKSPACE: set_cell(ccol, crow, NULL); break;
            case KEY_LEFT:move_left(); break;
            case KEY_RIGHT:move_right(); break;
            case KEY_UP: move_up(); break;
            case KEY_DOWN: move_down(); break;
            default:
                if (ch == KEY_ENTER || ch == KEY_ESC || (ch > 31 && ch < 128)) {
                    Cell* c = find_cell(ccol, crow);
                    if (c && c->content) {
                        memset(ln, 0, sizeof(ln));
                        strncpy(ln, c->content, sizeof(ln) - 1);
                    }
                    else {
                        ln[0] = 0;
                    }
                    if (ch > 31 && ch < 128) {
                        ln[0] = ch;
                        ln[1] = 0; // null-terminate                        
                    }

                    set_cursor_pos(0, INPUT_LINE_ROW);
                    char prompt[8];
                    sprintf(prompt, "%c%d", 'A' + ccol, crow + 1);
                    if (edit_line(prompt, NULL, ln, sizeof(ln) - 8)) {
                        set_cell(ccol, crow, ln);
                        move_down();
                    }
                }
                else {
                    for (Command* cmd = (Command*)commands; cmd->short_cut_key != NULL; ++cmd) {
                        if (ch == cmd->key) {
                            standard();
                            if (cmd->action) {
                                CommandAction action = cmd->action();
                                switch (action) {
                                    case COMMAND_ACTION_QUIT:
#ifdef MEMDBG
                                        free_cells();
                                        _CrtDumpMemoryLeaks();
#endif //MEMDBG
                                        return 0;
                                }
                            }
                            break;
                        }
                    }
                }
                break;
        }
    }

    return 0;
}