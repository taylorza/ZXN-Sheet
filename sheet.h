#ifndef SHEET_H__
#define SHEET_H__

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

struct Cell;

typedef enum { TYPE_NULL, TYPE_NUM, TYPE_STR, TYPE_TEXT, TYPE_ERROR } ValType;

/* Generic value returned by evaluator */
typedef struct Value {
    ValType type;
    union {
        float  num;
        const char* str;     // allocated if TYPE_STR, not allocated if TYPE_TEXT or TYPE_ERROR
    };
} Value;

typedef struct Dep {
    struct Cell* cell;
    struct Cell* owner;
    struct Dep* next;
    struct Dep* revnext;
} Dep;

// One spreadsheet cell
typedef struct Cell {
    Dep* deps;          // cells this cell references
    Dep* revdeps;       // cells referencing this cell

    uint8_t col, row;
    char* content;      // raw text
    Value cached;

    uint8_t flags;
} Cell;

#endif // SHEET_H__