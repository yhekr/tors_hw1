#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

const int max_servers = 10;
const int discovery_port = 9001;
const int task_port = 9002;
const int timeout_seconds = 2;
const char *broadcast_message = "SERVER_DISCOVERY_REQUEST";

double task_start_points[100];
double task_end_points[100];
double task_steps[100];
int total_tasks = 0;

struct sockaddr_in server_addresses[10];
int server_status[10];
int total_servers = 0;
pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;

void add_new_server(struct sockaddr_in address) {
    pthread_mutex_lock(&server_lock);

    for (int i = 0; i < total_servers; i++) {
        if (server_addresses[i].sin_addr.s_addr == address.sin_addr.s_addr) {
            server_status[i] = 1;
            pthread_mutex_unlock(&server_lock);
            return;
        }
    }

    if (total_servers < max_servers) {
        server_addresses[total_servers] = address;
        server_status[total_servers] = 1;
        total_servers++;
        printf("Added new server: %s:%d\n", inet_ntoa(address.sin_addr), ntohs(address.sin_port));
    }

    pthread_mutex_unlock(&server_lock);
}

void display_servers() {
    pthread_mutex_lock(&server_lock);
    printf("\nActive Servers:\n");
    for (int i = 0; i < total_servers; i++) {
        printf("%d: %s:%d (status: %s)\n", i, inet_ntoa(server_addresses[i].sin_addr),
               ntohs(server_addresses[i].sin_port), server_status[i] ? "active" : "inactive");
    }
    printf("End of server list.\n");
    pthread_mutex_unlock(&server_lock);
}

void discover_available_servers() {
    printf("\nStarting server discovery...\n");

    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int enable_broadcast = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &enable_broadcast, sizeof(enable_broadcast)) < 0) {
        perror("Setting SO_BROADCAST failed");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in broadcast_address;
    broadcast_address.sin_family = AF_INET;
    broadcast_address.sin_port = htons(discovery_port);
    broadcast_address.sin_addr.s_addr = INADDR_BROADCAST;

    if (sendto(socket_fd, broadcast_message, strlen(broadcast_message), 0,
               (struct sockaddr *)&broadcast_address, sizeof(broadcast_address)) < 0) {
        perror("Broadcast message sending failed");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    struct timeval timeout = {timeout_seconds, 0};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char buffer[256];
    struct sockaddr_in response_address;
    socklen_t address_length = sizeof(response_address);

    while (1) {
        int response_length = recvfrom(socket_fd, buffer, sizeof(buffer) - 1, 0,
                                       (struct sockaddr *)&response_address, &address_length);
        if (response_length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("Error receiving server response");
            continue;
        }

        buffer[response_length] = '\0';
        if (strcmp(buffer, "SERVER_DISCOVERY_RESPONSE") == 0) {
            response_address.sin_port = htons(discovery_port);
            add_new_server(response_address);
        }
    }

    close(socket_fd);
    display_servers();
}

double send_task_to_server(struct sockaddr_in server_address, double start, double end, double step) {
    printf("Sending task to server %s: start=%.2f, end=%.2f, step=%.2f\n",
           inet_ntoa(server_address.sin_addr), start, end, step);

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("Socket creation failed");
        return 0.0;
    }

    server_address.sin_port = htons(task_port);

    if (connect(socket_fd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("Connection to server failed");
        close(socket_fd);
        return 0.0;
    }

    double task_data[3] = {start, end, step};
    if (send(socket_fd, task_data, sizeof(task_data), 0) < 0) {
        perror("Task sending failed");
        close(socket_fd);
        return 0.0;
    }

    double result;
    if (recv(socket_fd, &result, sizeof(result), 0) < 0) {
        perror("Result receiving failed");
        close(socket_fd);
        return 0.0;
    }

    printf("Received result from server %s: %.5f\n", inet_ntoa(server_address.sin_addr), result);

    close(socket_fd);
    return result;
}

double distribute_all_tasks() {
    double total_result = 0.0;

    for (int i = 0; i < total_tasks; i++) {
        pthread_mutex_lock(&server_lock);
        for (int j = 0; j < total_servers; j++) {
            if (server_status[j]) {
                double result = send_task_to_server(server_addresses[j], task_start_points[i],
                                                    task_end_points[i], task_steps[i]);
                total_result += result;
                break;
            }
        }
        pthread_mutex_unlock(&server_lock);
    }

    return total_result;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <start> <end> <step>\n", argv[0]);
        return EXIT_FAILURE;
    }

    double range_start = atof(argv[1]);
    double range_end = atof(argv[2]);
    double step_size = atof(argv[3]);

    for (double x = range_start; x < range_end; x += 1.0) {
        task_start_points[total_tasks] = x;
        task_end_points[total_tasks] = (x + 1.0 > range_end) ? range_end : x + 1.0;
        task_steps[total_tasks] = step_size;
        total_tasks++;
    }

    printf("Master process starting...\n");
    discover_available_servers();

    if (total_servers == 0) {
        fprintf(stderr, "No servers discovered. Exiting.\n");
        return EXIT_FAILURE;
    }

    double final_result = distribute_all_tasks();
    printf("\nFinal computation result: %.5f\n", final_result);

    return EXIT_SUCCESS;
}
