/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f) //The Ctrl-key strips bits 5&6 of any character, we are mirroring that
#define ABUF_INIT {NULL, 0}
#define KILO_VERSION "0.0.1"

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT ,
    ARROW_UP ,
    ARROW_DOWN,
    DEL_KEY,
    PAGE_UP,
    PAGE_DOWN
}; //No HOME, END keys, they don't work for my PC

/*** data ***/

/* Editor row, stores a line of text as pointer to dynamically allocate character data and length */
typedef struct erow {
    int size;
    char *chars;
} erow;

struct editorConfig { //data about the default terminal
    int cx, cy;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    struct termios orig_termios; //original terminal configs
};
struct editorConfig E;

/*** terminal ***/

/**
 * @brief Called when we need to quit the program
 *
 * Quit the program by clearing the screen and resetting cursor location
 *
 * @param s Error message to be print
 */
void die(const char *s) {
    //clear the screen and set cursor at position 1,1 on exit
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s); //from stdio, to print error
    exit(1); //from stdlib, to exit safely
}

/**
 * @brief Disable raw mode, i.e without the default terminal settings
 */
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

/**
 * @brief Disable certain flags according to our application
 */
void enableRawMode() {
    //get the original terminal attributes so we can reset it once we quit
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); //Disable Ctrl + W, S or Q and other miscellaneous flags
    raw.c_oflag &= ~(OPOST); //Disable "\n" and "\r\n"
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    //control characters from termios
    raw.c_cc[VMIN] = 0; //Minimum no. of bytes before read() can return
    raw.c_cc[VTIME] = 1; //Max amount of time to wait before read() returns

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/**
 * @brief Read a single key press
 * @return The character read
 */
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if(c == '\x1b') { // If it's an escape character, we read two more bytee
        char seq[3]; //size 3 to handle longer escape sequences

        if( read( STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if( read( STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if( seq[0] == '[' ) {
            if( seq[1] >= '0' && seq[1] <= '9') {
                if( read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if( seq[2] == '~' ) {
                    switch (seq[1]) {
                        case '3':
                            return DEL_KEY; // <esc>[3~
                        case '5':
                            return  PAGE_UP; //<esc>[5~ is the esc seq for PAGE UP
                        case '6':
                            return PAGE_DOWN;// //<esc>[6~
                    }
                }
            }
            else {
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D' :
                        return ARROW_LEFT;
                }
            }
        }

        return '\x1b'; //return escape key
    } else {
        return c;
    }
}

/**
 * @brief Get the terminal cursor position using Cursor Position Report
 * @param rows The rows pointer which will have the number of rows
 * @param cols The columns pointer which will have the number of columns
 * @return 0 if successful -1 if failed
 */
int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;//request Cursor Position Report
    while (i < sizeof(buf) - 1) {//read STD-IN
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1; //check for escape \x1b
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1; //parse data into row, cols
    return 0;
}

/**
 * @brief Get the window by 1. Request from IO-control 2. (brute-force) moving cursor to 999-999 and read location
 * @param rows
 * @param cols
 * @return
 */
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // move the cursor 999 down and 999 right to reach the bottom right corner
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows = 1; //set to 1 to indicate erow containsa line that should be displayed
}


/*** file i/o ***/

void editorOpen( char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    linelen = getline(&line, &linecap, fp); //read a line from file pointer, linecap = memory allocated
    if (linelen != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorAppendRow(line, linelen);

    }
    free(line);
    fclose(fp);
}



/*** append buffer ***/
struct abuf {
    char *b;
    int len;
};

/**
 * @brief Append the abuf struct with number of chars defined in len
 * @param ab    The struct that stores the chars
 * @param s     The char string to be appended
 * @param len   Length of the string to be appended
 */
void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len); //reallocate the memory because we're appending char(s)

    if (new == NULL) return; //if realloc failed, else append
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

/**
 * Free the memory that was used to store strings
 * @param ab    The struct that stores char string
 */
void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

/**
 * Draw each row with a `tilda`, welcome message too, not every row
 * @param ab    The struct with char string
 */
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        if (y >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome),
                                           "Kilo editor -- version %s", KILO_VERSION); //like sprintf, but n = MAX char
                if (welcome_len > E.screencols) welcome_len = E.screencols;
                int padding = (E.screencols - welcome_len) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcome_len);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[y].size;
            if(len > E.screencols) len = E.screencols;
            abAppend(ab, E.row[y].chars, len);
        }


        abAppend(ab, "\x1b[K", 3);//erase current line
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

/**
 * Refresh the screen
 * by, hide cursor->go to pos 1,1->draw rows->go to pos 1,1->reset hide cursor->finally write from buffer
 */
void editorRefreshScreen() {

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); //hide cursor while refreshing screen
    /*we write an escape sequence into the terminal, an escape seq starts with a "escp" + "["
    / 2J means clear the entire screen. If it was 1J clear uptill the cursor. J is for clear screen
    / To use the max functionality of a terminal use ncurses
     removed as we decided to remove a line at a time*/
    // abAppend(&ab, "\x1b[2J", 4); //Write 4 bytes, \x1b = escape character, clear screen
    abAppend(&ab, "\x1b[H", 3); //Cursor position, use <esc>[12;40H to go to position 12,40

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1); //the terminal index are 1-n not 0-n
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); //reset hide cursor

    write(STDOUT_FILENO, ab.b, ab.len); //write what's in the buffer
    abFree(&ab);

}

/*** input ***/

/**
 * Move the cursor with WASD inputs
 * @param key Key Press
 */
void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if(E.cx != 0)               { E.cx--; }
            break;
        case ARROW_RIGHT:
            if(E.cx != E.screencols -1) { E.cx++; }
            break;
        case ARROW_UP:
            if(E.cy != 0)               { E.cy--; }
            break;
        case ARROW_DOWN:
            if(E.cy != E.screenrows -1) { E.cy++; }
            break;
    }
}

/**
 * Get window size to init editor
 */
void initEditor() {
    E.cx = 0; //Horizontal coordiate of the cursor
    E.cy = 0; //Vertical coordinate of the cusrsor
    E.numrows = 0;
    E.row = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

/**
 * Get each key and process it to do defined behaviour
 */
void editorProcessKeypress() {
    int c = editorReadKey();
    switch (c) {
        case CTRL_KEY('q'):
            //clear screen and set cursor at 1,1 on exit
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case PAGE_UP:
        case PAGE_DOWN:
        {
            int times = E.screenrows;
            while (times--)
                editorMoveCursor( c == PAGE_UP ? ARROW_UP: ARROW_DOWN);
        }
        break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

    }
}

/*** init ***/

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]); //Opening and reading a file
    }


    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}