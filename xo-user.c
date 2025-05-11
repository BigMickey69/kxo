#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "game.h"
#include "gamecount.h"
#include "log.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

#define REFRESH_INTERVAL (100000 - 100)  // adjusting for better accuracy
char clock_str[32];
struct {
    char move[3];
} move_logs[MAX_GAMES][N_GRIDS];
static int log_count[MAX_GAMES];
static int load_O_tenths[MAX_GAMES];
static int load_X_tenths[MAX_GAMES];


#include "gamecount.h"
char table_buf[MAX_GAMES][DRAWBUFFER_SIZE];
char empty_board[DRAWBUFFER_SIZE] =
    " | | | \n"
    "-------\n"
    " | | | \n"
    "-------\n"
    " | | | \n"
    "-------\n"
    " | | | \n"
    "-------\n";

// Exit code after 3 seconds
#include <signal.h>
#define ALARM_TIME -1
void handle_alarm(int sig)
{
    printf("\nTime's up! %d seconds.\n", ALARM_TIME);
    exit(0);
}


static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}


/* coroutine */
#include "coro.h"
#define MAX_COROS 3
#define MAX_MOVES N_GRIDS
static bool pause_attr, end_attr;
int coro_count = 0;
static coro_t coros[MAX_COROS];
static jmp_buf sched_env;
static coro_t *current_coro;

static int device_fd;

static void update_board_and_stats(unsigned const char buf[READ_DATA_SIZE])
{
    if (buf[0] & 0b10000000) {
        printf("Games have ended! Resesting boards...\n");
        for (int k = 0; k < game_count; k++) {
            for (int j = 0; j < (BOARD_SIZE << 1) * (BOARD_SIZE << 1);
                 j += (BOARD_SIZE << 2)) {
                for (int i = 0; i < BOARD_SIZE; i++)
                    table_buf[k][(i << 1) + j] = ' ';
            }
            memset(move_logs[k], 0, sizeof(move_logs[0]));
        }
        memset(log_count, 0, sizeof(log_count));
        return;  // return after all boards have been reset!
    }

    unsigned char g = buf[0];
    int mv = buf[1] >> 1;
    char turn = (buf[1] & 1) ? 'O' : 'X';

    // 1) update board buffer
    int row = mv / BOARD_SIZE, col = mv % BOARD_SIZE;
    int pos = (mv / BOARD_SIZE) * (BOARD_SIZE << 2) + (mv % BOARD_SIZE << 1);
    table_buf[g][pos] = turn;

    // 2) log move (e.g. "A1")
    if (log_count[g] < MAX_MOVES) {
        char m[3] = {'A' + row, '1' + col, '\0'};
        memcpy(move_logs[g][log_count[g]++].move, m, sizeof(m));
        LOG_DEBUG("Logged move %s: '%c'\n", m, turn);
    }

    // 3) load update (stub: kernel must send timing)
    // TODO: decode timing from kernel message
    int usec = 0;
    int pct10 = usec * 1000 / 1000000;
    if (turn == 'O')
        load_O_tenths[g] = (load_O_tenths[g] * 9 + pct10) / 10;
    else
        load_X_tenths[g] = (load_X_tenths[g] * 9 + pct10) / 10;
}


// update screen
static inline void repaint_screen(void)
{
    // clear
    printf("\033[2J\033[H");
    // printf("trying to paint screen now\n");

    // clock
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    printf("Time: %02d:%02d:%02d\n", tm.tm_hour, tm.tm_min, tm.tm_sec);

    // boards, loads, move logs
    for (int g = 0; g < game_count; g++) {
        printf("=== Game %d ===  Load O: %2d.%01d%%  X: %2d.%01d%%\n", g + 1,
               load_O_tenths[g] / 10, load_O_tenths[g] % 10,
               load_X_tenths[g] / 10, load_X_tenths[g] % 10);
        printf("%s\n", table_buf[g]);
        printf("Moves: ");
        for (int i = 0; i < log_count[g]; i++) {
            printf("%s", move_logs[g][i].move);
            if (i + 1 < log_count[g])
                printf(" -> ");
        }
        printf("\n");
    }
    LOG_DEBUG("Finished printing screen >W<\n");
}

static void io_co(void);
static void clock_co(void);
static void display_co(void);

static void schedule(void)
{
    coros[coro_count] = (coro_t){.alive = 1, .i = 0};
    memcpy(coros[coro_count++].name, "io_co", sizeof("io_co"));

    coros[coro_count] = (coro_t){.alive = 1, .i = 0};
    memcpy(coros[coro_count++].name, "clock_co", sizeof("clock_co"));

    coros[coro_count] = (coro_t){.alive = 1, .i = 0};
    memcpy(coros[coro_count++].name, "display_co", sizeof("display_co"));

    int r1 = setjmp(sched_env);
    switch (r1) {
    case 0:
        io_co();
        break;
    case 1:
        clock_co();
        break;
    case 2:
        display_co();
        break;
    case 3:
        break;
    default:
        printf("Error in schedule()\n");
        exit(1);
    }

    LOG_DEBUG("we starting\n");
    for (;;) {
        int r = setjmp(sched_env) % 3;
        if (end_attr)
            break;
        current_coro = &coros[r];

        if (!current_coro->alive)
            continue;
        longjmp(current_coro->env, 1);
    }
}

static void io_co(void)
{
    LOG_DEBUG("First entry in io_co!\n");
    current_coro = &coros[0];
    if (setjmp(current_coro->env) == 0)
        longjmp(sched_env, 1);

    LOG_DEBUG("in io_co\n");
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    unsigned char buf[2];
    fd_set rfds;

    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    FD_SET(device_fd, &rfds);

    int result = select(max_fd + 1, &rfds, NULL, NULL, NULL);
    if (result < 0) {
        printf("Error with select system call\n");
        exit(1);
    }

    // drain stdin
    if (FD_ISSET(STDIN_FILENO, &rfds)) {
        char ch;
        FD_CLR(STDIN_FILENO, &rfds);
        read(STDIN_FILENO, &ch, 1) == 1;
        if (ch == 16)
            pause_attr = !pause_attr;
        if (ch == 17)
            end_attr = true;
    }

    if (pause_attr)
        longjmp(sched_env, 1);

    // drain device
    if (FD_ISSET(device_fd, &rfds)) {
        FD_CLR(device_fd, &rfds);
        read(device_fd, buf, READ_DATA_SIZE);
        update_board_and_stats(buf);
    }

    longjmp(sched_env, 1);
}

static void clock_co(void)
{
    LOG_DEBUG("First entry in clock_co!\n");
    current_coro = &coros[1];
    if (setjmp(current_coro->env) == 0)
        longjmp(sched_env, 2);


    LOG_DEBUG("In clock_co\n");
    time_t now = time(NULL);
    const struct tm *tm_now = localtime(&now);
    if (tm_now) {
        strftime(clock_str, sizeof(clock_str), "Time: %H:%M:%S", tm_now);
    } else {
        snprintf(clock_str, sizeof(clock_str), "Time: (invalid)");
    }

    usleep(REFRESH_INTERVAL);
    longjmp(sched_env, 2);
}

static void display_co(void)
{
    LOG_DEBUG("First entry in display_co!\n");
    current_coro = &coros[2];

    if (setjmp(current_coro->env) == 0)
        longjmp(sched_env, 3);

    LOG_DEBUG("In display_co\n");

    repaint_screen();

    longjmp(sched_env, 3);
}


static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl+P */
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            pause_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!pause_attr)
                LOG_DEBUG("Stopping to display the chess board...\n");
            break;
        case 17: /* Ctrl+Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            pause_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            LOG_DEBUG("Stopping the kernel space tic-tac-toe game...\n");
            break;
        }
    }
    close(attr_fd);
}


void printer()
{
    printf("\033[H\033[J");
    for (int i = 0; i < game_count; i++) {
        if (pause_attr)
            printf("(PAUSED)");
        printf("============= Game: %d =============\n", i + 1);
        printf("%s\n", table_buf[i]);
    }
}


void user_print_board(unsigned char buf[2])
{
    // printf("Trying to print board now...\n");
    if (buf[0] & 0x80) {
        LOG_DEBUG("Games have ended! Resesting boards...\n");
        for (int k = 0; k < game_count; k++) {
            for (int j = 0; j < (BOARD_SIZE << 1) * (BOARD_SIZE << 1);
                 j += (BOARD_SIZE << 2)) {
                for (int i = 0; i < BOARD_SIZE; i++)
                    table_buf[k][(i << 1) + j] = ' ';
            }
        }
        return;  // return after all boards have been reset!
    }

    unsigned char id = buf[0];
    char turn = buf[1] & 1 ? 'O' : 'X';
    buf[1] >>= 1;

    int pos =
        (buf[1] / BOARD_SIZE) * (BOARD_SIZE << 2) + (buf[1] % BOARD_SIZE << 1);
    table_buf[id][pos] = turn;

    LOG_DEBUG("Placed '%c' at [%d]\n", turn, pos);
    printer();
}

int main(int argc, char *argv[])
{
    _Static_assert(DRAWBUFFER_SIZE >= sizeof(empty_board),
                   "DRAWBUFFER_SIZE too small!");

    for (int i = 0; i < MAX_GAMES; i++)
        memcpy(table_buf[i], empty_board, sizeof(empty_board));

    if (ALARM_TIME > 0) {
        signal(SIGALRM, handle_alarm);
        alarm(ALARM_TIME);
    }

    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    device_fd = open(XO_DEVICE_FILE, O_RDONLY | O_NONBLOCK);
    if (device_fd < 0) {
        perror("open /dev/kxo");
        exit(1);
    }

    LOG_DEBUG("ok, starting now...\n");

    schedule();

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}