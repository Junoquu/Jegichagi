#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 5001
#define BUF_SIZE 1024
#define MAX_GAMES 3

#define CLOSE_SOCKET(s) closesocket(s)
#define READ_DATA(s, buf, len) recv(s, buf, len, 0)
#define WRITE_DATA(s, buf, len) send(s, buf, len, 0)

// '\n' �Ǵ� '\0' ���� �� ������(readline) �ޱ�
static int recv_frame(SOCKET s, char* out, int max) {
    int total = 0;
    while (total < max - 1) {
        char c;
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return n; // 0=closed, <0=error
        if (c == '\0' || c == '\n') { // �� �� �����̹����� ����
            out[total] = '\0';
            return total;
        }
        out[total++] = c;
    }
    out[total] = '\0';
    return total;
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed.\n");
        return 1;
    }

    SOCKET server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    int client_addr_len = sizeof(client_addr);
    char buffer[BUF_SIZE];
    int read_len;
    char id[32] = { 0 }, mac[64] = { 0 }, name[64] = { 0 };

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        fprintf(stderr, "socket error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // ��Ʈ ����
    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind error: %d\n", WSAGetLastError());
        CLOSE_SOCKET(server_fd);
        WSACleanup();
        return 1;
    }

    if (listen(server_fd, 5) == SOCKET_ERROR) {
        fprintf(stderr, "listen error: %d\n", WSAGetLastError());
        CLOSE_SOCKET(server_fd);
        WSACleanup();
        return 1;
    }

    printf("������ ��Ʈ %d���� ��� ���Դϴ�...\n", PORT);

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_fd == INVALID_SOCKET) {
            fprintf(stderr, "accept error: %d\n", WSAGetLastError());
            CLOSE_SOCKET(server_fd);
            WSACleanup();
            return 1;
        }

        printf("Ŭ���̾�Ʈ �����: %s\n", inet_ntoa(client_addr.sin_addr));

        // 1) �ڵ����ũ: "id mac name" �� ������ ����
        read_len = recv_frame(client_fd, buffer, BUF_SIZE);
        if (read_len > 0) {
            UINT original_cp = GetConsoleOutputCP();
            printf("���� ���ڿ�: ");
            SetConsoleOutputCP(CP_UTF8);
            printf("[%s]\n", buffer);
            SetConsoleOutputCP(original_cp);

            if (sscanf(buffer, "%31s %63s %63s", id, mac, name) == 3) {
                int found = 0;
                FILE* fp = fopen("studentlist.dat", "r");
                if (fp == NULL) {
                    strcpy(buffer, "ERROR: file open\n");  // \n �߰�
                }
                else {
                    char line[256], fid[32], fmac[64], fname[64];
                    while (fgets(line, sizeof(line), fp)) {
                        line[strcspn(line, "\r\n")] = 0;
                        if (sscanf(line, "%31s %63s %63s", fid, fmac, fname) == 3 &&
                            strcmp(id, fid) == 0 && strcmp(mac, fmac) == 0 && strcmp(name, fname) == 0) {
                            printf("Confirmed --> (%s , %s, %s)\n", id, mac, name);
                            found = 1;
                            break;
                        }
                    }
                    fclose(fp);
                    strcpy(buffer, found ? "OK\n" : "NOT FOUND\n"); // �� Ŭ�� '\n' ��ٸ��Ƿ� ���� ����
                }
            }
            else {
                strcpy(buffer, "INPUT ERROR\n");
            }

            int send_len = (int)strlen(buffer);
            if (WRITE_DATA(client_fd, buffer, send_len) != send_len) {
                fprintf(stderr, "send error: %d\n", WSAGetLastError());
                CLOSE_SOCKET(client_fd); CLOSE_SOCKET(server_fd); WSACleanup(); return 1;
            }
        }
        else {
            printf("���ڿ� ���� ���� �Ǵ� ���� ����\n");
            CLOSE_SOCKET(client_fd); CLOSE_SOCKET(server_fd); WSACleanup(); return 1;
        }

        // 2) 3ȸ ���� �ݺ� ó�� (�� ȸ��: START 1������ + FINAL 1������)
        FILE* score_fp = fopen("studentscore.dat", "a");
        if (!score_fp) {
            fprintf(stderr, "studentscore.dat open error: %d\n", errno);
            CLOSE_SOCKET(client_fd); CLOSE_SOCKET(server_fd); WSACleanup(); return 1;
        }

        char game_result[MAX_GAMES][128];
        for (int game_round = 1; game_round <= MAX_GAMES; game_round++) {
            unsigned int ttime = 0;   // START: ���۽ð�(ms) / FINAL: ����ð�(ms)
            unsigned int dtime = 0;   // ǥ������ ��(������ 0)
            int jegiCount = 0;        // ���� ���� ī��Ʈ
            int timestamp_count = 0;
            char status[16] = "";

            printf("\n=== ���� %dȸ�� ���� ===\n", game_round);

            while (timestamp_count < 2) {
                read_len = recv_frame(client_fd, buffer, BUF_SIZE); // �� '\n' �Ǵ� '\0' ������ ���� ����
                if (read_len > 0) {
                    // START <ttime> <dtime> <value>
                    // FINAL <ttime> <dtime> <value>
                    int n = sscanf(buffer, "%15s %u %u %d", status, &ttime, &dtime, &jegiCount);
                    if (n >= 1 && strcmp(status, "START") == 0) {
                        timestamp_count++;
                        printf("START ����: %s %u %u %d\n", status, ttime, dtime, jegiCount);
                    }
                    else if (n >= 1 && strcmp(status, "FINAL") == 0) {
                        timestamp_count++;
                        printf("FINAL ����: %s %u %u %d\n", status, ttime, dtime, jegiCount);
                    }
                    else {
                        printf("���� ����: [%s] (len=%d)\n", buffer, read_len);
                    }
                }
                else if (read_len == 0) {
                    printf("Ŭ���̾�Ʈ ���� ����\n");
                    break;
                }
                else {
                    fprintf(stderr, "recv error: %d\n", WSAGetLastError());
                    break;
                }
            }

            printf("���� %dȸ�� ��� �� ��ü���ð�: %u ms, ȸ�� ü���ð� ��� %u, ����Ƚ��: %d\n",
                game_round, ttime, dtime, jegiCount);

            sprintf(game_result[game_round - 1], " %d %d %u %u ", game_round, jegiCount, ttime, dtime);

            snprintf(buffer, sizeof(buffer), "%d ROUND DONE \n", game_round); // ������ ���� ����
            WRITE_DATA(client_fd, buffer, (int)strlen(buffer));
        }

        printf("\n=== 3ȸ ���� ���� ===\n");
        fprintf(score_fp, "%s %s %s %s %s %s\n", id, mac, name, game_result[0], game_result[1], game_result[2]);
        fflush(score_fp);
        fclose(score_fp);

        CLOSE_SOCKET(client_fd);
        printf("Ŭ���̾�Ʈ ����. ���� Ŭ���̾�Ʈ ���...\n");

        // ��� �Է�
        printf("������ ��� ��Ϸ��� Enter, �����Ϸ��� q �Է� �� Enter: ");
        char cmd[8];
        fgets(cmd, sizeof(cmd), stdin);
        if (cmd[0] == 'q' || cmd[0] == 'Q') {
            printf("���� �����մϴ�.\n");
            break;
        }
    }

    CLOSE_SOCKET(server_fd);
    WSACleanup();
    return 0;
}
