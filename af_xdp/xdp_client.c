#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <xdp/xsk.h> 

// --- DEFAULTS ---
char *opt_ifname = "eth0";
char *opt_src_ip = "192.168.1.10";
int   opt_src_port = 12345;
char *opt_dst_ip = "192.168.1.20";
int   opt_dst_port = 5000;

unsigned char src_mac[ETH_ALEN];
unsigned char dst_mac[ETH_ALEN];

#define NUM_FRAMES 4096
#define FRAME_SIZE 2048
#define BATCH_SIZE 16

struct xsk_umem_info {
    struct xsk_ring_prod fq; 
    struct xsk_ring_cons cq; 
    struct xsk_umem *umem;
    void *buffer;
};

struct xsk_socket_info {
    struct xsk_ring_cons rx; 
    struct xsk_ring_prod tx; 
    struct xsk_umem_info *umem;
    struct xsk_socket *xsk;
};

// --- HELPER: Get Local MAC Address ---
int get_my_mac(const char *ifname, unsigned char *mac_out) {
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    memcpy(mac_out, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
    return 0;
}

// --- HELPER: Resolve Remote MAC (Ping + Read ARP Table) ---
int resolve_mac(const char *target_ip, unsigned char *mac_out) {
    char cmd[128];
    
    // 1. Force OS to resolve ARP by sending 1 ping (1 sec timeout)
    // We redirect output to /dev/null so it doesn't clutter your screen
    snprintf(cmd, sizeof(cmd), "ping -c 1 -W 1 %s > /dev/null 2>&1", target_ip);
    system(cmd);

    // 2. Read the specific ARP entry using 'ip neigh'
    snprintf(cmd, sizeof(cmd), "ip neigh show %s", target_ip);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    char line[256];
    if (fgets(line, sizeof(line), fp)) {
        // Output format example: "10.0.0.5 dev eth0 lladdr aa:bb:cc:dd:ee:ff REACHABLE"
        char *ptr = strstr(line, "lladdr");
        if (ptr) {
            unsigned int m[6];
            // Read the 6 hex bytes immediately after "lladdr"
            if (sscanf(ptr + 7, "%x:%x:%x:%x:%x:%x", 
                       &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
                for (int i=0; i<6; i++) mac_out[i] = (unsigned char)m[i];
                pclose(fp);
                return 0; // Success
            }
        }
    }
    
    pclose(fp);
    return -1; // Failed
}

unsigned short checksum(void *b, int len) {
    unsigned short *buf = b;
    unsigned int sum = 0;
    unsigned short result;
    for (sum = 0; len > 1; len -= 2) sum += *buf++;
    if (len == 1) sum += *(unsigned char *)buf;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    result = ~sum;
    return result;
}

static struct xsk_umem_info *configure_umem(void *buffer, uint64_t size) {
    struct xsk_umem_info *umem = calloc(1, sizeof(*umem));
    int ret = xsk_umem__create(&umem->umem, buffer, size, &umem->fq, &umem->cq, NULL);
    if (ret) { fprintf(stderr, "UMEM create failed: %s\n", strerror(-ret)); exit(1); }
    umem->buffer = buffer;
    return umem;
}

static struct xsk_socket_info *configure_xsk(struct xsk_umem_info *umem) {
    struct xsk_socket_info *xsk = calloc(1, sizeof(*xsk));
    xsk->umem = umem;
    struct xsk_socket_config cfg = {
        .rx_size = BATCH_SIZE,
        .tx_size = BATCH_SIZE,
        .libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD,
        .xdp_flags = XDP_FLAGS_SKB_MODE, // Generic mode (Works on AWS/VMs)
        .bind_flags = XDP_COPY,
    };
    int ret = xsk_socket__create(&xsk->xsk, opt_ifname, 0, umem->umem, &xsk->rx, &xsk->tx, &cfg);
    if (ret) { fprintf(stderr, "Socket create failed: %s\n", strerror(-ret)); exit(1); }
    return xsk;
}

void build_packet(void *buf, const char *msg, uint32_t *len) {
    struct ethhdr *eth = buf;
    struct iphdr *ip = (struct iphdr *)(eth + 1);
    struct udphdr *udp = (struct udphdr *)(ip + 1);
    char *payload = (char *)(udp + 1);
    int payload_len = strlen(msg);

    // Ethernet
    memcpy(eth->h_dest, dst_mac, ETH_ALEN);
    memcpy(eth->h_source, src_mac, ETH_ALEN);
    eth->h_proto = htons(ETH_P_IP);

    // IP
    ip->ihl = 5; ip->version = 4; ip->tos = 0;
    ip->tot_len = htons(sizeof(struct iphdr) + sizeof(struct udphdr) + payload_len);
    ip->id = htons(1); ip->frag_off = 0; ip->ttl = 64; ip->protocol = IPPROTO_UDP;
    ip->saddr = inet_addr(opt_src_ip);
    ip->daddr = inet_addr(opt_dst_ip);
    ip->check = 0;
    ip->check = checksum(ip, sizeof(struct iphdr));

    // UDP
    udp->source = htons(opt_src_port);
    udp->dest = htons(opt_dst_port);
    udp->len = htons(sizeof(struct udphdr) + payload_len);
    udp->check = 0;

    memcpy(payload, msg, payload_len);
    *len = sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) + payload_len;
}

int main(int argc, char **argv) {
    if (argc == 6) {
        opt_ifname = argv[1];
        opt_src_ip = argv[2];
        opt_src_port = atoi(argv[3]);
        opt_dst_ip = argv[4];
        opt_dst_port = atoi(argv[5]);
    } else {
        printf("Usage: %s <IFNAME> <SRC_IP> <SRC_PORT> <DST_IP> <DST_PORT>\n", argv[0]);
        return 1;
    }

    // 1. Resolve Local MAC
    if (get_my_mac(opt_ifname, src_mac) < 0) {
        fprintf(stderr, "Error: Could not find MAC for %s\n", opt_ifname);
        return 1;
    }
    printf("[Info] Local MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);

    // 2. Resolve Remote MAC
    printf("[Info] Resolving MAC for %s (via Ping)...\n", opt_dst_ip);
    if (resolve_mac(opt_dst_ip, dst_mac) < 0) {
        fprintf(stderr, "Error: Could not resolve MAC for %s.\n", opt_dst_ip);
        fprintf(stderr, " -> If the target is on a different subnet, please provide the GATEWAY IP as the destination argument instead.\n");
        return 1;
    }
    printf("[Info] Remote MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        dst_mac[0], dst_mac[1], dst_mac[2], dst_mac[3], dst_mac[4], dst_mac[5]);

    // 3. Setup AF_XDP
    void *bufs;
    if (posix_memalign(&bufs, getpagesize(), NUM_FRAMES * FRAME_SIZE)) return 1;

    struct xsk_umem_info *umem = configure_umem(bufs, NUM_FRAMES * FRAME_SIZE);
    struct xsk_socket_info *xsk = configure_xsk(umem);

    // 4. Fill Ring (With safety check)
    uint32_t idx_fq = 0;
    if (xsk_ring_prod__reserve(&umem->fq, BATCH_SIZE, &idx_fq) == BATCH_SIZE) {
        for (int i = 0; i < BATCH_SIZE; i++) *xsk_ring_prod__fill_addr(&umem->fq, idx_fq++) = i * FRAME_SIZE;
        xsk_ring_prod__submit(&umem->fq, BATCH_SIZE);
    }

    // 5. Send Packet
    uint32_t idx_tx, pkt_len;
    void *tx_frame = xsk_umem__get_data(umem->buffer, 0); 
    build_packet(tx_frame, "Hello", &pkt_len);

    printf("[TX] Sending 'Hello' to %s:%d...\n", opt_dst_ip, opt_dst_port);
    xsk_ring_prod__reserve(&xsk->tx, 1, &idx_tx);
    struct xdp_desc *desc = xsk_ring_prod__tx_desc(&xsk->tx, idx_tx);
    desc->addr = 0; desc->len = pkt_len;
    xsk_ring_prod__submit(&xsk->tx, 1);
    
    sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);

    // 6. Poll for Response
    printf("[RX] Waiting for response...\n");
    while (1) {
        uint32_t idx_rx;
        unsigned int rcvd = xsk_ring_cons__peek(&xsk->rx, BATCH_SIZE, &idx_rx);
        if (rcvd > 0) {
            for (int i = 0; i < rcvd; i++) {
                const struct xdp_desc *rx_desc = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx++);
                void *pkt = xsk_umem__get_data(umem->buffer, rx_desc->addr);
                
                struct ethhdr *eth = pkt;
                struct iphdr *ip = (struct iphdr *)(eth + 1);
                struct udphdr *udp = (struct udphdr *)(ip + 1);
                char *payload = (char *)(udp + 1);

                if (eth->h_proto == htons(ETH_P_IP) && ip->protocol == IPPROTO_UDP && 
                    ntohs(udp->dest) == opt_src_port) {
                    
                    char response[32] = {0};
                    int data_len = ntohs(udp->len) - sizeof(struct udphdr);
                    if (data_len > 31) data_len = 31;
                    memcpy(response, payload, data_len);

                    printf("[RX] Received from %s: %s\n", inet_ntoa(*(struct in_addr*)&ip->saddr), response);
                    if (strncmp(response, "Hello", 5) == 0) {
                        printf("[Success] Server confirmed.\n");
                        exit(0);
                    }
                }
            }
            xsk_ring_cons__release(&xsk->rx, rcvd);
            
            xsk_ring_prod__reserve(&umem->fq, rcvd, &idx_fq);
            for (int i = 0; i < rcvd; i++) *xsk_ring_prod__fill_addr(&umem->fq, idx_fq++) = i * FRAME_SIZE;
            xsk_ring_prod__submit(&umem->fq, rcvd);
        }
    }
    return 0;
}
