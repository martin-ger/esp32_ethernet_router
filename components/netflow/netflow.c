/* NetFlow v5 exporter.
 *
 * Tracks flows on the Ethernet downlink and exports them as NetFlow v5 UDP
 * datagrams to a configurable collector.  Two directions are supported:
 *   NF_DIR_INGRESS – packets arriving from LAN clients (pre-NAT source IPs)
 *   NF_DIR_EGRESS  – packets leaving toward LAN clients
 * Both hooks are in main/netif_hooks.c.
 */

#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"

#include "netflow.h"
#include "router_config.h"

#define TAG "netflow"

/* ── Configuration ─────────────────────────────────────────────────────────── */

#define NF_COLLECTOR_LEN    16      /* max dotted-decimal IP string */
#define NF_DEFAULT_PORT     2055
#define NF_DEFAULT_IDLE_S   60
#define NF_DEFAULT_ACTIVE_S 300
#define NF_SWEEP_MS         5000    /* export task wake interval */
#define NF_PROBE_LIMIT      8       /* linear probe window for hash lookup */
#define NF_MAX_RECS_PER_PKT 30      /* NetFlow v5: max records per UDP datagram */
#define ETH_HDR_LEN         14

/* Flow table size: smaller on single-core ESP32-C3 to save RAM */
#if CONFIG_IDF_TARGET_ESP32C3
#define NF_TABLE_SIZE  128
#else
#define NF_TABLE_SIZE  256
#endif

/* ── Data structures ────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  _pad[3];
} nf_key_t;

typedef struct {
    nf_key_t key;
    uint32_t pkts;
    uint32_t bytes;
    uint32_t first_ms;  /* esp_timer_get_time()/1000 at flow start */
    uint32_t last_ms;   /* esp_timer_get_time()/1000 at last packet */
    uint8_t  tcp_flags; /* OR of all TCP flags seen */
    uint8_t  active;
    uint8_t  _pad[2];
} nf_flow_t;            /* 32 bytes */

/* NetFlow v5 wire format — big-endian on the wire */
typedef struct __attribute__((packed)) {
    uint16_t version;       /* 5 */
    uint16_t count;
    uint32_t sys_uptime;    /* ms since boot */
    uint32_t unix_secs;
    uint32_t unix_nsecs;
    uint32_t seq;
    uint8_t  eng_type;
    uint8_t  eng_id;
    uint16_t sampling;
} nfv5_hdr_t;               /* 24 bytes */

typedef struct __attribute__((packed)) {
    uint32_t src;
    uint32_t dst;
    uint32_t nexthop;
    uint16_t in_if;
    uint16_t out_if;
    uint32_t pkts;
    uint32_t bytes;
    uint32_t first;
    uint32_t last;
    uint16_t sp;
    uint16_t dp;
    uint8_t  pad1;
    uint8_t  flags;
    uint8_t  proto;
    uint8_t  tos;
    uint16_t src_as;
    uint16_t dst_as;
    uint8_t  src_mask;
    uint8_t  dst_mask;
    uint16_t pad2;
} nfv5_rec_t;               /* 48 bytes */

/* ── Module state ───────────────────────────────────────────────────────────── */

static nf_flow_t  *s_table = NULL;
static SemaphoreHandle_t s_mutex = NULL;

static atomic_int  s_directions;  /* bitmask: NF_DIR_INGRESS | NF_DIR_EGRESS; 0 = disabled */
static char        s_collector_ip[NF_COLLECTOR_LEN] = {0};
static uint16_t    s_port       = NF_DEFAULT_PORT;
static uint32_t    s_idle_ms    = NF_DEFAULT_IDLE_S  * 1000U;
static uint32_t    s_active_ms  = NF_DEFAULT_ACTIVE_S * 1000U;

static int         s_sock       = -1;
static struct sockaddr_in s_dest_addr;

static uint32_t    s_seq        = 0;
static uint32_t    s_exported   = 0;    /* total flows exported since boot */
static TaskHandle_t s_export_task = NULL;

/* ── FNV-1a hash ────────────────────────────────────────────────────────────── */

static IRAM_ATTR uint32_t fnv1a(const nf_key_t *k)
{
    const uint8_t *p = (const uint8_t *)k;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(nf_key_t) - 3 /* skip _pad */; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* ── Flow table helpers ─────────────────────────────────────────────────────── */

/* Must be called with s_mutex held. */
static IRAM_ATTR nf_flow_t *flow_find_or_create(const nf_key_t *key, uint32_t now_ms)
{
    uint32_t bucket = fnv1a(key) % NF_TABLE_SIZE;

    /* First pass: look for existing match */
    for (int i = 0; i < NF_PROBE_LIMIT; i++) {
        nf_flow_t *e = &s_table[(bucket + i) % NF_TABLE_SIZE];
        if (e->active && memcmp(&e->key, key, sizeof(nf_key_t)) == 0) {
            return e;
        }
    }

    /* Second pass: find empty slot */
    for (int i = 0; i < NF_PROBE_LIMIT; i++) {
        nf_flow_t *e = &s_table[(bucket + i) % NF_TABLE_SIZE];
        if (!e->active) {
            e->key      = *key;
            e->pkts     = 0;
            e->bytes    = 0;
            e->first_ms = now_ms;
            e->last_ms  = now_ms;
            e->tcp_flags = 0;
            e->active   = 1;
            return e;
        }
    }

    /* Probe window full — evict oldest entry */
    nf_flow_t *oldest = &s_table[bucket % NF_TABLE_SIZE];
    for (int i = 1; i < NF_PROBE_LIMIT; i++) {
        nf_flow_t *e = &s_table[(bucket + i) % NF_TABLE_SIZE];
        if (e->active && (int32_t)(e->last_ms - oldest->last_ms) < 0) {
            oldest = e;
        }
    }
    /* Evict without exporting (export task will handle aged flows anyway) */
    oldest->key      = *key;
    oldest->pkts     = 0;
    oldest->bytes    = 0;
    oldest->first_ms = now_ms;
    oldest->last_ms  = now_ms;
    oldest->tcp_flags = 0;
    oldest->active   = 1;
    return oldest;
}

/* ── Hot path ───────────────────────────────────────────────────────────────── */

IRAM_ATTR void netflow_account(struct pbuf *p, uint8_t dir)
{
    if (!(atomic_load_explicit(&s_directions, memory_order_relaxed) & dir)) return;
    if (s_table == NULL) return;
    if (p == NULL || p->len < ETH_HDR_LEN + (int)sizeof(struct ip_hdr)) return;

    uint8_t *payload = (uint8_t *)p->payload;

    /* IPv4 only */
    if (payload[12] != 0x08 || payload[13] != 0x00) return;

    struct ip_hdr *iph = (struct ip_hdr *)(payload + ETH_HDR_LEN);
    if (IPH_V(iph) != 4) return;

    uint16_t ip_hdr_len = IPH_HL(iph) * 4;
    uint8_t  proto      = IPH_PROTO(iph);

    nf_key_t key = {
        .src_ip   = iph->src.addr,
        .dst_ip   = iph->dest.addr,
        .proto    = proto,
        .src_port = 0,
        .dst_port = 0,
        ._pad     = {0, 0, 0},
    };
    uint8_t tcp_flags = 0;

    if (proto == 6 /* TCP */ &&
        p->len >= ETH_HDR_LEN + ip_hdr_len + (int)sizeof(struct tcp_hdr)) {
        struct tcp_hdr *tcp = (struct tcp_hdr *)(payload + ETH_HDR_LEN + ip_hdr_len);
        key.src_port = lwip_ntohs(tcp->src);
        key.dst_port = lwip_ntohs(tcp->dest);
        tcp_flags    = TCPH_FLAGS(tcp) & 0x3F;
    } else if (proto == 17 /* UDP */ &&
               p->len >= ETH_HDR_LEN + ip_hdr_len + (int)sizeof(struct udp_hdr)) {
        struct udp_hdr *udp = (struct udp_hdr *)(payload + ETH_HDR_LEN + ip_hdr_len);
        key.src_port = lwip_ntohs(udp->src);
        key.dst_port = lwip_ntohs(udp->dest);
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        nf_flow_t *flow = flow_find_or_create(&key, now_ms);
        flow->pkts++;
        flow->bytes    += p->tot_len;
        flow->last_ms   = now_ms;
        flow->tcp_flags |= tcp_flags;
        xSemaphoreGive(s_mutex);
    }
    /* If mutex is taken by export task: silently skip — one missed packet is fine */
}

/* ── Socket management ──────────────────────────────────────────────────────── */

static esp_err_t open_socket(void)
{
    if (s_collector_ip[0] == '\0') return ESP_ERR_INVALID_STATE;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return ESP_FAIL;
    }

    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family      = AF_INET;
    s_dest_addr.sin_port        = htons(s_port);
    s_dest_addr.sin_addr.s_addr = inet_addr(s_collector_ip);

    if (s_dest_addr.sin_addr.s_addr == INADDR_NONE) {
        ESP_LOGE(TAG, "invalid collector IP: %s", s_collector_ip);
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "socket opened → %s:%u", s_collector_ip, s_port);
    return ESP_OK;
}

static void close_socket(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

/* ── Export ─────────────────────────────────────────────────────────────────── */

static void export_batch(nf_flow_t *flows[], int count, uint32_t now_ms)
{
    if (count == 0 || s_sock < 0) return;

    uint8_t buf[sizeof(nfv5_hdr_t) + NF_MAX_RECS_PER_PKT * sizeof(nfv5_rec_t)];

    int sent = 0;
    while (sent < count) {
        int batch = count - sent;
        if (batch > NF_MAX_RECS_PER_PKT) batch = NF_MAX_RECS_PER_PKT;

        nfv5_hdr_t *hdr = (nfv5_hdr_t *)buf;
        hdr->version    = htons(5);
        hdr->count      = htons((uint16_t)batch);
        hdr->sys_uptime = htonl(now_ms);
        struct timeval tv;
        gettimeofday(&tv, NULL);
        hdr->unix_secs  = htonl((uint32_t)tv.tv_sec);
        hdr->unix_nsecs = htonl((uint32_t)(tv.tv_usec * 1000UL));
        hdr->seq        = htonl(s_seq);
        hdr->eng_type   = 0;
        hdr->eng_id     = 0;
        hdr->sampling   = 0;
        s_seq          += (uint32_t)batch;

        nfv5_rec_t *rec = (nfv5_rec_t *)(buf + sizeof(nfv5_hdr_t));
        for (int i = 0; i < batch; i++, rec++) {
            nf_flow_t *f = flows[sent + i];
            memset(rec, 0, sizeof(*rec));
            rec->src    = f->key.src_ip;
            rec->dst    = f->key.dst_ip;
            rec->pkts   = htonl(f->pkts);
            rec->bytes  = htonl(f->bytes);
            rec->first  = htonl(f->first_ms);
            rec->last   = htonl(f->last_ms);
            rec->sp     = htons(f->key.src_port);
            rec->dp     = htons(f->key.dst_port);
            rec->flags  = f->tcp_flags;
            rec->proto  = f->key.proto;
        }

        size_t pkt_len = sizeof(nfv5_hdr_t) + (size_t)batch * sizeof(nfv5_rec_t);
        ssize_t r = sendto(s_sock, buf, pkt_len, 0,
                           (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));
        if (r < 0) {
            ESP_LOGW(TAG, "sendto failed: %d", errno);
        }
        sent += batch;
    }
    s_exported += (uint32_t)count;
}

static void sweep_and_export(void)
{
    if (s_table == NULL) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    /* Collect flows to export under the lock, then send outside the lock */
    nf_flow_t *to_export[NF_TABLE_SIZE];
    int n = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < NF_TABLE_SIZE; i++) {
        nf_flow_t *e = &s_table[i];
        if (!e->active) continue;

        uint32_t idle   = now_ms - e->last_ms;
        uint32_t age    = now_ms - e->first_ms;
        bool expired    = (idle >= s_idle_ms) || (age >= s_active_ms);

        if (expired) {
            to_export[n++] = e;
        }
    }
    xSemaphoreGive(s_mutex);

    export_batch(to_export, n, now_ms);

    /* Clear exported entries under the lock */
    if (n > 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            to_export[i]->active = 0;
        }
        xSemaphoreGive(s_mutex);
    }
}

static void export_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(NF_SWEEP_MS));
        if (!atomic_load_explicit(&s_directions, memory_order_relaxed)) continue;
        if (s_sock < 0) continue;
        sweep_and_export();
    }
}

/* ── NVS config ─────────────────────────────────────────────────────────────── */

static void load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(PARAM_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    int32_t v = 0;
    if (nvs_get_i32(h, "nf_dir", &v) == ESP_OK) {
        atomic_store(&s_directions, (int)(v & (NF_DIR_INGRESS | NF_DIR_EGRESS)));
    } else if (nvs_get_i32(h, "nf_en", &v) == ESP_OK && v != 0) {
        atomic_store(&s_directions, (int)NF_DIR_INGRESS);  /* migrate: old = ingress only */
    }
    size_t len = NF_COLLECTOR_LEN;
    nvs_get_str(h, "nf_collector", s_collector_ip, &len);
    if (nvs_get_i32(h, "nf_port", &v) == ESP_OK && v >= 1 && v <= 65535) {
        s_port = (uint16_t)v;
    }
    if (nvs_get_i32(h, "nf_idle_to", &v) == ESP_OK && v > 0) {
        s_idle_ms = (uint32_t)v * 1000U;
    }
    if (nvs_get_i32(h, "nf_active_to", &v) == ESP_OK && v > 0) {
        s_active_ms = (uint32_t)v * 1000U;
    }
    nvs_close(h);
}

static void save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_i32(h, "nf_dir", (int32_t)atomic_load(&s_directions));
    nvs_set_str(h, "nf_collector", s_collector_ip);
    nvs_set_i32(h, "nf_port", (int32_t)s_port);
    nvs_set_i32(h, "nf_idle_to",   (int32_t)(s_idle_ms   / 1000U));
    nvs_set_i32(h, "nf_active_to", (int32_t)(s_active_ms / 1000U));
    nvs_commit(h);
    nvs_close(h);
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

esp_err_t netflow_init(void)
{
    atomic_init(&s_directions, 0);
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;

    load_config();

    /* Allocate table now if enabled by saved config */
    if (atomic_load(&s_directions) && s_collector_ip[0] != '\0') {
        s_table = calloc(NF_TABLE_SIZE, sizeof(nf_flow_t));
        if (s_table == NULL) {
            ESP_LOGW(TAG, "flow table alloc failed at init — disabling");
            atomic_store(&s_directions, 0);
        }
    }

    if (atomic_load(&s_directions) && s_collector_ip[0] != '\0') {
        xTaskCreate(export_task, "netflow_exp", 5120, NULL, 4, &s_export_task);
    }
    ESP_LOGI(TAG, "initialized (table=%d, directions=0x%02x)", NF_TABLE_SIZE,
             (int)atomic_load(&s_directions));
    return ESP_OK;
}

void netflow_notify_connected(void)
{
    if (atomic_load(&s_directions) && s_collector_ip[0] != '\0' && s_sock < 0) {
        open_socket();
    }
}

esp_err_t netflow_enable(const char *collector_ip, uint16_t port, uint8_t directions)
{
    if (collector_ip == NULL || collector_ip[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (directions == 0) directions = NF_DIR_INGRESS;  /* default if caller passes 0 */

    close_socket();

    strlcpy(s_collector_ip, collector_ip, NF_COLLECTOR_LEN);
    if (port != 0) s_port = port;

    /* Allocate flow table if not already allocated */
    if (s_table == NULL) {
        s_table = calloc(NF_TABLE_SIZE, sizeof(nf_flow_t));
        if (s_table == NULL) {
            ESP_LOGE(TAG, "failed to allocate flow table (%u bytes)",
                     (unsigned)(NF_TABLE_SIZE * sizeof(nf_flow_t)));
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "flow table allocated (%u entries, %u bytes)",
                 NF_TABLE_SIZE, (unsigned)(NF_TABLE_SIZE * sizeof(nf_flow_t)));
    } else {
        /* Re-enabling: clear stale flows */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memset(s_table, 0, NF_TABLE_SIZE * sizeof(nf_flow_t));
        xSemaphoreGive(s_mutex);
    }

    atomic_store(&s_directions, (int)directions);
    save_config();

    esp_err_t err = open_socket();
    if (err == ESP_OK) {
        if (s_export_task == NULL) {
            xTaskCreate(export_task, "netflow_exp", 5120, NULL, 4, &s_export_task);
        }
        ESP_LOGI(TAG, "enabled → %s:%u (dir=0x%02x)", s_collector_ip, s_port, directions);
    }
    return err;
}

esp_err_t netflow_set_directions(uint8_t directions)
{
    if (!netflow_is_enabled()) return ESP_ERR_INVALID_STATE;
    if (directions == 0) return ESP_ERR_INVALID_ARG;
    atomic_store(&s_directions, (int)directions);
    save_config();
    ESP_LOGI(TAG, "directions updated to 0x%02x", directions);
    return ESP_OK;
}

esp_err_t netflow_disable(void)
{
    atomic_store(&s_directions, 0);
    close_socket();

    if (s_export_task != NULL) {
        vTaskDelete(s_export_task);
        s_export_task = NULL;
    }

    /* Free flow table to return RAM to the heap */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    free(s_table);
    s_table = NULL;
    xSemaphoreGive(s_mutex);

    save_config();
    ESP_LOGI(TAG, "disabled");
    return ESP_OK;
}

bool netflow_is_enabled(void)
{
    return atomic_load(&s_directions) != 0;
}

void netflow_set_timeouts(uint32_t idle_sec, uint32_t active_sec)
{
    if (idle_sec > 0)   s_idle_ms   = idle_sec   * 1000U;
    if (active_sec > 0) s_active_ms = active_sec * 1000U;
    save_config();
}

void netflow_get_config(bool *enabled, char *ip_out, size_t ip_len,
                        uint16_t *port_out,
                        uint32_t *idle_sec_out, uint32_t *active_sec_out,
                        uint8_t *directions_out)
{
    int dirs = atomic_load(&s_directions);
    if (enabled)        *enabled        = dirs != 0;
    if (ip_out)         strlcpy(ip_out, s_collector_ip, ip_len);
    if (port_out)       *port_out       = s_port;
    if (idle_sec_out)   *idle_sec_out   = s_idle_ms   / 1000U;
    if (active_sec_out) *active_sec_out = s_active_ms / 1000U;
    if (directions_out) *directions_out = (uint8_t)dirs;
}

uint32_t netflow_get_active_flows(void)
{
    if (s_table == NULL) return 0;
    uint32_t count = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < NF_TABLE_SIZE; i++) {
        if (s_table[i].active) count++;
    }
    xSemaphoreGive(s_mutex);
    return count;
}

uint32_t netflow_get_exported_flows(void)
{
    return s_exported;
}

void netflow_print_flows(void)
{
    if (s_table == NULL) { printf("NetFlow not enabled.\n"); return; }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    printf("\n%-16s  %-16s  %-5s  %-5s  %-5s  %-8s  %-8s  %s\n",
           "Src IP", "Dst IP", "Proto", "SPort", "DPort", "Packets", "Bytes", "Age(s)");
    printf("%-16s  %-16s  %-5s  %-5s  %-5s  %-8s  %-8s  %s\n",
           "----------------", "----------------", "-----", "-----", "-----",
           "--------", "--------", "------");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < NF_TABLE_SIZE; i++) {
        nf_flow_t *e = &s_table[i];
        if (!e->active) continue;

        struct in_addr src_addr = { .s_addr = e->key.src_ip };
        struct in_addr dst_addr = { .s_addr = e->key.dst_ip };
        char src_str[16], dst_str[16];
        inet_ntoa_r(src_addr, src_str, sizeof(src_str));
        inet_ntoa_r(dst_addr, dst_str, sizeof(dst_str));

        const char *proto_name;
        switch (e->key.proto) {
            case 1:  proto_name = "ICMP"; break;
            case 6:  proto_name = "TCP";  break;
            case 17: proto_name = "UDP";  break;
            default: proto_name = "IP";   break;
        }

        uint32_t age_s = (now_ms - e->first_ms) / 1000U;
        printf("%-16s  %-16s  %-5s  %-5u  %-5u  %-8lu  %-8lu  %lu\n",
               src_str, dst_str, proto_name,
               e->key.src_port, e->key.dst_port,
               (unsigned long)e->pkts,
               (unsigned long)e->bytes,
               (unsigned long)age_s);
    }
    xSemaphoreGive(s_mutex);
}
