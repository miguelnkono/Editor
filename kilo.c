/// Left at " Hide the cursor when repainting. "


/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

/// struct that is going to all our editor state.
struct editorConfig {
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;

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

char editorReadKey() {
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    return c;
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

/// To draw tidles on the left hand side of the screen.
void editorDrawRows(struct abuf *ab) {
    int y;
    for(y = 0; y < E.screenrows; y++) {
        //write(STDOUT_FILENO, "~", 1);
        abAppend(ab, "~",  1);

        if (y < E.screenrows - 1) {
            //write(STDOUT_FILENO, "\r\n", 2);
            abAppend(ab, "\r\n",  2);
        }
    }
}

void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    //write(STDOUT_FILENO, "\x1b[2J", 4);
    //write(STDOUT_FILENO, "\x1b[H", 3);
    abAppend(&ab, "\x1b[2J", 4);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    //write(STDOUT_FILENO, "\x1b[H", 3);
    abAppend(&ab, "\x1b[H", 3);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** init ***/

void initEditor() {
    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
}

void editorProcessKeypress() {
    char c = editorReadKey();

    switch(c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(1);
            break;
    }
}

int main() {
    enableRawMode();
    initEditor();

    while(1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
