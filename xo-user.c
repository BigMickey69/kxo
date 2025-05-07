#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "game.h"
#include "gamecount.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"


#define REFRESH_INTERVAL (100000 - 100)  // adjusting for better accuracy
char clock_str[32];

bool pause_flag = false, quit_flag = false;
struct {
    char move[3];
} move_logs[MAX_GAMES][N_GRIDS];
int log_count[MAX_GAMES];

// logging cpu times
int load_O_tenths[MAX_GAMES];
int load_X_tenths[MAX_GAMES];


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

// raw mode stuff
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


// Exit code after 3 seconds
#include <signal.h>
#define ALARM_TIME -1
static void handle_alarm(int sig)
{
    (void) sig;
    quit_flag = true;
}

// coroutine
#define STACK_SIZE (1 * 1024)  // I'm a cheapass, just 1 kb for each fake stack
#define MAX_MOVES N_GRIDS
#define MAX_CORO_COUNT 5

typedef void (*co_func_t)(void *);

typedef struct {
    jmp_buf env;
    co_func_t func;
    void *arg;
    char *stack;
    int finished;
} coroutine_t;

static coroutine_t coros[MAX_CORO_COUNT];
static int ncoros = 0;
static int current = -1;
static jmp_buf sched_env;

// tells compiler co_bootstrap NEVER returns
static void co_bootstrap(void) __attribute__((noreturn));
static void co_bootstrap(void)
{
    // 1) capture this coroutine’s own context & stack
    if (setjmp(coros[current].env) == 0)
        longjmp(sched_env, current + 1);


    coroutine_t *c = &coros[current];

    // 2) now run the user function
    c->func(c->arg);

    // 3) mark finished and return to scheduler for good
    c->finished = 1;
    longjmp(sched_env, current + 1);
}

static void co_create(co_func_t func, void *arg)
{
    int index = ncoros++;
    coroutine_t *c = &coros[index];
    c->func = func;
    c->arg = arg;
    c->finished = 0;
    c->stack = malloc(STACK_SIZE);
    if (!c->stack) {
        perror("malloc");
        exit(1);
    }

    char *top = c->stack + STACK_SIZE;
    void **sp = (void **) ((uintptr_t) top & ~0xF);
    *(--sp) = NULL;
    *(--sp) = (void *) co_bootstrap;


    current = index;
    asm volatile(
        "mov %0, %%rsp\n"
        "ret\n" ::"r"(sp)
        : "memory");
}


static void co_yield (void)
{
    int prev = current;

    do {
        current = (current + 1) % ncoros;
    } while (coros[current].finished);

    if (setjmp(coros[prev].env) == 0) {
        printf("[YIELD] %d -> %d\n", prev, current);
        longjmp(coros[current].env, 1);
    }
}



// update board
static void update_board_and_stats(const unsigned char buf[READ_DATA_SIZE])
{
    if (buf[0] & 0x80) {
        printf("Games have ended! Resetting boards...\n");
        for (int k = 0; k < game_count; k++) {
            for (int j = 0; j < (BOARD_SIZE << 1) * (BOARD_SIZE << 1);
                 j += (BOARD_SIZE << 2)) {
                for (int i = 0; i < BOARD_SIZE; i++)
                    table_buf[k][(i << 1) + j] = ' ';
            }
            log_count[k] = 0;
        }
        return;
    }

    int g = buf[0];
    int mv = buf[1] >> 1;
    char turn = (buf[1] & 1) ? 'O' : 'X';

    // 1) update board buffer
    int row = mv / BOARD_SIZE, col = mv % BOARD_SIZE;
    int pos = row * (BOARD_SIZE * 2) + col * 2;
    table_buf[g][pos] = turn;

    // 2) log move (e.g. "A1")
    if (log_count[g] < MAX_MOVES) {
        const char m[3] = {'A' + row, '1' + col, '\0'};
        memcpy(move_logs[g][log_count[g]++].move, m, sizeof(m));
        printf("Logged move %s: '%c'\n", m, turn);
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
static void repaint_screen(void)
{
    // clear
    // printf("\033[2J\033[H");
    printf("trying to paint screen now\n");
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
            printf("%s%s", move_logs[g][i].move,
                   (i + 1 < log_count[g]) ? " -> " : "\n");
        }
    }
    fflush(stdout);
}

/* coroutines
 * io_co      -> 0
 * clock_co   -> 1
 * display_co -> 2
 */
static void io_co(void *arg)
{
    int device_fd = *(int *) arg;
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    unsigned char buf[2];
    fd_set rfds;

    for (;;) {
        printf("in io_co\n");
        struct timeval tv = {.tv_sec = 0, .tv_usec = 1000};
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(device_fd, &rfds);
        select(max_fd + 1, &rfds, NULL, NULL, &tv);

        // drain stdin
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char ch;
            while (read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == 16)
                    pause_flag = !pause_flag;
                if (ch == 17)
                    quit_flag = true;
            }
        }

        // drain device
        if (FD_ISSET(device_fd, &rfds)) {
            ssize_t n;
            while ((n = read(device_fd, buf, 2)) > 0) {
                if (n == 4294967285)
                    break;
                if (n == 2)
                    update_board_and_stats(buf);
            }

        }

        co_yield ();
    }
}

static void display_co(void *unused)
{
    for (;;) {
        printf("In display_co\n");
        if (!pause_flag)
            repaint_screen();
        co_yield ();
    }
}

static void clock_co(void *arg)
{
    (void) arg;

    for (;;) {
        printf("In clock_co\n");
        time_t now = time(NULL);
        const struct tm *tm_now = localtime(&now);
        if (tm_now) {
            strftime(clock_str, sizeof(clock_str), "Time: %H:%M:%S", tm_now);
        } else {
            snprintf(clock_str, sizeof(clock_str), "Time: (invalid)");
        }

        for (int i = 0; i < 10; i++) {
            usleep(REFRESH_INTERVAL);
            co_yield ();
        }
    }
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



int main(int argc, char *argv[])
{
    _Static_assert(DRAWBUFFER_SIZE >= sizeof(empty_board),
                   "DRAWBUFFER_SIZE too small!");

    setvbuf(stdout, NULL, _IONBF, 0);  // Disable buffering

    for (int i = 0; i < MAX_GAMES; i++) {
        memcpy(table_buf[i], empty_board, sizeof(empty_board));
        log_count[i] = 0;
        load_O_tenths[i] = 0;
        load_X_tenths[i] = 0;
    }

    if (ALARM_TIME > 0) {
        signal(SIGALRM, handle_alarm);
        alarm(ALARM_TIME);
    }
    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY | O_NONBLOCK);
    if (device_fd < 0) {
        perror("open /dev/kxo");
        exit(1);
    }

    printf("ok, starting now...\n");

    // Very scuffed start-up
    int n;
    while ((n = setjmp(sched_env)) < 3) {
        switch (n) {
        case 0:
            printf("creating io_co...\n");
            co_create(io_co, &device_fd);
            break;
        case 1:
            printf("creating clock_co...\n");
            co_create(clock_co, NULL);
            break;
        case 2:
            printf("creating display_co...\n");
            co_create(display_co, NULL);
            break;
        }
    }

    // scheduler loop
    while (!quit_flag) {
        co_yield ();
    }


    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}
