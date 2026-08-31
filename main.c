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
#include <limits.h>

#include "platform.h"
#include "crtio.h"
#include "sheet.h"

#define VERSION "0.4a"

typedef enum { REDRAW_ALL, REDRAW_CONTENT, REDRAW_CURSOR } REDRAW_MODE;

char ln[80];
char* e_filename = NULL;
uint8_t is_dirty = 0;
uint8_t was_dirty = 0;
uint8_t has_error = 0; // set if error occurred during evaluation
REDRAW_MODE redraw = REDRAW_ALL;

Value errInvalidArg = { .type = TYPE_ERROR };
Value errExprInvalid = { .type = TYPE_ERROR };
Value errExprDivZero = { .type = TYPE_ERROR };
Value errExprCyclicRef = { .type = TYPE_ERROR };
Value errExprExpectLParen = { .type = TYPE_ERROR };
Value errExprExpectRParen = { .type = TYPE_ERROR };
Value errExprExpectNumeric = { .type = TYPE_ERROR };
Value errExprExpectString = { .type = TYPE_ERROR };
Value errOutOfMemory = { .type = TYPE_ERROR };

// Forward declarations
Value  make_num(float v) MYCC;
Value  make_str(const char* s) MYCC;

void free_val(Value *v) MYCC;
void error(const char* fmt, ...) MYCC;
void status(const char* fmt, ...) MYCC;
uint8_t get_overlapped_text(int target_col, int row, char* out, int out_size);

typedef struct HNode {
    Cell* cell;
    struct HNode* next;
} HNode;

uint8_t is_str_value(Value *v) {
    return v->type == TYPE_STR || v->type == TYPE_TEXT;
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
    tokCellRef, tokRange, tokNumber, tokString, tokCurrentCell,
    tokEq, tokNe, tokLt, tokLe, tokGt, tokGe, 
    tokPlus, tokMinus, tokMul, tokDiv, tokMod, tokPow, tokLParen, tokRParen, tokComma, tokEnd,
    tokScalarFunc,
    tokRangeFunc, tokConditionalRangeFunc,
    tokLookupFunc,

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
void countif_range(void* state, Cell* cell);
void sumif_range(void* state, Cell* cell);
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
Value lookup_eval(void* state);

Function functions[] = {
    { "SUM", sum_range, sum_eval, tokRangeFunc, 1, 1},
    { "AVG", sum_range, avg_eval, tokRangeFunc, 1, 1 },
    { "COUNT", count_range, count_eval, tokRangeFunc, 1, 1 },
    { "MAX", max_range, best_eval, tokRangeFunc, 1, 1 },
    { "MIN", min_range, best_eval, tokRangeFunc, 1, 1 },
    { "COUNTIF", countif_range, count_eval, tokConditionalRangeFunc, 2, 2 },
    { "SUMIF", sumif_range, sum_eval, tokConditionalRangeFunc, 2, 2 },
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
    { "LEN", NULL, len_eval, tokScalarFunc, 1, 1},
    { "LEFT", NULL, left_eval, tokScalarFunc, 2, 2},
    { "RIGHT", NULL, right_eval, tokScalarFunc, 2, 2},
    { "MID", NULL, mid_eval, tokScalarFunc, 3, 3},
    { "LOOKUP", NULL, lookup_eval, tokLookupFunc, 2, 3},

    { NULL, NULL }  /* sentinel */
};

TokenType tok_type;
char token[32];
char* expr;
char ch;
Function* current_function;

void error(const char* fmt, ...) MYCC {
    uint8_t ox, oy;

    has_error = 1;
    get_cursor_pos(&ox, &oy);
    va_list args;
    va_start(args, fmt);
    char fmtbuf[80];
    strncpy(fmtbuf, fmt ? fmt : "", sizeof(fmtbuf) - 1);
    fmtbuf[sizeof(fmtbuf) - 1] = 0;
    vsnprintf(ln, sizeof(ln), fmtbuf, args);
    va_end(args);
    set_cursor_pos(0, STATUS_LINE_ROW);
    prints(ln); clreol();
    set_cursor_pos(ox, oy);

    if (fmt == errOutOfMemory.str) getch();
}

void status(const char* fmt, ...) MYCC {
    uint8_t ox, oy;

    get_cursor_pos(&ox, &oy);
    va_list args;
    va_start(args, fmt);
    char fmtbuf[80];
    strncpy(fmtbuf, fmt ? fmt : "", sizeof(fmtbuf) - 1);
    fmtbuf[sizeof(fmtbuf) - 1] = 0;
    vsnprintf(ln, sizeof(ln), fmtbuf, args);
    va_end(args);
    set_cursor_pos(0, STATUS_LINE_ROW);
    prints(ln); clreol();
    set_cursor_pos(ox, oy);
}

/* Case-insensitive text comparison */
uint8_t cmp_text_equal(const char* left, const char* right) MYCC {
    if (!left || !right) return left == right;
    while (*left && *right) {
        if (toupper((unsigned char)*left) != toupper((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == *right;
}

float value_to_num(Value v) MYCC {
    if (v.type == TYPE_NUM) return v.num;
    if (!v.str) return 0;

    char numbuf[80];
    strncpy(numbuf, v.str, sizeof(numbuf) - 1);
    numbuf[sizeof(numbuf) - 1] = 0;

    float parsed = 0;
    if (sscanf(numbuf, "%f", &parsed) == 1) {
        return parsed;
    }
    return 0;
}

uint8_t parse_int_base(const char* s, int base, int* out) MYCC {
    if (!s || !*s) return 0;

    int sign = 1;
    if (*s == '-') {
        sign = -1;
        ++s;
    }

    int value = 0;
    int digits = 0;
    while (*s) {
        char ch = (char)toupper((unsigned char)*s);
        int digit;
        if (ch >= '0' && ch <= '9') {
            digit = ch - '0';
        }
        else if (ch >= 'A' && ch <= 'F') {
            digit = 10 + (ch - 'A');
        }
        else {
            return 0;
        }

        if (digit >= base) return 0;

        if (value > (INT_MAX - digit) / base) return 0;

        value = value * base + digit;
        ++digits;
        ++s;
    }

    if (digits == 0) return 0;

    *out = value * sign;
    return 1;
}

void next_char(void) MYCC {
    ch = *expr;
    if (ch == 0) return;
    ++expr;
}

typedef struct {
    int col;
    int row;
    uint8_t col_anchor;
    uint8_t row_anchor;
    size_t len;
} CellRefInfo;

uint8_t is_cellref_token_char(char c) MYCC {
    return (uint8_t)(isalpha((unsigned char)c) || isdigit((unsigned char)c) || c == '$');
}

uint8_t scan_cellref(const char* s, CellRefInfo* out) MYCC {
    const char* p = s;
    int parsed_col;
    int parsed_row = 0;
    int has_digit = 0;
    uint8_t col_anchor = 0;
    uint8_t row_anchor = 0;

    if (*p == '$') {
        col_anchor = 1;
        ++p;
    }

    if (!isalpha((unsigned char)*p)) {
        return 0;
    }

    parsed_col = toupper((unsigned char)*p++) - 'A';
    if (parsed_col < 0 || parsed_col >= MAX_COLS) {
        return 0;
    }

    if (*p == '$') {
        row_anchor = 1;
        ++p;
    }

    while (isdigit((unsigned char)*p)) {
        int digit = *p++ - '0';
        has_digit = 1;
        if (parsed_row > (MAX_ROWS - digit) / 10) {
            return 0;
        }
        parsed_row = parsed_row * 10 + digit;
    }

    if (!has_digit || parsed_row < 1 || parsed_row > MAX_ROWS) {
        return 0;
    }

    if (out) {
        out->col = parsed_col;
        out->row = parsed_row - 1;
        out->col_anchor = col_anchor;
        out->row_anchor = row_anchor;
        out->len = (size_t)(p - s);
    }
    return 1;
}

void parse_cellref(const char** sp, int* col, int* row) MYCC;
void parse_range(const char** sp, int* from_col, int* from_row, int* to_col, int* to_row) MYCC;

uint8_t is_cellref(const char* s) {
    CellRefInfo ref;
    return scan_cellref(s, &ref) && s[ref.len] == 0;
}

void get_token(void) {
    tok_type = tokNone;

    while (isspace((unsigned char)ch)) next_char();
    if (ch == 0) {
        tok_type = tokEnd;
        return;
    }
    if (isalpha((unsigned char)ch) || ch == '$') {
        int i = 0;
        while (is_cellref_token_char(ch)) {
            if (i < sizeof(token) - 1)
                token[i++] = toupper((unsigned char)ch);
            next_char();
        }
        token[i] = 0;
        if (is_cellref(token)) {
            tok_type = tokCellRef;
            if (ch == ':') {
                token[i++] = ch; // include ':' in token
                next_char(); // skip ':'
                char* s = token + i;
                while (is_cellref_token_char(ch)) {
                    if (i < sizeof(token) - 1)
                        token[i++] = toupper((unsigned char)ch);
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
    else if (isdigit((unsigned char)ch)) {
        int i = 0;
        while (isdigit((unsigned char)ch)) {
            if (i < sizeof(token) - 1)
                token[i++] = ch;
            next_char();
        }
        if (ch == '.') {
            if (i < sizeof(token) - 1)
                token[i++] = ch;
            next_char();
            while (isdigit((unsigned char)ch)) {
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
            case '?':
                token[0] = '?';
                token[1] = 0;
                tok_type = tokCurrentCell;
                next_char();
                break;
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
            case '^': tok_type = tokPow; next_char(); break;
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

/* Globals */
static int view_r = 0, view_c = 0;
static int ccol = 0, crow = 0;
static int prev_ccol = 0, prev_crow = 0;
static int last_move_dc = 0, last_move_dr = 0;
static char* clipboard_content = NULL;
static int clipboard_col = -1;
static int clipboard_row = -1;
static Cell* condition_cell = NULL;

/* Helpers for Value */
Value make_num(float v) MYCC {
    Value x; x.type = TYPE_NUM; x.num = v; return x;
}

Value make_str(const char* s) MYCC {
    Value x; x.type = TYPE_STR;
    if (s) x.str = strdup(s); else x.str = NULL;
    return x;
}

void free_val(Value *v) MYCC {
    if (v->type == TYPE_STR && v->str) {
        free((void*)v->str);
    }
    v->type = TYPE_NULL;
    v->str = NULL;
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
    if (owner == NULL || dep == NULL) {
        error(errOutOfMemory.str);
        return;
    }

    Dep* d = malloc(sizeof * d);
    if (d == NULL) {
        error(errOutOfMemory.str);
        return;
    }

    Dep* r = malloc(sizeof * r);
    if (r == NULL) {
        free(d);
        error(errOutOfMemory.str);
        return;
    }

    d->cell = dep;
    d->next = owner->deps;
    owner->deps = d;

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
void eval_cell_core(Cell* c);
Value eval_expr(void);
Value eval_expr1(void);
Value eval_term(void);
Value eval_power(void);
Value eval_factor(void);
Value parse_expr(char* e);
Value process_conditional_range(
    int c1, int r1, int c2, int r2,
    const char* condition,
    void* state,
    PFN_ACCUM pfn_accum,
    PFN_EVAL pfn_eval);

void parse_cellref(const char** sp, int* col, int* row) MYCC {
    CellRefInfo ref;
    *col = -1;
    *row = -1;

    if (!scan_cellref(*sp, &ref)) {
        return;
    }

    *sp += ref.len;
    *col = ref.col;
    *row = ref.row;
}

void parse_range(const char** sp, int* from_col, int* from_row, int* to_col, int* to_row) MYCC {
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

void countif_range(void* state, Cell* cell) {
    AccumState* acc = (AccumState*)state;
    (void)cell;
    acc->count++;
}

void sumif_range(void* state, Cell* cell) {
    AccumState* acc = (AccumState*)state;
    if (cell->cached.type == TYPE_NUM) {
        acc->total += cell->cached.num;
    }
}

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
    Value v = make_num(arg.num >= 0 ? floorf(arg.num + 0.5f) : ceilf(arg.num - 0.5f));
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
    if (!is_str_value(&arg)) return errInvalidArg;

    int n;
    if (!parse_int_base(arg.str, 2, &n)) return errExprInvalid;

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
    if (!is_str_value(&arg)) return errInvalidArg;

    int n;
    if (!parse_int_base(arg.str, 16, &n)) return errExprInvalid;

    Value v = make_num((float)n);
    return v;
}

Value if_eval(void* state) {
    Value* args = (Value*)state;
    uint8_t condition_met = 0;

    if (args[0].type == TYPE_NULL) {
        condition_met = 2; // return false branch
    } else if (args[0].type == TYPE_NUM) {
        condition_met = args[0].num ? 1 : 2; // return true or false branch
    } else if (is_str_value(&args[0])) {
        condition_met = args[0].str && *args[0].str ? 1 : 2; // non-empty string is true
    } else {
        return errExprInvalid; // unsupported type for condition
    }

    Value v = args[condition_met];
    if (v.type == TYPE_STR) {
        Value copy = make_str(v.str);
        if (v.str != NULL && copy.str == NULL) {
            return errOutOfMemory;
        }
        return copy;
    }
    return v;
}

Value len_eval(void* state) {
    Value arg = *(Value*)state;
    if (!is_str_value(&arg)) return errExprExpectString;
    int length = arg.str ? strlen(arg.str) : 0;
    Value v = make_num((float)length);
    return v;
}

Value left_eval(void* state) {
    Value* args = (Value*)state;
    if (!is_str_value(&args[0]) || args[1].type != TYPE_NUM) return errExprInvalid;

    int n = (int)args[1].num;
    if (n < 0) return errExprInvalid;

    char* s = args[0].str ? args[0].str : "";
    size_t len = strlen(s);
    if ((size_t)n > len) n = (int)len;

    char* result_str = malloc(n + 1);
    if (!result_str) return errOutOfMemory;

    strncpy(result_str, s, n);
    result_str[n] = '\0';

    Value v = make_str(result_str);
    free(result_str);
    return v;
}

Value right_eval(void* state) {
    Value* args = (Value*)state;
    if (!is_str_value(&args[0]) || args[1].type != TYPE_NUM) return errExprInvalid;

    int n = (int)args[1].num;
    if (n < 0) return errExprInvalid;

    char* s = args[0].str ? args[0].str : "";
    size_t len = strlen(s);
    if ((size_t)n > len) n = (int)len;

    char* result_str = malloc(n + 1);
    if (!result_str) return errOutOfMemory;

    strncpy(result_str, s + len - n, n);
    result_str[n] = '\0';

    Value v = make_str(result_str);
    free(result_str);
    return v;
}

Value mid_eval(void* state) {
    Value v = {.type = TYPE_NULL};

    Value* args = (Value*)state;
    if (!is_str_value(&args[0]) || args[1].type != TYPE_NUM || args[2].type != TYPE_NUM) return errExprInvalid;
    int start = (int)args[1].num;
    int length = (int)args[2].num;
    if (start < 1 || length < 0) return errExprInvalid;
    char* s = args[0].str ? args[0].str : "";
    size_t len = strlen(s);
    
    if ((size_t)(start - 1) > len) return v; // start is beyond the end of the string

    if ((size_t)length > len - (start - 1)) length = (int)(len - (start - 1));

    char* result_str = malloc(length + 1);
    if (!result_str) return errOutOfMemory;

    strncpy(result_str, s + start - 1, length);
    result_str[length] = '\0';

    v = make_str(result_str);
    free(result_str);
    return v;
}

typedef struct {
    Value value;
    int range1_c1;
    int range1_r1;
    int range1_c2;
    int range1_r2;
    int range2_c1;
    int range2_r1;
    int range2_c2;
    int range2_r2;
    uint8_t has_second_range;
} LookupArgs;

Value lookup_copy_value(Value source) {
    Value result = source;
    if (is_str_value(&source)) {
        result = make_str(source.str);
        if (source.str && !result.str) result = errOutOfMemory;
    }
    return result;
}

Value lookup_eval(void* state) {
    LookupArgs* args = (LookupArgs*)state;
    Cell empty = { 0 };
    Value result = {.type = TYPE_NULL};    
    int range2_height = args->range2_r2 - args->range2_r1 + 1;
    int index = 0;

    for (int cc = args->range1_c1; cc <= args->range1_c2; cc++) {
        for (int rr = args->range1_r1; rr <= args->range1_r2; rr++) {
            Cell* cell = find_cell(cc, rr);
            if (!cell) {
                empty.col = cc;
                empty.row = rr;
                cell = &empty;
            }
            else {
                eval_cell(cell);
            }

            Value candidate = cell->cached;
            uint8_t matched = 0;

            if (args->value.type == TYPE_NUM && candidate.type == TYPE_NUM) {
                matched = args->value.num == candidate.num;
            }
            else if (is_str_value(&args->value) && is_str_value(&candidate)) {
                matched = cmp_text_equal(args->value.str, candidate.str);
            }

            if (matched) {
                if (args->has_second_range) {
                    int range2_size = (args->range2_c2 - args->range2_c1 + 1) * range2_height;
                    if (index < range2_size) {
                        int range2_col = args->range2_c1 + index / range2_height;
                        int range2_row = args->range2_r1 + index % range2_height;
                        Cell* result_cell = find_cell(range2_col, range2_row);
                        if (!result_cell) {
                            empty.col = range2_col;
                            empty.row = range2_row;
                            result_cell = &empty;
                        }
                        else {
                            eval_cell(result_cell);
                        }
                        result = lookup_copy_value(result_cell->cached);
                    }
                }
                else {
                    result = lookup_copy_value(candidate);
                }
                return result;
            }

            index++;
        }
    }
    return result;
}
    
void add_range_deps(Cell* root, int c1, int r1, int c2, int r2) {
    for (int cc = c1; cc <= c2; cc++) {
        for (int rr = r1; rr <= r2; rr++) {
            Cell* cell = find_cell(cc, rr);
            if (!cell) cell = new_cell(cc, rr);
            if (cell) add_dep(root, cell);
        }
    }
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
            else {
                eval_cell(cell);
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

Value process_conditional_range(
    int c1, int r1, int c2, int r2,
    const char* condition,
    void* state,
    PFN_ACCUM pfn_accum,
    PFN_EVAL pfn_eval) {

    Cell empty = { 0 };
    Cell* previous_condition_cell = condition_cell;
    empty.flags = FLG_EMPTY;

    for (int cc = c1; cc <= c2; cc++) {
        for (int rr = r1; rr <= r2; rr++) {
            Cell* cell = find_cell(cc, rr);
            if (!cell) {
                empty.col = cc;
                empty.row = rr;
                cell = &empty;
            }
            else {
                eval_cell(cell);
            }

            condition_cell = cell;
            Value result = parse_expr((char*)condition);
            if (result.type == TYPE_ERROR) {
                condition_cell = previous_condition_cell;
                return result;
            }
            if (result.type == TYPE_NUM && result.num != 0) {
                pfn_accum(state, cell);
            }
            free_val(&result);
        }
    }

    condition_cell = previous_condition_cell;

    Value result = pfn_eval(state);
    return result;
}

void reset_cell_state(Cell* p) MYCC {
    free_val(&p->cached);
    p->content = NULL;
    p->flags = 0;
    remove_deps(p);
}

/* Assignment: text may be formula, pure-number, or string */
void set_cell(int c, int r, const char* s) MYCC {
    Cell* p = find_cell(c, r);
    char* txtbuf = NULL;

    const char* txt_start = s;
    const char* txt_end = s;
    size_t txt_len = 0;

    if (s) {
        while (*txt_start && isspace((unsigned char)*txt_start)) {
            ++txt_start;
        }
        txt_end = txt_start + strlen(txt_start);
        while (txt_end > txt_start && isspace((unsigned char)*(txt_end - 1))) {
            --txt_end;
        }
        txt_len = (size_t)(txt_end - txt_start);
    }

    if (!p && txt_len == 0) return; // no cell, no text -> nothing to do

    if (!p) {
        p = new_cell(c, r);
        if (!p) {
            error(errOutOfMemory.str);
            return;
        }
    }
    else if (p->content) {
        size_t prev_len = strlen(p->content);
        if (txt_len == prev_len && (txt_len == 0 || strncmp(p->content, txt_start, txt_len) == 0)) {
            return; // No change -> nothing to do
        }
        free(p->content);
        p->content = NULL;
    }

    if (txt_len == 0) {
        reset_cell_state(p);
        goto reevaluate;
    }

    txtbuf = malloc(txt_len + 1);
    if (txtbuf == NULL) {
        error(errOutOfMemory.str);
        reset_cell_state(p);
        return;
    }
    memcpy(txtbuf, txt_start, txt_len);
    txtbuf[txt_len] = 0;

    /* detect formula vs numeric vs string */
    if (*txtbuf == '=') {
        p->flags |= FLG_FORMULA;
        p->content = txtbuf;
        txtbuf = NULL;
    }
    else if (*txtbuf =='\'') {
        /* string literal */
        p->flags &= MSK_FORMULA; // clear formula flag
        p->content = txtbuf;
        txtbuf = NULL;
    }
    else {
        char* end;
        float v = strtof(txtbuf, &end);
        if (end > txtbuf && *end == 0) {
            /* pure number → treat as single-term formula */
            p->flags |= FLG_FORMULA;
            p->content = malloc(txt_len + 2);
            if (!p->content) {
                error(errOutOfMemory.str);
                reset_cell_state(p);
                free(txtbuf);
                return;
            }
            snprintf(p->content, txt_len + 2, "=%s", txtbuf);
        }
        else {
            /* string */
            p->flags &= MSK_FORMULA;
            p->content = txtbuf;
            txtbuf = NULL;
        }

        (void)v;
    }

    free(txtbuf);

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
                if (cc < 0 || cc >= MAX_COLS || rr < 0 || rr >= MAX_ROWS) {
                    get_token();
                    continue;
                }
                Cell* d = find_cell(cc, rr);
                if (!d) d = new_cell(cc, rr);
                if (!d) {
                    error(errOutOfMemory.str);
                    break;
                }
                add_dep(p, d);
            }
            else if (tok_type == tokRange) {
                /* range reference */
                int c1, r1, c2, r2;
                parse_range(&ref, &c1, &r1, &c2, &r2);
                if (c1 >= 0 && c1 < MAX_COLS && c2 >= 0 && c2 < MAX_COLS &&
                    r1 >= 0 && r1 < MAX_ROWS && r2 >= 0 && r2 < MAX_ROWS) {
                    add_range_deps(p, c1, r1, c2, r2);
                }
            }
            get_token();
        }
    }

reevaluate:
    is_dirty = 1; // mark spreadsheet dirty
    redraw = REDRAW_CONTENT;
    propagate_dirty(p);
    eval_cell(p); // force evaluation
}

Value parse_expr(char* e) {
    expr = e;
    next_char();
    get_token();
    Value v;
    v = eval_expr();
    if (v.type == TYPE_ERROR) return v;

    // Reject formulas that parsed only a valid prefix but have extra junk at the end
    if (tok_type != tokEnd) {
        free_val(&v);
        return errExprInvalid;
    }

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

        if (v.type == TYPE_NULL || v2.type == TYPE_NULL) {
            res = make_num((float)(v.type == TYPE_NULL && v2.type == TYPE_NULL));
        }
        else if (is_str_value(&v) || is_str_value(&v2)) {
            int cmp = 0;
            if (is_str_value(&v) && is_str_value(&v2)) {
                cmp = strcmp(v.str, v2.str);
            }
            else {
                cmp = 1;
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
        else {
            float n1 = value_to_num(v);
            float n2 = value_to_num(v2);
            int cmp = (n1 < n2 ? -1 : (n1 > n2 ? 1 : 0));
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
        if (op == tokPlus && (is_str_value(&v) || is_str_value(&v2))) {
            char buf[CELL_W] = { 0 }, tmp1[CELL_W] = { 0 }, tmp2[CELL_W] = { 0 };
            
            if (is_str_value(&v)) {
                if (v.str) strncpy(tmp1, v.str, CELL_W - 1);
            }
            else snprintf(tmp1, sizeof(tmp1), "%g", v.num);

            if (is_str_value(&v2)) {
                if (v2.str) strncpy(tmp2, v2.str, CELL_W - 1);
            }
            else snprintf(tmp2, sizeof(tmp2), "%g", v2.num);

            snprintf(buf, sizeof(buf), "%s%s", tmp1, tmp2);
            res = make_str(buf);
        }
        else {
            float n1 = value_to_num(v);
            float n2 = value_to_num(v2);
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

/* term = power {(*|/) power} */
Value eval_term(void) {
    Value v = eval_power();
    if (v.type == TYPE_ERROR) return v; // propagate error
    Value res = v;
    while (tok_type == tokMul || tok_type == tokDiv || tok_type == tokMod) {
        TokenType op = tok_type;
        get_token();  // skip operator
        Value v2 = eval_power();
        if (v2.type == TYPE_ERROR) return v2; // propagate error
        float n1 = value_to_num(v);
        float n2 = value_to_num(v2);
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

/* power = factor {^ factor} */
Value eval_power(void) {
    Value v = eval_factor();
    if (v.type == TYPE_ERROR) return v; // propagate error
    Value res = v;
    while (tok_type == tokPow) {
        get_token();  // skip operator
        Value v2 = eval_factor();
        if (v2.type == TYPE_ERROR) return v2; // propagate error
        float n1 = value_to_num(v);
        float n2 = value_to_num(v2);
        res = make_num(powf(n1, n2));
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

        case tokCurrentCell:
            if (condition_cell) {
                eval_cell(condition_cell);
                if (condition_cell->cached.type == TYPE_STR) {
                    v = make_str(condition_cell->cached.str);
                    if (condition_cell->cached.str && !v.str) {
                        v = errOutOfMemory;
                    }
                }
                else {
                    v = condition_cell->cached;
                }
            }
            else {
                v.type = TYPE_NULL;
            }
            get_token();
            break;

        case tokCellRef: {
            int cc, rr;
            const char* ref = &token[0];
            parse_cellref(&ref, &cc, &rr);
            if (cc < 0 || cc >= MAX_COLS || rr < 0 || rr >= MAX_ROWS) {
                v = errExprInvalid;
                get_token();
                break;
            }
            Cell* c = find_cell(cc, rr);
            if (c) {
                eval_cell(c);
                if (c->cached.type == TYPE_STR) {
                    v = make_str(c->cached.str);
                    if (c->cached.str != NULL && v.str == NULL) {
                        v = errOutOfMemory;
                    }
                }
                else {
                    v = c->cached;
                }
            }
            get_token();  // skip cell reference            
        }
                       break;

        case tokConditionalRangeFunc: {
            Function* conditional_function = current_function;
            get_token();

            if (!expect_token(tokLParen)) {
                v = errExprExpectLParen;
                break;
            }

            if (tok_type != tokRange) {
                v = errExprInvalid;
                break;
            }

            int c1, r1, c2, r2;
            const char* ref = &token[0];
            parse_range(&ref, &c1, &r1, &c2, &r2);
            get_token();

            if (tok_type != tokComma) {
                v = errExprInvalid;
                break;
            }

            /* Capture the condition from the source, before token state changes. */
            const char* condition_start = expr - 1;
            while (isspace((unsigned char)*condition_start)) condition_start++;
            get_token();

            /* Capture the condition, including nested function calls. */
            const char* condition_end = condition_start;
            int depth = 0;
            char quote = 0;

            while (*condition_end) {
                if (quote) {
                    if (*condition_end == quote) quote = 0;
                }
                else if (*condition_end == '\'' || *condition_end == '"') {
                    quote = *condition_end;
                }
                else if (*condition_end == '(') {
                    depth++;
                }
                else if (*condition_end == ')') {
                    if (depth == 0) break;
                    depth--;
                }
                condition_end++;
            }

            if (*condition_end != ')') {
                v = errExprExpectRParen;
                break;
            }

            size_t condition_len = (size_t)(condition_end - condition_start);
            char* condition = malloc(condition_len + 1);
            if (!condition) {
                v = errOutOfMemory;
                break;
            }
            memcpy(condition, condition_start, condition_len);
            condition[condition_len] = 0;

            AccumState acc = { 0, 0, 0 };
            v = process_conditional_range(
                c1, r1, c2, r2,
                condition,
                &acc,
                conditional_function->pfn_accum,
                conditional_function->pfn_eval);

            free(condition);
            expr = (char*)condition_end + 1;
            next_char();
            get_token();
        }
        break;

        case tokLookupFunc: {
            LookupArgs lookup;
            memset(&lookup, 0, sizeof(lookup));
            Function* lookup_function = current_function;

            get_token();
            if (!expect_token(tokLParen)) {
                v = errExprExpectLParen;
                break;
            }

            {
                Value value = eval_expr();
                lookup.value = value;
            }
            if (lookup.value.type == TYPE_ERROR) {
                v = lookup.value;
                break;
            }
            if (!expect_token(tokComma) || tok_type != tokRange) {
                v = errInvalidArg;
                free_val(&lookup.value);
                break;
            }

            {
                const char* ref = &token[0];
                parse_range(&ref, &lookup.range1_c1, &lookup.range1_r1,
                    &lookup.range1_c2, &lookup.range1_r2);
            }
            get_token();

            if (tok_type == tokComma) {
                lookup.has_second_range = 1;
                get_token();
                if (tok_type != tokRange) {
                    v = errInvalidArg;
                    goto lookup_cleanup;
                }
                {
                    const char* ref = &token[0];
                    parse_range(&ref, &lookup.range2_c1, &lookup.range2_r1,
                        &lookup.range2_c2, &lookup.range2_r2);
                }
                get_token();
            }

            if (!expect_token(tokRParen)) {
                v = errExprExpectRParen;
                goto lookup_cleanup;
            }
            {
                Value result = lookup_function->pfn_eval(&lookup);
                v = result;
            }

lookup_cleanup:
            free_val(&lookup.value);
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
                goto scalar_cleanup;
            }


            if (v.type == TYPE_ERROR) {
                goto scalar_cleanup; // propagate error
            }

            v = local_function->pfn_eval(args);

            if (!expect_token(tokRParen)) {
                v = errExprExpectRParen;
            }

scalar_cleanup:
            for (int i = 0; i < arg_count; i++) {
                free_val(&args[i]);
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

// Evaluate cell and its dependencies, caching the result (lazy for reverse deps)
void eval_cell_core(Cell* c) {
    if (!c) {
        return;
    }

    if (!(c->flags & FLG_DIRTY)) {
        return;
    }

    if (c->flags & FLG_VISITING) {
        // cycle detected
        c->cached = errExprCyclicRef;
        return;
    }

    c->flags |= FLG_VISITING;

    if (c->deps) {
        // ensure all dependencies are evaluated first
        for (Dep* d = c->deps; d; d = d->next) {
            eval_cell(d->cell);
        }
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
    else {
        Value v = parse_expr(c->content + 1);
        c->cached = v; // takes ownership of TYPE_STR allocations returned by parser
    }

    c->flags &= MSK_VISITING; // reset visiting
    c->flags &= MSK_DIRTY; // mark evaluated
}

/* Cell evaluation parses nested formulas, so preserve the caller's lexer. */
void eval_cell(Cell* c) {
    TokenType saved_tok_type = tok_type;
    char saved_token[sizeof(token)];
    char* saved_expr = expr;
    char saved_ch = ch;
    Function* saved_function = current_function;

    memcpy(saved_token, token, sizeof(token));
    eval_cell_core(c);
    memcpy(token, saved_token, sizeof(token));
    tok_type = saved_tok_type;
    expr = saved_expr;
    ch = saved_ch;
    current_function = saved_function;
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
CommandAction sheet_copy(void) MYCC;
CommandAction sheet_paste(void) MYCC;

char* translate_formula_for_paste(const char* formula, int dcol, int drow);

Command commands[] = {
    {"^S", "Save", KEY_SAVE, sheet_save},
    {"^G", "Goto", KEY_GOTO, sheet_goto},
    {"^C", "Copy", KEY_COPY, sheet_copy},
    {"^V", "Paste", KEY_PASTE, sheet_paste},
    {"^Q", "Quit", KEY_QUIT, sheet_quit},
    {NULL, NULL, 0, NULL}
};

char* translate_formula_for_paste(const char* formula, int dcol, int drow) {
    if (!formula) return NULL;

    size_t len = strlen(formula);
    size_t cap = len * 2 + 16;
    char* out = malloc(cap);
    if (!out) {
        error(errOutOfMemory.str);
        return NULL;
    }

    size_t i = 0;
    size_t j = 0;

    while (i < len) {
        CellRefInfo ref;

        if (formula[i] == '\'' || formula[i] == '"') {
            char quote = formula[i];
            out[j++] = formula[i++];
            while (i < len) {
                char current = formula[i++];
                out[j++] = current;
                if (current == quote) {
                    break;
                }
            }
            continue;
        }

        if (scan_cellref(formula + i, &ref) &&
            (i == 0 || !(isalpha((unsigned char)formula[i - 1]) ||
                         isdigit((unsigned char)formula[i - 1]) ||
                         formula[i - 1] == '$'))) {
            int new_col = ref.col;
            int new_row = ref.row;

            if (!ref.col_anchor) new_col += dcol;
            if (!ref.row_anchor) new_row += drow;

            if (new_col < 0 || new_col >= MAX_COLS ||
                new_row < 0 || new_row >= MAX_ROWS) {
                free(out);
                error(errExprInvalid.str);
                return NULL;
            }

            if (ref.col_anchor) out[j++] = '$';
            out[j++] = (char)('A' + new_col);
            if (ref.row_anchor) out[j++] = '$';
            j += (size_t)snprintf(out + j, cap - j, "%d", new_row + 1);
            i += ref.len;
            continue;
        }

        out[j++] = formula[i++];
    }

    out[j] = 0;
    return out;
}

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

int print_cell(int col, int row, Cell* c) MYCC {
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

    if (c) {
        eval_cell(c);  // lazy update on render path
    }

    int span_cols = 1;

    if (!(c && c->content)) {
        goto render_empty;
    }

    {
        Value v = c->cached;
        if (v.type == TYPE_ERROR) {
            print("%*s", CELL_W, "<error>");
            if (col == ccol && row == crow) {
                error("Error: %s", v.str ? v.str : "Unknown error");
            }
        }
        else if (v.type == TYPE_NUM) {
            int written = snprintf(ln, sizeof(ln), "%*g", CELL_W, v.num);
            if (written < 0 || written > CELL_W) {
                snprintf(ln, sizeof(ln), "%*.5g", CELL_W, v.num);
            }
            prints(ln);
        }
        else if (v.str) {
            int skip_first = 0;
            if (*v.str == '\'') skip_first = 1;
            const char* text = v.str + skip_first;
            size_t text_len = strlen(text);

            if (text_len == 0) {
                goto render_empty;
            }

            if (text_len > CELL_W) {
                /* Span across empty adjacent cells until edge or occupied cell */
                int max_col = view_c + VIEW_COLS - 1;
                int extra_cols = 0;
                int total_width = CELL_W;

                for (int next_col = col + 1; next_col <= max_col; next_col++) {
                    Cell* neighbour = find_cell(next_col, row);
                    if (neighbour && neighbour->content && *neighbour->content) {
                        break; // stop at first occupied neighbour
                    }
                    extra_cols++;
                    total_width += (CELL_W + 1); // include the inter-cell spacing
                }

                int chars_to_print = (text_len < (size_t)total_width) ? (int)text_len : total_width;
                print("%.*s", chars_to_print, text);

                if (chars_to_print < total_width) {
                    for (int i = chars_to_print; i < total_width; i++) {
                        putch(' ');
                    }
                }

                span_cols = 1 + extra_cols;
            }
            else {
                snprintf(ln, sizeof(ln), "%*.*s", CELL_W, CELL_W, text);
                prints(ln);
            }
        }
        else {
            goto render_empty;
        }
    }

    putch(' ');
    standard();
    return span_cols;

render_empty:
    /* Keep empty and evaluated-empty rendering identical */
        clrcell();
        standard();
        putch(' ');
        return span_cols;
}

uint8_t get_overlapped_text(int target_col, int row, char* out, int out_size) {
    if (!out || out_size <= 0) return 0;
    out[0] = '\0';

    int src_col = target_col - 1;
    Cell* src = NULL;

    while (src_col >= 0) {
        src = find_cell(src_col, row);
        if (src && src->content && *src->content) {
            break;
        }
        src_col--;
    }

    if (!src || src_col < 0) return 0;

    eval_cell(src);
    Value v = src->cached;
    if (!is_str_value(&v)) return 0;

    const char* text = v.str;
    if (!text) return 0;
    if (*text == '\'') text++;

    size_t text_len = strlen(text);
    if (text_len <= CELL_W) return 0;

    int block_col = src_col + 1;
    while (block_col < MAX_COLS) {
        Cell* n = find_cell(block_col, row);
        if (n && n->content && *n->content) break;
        block_col++;
    }

    int max_span_col = block_col - 1;
    if (target_col > max_span_col) return 0;

    int span_index = target_col - src_col;
    size_t start_index = (size_t)span_index * (CELL_W + 1);
    if (start_index >= text_len) return 0;

    int copy_len = (int)(text_len - start_index);
    if (copy_len > out_size - 1) copy_len = out_size - 1;

    memcpy(out, text + start_index, copy_len);
    out[copy_len] = '\0';
    return 1;
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
    int offs = 0;
    char ellipsis[] = "...";
    char empty[] = "";
    char untitled[] = "Untitled";
    char format[] = "Filename:%s%s%c";
    char* filepart = e_filename ? e_filename + offs : untitled;
    if (e_filename && strlen(e_filename) >= SCREEN_WIDTH) {
        offs = strlen(e_filename) - SCREEN_WIDTH;
        filepart = e_filename + offs;
    }
    print(format, offs ? ellipsis : empty, filepart, is_dirty ? '*' : ' ');
    standard();
    clreol();
}

char* get_filename(char* path) MYCC {
    char* s = &path[0];
    char* p = s + strlen(path);
    while (p > s && *(p - 1) != '/' && *(p - 1) != '\\') --p;
    return p;
}

int do_save(void) MYCC {
    status("Saving...");
    char tmpfmt[] = "%s.tmp";
    char empty[] = "";
    int tmpname_len = snprintf(tmpbuffer, sizeof(tmpbuffer), tmpfmt, e_filename ? e_filename : empty);
    if (tmpname_len < 0 || tmpname_len >= (int)sizeof(tmpbuffer)) {
        return EINVAL;
    }

    errno = 0;
    void* f = create_file(tmpbuffer);
    if (errno) return errno;

    for (int i = 0; i < CELL_TBL_SIZE; i++) {
        HNode* node = hash_table[i];
        while (node) {
            Cell* c = node->cell;
            if (c->content && *c->content) {
                char prefix[12];
                int prefix_len = snprintf(prefix, sizeof(prefix), "%c%d:", 'A' + c->col, c->row + 1);
                if (prefix_len < 0) {
                    close_file(f);
                    return EIO;
                }

                write_file(f, prefix, (size_t)prefix_len);
                if (errno) {
                    close_file(f);
                    return errno;
                }

                write_file(f, c->content, strlen(c->content));
                if (errno) {
                    close_file(f);
                    return errno;
                }

                write_file(f, "\r\n", 2);
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

void load_line(char* p) {
    char* sep = strchr(p, ':');
    CellRefInfo ref;

    if (!(sep && sep > p + 1 && isalpha((unsigned char)p[0]))) {
        return;
    }

    if (!scan_cellref(p, &ref) || p + ref.len != sep) {
        return;
    }

    int col = ref.col;
    int row = ref.row;
    char* content = sep + 1;

    if (col >= 0 && col < MAX_COLS && row >= 0 && row < MAX_ROWS) {
        set_cell(col, row, content);
    }
}

void do_load(const char* filepath) MYCC {
    status("Loading...");
    void* f = open_file(filepath);

    if (f != NULL) {
        int bytes = read_file(f, tmpbuffer, sizeof(tmpbuffer));
        int pos = 0;
        while (bytes > 0) {
            for (int i = 0; i < bytes; i++) {
                if (tmpbuffer[i] == '\r' || tmpbuffer[i] == '\n') {
                    if (pos > 0) {
                        ln[pos] = 0; // terminate line
                        load_line(ln);
                        pos = 0; // reset position for next line
                    }
                }
                else if (pos < sizeof(ln) - 1) // prevent overflow
                    ln[pos++] = tmpbuffer[i];
            }
            bytes = read_file(f, tmpbuffer, sizeof(tmpbuffer));
        }

        if (pos > 0) {
            ln[pos] = 0; // process last line even without trailing CR/LF
            load_line(ln);
        }

        close_file(f);
    }

    is_dirty = 0; // clear dirty flag
    strncpy(filename, filepath, 250);
    filename[250] = 0;
    e_filename = &filename[0];
}

CommandAction sheet_save(void) MYCC {
    set_cursor_pos(0, INPUT_LINE_ROW);
    
    char* p = get_filename(filename);
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

CommandAction sheet_copy(void) MYCC {
    Cell* src;
    char* copied;

    src = find_cell(ccol, crow);
    if (!src || !src->content || !*src->content) {        
        return COMMAND_ACTION_NONE;
    }

    free(clipboard_content);
    clipboard_content = NULL;
    clipboard_col = -1;
    clipboard_row = -1;

    copied = strdup(src->content);
    if (!copied) {
        error(errOutOfMemory.str);
        return COMMAND_ACTION_NONE;
    }

    clipboard_content = copied;
    clipboard_col = ccol;
    clipboard_row = crow;
    return COMMAND_ACTION_NONE;
}

CommandAction sheet_paste(void) MYCC {
    char* to_write = NULL;
    int dcol = 0;
    int drow = 0;
    int pasted_col = ccol;
    int pasted_row = crow;
    int next_col;
    int next_row;

    if (!clipboard_content || clipboard_col < 0 || clipboard_row < 0) {
        return COMMAND_ACTION_NONE;
    }

    if (clipboard_content[0] == '=') {
        dcol = ccol - clipboard_col;
        drow = crow - clipboard_row;
        to_write = translate_formula_for_paste(clipboard_content, dcol, drow);
    }
    else {
        to_write = strdup(clipboard_content);
        if (!to_write) {
            error(errOutOfMemory.str);
            return COMMAND_ACTION_NONE;
        }
    }

    if (!to_write) {
        return COMMAND_ACTION_NONE;
    }

    set_cell(ccol, crow, to_write);
    free(to_write);
    if (last_move_dc != 0 || last_move_dr != 0) {
        next_col = ccol + last_move_dc;
        next_row = crow + last_move_dr;

        if (next_col >= 0 && next_col < MAX_COLS && next_row >= 0 && next_row < MAX_ROWS) {
            prev_ccol = ccol;
            prev_crow = crow;
            ccol = next_col;
            crow = next_row;

            if (ccol < view_c) {
                view_c = ccol;
                redraw = REDRAW_ALL;
            }
            else if (ccol >= view_c + VIEW_COLS) {
                view_c = ccol - VIEW_COLS + 1;
                redraw = REDRAW_ALL;
            }

            if (crow < view_r) {
                view_r = crow;
                redraw = REDRAW_ALL;
            }
            else if (crow >= view_r + VIEW_ROWS) {
                view_r = crow - VIEW_ROWS + 1;
                redraw = REDRAW_ALL;
            }
        }
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

void print_view_cell(int realc, int realr) {
    Cell* c = find_cell(realc, realr);
    if (c && c->content) {
        (void)print_cell(realc, realr, c);
        return;
    }

    char spill[CELL_W + 2] = {0};
    if (get_overlapped_text(realc, realr, spill, sizeof(spill))) {
        int cx = 4 + (realc - view_c) * (CELL_W + 1);
        int cy = 1 + (realr - view_r);
        char gap = spill[CELL_W] ? spill[CELL_W] : ' ';
        set_cursor_pos(cx, cy);
        if (realr == crow && realc == ccol) highlight(); else standard();
        snprintf(ln, sizeof(ln), "%-*.*s", CELL_W, CELL_W, spill);
        prints(ln);
        putch(gap);
        standard();
        return;
    }

    int cx = 4 + (realc - view_c) * (CELL_W + 1);
    int cy = 1 + (realr - view_r);
    set_cursor_pos(cx, cy);
    if (realr == crow && realc == ccol) highlight(); else standard();
    clrcell();
    putch(' ');
    standard();
}


/* Print viewport */
void print_view(void) {
    static int input_ccol = -1;
    static int input_crow = -1;

    has_error = 0; // reset error flag
    set_cursor_pos(0, 0);

    REDRAW_MODE draw_mode = redraw;

    if (draw_mode == REDRAW_ALL) {
        print_col_headers();
        print_row_headers();
        draw_mode = REDRAW_CONTENT;
    }

    if (draw_mode == REDRAW_CONTENT) {
        for (int rr = 0; rr < VIEW_ROWS; rr++) {
            int realr = view_r + rr;

            uint8_t covered[VIEW_COLS] = {0};
            for (int cc = 0; cc < VIEW_COLS; cc++) {
                int realc = view_c + cc;

                if (covered[cc]) {
                    if (realr == crow && realc == ccol) {
                        print_view_cell(realc, realr);
                    }
                    continue;
                }

                Cell* c = find_cell(realc, realr);
                if (c && c->content) {
                    int cell_span = print_cell(realc, realr, c);
                    for (int k = 1; k < cell_span && (cc + k) < VIEW_COLS; k++) {
                        covered[cc + k] = 1;
                    }
                    continue;
                }

                print_view_cell(realc, realr);
            }
        }
    }
    else if (draw_mode == REDRAW_CURSOR) {
        if (prev_ccol >= view_c && prev_ccol < view_c + VIEW_COLS &&
            prev_crow >= view_r && prev_crow < view_r + VIEW_ROWS) {
            print_view_cell(prev_ccol, prev_crow);
        }
        if ((ccol != prev_ccol || crow != prev_crow) &&
            ccol >= view_c && ccol < view_c + VIEW_COLS &&
            crow >= view_r && crow < view_r + VIEW_ROWS) {
            print_view_cell(ccol, crow);
        }
    }

    redraw = REDRAW_CURSOR;
    prev_ccol = ccol;
    prev_crow = crow;
    
    if (!has_error) {
        set_cursor_pos(0, STATUS_LINE_ROW);
        clreol();
    }
    
    if (is_dirty != was_dirty) {
        was_dirty = is_dirty;
        sheet_update_filename();
    }

    if (draw_mode != REDRAW_CURSOR || ccol != input_ccol || crow != input_crow) {
        set_cursor_pos(0, INPUT_LINE_ROW);
        highlight();
        print("%c%d:", 'A' + ccol, crow + 1);
        Cell* c = find_cell(ccol, crow);
        if (c && c->content) {
            prints(c->content);
        }
        standard();
        clreol();
        input_ccol = ccol;
        input_crow = crow;
    }
}

void move_cursor(int dc, int dr) MYCC {
    int nc = ccol + dc;
    int nr = crow + dr;

    if (nc < 0 || nc >= MAX_COLS || nr < 0 || nr >= MAX_ROWS) {
        return;
    }

    prev_ccol = ccol;
    prev_crow = crow;
    ccol = nc;
    crow = nr;
    last_move_dc = dc;
    last_move_dr = dr;

    if (ccol < view_c) {
        view_c = ccol;
        redraw = REDRAW_ALL;
    }
    else if (ccol >= view_c + VIEW_COLS) {
        view_c = ccol - VIEW_COLS + 1;
        redraw = REDRAW_ALL;
    }

    if (crow < view_r) {
        view_r = crow;
        redraw = REDRAW_ALL;
    }
    else if (crow >= view_r + VIEW_ROWS) {
        view_r = crow - VIEW_ROWS + 1;
        redraw = REDRAW_ALL;
    }
}

void move_left(void) {
    move_cursor(-1, 0);
}

void move_right(void) {
    move_cursor(1, 0);
}

void move_up(void) {
    move_cursor(0, -1);
}

void move_down(void) {
    move_cursor(0, 1);
}


int main(int argc, char* argv[]) {
    errInvalidArg.str = "Invalid argument";
    errExprInvalid.str = "Invalid expression";
    errExprDivZero.str = "Division by zero";
    errExprCyclicRef.str = "Cyclic reference";
    errExprExpectLParen.str = "Expected '('";
    errExprExpectRParen.str = "Expected ')'";
    errExprExpectNumeric.str = "Expected numeric";
    errExprExpectString.str = "Expected string";
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
                    snprintf(prompt, sizeof(prompt), "%c%d", 'A' + ccol, crow + 1);
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

}