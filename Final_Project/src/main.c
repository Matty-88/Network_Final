#include "../include/display.h"
#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// Function declarations
void *send_position(void *arg);
void *receive_position(void *arg);
void *auto_move(void *arg);

// Position structure
typedef struct
{
    int x, y;
} Position;

// Packet structure
typedef struct
{
    int      player_id;
    Position pos;
} Packet;

// Game state structure
typedef struct
{
    Position           local_pos;
    Position           remote_pos;
    int                sock_fd;
    struct sockaddr_in remote_addr;
    socklen_t          addr_len;
    int                mode;    // 0 for keyboard, 1 for auto-move, 2 for controller
    int                running;
} GameInfo;

#define TIMEOUT_SEC 5000000U
#define BASE_TEN 10
#define BUFF_SIZE 16

//send the local player's pos
noreturn void *send_position(void *arg)
{
    // cppcheck-suppress constVariablePointer
    GameInfo       *data       = (GameInfo *)arg;
    struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = TIMEOUT_SEC};

    Packet packet = {.player_id = 0};

    while(data->running)
    {
        packet.pos = data->local_pos;

        if(sendto(data->sock_fd, &packet, sizeof(packet), 0, (struct sockaddr *)&data->remote_addr, data->addr_len) < 0)
        {
            perror("Error sending packet");
        }
        nanosleep(&sleep_time, NULL);
    }
    pthread_exit(NULL);
}

//rec local player pos
noreturn void *receive_position(void *arg)
{
    // cppcheck-suppress constVariablePointer
    GameInfo *data = (GameInfo *)arg;
    Packet    packet;

    while(data->running)
    {
        ssize_t received = recvfrom(data->sock_fd, &packet, sizeof(packet), 0, NULL, NULL);
        if(received > 0)
        {
            if(packet.player_id == 1)
            {
                data->remote_pos = packet.pos;
            }
        }
        else if(received < 0)
        {
            perror("Peer disconnected");
            break;
        }
    }
    pthread_exit(NULL);
}

noreturn void *auto_move(void *arg)
{
    // cppcheck-suppress constVariablePointer
    GameInfo       *data       = (GameInfo *)arg;
    struct timespec sleep_time = {.tv_sec = 2, .tv_nsec = 0};

    int x_direction = 1;    // Right
    int y_direction = 1;    // Down

    while(data->running)
    {
        data->local_pos.x += x_direction;
        if(data->local_pos.x <= 0 || data->local_pos.x >= COLS - 1)
        {
            x_direction *= -1;
        }
        nanosleep(&sleep_time, NULL);

        data->local_pos.y += y_direction;
        if(data->local_pos.y <= 0 || data->local_pos.y >= LINES - 1)
        {
            y_direction *= -1;
        }
        nanosleep(&sleep_time, NULL);
    }
    pthread_exit(NULL);
}

void keyboard_input(GameInfo *data);

void keyboard_input(GameInfo *data)
{
    int c;
    while(data->running && (c = getch()) != 'q')
    {
        switch(c)
        {
            case KEY_UP:
                if(data->local_pos.y > 0)
                {
                    data->local_pos.y--;
                }
                break;
            case KEY_DOWN:
                if(data->local_pos.y < LINES - 1)
                {
                    data->local_pos.y++;
                }
                break;
            case KEY_LEFT:
                if(data->local_pos.x > 0)
                {
                    data->local_pos.x--;
                }
                break;
            case KEY_RIGHT:
                if(data->local_pos.x < COLS - 1)
                {
                    data->local_pos.x++;
                }
                break;
            default:
                break;
        }
        clear();
        attron(COLOR_PAIR(1));
        mvprintw(data->local_pos.y, data->local_pos.x, "x");
        attroff(COLOR_PAIR(1));

        attron(COLOR_PAIR(2));
        mvprintw(data->remote_pos.y, data->remote_pos.x, "o");
        attroff(COLOR_PAIR(2));

        mvprintw(0, 0, "Use arrow keys to move. Press 'q' to quit.");
        refresh();
    }
    data->running = 0;
}

int main(int argc, const char *argv[])
{
    int                 local_port;
    int                 remote_port;
    const char         *remote_ip;
    struct sockaddr_in  local_addr = {0};
    struct timeval      timeout;
    pthread_t           send_thread;
    pthread_t           recv_thread;
    pthread_t           auto_move_thread = 0;
    SDL_GameController *controller       = NULL;
    char                input_buffer[BUFF_SIZE];
    GameInfo            data = {
                   .local_pos  = {0, 0},
                   .remote_pos = {0, 0},
                   .sock_fd    = -1,
                   .addr_len   = sizeof(struct sockaddr_in),
                   .mode       = 0,
                   .running    = 1
    };

    struct timespec refresh_rate = {.tv_sec = 0, .tv_nsec = TIMEOUT_SEC};

    if(SDL_Init(SDL_INIT_GAMECONTROLLER) < 0)
    {
        fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        return 1;
    }

    if(SDL_NumJoysticks() > 0 && SDL_IsGameController(0))
    {
        controller = SDL_GameControllerOpen(0);
        if(!controller)
        {
            fprintf(stderr, "Could not open game controller: %s\n", SDL_GetError());
        }
    }

    if(argc != 4)
    {
        printf("Usage: %s <local_port> <remote_ip> <remote_port>\n", argv[0]);
        return 1;
    }

    remote_ip   = argv[2];
    local_port  = (int)strtol(argv[1], NULL, BASE_TEN);
    remote_port = (int)strtol(argv[3], NULL, BASE_TEN);

    data.sock_fd               = socket(AF_INET, SOCK_DGRAM, 0);
    local_addr.sin_family      = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port        = htons((uint16_t)local_port);

    if(bind(data.sock_fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        perror("Bind failed");
        goto cleanup;
    }

    data.remote_addr.sin_family = AF_INET;
    data.remote_addr.sin_port   = htons((uint16_t)remote_port);
    if(inet_pton(AF_INET, remote_ip, &data.remote_addr.sin_addr) <= 0)
    {
        perror("Invalid remote IP address");
        goto cleanup;
    }

    data.addr_len = sizeof(data.remote_addr);

    timeout.tv_sec  = TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(data.sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    printf("Choose mode (0 for keyboard, 1 for auto-move, 2 for controller): ");
    fgets(input_buffer, sizeof(input_buffer), stdin);
    data.mode = (int)strtol(input_buffer, NULL, BASE_TEN);

    pthread_create(&send_thread, NULL, send_position, &data);
    pthread_create(&recv_thread, NULL, receive_position, &data);

    initscr();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(0);
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    nodelay(stdscr, TRUE);

    if(data.mode == 1)    //this calls auto moe
    {
        pthread_create(&auto_move_thread, NULL, auto_move, &data);
    }

    while(data.running)
    {
        int c = getch();
        //q to quit
        if(c == 'q')
        {
            data.running = 0;
        }

        // keyboard
        if(data.mode == 0)
        {
            switch(c)
            {
                case KEY_UP:
                    if(data.local_pos.y > 0)
                        data.local_pos.y--;
                    break;
                case KEY_DOWN:
                    if(data.local_pos.y < LINES - 1)
                        data.local_pos.y++;
                    break;
                case KEY_LEFT:
                    if(data.local_pos.x > 0)
                        data.local_pos.x--;
                    break;
                case KEY_RIGHT:
                    if(data.local_pos.x < COLS - 1)
                        data.local_pos.x++;
                    break;
                default:
                    break;
            }
        }
        // controller
        else if(data.mode == 2)
        {
            SDL_Event e;
            while(SDL_PollEvent(&e))
            {
                if(e.type == SDL_CONTROLLERBUTTONDOWN)
                {
                    switch(e.cbutton.button)
                    {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:
                            if(data.local_pos.y > 0)
                                data.local_pos.y--;
                            break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                            if(data.local_pos.y < LINES - 1)
                                data.local_pos.y++;
                            break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                            if(data.local_pos.x > 0)
                                data.local_pos.x--;
                            break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                            if(data.local_pos.x < COLS - 1)
                                data.local_pos.x++;
                            break;
                        case SDL_CONTROLLER_BUTTON_A:
                            data.running = 0;
                            break;
                        default:
                            break;
                    }
                }
            }
        }

        clear();
        attron(COLOR_PAIR(1));
        mvprintw(data.local_pos.y, data.local_pos.x, "x");
        attroff(COLOR_PAIR(1));

        attron(COLOR_PAIR(2));
        mvprintw(data.remote_pos.y, data.remote_pos.x, "o");
        attroff(COLOR_PAIR(2));

        mvprintw(0, 0, "Use keyboard, controller, or auto-move. Press 'q' to quit.");
        refresh();
        nanosleep(&refresh_rate, NULL);
    }

    endwin();
    pthread_cancel(send_thread);
    pthread_cancel(recv_thread);
    if(data.mode == 1)
    {
        pthread_cancel(auto_move_thread);
        pthread_join(auto_move_thread, NULL);
    }
    pthread_join(send_thread, NULL);
    pthread_join(recv_thread, NULL);

cleanup:
    if(controller)
    {
        SDL_GameControllerClose(controller);
    }
    SDL_Quit();
    close(data.sock_fd);
    return 0;
}
