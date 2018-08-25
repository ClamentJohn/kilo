/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f) //The Ctrl-key strips bits 5&6 of any character, we are mirroring that
#define ABUF_INIT {NULL, 0}
#define KILO_VERSION "0.0.1"

/*** data ***/

struct editorConfig { //data about the default terminal
    int screenrows;
    int screencols;
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
 * @return c The character read
 */
char editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    return c;
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
 * Draw each raw with a `tilda`, welcome message too, not every row
 * @param ab    The struct with char string
 */
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        if (y == E.screenrows / 3) {
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome),
                                      "Kilo editor -- version %s", KILO_VERSION); //like sprintf, but n = MAX char
            if (welcomelen > E.screencols) welcomelen = E.screencols;
            abAppend(ab, welcome, welcomelen);
        } else {
            abAppend(ab, "~", 1);
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

    abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, "\x1b[?25h", 6); //reset hide cursor

    write(STDOUT_FILENO, ab.b, ab.len); //write what's in the buffer
    abFree(&ab);

}

/*** input ***/

/**
 * Get window size to init editor
 */
void initEditor() {
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

/**
 * Get each key and process it to do defined behaviour
 */
void editorProcessKeypress() {
    char c = editorReadKey();
    switch (c) {
        case CTRL_KEY('q'):
            //clear screen and set cursor at 1,1 on exit
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);

            exit(0);
            break;
    }
}

/*** init ***/

int main() {
    enableRawMode();
    initEditor();

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}