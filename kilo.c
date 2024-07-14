/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.4"
#define KILO_TAB_STOP 8

#define CTRL_KEY(k) ((k) & 0x1f)

// Directions Keys.
enum editorKey {
    BACKSPACE = 127 ,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/

// Data type to store a row of text.
typedef struct erow {
    int size;       // The length of the row
    int rsize;      // the length of the caractere to render
    char *chars;    // the caractere which is on the line
    char *render;   // the caractere to render on the screen
} erow;

/// struct that is going to all our editor state.
struct editorConfig {
    int cx, cy, rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;  // For low-level stuff "Raw Mode"
};

// The state of the editor.
struct editorConfig E;

/*** prototypes  ***/
void editorSetStatusMessage(const char *fmt, ...);

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[1;1H", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP |IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /// setting the control characters
    raw.c_cc[VMIN] = 0;     /// set the minimum number of bytes of input needed before read() can return.
    raw.c_cc[VTIME] = 1;    /// set the maximum amount of time to wait before read() returns. It is in milliseconds.

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// Reading a key.
int editorReadKey() {
    int nread; // numbers of bits of key read.
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {

            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) 
                    return '\x1b';
                if (seq[2] == '~') {
                    switch(seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch(seq[1]) {
                    case 'A': return ARROW_UP;   // Up.
                    case 'B': return ARROW_DOWN;   // Down.
                    case 'C': return ARROW_RIGHT;   // Right.
                    case 'D': return ARROW_LEFT;   // Left.
                    case 'H': return HOME_KEY;  // Home key
                    case 'F': return END_KEY;   // End key
                }
            }
        } else if (seq[0] == 'O') {
            switch(seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;


    if (write(STDOUT_FILENO, "\x1[6n", 4) != 4) return -1;

    printf("\r\n");

    while(i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

   if (buf[0] != '\x1b' || buf[1] != '[') return -1;
   if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

   return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

// To convert a chars filed column to a render column. (cx -> rx).
int editorRowCxToRx(erow *row, int cx) {
    int j;
    int rx = 0;
    for(j = 0; j < cx; j++) 
    {
        if (row->chars[j] == '\t') {
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

void editorUpdateRow(erow *row)
{
    int tabs = 0;
    int j;
    // Check for tab.
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }

    // free the memory of the the row->render.
    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    // Copie the content of row->chars into row->render.
    int idx = 0;
    for (j = 0; j < row->size; j++) 
    {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while(idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) *(E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    // for the content to be render.
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
}

/*
* A function to insert a single character at a given position into a erow. 
* params : 
*   at -> the position where to insert
*   c -> the caracter to insert
*   row -> the row to insert into.
*/
void editorRowInsertChar (erow *row, int at, int c)
{
    // Check than we don't insert at wrong position.
    if (at < 0 || at > row->size)
    {
        at = row->size;
    }

    // Change the lenght of the row data.
    row->chars = realloc(row->chars, row->size + 2);
    // Move the text forward by one.
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    // Update the lenght of the line.
    row->size++;
    // Put the caracter.
    row->chars[at] = c;
    editorUpdateRow(row);
}

/* editor operations. */
/*
 * A function to be call in editorProcessKeypress() to insert a character.
 * params :
 *  c -> the caracter to be insert.
 */
void editorInsertChar (int c)
{
    // Add a new line if neccesary.
    if (E.cy == E.numrows)
    {
        editorAppendRow("", 0);
    }
    // Insert the caracter c at the E.cx of line E.row[E.cy].
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    // Increment the number of the column.
    E.cx++;
}

/*** file i/o ***/
/*
 * Function to convert our array of erow into a single big string so than we can save it to the disk.
 */
char *editorRowsToString(int *buflen)
{
    int totlen = 0; // to compute the total amount of the caracters.
    int j;

    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;  // [p] point to [buf]

    for (j = 0; j < E.numrows; j++)
    {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename)
{
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");
    

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    //linelen = getline(&line, &linecap, fp); 
    // read a line in fp.
    while ((linelen = getline(&line, &linelen, fp)) != -1) {
        // We remove the '\n' and '\r'
        while(linelen > 0 && 
                (line[linelen - 1] == '\n' ||
                 line[linelen - 1] == '\r'))
            linelen--;
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
}

/*
* Function to save the content of the file on the disk.
*/
void editorSave()
{
    if (E.filename == NULL)
        return;

    int len;
    char *buf = editorRowsToString(&len);

    // Open the store in E.filename.
    // O_CREAT -> To create a new file if it's doesn't already exist.
    // O_RDWR -> Open the file for reading and writing.
    // [0644] -> Give permission to the owner of the file to read and write into the file, for the others they just have the permision to read.
    int fd = open(E.filename, O_RDWR || O_CREAT, 0644);
    if (fd != -1)
    {
        // Set the file's size to the specified length.
        if (ftruncate(fd, len) != -1)
        {
            // Write the content of [buf] into [fd]
            if (write(fd, buf, len) == len)
            {
                // Close [fd]
                close(fd);
                // Free the memory allocate by but.
                free(buf);
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save I/O error: %s", strerror(errno));
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

/// Constructor for our dynamic string type.
#define ABUF_INIT {NULL, 0}

/// Ading a new string.
void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if(new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab-> len += len;
}

/// Descontructor for freeing the memory.
void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** output ***/

/// To be able to scroll.
void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

/// To draw tidles on the left hand side of the screen.
void editorDrawRows(struct abuf *ab) {
    int y;
    for(y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            // Display the welcome message only if the user
            // open the editor without any argument as input.
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), 
                    "KILO editor -- version %s", KILO_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;
            
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while(padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else 
            {
                abAppend(ab, "~", 1);
            }
        } else {
           int len = E.row[filerow].rsize - E.coloff;
           if (len < 0) len = 0;
           abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        //<esc>[K -> clear one line at the time.
        abAppend(ab, "\x1b[K", 3);
        //if (y < E.screenrows - 1) {
        abAppend(ab, "\r\n",  2);
        //}
    }
}

/// Function to draw a status bar.
void editorDrawStatusBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[7m", 4);
    
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
            E.filename ? E.filename : "[No Name]", E.numrows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);

    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);

    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        } else 
        {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) -  E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    /// (h, l) commands are used to turn on and turn off varius terminal features or "modes".
    // So "l" it's for hiding and "h" for showing.
    // We use here ?25l -> for hiding the cursor and
    // ?25h -> to show it again.
    abAppend(&ab, "\x1b[?25l", 6);
    /*abAppend(&ab, "\x1b[2J", 4);*/  /// 2J -> clear the entire screen.
                                  //<esc>[K -> clear one line at the time.
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);       /// To draw the status bar.
    editorDrawMessageBar(&ab);

    // Move the cursor to the position stored in E.cx and E.cy.
    // \x1b[H is for the cursor.
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", 
            (E.cy - E.rowoff) + 1, 
            (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    //abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, "\x1b[?25h", 6);  

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

/// Allow the user to move the cursor around.
void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch(key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;

        case ARROW_RIGHT:
            if (row && E.cx < row->size){
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            //E.cx++;
            break;

        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;

        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();

    switch(c) {
        // The ENTER key.
        case '\r':
            /* TODO */
            break;

        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(1);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

            /* For now we simplt make the HOME_KEY and the
             * END_KEY move the cursor left and right.*/
        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            //E.cx = E.screencols - 1;
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;

        // All handle the behavior of backspace.
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            /* TODO */
            break;

            /* For now PAGE_UP and PAGE_DOWN simply move the
             * cursor to the top or the bottom of the page.*/
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) 
                {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN)
                {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) 
                        E.cy = E.numrows;
                }

                int times = E.screenrows;
                while(times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        // Ctlr + l is traditional use to refresh the screen of terminal programs.
        case CTRL_KEY('l'):
        case '\x1b':
            /* TODO */
            break;

        default:
            editorInsertChar(c);
            break;
        }
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP:Ctrl-S = save | Ctrl-Q = quit");

    while(1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
