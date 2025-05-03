#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

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

static bool read_attr, end_attr;

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
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!read_attr)
                printf("Stopping to display the chess board...\n");
            break;
        case 17: /* Ctrl+Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf("Stopping the kernel space tic-tac-toe game...\n");
            break;
        }
    }
    close(attr_fd);
}


void printer()
{
    printf("\033[2J\033[H");  // better escape, '[2J' clears entire screen while
                              // '[H' moves to top left
    for (int i = 0; i < game_count; i++) {
        printf("============= Game: %d =============\n", i + 1);
        printf("%s\n", table_buf[i]);
    }
    fflush(stdout);
}


void user_print_board(unsigned char buf[2])
{
    // printf("Trying to print board now...\n");
    if (buf[0] & 0x80) {
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

    printf("Placed '%c' at [%d]\n", turn, pos);
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

    unsigned char buf[2];

    fd_set readset;
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    read_attr = true;
    end_attr = false;

    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(device_fd, &readset);

        // printf("ok, starting to read\n");
        int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
        if (result < 0) {
            printf("Error with select system call\n");
            exit(1);
        }

        // printf("Probably read something\n");

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            FD_CLR(STDIN_FILENO, &readset);
            listen_keyboard_handler();
        } else if (read_attr && FD_ISSET(device_fd, &readset)) {
            FD_CLR(device_fd, &readset);
            read(device_fd, buf, READ_DATA_SIZE);
            user_print_board(buf);
        }
    }

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}
