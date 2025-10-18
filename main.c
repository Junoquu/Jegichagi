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

// '\n' 또는 '\0' 까지 한 프레임(readline) 받기
static int recv_frame(SOCKET s, char* out, int max) {
    int total = 0;
    while (total < max - 1) {
        char c;
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return n; // 0=closed, <0=error
        if (c == '\0' || c == '\n') { // 둘 다 프레이밍으로 인정
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

    // 포트 재사용
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

    printf("서버가 포트 %d에서 대기 중입니다...\n", PORT);

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_fd == INVALID_SOCKET) {
            fprintf(stderr, "accept error: %d\n", WSAGetLastError());
            CLOSE_SOCKET(server_fd);
            WSACleanup();
            return 1;
        }

        printf("클라이언트 연결됨: %s\n", inet_ntoa(client_addr.sin_addr));

        // 1) 핸드셰이크: "id mac name" 한 프레임 수신
        read_len = recv_frame(client_fd, buffer, BUF_SIZE);
        if (read_len > 0) {
            UINT original_cp = GetConsoleOutputCP();
            printf("받은 문자열: ");
            SetConsoleOutputCP(CP_UTF8);
            printf("[%s]\n", buffer);
            SetConsoleOutputCP(original_cp);

            if (sscanf(buffer, "%31s %63s %63s", id, mac, name) == 3) {
                int found = 0;
                FILE* fp = fopen("studentlist.dat", "r");
                if (fp == NULL) {
                    strcpy(buffer, "ERROR: file open\n");  // \n 추가
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
                    strcpy(buffer, found ? "OK\n" : "NOT FOUND\n"); // ★ 클라가 '\n' 기다리므로 개행 포함
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
            printf("문자열 수신 실패 또는 연결 종료\n");
            CLOSE_SOCKET(client_fd); CLOSE_SOCKET(server_fd); WSACleanup(); return 1;
        }

        // 2) 3회 게임 반복 처리 (각 회차: START 1프레임 + FINAL 1프레임)
        FILE* score_fp = fopen("studentscore.dat", "a");
        if (!score_fp) {
            fprintf(stderr, "studentscore.dat open error: %d\n", errno);
            CLOSE_SOCKET(client_fd); CLOSE_SOCKET(server_fd); WSACleanup(); return 1;
        }

        char game_result[MAX_GAMES][128];
        for (int game_round = 1; game_round <= MAX_GAMES; game_round++) {
            unsigned int ttime = 0;   // START: 시작시각(ms) / FINAL: 경과시간(ms)
            unsigned int dtime = 0;   // 표준편차 등(지금은 0)
            int jegiCount = 0;        // 라운드 증분 카운트
            int timestamp_count = 0;
            char status[16] = "";

            printf("\n=== 게임 %d회차 시작 ===\n", game_round);

            while (timestamp_count < 2) {
                read_len = recv_frame(client_fd, buffer, BUF_SIZE); // ★ '\n' 또는 '\0' 프레임 단위 수신
                if (read_len > 0) {
                    // START <ttime> <dtime> <value>
                    // FINAL <ttime> <dtime> <value>
                    int n = sscanf(buffer, "%15s %u %u %d", status, &ttime, &dtime, &jegiCount);
                    if (n >= 1 && strcmp(status, "START") == 0) {
                        timestamp_count++;
                        printf("START 수신: %s %u %u %d\n", status, ttime, dtime, jegiCount);
                    }
                    else if (n >= 1 && strcmp(status, "FINAL") == 0) {
                        timestamp_count++;
                        printf("FINAL 수신: %s %u %u %d\n", status, ttime, dtime, jegiCount);
                    }
                    else {
                        printf("형식 오류: [%s] (len=%d)\n", buffer, read_len);
                    }
                }
                else if (read_len == 0) {
                    printf("클라이언트 연결 종료\n");
                    break;
                }
                else {
                    fprintf(stderr, "recv error: %d\n", WSAGetLastError());
                    break;
                }
            }

            printf("게임 %d회차 결과 → 총체류시간: %u ms, 회당 체류시간 평균 %u, 제기횟수: %d\n",
                game_round, ttime, dtime, jegiCount);

            sprintf(game_result[game_round - 1], " %d %d %u %u ", game_round, jegiCount, ttime, dtime);

            snprintf(buffer, sizeof(buffer), "%d ROUND DONE \n", game_round); // 응답은 개행 포함
            WRITE_DATA(client_fd, buffer, (int)strlen(buffer));
        }

        printf("\n=== 3회 게임 종료 ===\n");
        fprintf(score_fp, "%s %s %s %s %s %s\n", id, mac, name, game_result[0], game_result[1], game_result[2]);
        fflush(score_fp);
        fclose(score_fp);

        CLOSE_SOCKET(client_fd);
        printf("클라이언트 종료. 다음 클라이언트 대기...\n");

        // 운영자 입력
        printf("서버를 계속 운영하려면 Enter, 종료하려면 q 입력 후 Enter: ");
        char cmd[8];
        fgets(cmd, sizeof(cmd), stdin);
        if (cmd[0] == 'q' || cmd[0] == 'Q') {
            printf("서버 종료합니다.\n");
            break;
        }
    }

    CLOSE_SOCKET(server_fd);
    WSACleanup();
    return 0;
}
