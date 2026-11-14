#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PORT 18080
#define CLIENT_PORT 40000
#define ORIGINAL_PORT 54321

static void put16(unsigned char *p, uint16_t value)
{
    value = htons(value);
    memcpy(p, &value, sizeof(value));
}

static void put32(unsigned char *p, uint32_t value)
{
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
}

static uint16_t get16(const unsigned char *p)
{
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return ntohs(value);
}

static uint32_t get32(const unsigned char *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return ntohl(value);
}

static uint16_t checksum(const unsigned char *data, size_t length)
{
    uint32_t sum = 0;
    while (length > 1) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        length -= 2;
    }
    if (length)
        sum += (uint32_t)data[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static int send_packet(int fd, uint8_t flags, uint32_t sequence, uint32_t acknowledgement)
{
    unsigned char packet[48] = {0};
    unsigned char pseudo[40] = {0};
    unsigned char *tcp = packet + 20;
    struct sockaddr_in destination = {.sin_family = AF_INET};

    packet[0] = 0x45;
    put16(packet + 2, sizeof(packet));
    put16(packet + 4, 1);
    packet[8] = 64;
    packet[9] = IPPROTO_TCP;
    inet_pton(AF_INET, "198.51.100.2", packet + 12);
    inet_pton(AF_INET, "127.0.0.1", packet + 16);
    put16(packet + 10, checksum(packet, 20));

    put16(tcp, CLIENT_PORT);
    put16(tcp + 2, SERVER_PORT);
    put32(tcp + 4, sequence);
    put32(tcp + 8, acknowledgement);
    tcp[12] = 7 << 4;
    tcp[13] = flags;
    put16(tcp + 14, 65535);
    tcp[20] = 254;
    tcp[21] = 8;
    put16(tcp + 22, ORIGINAL_PORT);
    inet_pton(AF_INET, "192.0.2.44", tcp + 24);

    memcpy(pseudo, packet + 12, 8);
    pseudo[9] = IPPROTO_TCP;
    put16(pseudo + 10, 28);
    memcpy(pseudo + 12, tcp, 28);
    put16(tcp + 16, checksum(pseudo, sizeof(pseudo)));
    inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr);
    return sendto(fd, packet, sizeof(packet), 0, (struct sockaddr *)&destination, sizeof(destination)) == sizeof(packet) ? 0 : -1;
}

int main(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(SERVER_PORT)};
    struct sockaddr_in peer;
    socklen_t peer_length = sizeof(peer);
    unsigned char packet[2048];
    uint32_t server_sequence = 0;
    int got_synack = 0;
    int listener, sender, capture, accepted;
    struct pollfd wait_packet;

    alarm(20);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    sender = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    capture = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (listener < 0 || sender < 0 || capture < 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listener, 8) < 0) {
        perror("setup sockets");
        return 1;
    }
    if (send_packet(sender, 0x02, 12345, 0) < 0) {
        perror("send SYN");
        return 1;
    }
    wait_packet.fd = capture;
    wait_packet.events = POLLIN;
    while (poll(&wait_packet, 1, 5000) > 0) {
        ssize_t n = recv(capture, packet, sizeof(packet), 0);
        if (n < 54 || packet[14] >> 4 != 4 || packet[23] != IPPROTO_TCP)
            continue;
        size_t ip_length = (packet[14] & 15) * 4;
        if (n < (ssize_t)(14 + ip_length + 20))
            continue;
        const unsigned char *tcp = packet + 14 + ip_length;
        if (get16(tcp) == SERVER_PORT && get16(tcp + 2) == CLIENT_PORT && (tcp[13] & 0x12) == 0x12) {
            server_sequence = get32(tcp + 4);
            got_synack = 1;
            break;
        }
    }
    if (!got_synack || send_packet(sender, 0x10, 12346, server_sequence + 1) < 0) {
        fprintf(stderr, "TCP handshake failed\n");
        return 1;
    }
    accepted = accept(listener, NULL, NULL);
    if (accepted < 0 || getpeername(accepted, (struct sockaddr *)&peer, &peer_length) < 0) {
        perror("accept connection");
        return 1;
    }
    struct in_addr expected;
    inet_pton(AF_INET, "192.0.2.44", &expected);
    if (peer.sin_addr.s_addr != expected.s_addr || ntohs(peer.sin_port) != ORIGINAL_PORT) {
        fprintf(stderr, "unexpected original address: %s:%u\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        return 1;
    }
    close(accepted);
    close(capture);
    close(sender);
    close(listener);
    puts("original client address recovered");
    return 0;
}
