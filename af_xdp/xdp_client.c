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
