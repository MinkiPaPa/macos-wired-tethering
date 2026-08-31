/*
 * tether_engine.c
 * Packet pump: USB RNDIS <-> Ethernet/ARP <-> utun, plus DHCP.
 * 패킷 펌프: USB RNDIS와 이더넷/ARP와 utun을 연결하고 DHCP를 수행한다.
 */
#include "tether_engine.h"
#include "utun_if.h"
#include "dhcp_client.h"

#include <stdatomic.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define AT_TXQ_CAP  128
#define AT_TXQ_SLOT 4096
#define AT_TX_OUT_MAX_RETRIES 2

struct at_engine {
    at_engine_opts_t opts;
    at_usb_rndis_t *usb;
    at_utun_t *utun;
    pthread_t usb_rx;
    pthread_t utun_rx;
    pthread_t maint;
    int usb_rx_ok;
    int utun_rx_ok;
    int maint_ok;
    atomic_int running;
    atomic_int state;
    pthread_mutex_t mu;
    atomic_uint_fast64_t rx_bytes;
    atomic_uint_fast64_t tx_bytes;
    atomic_uint_fast64_t rx_frames;
    atomic_uint_fast64_t tx_frames;
    atomic_uint_fast64_t rx_errors;
    atomic_uint_fast64_t tx_errors;
    at_net_info_t net;
    uint8_t host_mac[AT_MAC_LEN];
    uint8_t device_mac[AT_MAC_LEN];
    uint8_t gw_mac[AT_MAC_LEN];
    atomic_int gw_mac_ok;
    uint32_t host_ip;
    uint32_t gw_ip;
    uint32_t link_mbps;
    char error[256];
    int prefer_default_route;
    atomic_int lease_applied;
    at_dhcp_t *dhcp;
    /* Consecutive hard USB IN errors. Reset on a good read. USB thread / IN cb. */
    /* 연속 USB IN 하드 오류. 정상 읽기에서 0. USB 스레드·IN 콜백. */
    atomic_int usb_hard_fails;
    /* Async OUT retries after ClearPipeStall. USB I/O thread only. */
    /* ClearPipeStall 뒤 비동기 OUT 재시도. USB I/O 스레드만. */
    int tx_out_retries;
    /* Set once a DHCP lease has been applied; used to fail a stuck renew. */
    /* DHCP 임대를 한 번이라도 적용했으면 1. 갱신 실패 판별에 쓴다. */
    atomic_int saw_lease;

    /* USB bulk TX queue. usb_io thread writes OUT while async IN stays pending. */
    /* USB 벌크 TX 큐. usb_io 스레드가 OUT을 쓰고, 비동기 IN은 걸려 있는다. */
    pthread_mutex_t txq_mu;
    uint8_t *txq_flat;
    uint8_t *usb_inbuf[AT_USB_IN_DEPTH];
    size_t txq_len[AT_TXQ_CAP];
    int txq_r;
    int txq_w;
    int txq_n;
};

static uint8_t *txq_slot(at_engine_t *e, int i) {
    return e->txq_flat + (size_t)i * AT_TXQ_SLOT;
}

static void add_rx(at_engine_t *e, size_t n) {
    atomic_fetch_add_explicit(&e->rx_bytes, n, memory_order_relaxed);
    atomic_fetch_add_explicit(&e->rx_frames, 1, memory_order_relaxed);
}

static void add_tx(at_engine_t *e, size_t n) {
    atomic_fetch_add_explicit(&e->tx_bytes, n, memory_order_relaxed);
    atomic_fetch_add_explicit(&e->tx_frames, 1, memory_order_relaxed);
}

static void add_rx_err(at_engine_t *e) {
    atomic_fetch_add_explicit(&e->rx_errors, 1, memory_order_relaxed);
}

static void add_tx_err(at_engine_t *e) {
    atomic_fetch_add_explicit(&e->tx_errors, 1, memory_order_relaxed);
}

/* Fail a live engine. Does not join threads; main sees AT_STATE_ERROR and stops. */
/* 살아 있는 엔진을 실패로 표시한다. 스레드는 join 하지 않는다. main 이 ERROR 를 보고 정리한다. */
static void engine_fail(at_engine_t *e, const char *msg) {
    if (!e) return;
    char copy[256];
    strlcpy(copy, (msg && msg[0]) ? msg : "Engine error", sizeof(copy));
    for (;;) {
        int cur = atomic_load(&e->state);
        if (cur == AT_STATE_STOPPING || cur == AT_STATE_ERROR) return;
        int next = AT_STATE_ERROR;
        if (atomic_compare_exchange_weak(&e->state, &cur, next)) break;
    }
    pthread_mutex_lock(&e->mu);
    strlcpy(e->error, copy, sizeof(e->error));
    pthread_mutex_unlock(&e->mu);
    at_log(AT_LOG_ERROR, "%s", copy);
    atomic_store(&e->running, 0);
}

/* Pause forwarding and force the next lease to re-learn the gateway MAC. */
/* 포워딩을 멈추고, 다음 임대에서 게이트웨이 MAC 을 다시 배우게 한다. */
static void pause_datapath(at_engine_t *e) {
    if (!e) return;
    atomic_store(&e->lease_applied, 0);
    atomic_store(&e->gw_mac_ok, 0);
    int cur = atomic_load(&e->state);
    if (cur == AT_STATE_CONNECTED)
        atomic_store(&e->state, AT_STATE_CONNECTING);
}

/* Count a TX-queue drop and warn without flooding the GUI log. */
/* TX 큐 폐기를 세고, 로그가 폭주하지 않게 경고한다. */
static void note_tx_overflow(at_engine_t *e) {
    uint64_t n = atomic_fetch_add_explicit(&e->tx_errors, 1, memory_order_relaxed) + 1;
    if (n == 1 || (n % 256ull) == 0) {
        at_log(AT_LOG_WARN,
               "TX queue full, dropping frames (total %llu)",
               (unsigned long long)n);
    }
}

static uint16_t ethertype(const uint8_t *eth) {
    return (uint16_t)(((uint16_t)eth[12] << 8) | eth[13]);
}

/* Cheap pre-filter so the datapath does not lock on every USB frame. */
/* 데이터 경로가 모든 USB 프레임에서 락을 잡지 않도록 하는 사전 필터. */
static int looks_like_dhcp_to_client(const uint8_t *frame, size_t len) {
    if (len < 14 + 20 + 8) return 0;
    if (ethertype(frame) != 0x0800) return 0;
    const uint8_t *ip = frame + 14;
    if ((ip[0] >> 4) != 4) return 0;
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4u;
    if (ihl < 20 || 14 + ihl + 8 > len) return 0;
    if (ip[9] != 17) return 0;
    uint16_t dport = (uint16_t)(((uint16_t)ip[ihl + 2] << 8) | ip[ihl + 3]);
    return dport == 68;
}

static int send_eth_usb(at_engine_t *e, const uint8_t *eth, size_t len) {
    if (!e->txq_flat) {
        add_tx_err(e);
        return -1;
    }
    pthread_mutex_lock(&e->txq_mu);
    if (e->txq_n >= AT_TXQ_CAP) {
        pthread_mutex_unlock(&e->txq_mu);
        note_tx_overflow(e);
        return -1;
    }
    /* Wrap RNDIS in-place so we skip an extra memcpy. */
    /* RNDIS 래핑을 슬롯에 바로 써서 추가 memcpy를 없앤다. */
    size_t wrapped = rndis_wrap_ethernet_aligned(
        txq_slot(e, e->txq_w), AT_TXQ_SLOT, eth, len, at_usb_packet_align(e->usb));
    if (wrapped == 0 || wrapped > AT_TXQ_SLOT) {
        pthread_mutex_unlock(&e->txq_mu);
        add_tx_err(e);
        return -1;
    }
    e->txq_len[e->txq_w] = wrapped;
    e->txq_w = (e->txq_w + 1) % AT_TXQ_CAP;
    e->txq_n++;
    pthread_mutex_unlock(&e->txq_mu);
    at_usb_async_wake(e->usb);
    return 0;
}

static void on_usb_out(void *user, int rc) {
    at_engine_t *e = user;
    size_t n = 0;
    pthread_mutex_lock(&e->txq_mu);
    if (rc != 0 && e->tx_out_retries < AT_TX_OUT_MAX_RETRIES && e->txq_n > 0) {
        e->tx_out_retries++;
        pthread_mutex_unlock(&e->txq_mu);
        (void)at_usb_clear_stall(e->usb, 0);
        usleep(300);
        return;
    }
    e->tx_out_retries = 0;
    if (e->txq_n > 0) {
        n = e->txq_len[e->txq_r];
        e->txq_r = (e->txq_r + 1) % AT_TXQ_CAP;
        e->txq_n--;
    }
    pthread_mutex_unlock(&e->txq_mu);
    if (rc == 0) {
        if (n) add_tx(e, n);
    } else {
        add_tx_err(e);
    }
}

/* Start at most one async OUT. Must run on the USB I/O thread, not from an IN callback. */
/* 비동기 OUT은 최대 하나. USB I/O 스레드에서만 호출하고, IN 콜백에서는 호출하지 않는다. */
static void usb_try_start_out(at_engine_t *e) {
    if (!e->usb || !e->txq_flat) return;
    if (at_usb_async_out_busy(e->usb)) return;
    pthread_mutex_lock(&e->txq_mu);
    if (e->txq_n <= 0) {
        pthread_mutex_unlock(&e->txq_mu);
        return;
    }
    int idx = e->txq_r;
    size_t n = e->txq_len[idx];
    uint8_t *ptr = txq_slot(e, idx);
    pthread_mutex_unlock(&e->txq_mu);
    int wr = at_usb_async_submit_out(e->usb, ptr, n, on_usb_out, e);
    if (wr == 1) return;
    if (wr != 0) {
        pthread_mutex_lock(&e->txq_mu);
        if (e->tx_out_retries < AT_TX_OUT_MAX_RETRIES && e->txq_n > 0 && e->txq_r == idx) {
            e->tx_out_retries++;
            pthread_mutex_unlock(&e->txq_mu);
            (void)at_usb_clear_stall(e->usb, 0);
            usleep(300);
            wr = at_usb_async_submit_out(e->usb, ptr, n, on_usb_out, e);
            if (wr == 0 || wr == 1) return;
            pthread_mutex_lock(&e->txq_mu);
        }
        e->tx_out_retries = 0;
        if (e->txq_n > 0 && e->txq_r == idx) {
            e->txq_r = (e->txq_r + 1) % AT_TXQ_CAP;
            e->txq_n--;
        }
        pthread_mutex_unlock(&e->txq_mu);
        add_tx_err(e);
    }
}

/* Drain the TX queue. One consumer; slot at r stays owned until we advance r. */
/* TX 큐를 비운다. 소비자는 하나이므로 r 슬롯은 r을 전진시킬 때까지 유효하다. */
static void usb_drain_tx(at_engine_t *e) {
    /* One PACKET_MSG per bulk OUT. Samsung RNDIS often mishandles host-side aggregation. */
    /* 벌크 OUT마다 PACKET_MSG 하나. Samsung RNDIS는 호스트 측 모아보내기를 자주 깨먹는다. */
    for (;;) {
        pthread_mutex_lock(&e->txq_mu);
        if (e->txq_n <= 0) {
            pthread_mutex_unlock(&e->txq_mu);
            return;
        }
        int idx = e->txq_r;
        size_t n = e->txq_len[idx];
        uint8_t *ptr = txq_slot(e, idx);
        pthread_mutex_unlock(&e->txq_mu);
        int wr = at_usb_bulk_write(e->usb, ptr, n);
        pthread_mutex_lock(&e->txq_mu);
        e->txq_r = (e->txq_r + 1) % AT_TXQ_CAP;
        e->txq_n--;
        pthread_mutex_unlock(&e->txq_mu);
        if (wr == 0) add_tx(e, n);
        else add_tx_err(e);
    }
}

/* Build Ethernet + ARP (request op=1, reply op=2). */
/* 이더넷+ARP 프레임을 만든다. 요청=1, 응답=2. */
static size_t build_arp(uint8_t *eth, size_t cap,
                        const uint8_t sha[AT_MAC_LEN], uint32_t spa,
                        const uint8_t tha[AT_MAC_LEN], uint32_t tpa,
                        uint16_t op) {
    if (cap < 42) return 0;
    memset(eth, 0, 42);
    if (op == 1) memset(eth, 0xff, 6);
    else if (tha) memcpy(eth, tha, 6);
    memcpy(eth + 6, sha, AT_MAC_LEN);
    eth[12] = 0x08;
    eth[13] = 0x06;
    eth[14] = 0x00;
    eth[15] = 0x01;
    eth[16] = 0x08;
    eth[17] = 0x00;
    eth[18] = 6;
    eth[19] = 4;
    eth[20] = (uint8_t)(op >> 8);
    eth[21] = (uint8_t)op;
    memcpy(eth + 22, sha, 6);
    memcpy(eth + 28, &spa, 4);
    if (op == 2 && tha) memcpy(eth + 32, tha, 6);
    memcpy(eth + 38, &tpa, 4);
    return 42;
}

static size_t wrap_ip_eth(at_engine_t *e, const uint8_t *ip, size_t ip_len,
                          uint8_t *eth, size_t cap) {
    if (14 + ip_len > cap) return 0;
    /* Snapshot under mu: gw_mac is written by ARP / USB learning on other threads. */
    /* mu 아래 스냅샷. gw_mac 은 다른 스레드의 ARP·USB 학습이 쓴다. */
    at_mac_snapshot_pair(&e->mu, e->gw_mac, e->host_mac, eth, eth + 6);
    uint8_t ver = (uint8_t)(ip[0] >> 4);
    if (ver == 6) {
        eth[12] = 0x86;
        eth[13] = 0xDD;
    } else {
        eth[12] = 0x08;
        eth[13] = 0x00;
    }
    memcpy(eth + 14, ip, ip_len);
    return 14 + ip_len;
}

static void handle_arp(at_engine_t *e, const uint8_t *eth, size_t len) {
    if (len < 42) return;
    uint16_t op = (uint16_t)((eth[20] << 8) | eth[21]);
    uint32_t spa, tpa;
    memcpy(&spa, eth + 28, 4);
    memcpy(&tpa, eth + 38, 4);

    pthread_mutex_lock(&e->mu);
    uint32_t host_ip = e->host_ip;
    uint8_t host_mac[AT_MAC_LEN];
    memcpy(host_mac, e->host_mac, AT_MAC_LEN);
    pthread_mutex_unlock(&e->mu);

    if (op == 1 && host_ip && tpa == host_ip) {
        uint8_t reply[64];
        uint8_t tha[AT_MAC_LEN];
        memcpy(tha, eth + 22, AT_MAC_LEN);
        size_t n = build_arp(reply, sizeof(reply), host_mac, host_ip, tha, spa, 2);
        if (n) (void)send_eth_usb(e, reply, n);
    }
}

static void on_eth_from_usb(const uint8_t *frame, size_t len, void *user) {
    at_engine_t *e = user;
    if (!e || len < 14) return;
    uint16_t et = ethertype(frame);

    /* Learn only from ARP replies / IPv4 whose source is the DHCP gateway. */
    /* DHCP 게이트웨이가 보낸 ARP 응답·IPv4 에서만 배운다. */
    if (!atomic_load(&e->gw_mac_ok)) {
        uint8_t host_mac[AT_MAC_LEN];
        uint8_t learned[AT_MAC_LEN];
        uint32_t gw_ip;
        pthread_mutex_lock(&e->mu);
        gw_ip = e->gw_ip;
        memcpy(host_mac, e->host_mac, AT_MAC_LEN);
        pthread_mutex_unlock(&e->mu);
        if (at_eth_learn_gw_mac(frame, len, host_mac, gw_ip, learned)) {
            at_mac_store(&e->mu, e->gw_mac, learned);
            atomic_store(&e->gw_mac_ok, 1);
            char macs[24];
            at_mac_format(learned, macs, sizeof(macs));
            at_log(AT_LOG_INFO, "Gateway MAC %s (USB frame)", macs);
        }
    }

    /* Keep feeding DHCP after bind so T1/T2 Renew/Rebind ACKs are seen. */
    /* 바인드 이후에도 DHCP를 넣어 T1/T2 Renew·Rebind ACK를 받는다. */
    int dhcp_ev = 0;
    if (looks_like_dhcp_to_client(frame, len)) {
        pthread_mutex_lock(&e->mu);
        if (e->dhcp) dhcp_ev = at_dhcp_on_frame(e->dhcp, frame, len);
        pthread_mutex_unlock(&e->mu);
        if (dhcp_ev < 0) {
            pause_datapath(e);
            at_log(AT_LOG_WARN, "DHCP NAK — pausing the data path and renegotiating");
        }
    }

    if (et == 0x0806) {
        handle_arp(e, frame, len);
        add_rx(e, len);
        return;
    }
    /* Do not inject IPv6 into utun; Android USB tethering is IPv4 NAT. */
    /* IPv6 는 utun에 넣지 않는다. Android USB 테더링은 IPv4 NAT 이다. */
    if (et == 0x86DD) {
        add_rx(e, len);
        return;
    }
    if (et == 0x0800 && e->utun && atomic_load(&e->lease_applied) && len > 14) {
        if (at_utun_write(e->utun, frame + 14, len - 14, AF_INET) == 0) add_rx(e, len);
        else add_rx_err(e);
        return;
    }
    add_rx(e, len);
}

#define AT_USB_GONE_FAILS 8
#define AT_DHCP_RENEW_FAIL_TICKS 24
#define AT_KA_FAIL_LIMIT 3
#define AT_MSG_USB_GONE "The USB cable was unplugged"
#define AT_MSG_TETHER_OFF "USB tethering is off. Turn USB tethering back on on the phone"

/* Fail the engine after enough hard USB IN errors (unplug / no device). */
/* USB IN 하드 오류가 쌓이면 엔진을 실패시킨다 (분리·장치 없음). */
static void note_usb_hard_fail(at_engine_t *e) {
    int n = atomic_fetch_add_explicit(&e->usb_hard_fails, 1, memory_order_relaxed) + 1;
    if (n < AT_USB_GONE_FAILS) return;
    if (e->usb && at_usb_is_gone(e->usb))
        engine_fail(e, AT_MSG_USB_GONE);
    else
        engine_fail(e, AT_MSG_TETHER_OFF);
}

static void on_usb_in(void *user, int rc, size_t got, uint8_t *buf) {
    at_engine_t *e = user;
    /* Unwrap first; the other IN URB stays pending so the gadget is not starved.
     * Do not WritePipe here — sync OUT from an IN completion serializes in IOKit. */
    /* 먼저 unwrap 한다. 다른 IN URB가 남아 있어 기기가 굶지 않는다.
     * 여기서 WritePipe 하지 않는다. IN 완료에서 동기 OUT 을 하면 IOKit가 직렬화한다. */
    if (rc == 0 && got > 0 && buf) {
        atomic_store_explicit(&e->usb_hard_fails, 0, memory_order_relaxed);
        (void)rndis_unwrap_bulk_aligned(buf, got, at_usb_packet_align(e->usb),
                                        on_eth_from_usb, e);
    } else if (rc < 0) {
        add_rx_err(e);
        (void)at_usb_clear_stall(e->usb, 1);
        note_usb_hard_fail(e);
    }
    if (atomic_load(&e->running) && buf) {
        if (at_usb_async_submit_in(e->usb, buf, AT_MAX_TRANSFER, on_usb_in, e) != 0)
            note_usb_hard_fail(e);
    }
}

static void usb_io_sync_fallback(at_engine_t *e) {
    at_log(AT_LOG_WARN, "Falling back to synchronous USB IN (TX first, 1ms)");
    while (atomic_load(&e->running)) {
        usb_drain_tx(e);
        if (at_usb_is_gone(e->usb)) {
            engine_fail(e, AT_MSG_USB_GONE);
            break;
        }
        size_t got = 0;
        int rc = at_usb_bulk_read(e->usb, e->usb_inbuf[0], at_usb_max_transfer(e->usb), &got, 1);
        if (rc < 0) {
            add_rx_err(e);
            note_usb_hard_fail(e);
            usleep(500);
            continue;
        }
        if (rc == 0 && got > 0) {
            atomic_store_explicit(&e->usb_hard_fails, 0, memory_order_relaxed);
            (void)rndis_unwrap_bulk_aligned(e->usb_inbuf[0], got,
                                            at_usb_packet_align(e->usb),
                                            on_eth_from_usb, e);
        }
    }
    usb_drain_tx(e);
}

static void *usb_io_thread(void *arg) {
    at_engine_t *e = arg;
    e->usb_inbuf[0] = malloc(AT_MAX_TRANSFER);
    e->usb_inbuf[1] = malloc(AT_MAX_TRANSFER);
    if (!e->usb_inbuf[0] || !e->usb_inbuf[1]) {
        free(e->usb_inbuf[0]);
        free(e->usb_inbuf[1]);
        e->usb_inbuf[0] = e->usb_inbuf[1] = NULL;
        engine_fail(e, "Could not allocate the USB receive buffer");
        return NULL;
    }

    if (at_usb_async_attach(e->usb) != 0) {
        usb_io_sync_fallback(e);
        free(e->usb_inbuf[0]);
        free(e->usb_inbuf[1]);
        e->usb_inbuf[0] = e->usb_inbuf[1] = NULL;
        return NULL;
    }
    int primed = 0;
    for (int i = 0; i < AT_USB_IN_DEPTH; i++) {
        if (at_usb_async_submit_in(e->usb, e->usb_inbuf[i], AT_MAX_TRANSFER,
                                   on_usb_in, e) == 0)
            primed++;
    }
    if (primed == 0) {
        at_usb_async_detach(e->usb);
        usb_io_sync_fallback(e);
        free(e->usb_inbuf[0]);
        free(e->usb_inbuf[1]);
        e->usb_inbuf[0] = e->usb_inbuf[1] = NULL;
        return NULL;
    }
    at_log(AT_LOG_INFO,
           "USB async IN x%d + async OUT (no sync Write from callback)",
           primed);
    while (atomic_load(&e->running)) {
        if (at_usb_is_gone(e->usb)) {
            engine_fail(e, AT_MSG_USB_GONE);
            break;
        }
        (void)at_usb_async_run(e->usb, 0.05);
        usb_try_start_out(e);
    }
    (void)at_usb_abort_in(e->usb);
    (void)at_usb_abort_out(e->usb);
    (void)at_usb_async_run(e->usb, 0.05);
    at_usb_async_detach(e->usb);
    usb_drain_tx(e);
    free(e->usb_inbuf[0]);
    free(e->usb_inbuf[1]);
    e->usb_inbuf[0] = e->usb_inbuf[1] = NULL;
    return NULL;
}

static void *utun_rx_thread(void *arg) {
    at_engine_t *e = arg;
    uint8_t ip[2048];
    uint8_t eth[2048];
    int wait_ms = 1;
    while (atomic_load(&e->running)) {
        size_t len = 0;
        int family = AF_INET;
        int rc = at_utun_read(e->utun, ip, sizeof(ip), &len, &family, wait_ms);
        if (rc == AT_UTUN_READ_DROP) {
            add_tx_err(e);
            wait_ms = 0;
            continue;
        }
        if (rc != 0 || len < 20) {
            wait_ms = 1;
            continue;
        }
        wait_ms = 0; /* drain a burst without sleeping / 대기 없이 버스트로 비운다 */
        /* IPv6 has no NAT on typical Android USB tethering; drop it. */
        /* 일반 Android USB 테더링은 IPv6 NAT가 없으므로 버린다. */
        if (family == AF_INET6) continue;
        /* Match USB→utun: do not forward while DHCP is renegotiating. */
        /* USB→utun 과 같이, DHCP 재협상 중에는 보내지 않는다. */
        if (!atomic_load(&e->lease_applied)) continue;
        if (family == AF_INET) (void)at_ip4_fix_checksums(ip, len);
        size_t elen = wrap_ip_eth(e, ip, len, eth, sizeof(eth));
        if (!elen) {
            add_tx_err(e);
            continue;
        }
        (void)send_eth_usb(e, eth, elen);
    }
    return NULL;
}

static int apply_lease_now(at_engine_t *e, const at_net_info_t *lease) {
    const char *ifname = at_utun_name(e->utun);
    at_net_info_t filled = *lease;
    if (!filled.dns[0] && filled.gateway[0])
        strlcpy(filled.dns, filled.gateway, sizeof(filled.dns));
    if (at_net_apply_lease(ifname, &filled, e->prefer_default_route) != 0) return -1;

    struct in_addr a;
    pthread_mutex_lock(&e->mu);
    if (inet_aton(filled.ip, &a)) e->host_ip = a.s_addr;
    if (filled.gateway[0] && inet_aton(filled.gateway, &a)) e->gw_ip = a.s_addr;
    memcpy(e->gw_mac, e->device_mac, AT_MAC_LEN);
    e->net = filled;
    uint32_t host_ip = e->host_ip;
    uint32_t gw_ip = e->gw_ip;
    uint8_t host_mac[AT_MAC_LEN];
    memcpy(host_mac, e->host_mac, AT_MAC_LEN);
    pthread_mutex_unlock(&e->mu);
    /* Placeholder MAC until ARP/IPv4 from the new gateway is learned. */
    /* 새 게이트웨이의 ARP·IPv4 를 배우기 전까지는 자리만 채운다. */
    atomic_store(&e->gw_mac_ok, 0);
    atomic_store(&e->lease_applied, 1);
    atomic_store(&e->saw_lease, 1);
    atomic_store(&e->state, AT_STATE_CONNECTED);

    /* Only steal the default path and power off Wi-Fi when the user asked. */
    /* 「기본 연결로 사용」일 때만 기본 경로를 가져오고 Wi-Fi 전원을 끈다. */
    if (e->prefer_default_route) {
        if (filled.gateway[0])
            (void)at_net_prefer_default_route(ifname, filled.gateway);
        (void)at_net_disable_wifi_during_tether();
    }

    uint8_t arp[64];
    if (gw_ip && host_ip) {
        size_t n = build_arp(arp, sizeof(arp), host_mac, host_ip, NULL, gw_ip, 1);
        if (n) (void)send_eth_usb(e, arp, n);
    }
    at_log(AT_LOG_INFO, "Tethering path is up (%s)", ifname);
    return 0;
}

static void *maint_thread(void *arg) {
    at_engine_t *e = arg;
    int ticks = 0;
    uint64_t prev_rx = 0, prev_tx = 0;
    int ka_fails = 0;
    uint64_t ka_rx = 0, ka_tx = 0;
    int lease_lost_ticks = 0;
    while (atomic_load(&e->running)) {
        usleep(500000);
        ticks++;

        if (at_usb_is_gone(e->usb)) {
            engine_fail(e, AT_MSG_USB_GONE);
            break;
        }

        if (!atomic_load(&e->lease_applied) && (ticks % 2) == 0) {
            /* Do not sync-poll USB from this thread while the datapath is up.
             * A 50ms ReadPipeTO on the control iface serializes IOKit and delays ACKs. */
            /* 데이터 경로가 살아 있는 동안 이 스레드에서 USB를 동기 poll 하지 않는다.
             * 제어 인터페이스의 50ms ReadPipeTO 는 IOKit를 직렬화해 ACK를 늦춘다. */
            if (at_usb_poll_interrupt(e->usb, 20) > 0)
                (void)at_usb_handle_control_events(e->usb);
        }
        if ((ticks % 10) == 0) {
            if (at_usb_keepalive(e->usb) != 0) {
                if (ka_fails == 0) {
                    ka_rx = atomic_load_explicit(&e->rx_bytes, memory_order_relaxed);
                    ka_tx = atomic_load_explicit(&e->tx_bytes, memory_order_relaxed);
                }
                ka_fails++;
                /* Three failed keepalives and no traffic: tethering is off or the gadget is dead. */
                /* keepalive 세 번 실패 + 트래픽 없음: 테더링 OFF 또는 기기가 죽은 것으로 본다. */
                if (atomic_load(&e->lease_applied) && ka_fails >= AT_KA_FAIL_LIMIT) {
                    uint64_t rx = atomic_load_explicit(&e->rx_bytes, memory_order_relaxed);
                    uint64_t tx = atomic_load_explicit(&e->tx_bytes, memory_order_relaxed);
                    if (rx == ka_rx && tx == ka_tx) {
                        if (at_usb_is_gone(e->usb))
                            engine_fail(e, AT_MSG_USB_GONE);
                        else
                            engine_fail(e, AT_MSG_TETHER_OFF);
                    }
                }
            } else {
                ka_fails = 0;
            }
        }

        uint8_t dhcp_eth[1518];
        size_t dhcp_len = 0;
        int just_bound = 0;
        at_net_info_t lease;
        memset(&lease, 0, sizeof(lease));
        int leased = atomic_load(&e->lease_applied);
        int still_bound = 0;
        pthread_mutex_lock(&e->mu);
        if (e->dhcp) {
            /* Also emit Renew/Rebind while the lease is live. */
            /* 임대가 살아 있는 동안에도 Renew/Rebind를 보낸다. */
            dhcp_len = at_dhcp_want_tx(e->dhcp, dhcp_eth, sizeof(dhcp_eth));
            still_bound = at_dhcp_bound(e->dhcp);
            if (still_bound) {
                at_dhcp_lease(e->dhcp, &lease);
                if (!leased)
                    just_bound = 1;
                else if (lease.ip[0] && strcmp(e->net.ip, lease.ip) != 0)
                    just_bound = 1;
            }
        }
        uint32_t gw_ip = e->gw_ip;
        uint32_t host_ip = e->host_ip;
        uint8_t host_mac[AT_MAC_LEN];
        memcpy(host_mac, e->host_mac, AT_MAC_LEN);
        pthread_mutex_unlock(&e->mu);
        if (dhcp_len > 0) (void)send_eth_usb(e, dhcp_eth, dhcp_len);
        if (just_bound && apply_lease_now(e, &lease) != 0)
            engine_fail(e, "Could not apply the DHCP lease to the interface");
        /* Expiry (T1/T2 실패) or NAK: pause forwarding until a new ACK. */
        /* 만료(T1/T2 실패) 또는 NAK: 새 ACK 전까지 포워딩을 멈춘다. */
        if (leased && !still_bound && atomic_load(&e->lease_applied)) {
            pause_datapath(e);
            at_log(AT_LOG_WARN, "DHCP lease expired; retrying address acquisition");
        }
        /* After a live lease is lost, do not stay "connected" with a dead datapath. */
        /* 살아 있던 임대를 잃으면, 죽은 데이터 경로를 연결됨으로 두지 않는다. */
        if (atomic_load(&e->saw_lease) && !atomic_load(&e->lease_applied) &&
            atomic_load(&e->state) == AT_STATE_CONNECTING) {
            lease_lost_ticks++;
            if (lease_lost_ticks >= AT_DHCP_RENEW_FAIL_TICKS)
                engine_fail(e, "Could not renew the DHCP lease");
        } else {
            lease_lost_ticks = 0;
        }

        if (atomic_load(&e->lease_applied) && gw_ip &&
            !atomic_load(&e->gw_mac_ok) && (ticks % 4) == 0) {
            uint8_t arp[64];
            size_t n = build_arp(arp, sizeof(arp), host_mac, host_ip, NULL, gw_ip, 1);
            if (n) (void)send_eth_usb(e, arp, n);
        }

        if (atomic_load(&e->lease_applied) && e->prefer_default_route && (ticks % 20) == 0)
            (void)at_net_drop_ipv6_default();

        /* Log throughput every 2s while the lease is up and bytes moved. */
        /* 리스가 살아 있고 바이트가 움직이면 2초마다 처리량을 기록한다. */
        if (atomic_load(&e->lease_applied) && (ticks % 4) == 0) {
            uint64_t rx = atomic_load_explicit(&e->rx_bytes, memory_order_relaxed);
            uint64_t tx = atomic_load_explicit(&e->tx_bytes, memory_order_relaxed);
            uint64_t drx = rx - prev_rx;
            uint64_t dtx = tx - prev_tx;
            prev_rx = rx;
            prev_tx = tx;
            if (drx >= 2048 || dtx >= 2048) {
                double rx_mbps = (double)drx * 8.0 / 2.0 / 1000000.0;
                double tx_mbps = (double)dtx * 8.0 / 2.0 / 1000000.0;
                uint64_t rxe = atomic_load_explicit(&e->rx_errors, memory_order_relaxed);
                uint64_t txe = atomic_load_explicit(&e->tx_errors, memory_order_relaxed);
                at_log(AT_LOG_INFO,
                       "Throughput RX %.2f Mbps TX %.2f Mbps (USB cap %u Mbps, err RX %llu TX %llu)",
                       rx_mbps, tx_mbps, e->link_mbps,
                       (unsigned long long)rxe, (unsigned long long)txe);
            }
        }
    }
    return NULL;
}

static char g_start_err[256];

const char *at_engine_last_error(void) {
    return g_start_err;
}

/* Tear down a half-started engine and return NULL with a logged reason. */
/* 절반만 올라간 엔진을 정리하고, 이유를 남긴 뒤 NULL 을 반환한다. */
static at_engine_t *fail_start(at_engine_t *e, const char *msg) {
    strlcpy(g_start_err, (msg && msg[0]) ? msg : "Could not start the engine",
            sizeof(g_start_err));
    at_log(AT_LOG_ERROR, "%s", g_start_err);
    if (e) at_engine_stop(e);
    return NULL;
}

at_engine_t *at_engine_start(const at_engine_opts_t *opts) {
    g_start_err[0] = '\0';
    if (!opts) return fail_start(NULL, "Engine options are missing");
    at_engine_t *e = calloc(1, sizeof(*e));
    if (!e) return fail_start(NULL, "Out of memory");
    e->opts = *opts;
    e->prefer_default_route = opts->prefer_default_route;
    pthread_mutex_init(&e->mu, NULL);
    pthread_mutex_init(&e->txq_mu, NULL);
    atomic_init(&e->running, 1);
    atomic_init(&e->state, AT_STATE_CONNECTING);
    atomic_init(&e->lease_applied, 0);
    atomic_init(&e->saw_lease, 0);
    atomic_init(&e->gw_mac_ok, 0);
    atomic_init(&e->usb_hard_fails, 0);
    e->tx_out_retries = 0;
    atomic_init(&e->rx_bytes, 0);
    atomic_init(&e->tx_bytes, 0);
    atomic_init(&e->rx_frames, 0);
    atomic_init(&e->tx_frames, 0);
    atomic_init(&e->rx_errors, 0);
    atomic_init(&e->tx_errors, 0);
    e->txq_flat = malloc((size_t)AT_TXQ_CAP * AT_TXQ_SLOT);
    if (!e->txq_flat)
        return fail_start(e, "Could not allocate the transmit buffer");

    e->usb = at_usb_open(&opts->device);
    if (!e->usb)
        return fail_start(e, "Could not open the USB RNDIS device");

    rndis_init_result_t init;
    memset(&init, 0, sizeof(init));
    if (at_usb_rndis_init(e->usb, e->device_mac, &init) != 0)
        return fail_start(e, "RNDIS initialization failed");

    memcpy(e->host_mac, e->device_mac, AT_MAC_LEN);
    e->host_mac[0] = (uint8_t)((e->host_mac[0] | 0x02) & ~0x01);
    e->host_mac[5] ^= 0x5A;
    memcpy(e->gw_mac, e->device_mac, AT_MAC_LEN);
    e->link_mbps = at_usb_bus_mbps(e->usb);
    at_log(AT_LOG_INFO,
           "Speed cap: USB %u Mbps (RNDIS reports %u Mbps), max_xfer=%u max_pkts=%u align=%uB, utun MTU %d",
           e->link_mbps, at_usb_rndis_link_mbps(e->usb),
           at_usb_max_transfer(e->usb), at_usb_max_packets(e->usb),
           1u << at_usb_packet_align(e->usb), AT_UTUN_MTU);
    if (e->link_mbps <= 480)
        at_log(AT_LOG_INFO,
               "Even on USB-C, Android tethering (RNDIS) runs at USB 2.0 High Speed");

    e->utun = at_utun_open();
    if (!e->utun)
        return fail_start(e, "Could not open the utun tunnel (administrator access required)");
    {
        char macs[24];
        at_mac_format(e->host_mac, macs, sizeof(macs));
        at_log(AT_LOG_INFO, "Host MAC %s  tunnel %s", macs, at_utun_name(e->utun));
    }
    e->dhcp = at_dhcp_create(e->host_mac);
    if (!e->dhcp)
        return fail_start(e, "Could not create the DHCP client");
    /* Without a snapshot we must not later steal the default route. */
    /* 스냅샷이 없으면 이후 기본 경로를 가져오면 안 된다. */
    if (at_net_begin_session() != 0)
        return fail_start(e, "Could not start the network session (route backup failed)");

    e->usb_rx_ok = pthread_create(&e->usb_rx, NULL, usb_io_thread, e) == 0;
    e->utun_rx_ok = pthread_create(&e->utun_rx, NULL, utun_rx_thread, e) == 0;
    if (!e->usb_rx_ok || !e->utun_rx_ok)
        return fail_start(e, "Could not start worker threads");
    usleep(200000);
    (void)at_net_register_service(at_utun_name(e->utun));
    {
        uint8_t eth[1518];
        size_t n = 0;
        pthread_mutex_lock(&e->mu);
        if (e->dhcp) n = at_dhcp_want_tx(e->dhcp, eth, sizeof(eth));
        pthread_mutex_unlock(&e->mu);
        if (n) (void)send_eth_usb(e, eth, n);
    }

    e->maint_ok = pthread_create(&e->maint, NULL, maint_thread, e) == 0;
    if (!e->maint_ok)
        return fail_start(e, "Could not start worker threads");

    at_log(AT_LOG_INFO, "Tethering engine started");
    return e;
}

void at_engine_stop(at_engine_t *e) {
    if (!e) return;
    atomic_store(&e->state, AT_STATE_STOPPING);
    atomic_store(&e->running, 0);
    /* Restore Wi-Fi before joining USB threads. A hung join + osascript SIGHUP
     * used to kill us before airport power came back. */
    /* USB 스레드 join 전에 Wi-Fi를 복구한다. join이 멈추고 osascript 가
     * SIGHUP 을 보내면 전원 복구 전에 프로세스가 죽었다. */
    (void)at_net_restore_wifi();
    if (e->usb) {
        at_usb_async_wake(e->usb);
        (void)at_usb_abort_in(e->usb);
    }
    if (e->usb_rx_ok) pthread_join(e->usb_rx, NULL);
    if (e->utun_rx_ok) pthread_join(e->utun_rx, NULL);
    if (e->maint_ok) pthread_join(e->maint, NULL);
    if (e->utun)
        at_net_clear_dhcp(at_utun_name(e->utun));
    else
        (void)at_net_end_session();
    at_dhcp_destroy(e->dhcp);
    e->dhcp = NULL;
    if (e->usb) {
        if (!at_usb_is_gone(e->usb))
            (void)at_usb_rndis_halt(e->usb);
        at_usb_close(e->usb);
        e->usb = NULL;
    }
    if (e->utun) {
        at_utun_close(e->utun);
        e->utun = NULL;
    }
    free(e->txq_flat);
    e->txq_flat = NULL;
    pthread_mutex_destroy(&e->txq_mu);
    pthread_mutex_destroy(&e->mu);
    free(e);
}

at_state_t at_engine_state(const at_engine_t *e) {
    return e ? (at_state_t)atomic_load(&e->state) : AT_STATE_IDLE;
}

void at_engine_stats(const at_engine_t *e, at_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!e) return;
    out->rx_bytes = atomic_load_explicit(&e->rx_bytes, memory_order_relaxed);
    out->tx_bytes = atomic_load_explicit(&e->tx_bytes, memory_order_relaxed);
    out->rx_frames = atomic_load_explicit(&e->rx_frames, memory_order_relaxed);
    out->tx_frames = atomic_load_explicit(&e->tx_frames, memory_order_relaxed);
    out->rx_errors = atomic_load_explicit(&e->rx_errors, memory_order_relaxed);
    out->tx_errors = atomic_load_explicit(&e->tx_errors, memory_order_relaxed);
}

void at_engine_net(const at_engine_t *e, at_net_info_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!e) return;
    pthread_mutex_lock((pthread_mutex_t *)&e->mu);
    *out = e->net;
    pthread_mutex_unlock((pthread_mutex_t *)&e->mu);
}

const char *at_engine_iface(const at_engine_t *e) {
    return (e && e->utun) ? at_utun_name(e->utun) : "";
}

const char *at_engine_error(const at_engine_t *e) {
    static _Thread_local char buf[256];
    if (!e) return "";
    pthread_mutex_lock((pthread_mutex_t *)&e->mu);
    strlcpy(buf, e->error, sizeof(buf));
    pthread_mutex_unlock((pthread_mutex_t *)&e->mu);
    return buf;
}

void at_engine_mac(const at_engine_t *e, uint8_t mac[AT_MAC_LEN]) {
    if (!mac) return;
    memset(mac, 0, AT_MAC_LEN);
    if (e) memcpy(mac, e->host_mac, AT_MAC_LEN);
}

uint32_t at_engine_link_mbps(const at_engine_t *e) {
    return e ? e->link_mbps : 0;
}
