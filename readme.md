# Spreadsheet

A native spreadsheet for the ZX Spectrum Next.

## Launching the program
Ensure that the `sheet` command is in your `/DOT` folder on the ZX Spectrum Next OS card.

To open an untitled, empty document

`.sheet`

Load an existing document

`.sheet myfile.zsc`

* If `myfile.zsc` exists, it will be loaded.
* If ir does not exist, a new document will be created with that name as the default.

`.zsc` is the recommended file extension, but is not enforced by the program.

## Navigation
Use the cursor keys to move between cells:

|Key|Action|
|---|------|
|`⇦`|Move left one cell|
|`⇨`|Move right one cell|
|`⇧`|Move up one cell|
|`⇩`|Move down one cell|

## Extend Mode commands
Activate Extend Mode by pressing the `Extend Mode` key or `CTRL`+`SHIFT` in CSpect, followed by the command key:

|Key|Description|
|---|-----------|
| `↑S` Save | Saves the current document (recommend using `.zsc` file extention) |
| `↑G` Goto | Moves directly to a specified cell | 
| `↑Q` Quit | Exits the editor. You will be prompted to save if the document has unsaved changes. |

`↑` indicates the Extended Mode modifier (`Extend Mode` key or `CTRL`+`SHIFT`)

## Entering data
Data entry works like a traditional spreadsheet. Navigate to the desired cell and start typing.

* Numeric entries are treated as numbers.
* All other entries are treated as text.
* To enter a formula, start the entry with an `=`.

To edit the contents of a cell, press `Enter` or the `Edit` key. While editing, you can cancel the operation by pressing `Edit`. 

Pressing `Enter` while editing a cell will update the value of the cell to the new value and move one cell down.

All function names and cell references are case-insensitive ie. `A1` and `a1` both refer to cell `A1` in the sheet.

### Examples:
|Entry|Description|
|-----|-----------|
| `=A1` | References the value in cell A1 |
| `=A1*2` | Will show a value double that of the value in cell A1 |

## Formulas and Function

Expressions in formulas use standard precedence rules (BODMAS/PEMDAS) and allow references like `A1`, `B2` etc. 

To operate on a range of cells, use the format `A1:B2` (top-left to bottom-right).

### Example:

Add all values in A1, A2, B1 and B2
`=sum(A1:B2)`

### Range functions
|Function|Description|
|--------|-----------|
| `SUM` | Returns the **sum** of cell values in a range: =sum(A1:B3) |
| `AVG` | Returns the **average** of numeric values in a range: =avg(A1:B3) |
| `COUNT` | Returns how many cells contain values in a range: =count(A1:B3) |
| `MIN` | Returns the **smallest** numeric value in a range: =min(A1:B3) |
| `MAX` | Return the **largest** numeric value in a range: =min(A1:B3) |

### Simple functions
These take numeric literals of cell references.

### Example

Sine of 2.3 radian

`=sin(2.3)` 

Sine of the value in A1

`=sin(A1)` 

|Function|Description|
|--------|-----------|
| `SIN` | Sine of a radian value |
| `COS` | Cosine of a radian value |
| `TAN` | Tangent of a radian value |
| `ASIN` | Inverse sine (returns angle in radians) |
| `ACOS` | Inverse cosine (returns angle in radians) |
| `ATAN` | Inverse tangent (returns angle in radians) | 
| `ABS` | Absolute value |
| `CEIL` | Ceiling (rounds up) |
| `FLOOR` | Floor (rounds down) |
| `ROUND` | Rounds to nearest integer |
| `TRUNC` | Truncates decimal part |
| `SQRT` | Square root |
| `EXP` | Exponential (eⁿ) |
| `LOG` | Natural logarithm |
| `LOG10` | Logarithm base 10 |
| `LOG2` | Logarithm base 2 |