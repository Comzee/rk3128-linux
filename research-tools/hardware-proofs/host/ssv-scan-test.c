/* SPDX-License-Identifier: MIT
 * Deterministic schedule/fault injection around the actual #492 driver functions (driver.inc).
 * Locks assert ownership. No elapsed-time sleeps: usleep_range advances the simulated clock.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static bool mutex_trylock(struct mutex *m) { if (m->held) return false; mutex_lock(m); return true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
#define WRITE_ONCE(x, v) ((x) = (v))
#define READ_ONCE(x) (x)
#define lockdep_assert_held(p) assert((p)->held)
#define BIT(x) (1U << (x))
#define container_of(p, t, f) ((t *)((char *)(p) - offsetof(t, f)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
#define SSV6XXX_INT_RX 1
#define SC_OP_OFFCHAN 1
#define SSV_HW_TXQ_NUM 5
#define IEEE80211_FCTL_PM 0x1000
#define cpu_to_le16(x) (x)
#define dev_dbg(...) ((void)0)
#define msecs_to_jiffies(x) (x)
#define jiffies_to_msecs(x) (x)
#define time_after_eq(a,b) ((long)((a)-(b)) >= 0)
static unsigned long jiffies;
static int sleeps;
static void usleep_range(int a, int b) { jiffies++; sleeps++; }
static int ktime_get(void) { return (int)jiffies; }
static int ktime_ms_delta(int a, int b) { return a-b; }
typedef int atomic_t;
static int atomic_read(atomic_t *p) { return *p; }
static void atomic_set(atomic_t *p, int v) { *p = v; }
static int atomic_cmpxchg(atomic_t *p, int old, int val) { int prev = *p; if (prev == old) *p = val; return prev; }
struct completion { int done; };
static void complete(struct completion *p) { p->done++; }
static void reinit_completion(struct completion *p) { p->done = 0; }
struct ieee80211_hdr { uint16_t frame_control; };
struct sk_buff { void *data; struct ieee80211_hdr hdr; };
struct sk_buff_head { int count; };
static bool skb_queue_empty(struct sk_buff_head *q) { return !q->count; }
struct ieee80211_channel { int band, hw_value; };
enum nl80211_channel_type { NL80211_CHAN_NO_HT, NL80211_CHAN_HT20, NL80211_CHAN_HT40PLUS };
struct ieee80211_vif { int unused; };
struct ieee80211_hw { int unused; };
struct cfg80211_scan_request { int n_channels; struct ieee80211_channel **channels; };
struct ieee80211_scan_request { struct cfg80211_scan_request req; };
struct cfg80211_scan_info { bool aborted; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct ssv_hwscan {
    struct delayed_work work;
    struct mutex lock;
    struct ieee80211_vif *vif;
    struct ieee80211_scan_request *req;
    struct ieee80211_channel *home, *pending_home;
    enum nl80211_channel_type home_type, pending_type;
    struct completion null_done;
    atomic_t null_result;
    unsigned long left_home;
    int t0, idx, state, trips;
    bool running, abort, assoc, away;
};
#define USE_THREAD_RX 1
#define MAX_HCI_RX_AGGR_SIZE 1024
#define MAX_FRAME_SIZE_DMG 1024
#define MAX_RX_PKT_RSVD 16
#define RX_HW_AGG_MODE 2
#define HCI_DBG_PRINT(...) ((void)0)
struct ssv6xxx_hci_info {
    struct ssv_softc *sc;
    int (*hci_rx_mode_cb)(void *);
    struct sk_buff *(*skb_alloc)(void *, int);
    int (*hci_peek_next_pkt_len_cb)(struct sk_buff *, void *);
    int (*hci_rx_cb)(struct sk_buff *, void *);
};
struct ssv6xxx_hci_ctrl {
    struct ssv6xxx_hci_info *shi;
    struct sk_buff *rx_buf;
    struct work_struct hci_rx_work;
    int rx_pkt, rx_work_running;
    struct mutex rx_capture_mutex, tx_drain_mutex;
    u32 txq_mask;
    int q[SSV_HW_TXQ_NUM];
};
struct ssv_hw { struct { struct ssv6xxx_hci_ctrl *hci_ctrl; } hci; };
struct ssv_softc {
    struct ssv_hw *sh;
    struct ieee80211_hw *hw;
    struct mutex mutex, tx_drain_mutex;
    struct sk_buff_head tx_skb_q;
    atomic_t ampdu_tx_frame;
    u8 rx_stamp_chan;
    int sc_flags, hw_chan;
    enum nl80211_channel_type hw_chan_type;
    struct ssv_hwscan hwscan;
};
struct ssv6xxx_hci_txq_info { u32 tx_use_page, tx_use_id; };
#define SSV_SC(h) ((h)->shi->sc)
static void *skb_put(struct sk_buff *s, size_t n) { return s->data; }
static bool ssv_rx_flush_stale(struct ssv_softc *sc, u8 channel);
static int receive_calls, capture_calls;
static u8 received_stamp;
static int if_recv(struct ssv6xxx_hci_ctrl *h, void *buf, size_t *len, u32 mode) {
    receive_calls++;
    assert(h->rx_capture_mutex.held);
    /* RF transition begins with this buffer already off SDIO, before the callback captures its channel. */
    WRITE_ONCE(h->shi->sc->rx_stamp_chan, 0);
    assert(!ssv_rx_flush_stale(h->shi->sc, 36));
    assert(h->shi->sc->rx_stamp_chan == 0);
    *len = 1;
    return 0;
}
#define IF_RECV(h,b,l,m) if_recv(h,b,l,m)
static int irq_status, irq_error, reg_error, chip_ids, chip_pages, chip_input;
static int hci_irq_status(struct ssv6xxx_hci_ctrl *h, void *status) {
    assert(h->rx_capture_mutex.held); *(u32 *)status = irq_status; return irq_error;
}
#define HCI_IRQ_STATUS(h, s) hci_irq_status(h, s)
#define HCI_TXQ_EMPTY(sh, qid) (!(sh)->hci.hci_ctrl->q[qid])
static int hal_read_txq(struct ssv_hw *sh, u32 *val, int *used) {
    assert(sh->hci.hci_ctrl->tx_drain_mutex.held);
    struct ssv6xxx_hci_txq_info *i = (void *)val;
    i->tx_use_page = chip_pages; i->tx_use_id = chip_ids; *used = chip_input;
    return reg_error;
}
#define HAL_READRG_TXQ_INFO(sh,v,u) hal_read_txq(sh,v,u)
static int ack_result, null_submits, wake_nulls, retunes, offchannel, completed;
static bool allocation_fail, prepare_fail, final_aborted, queues_stopped, post_null_busy;
static int radio_channel, radio_type;
static struct ssv_softc *active;
void ssv_scan_null_report(struct ssv_softc *sc, bool ack);
static struct sk_buff *ieee80211_nullfunc_get(struct ieee80211_hw *hw, struct ieee80211_vif *vif, int link, bool qos) {
    if (allocation_fail) return NULL;
    struct sk_buff *s = calloc(1, sizeof(*s)); assert(s); s->data = &s->hdr; return s;
}
static void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *s) { free(s); }
static bool ssv_tx_scan_null(struct ssv_softc *sc, struct ieee80211_vif *vif, struct sk_buff *s, int band) {
    assert(s->hdr.frame_control & IEEE80211_FCTL_PM); assert(queues_stopped);
    free(s); null_submits++;
    if (prepare_fail) return false;
    if (post_null_busy) chip_pages = 1;
    if (ack_result >= 0) ssv_scan_null_report(sc, ack_result != 0);
    return true;
}
static int wait_for_completion_timeout(struct completion *c, int ms) { return c->done; }
static void ssv_tx_frame(struct ssv_softc *sc, struct ieee80211_vif *vif, struct sk_buff *s, int band) {
    assert(!(s->hdr.frame_control & IEEE80211_FCTL_PM)); wake_nulls++; free(s);
}
static void ieee80211_stop_queues(struct ieee80211_hw *hw) { queues_stopped = true; }
static void ieee80211_wake_queues(struct ieee80211_hw *hw) { queues_stopped = false; }
static void synchronize_net(void) { assert(queues_stopped); }
static void ssv_retune(struct ssv_softc *sc, struct ieee80211_channel *c, enum nl80211_channel_type t, bool away) {
    assert(sc->mutex.held); radio_channel = c->hw_value; radio_type = t; sc->hw_chan = c->hw_value; sc->hw_chan_type = t; sc->sc_flags = away ? SC_OP_OFFCHAN : 0; retunes++; offchannel += away;
}
static void ssv_scan_leave(struct ssv_softc *sc) { assert(sc->mutex.held); }
static void ieee80211_scan_completed(struct ieee80211_hw *hw, struct cfg80211_scan_info *i) {
    assert(!active->mutex.held); assert(!active->hwscan.running); assert(!queues_stopped);
    completed++; final_aborted = i->aborted;
}
static int ssv_scan_dwell_ms(struct cfg80211_scan_request *r, struct ieee80211_channel *c) { return 50; }
static bool ssv_scan_passive(struct cfg80211_scan_request *r, struct ieee80211_channel *c) { return true; }
static void ssv_scan_probes(struct ssv_softc *sc, struct ieee80211_channel *c) { abort(); }
static void ssv_scan_queue(struct ssv_softc *sc, int ms) { }
static int scan_away_ms = 300, scan_home_ms = 100;
static unsigned int scan_stats[5], scan_failures[3], rx_drain_stats[3];
#define SSV_SCAN_SWITCH_MS 15
enum { SSV_SCAN_LEAVE, SSV_SCAN_TUNE, SSV_SCAN_DWELT, SSV_SCAN_HOME, SSV_SCAN_DONE };
#include "driver.inc"
static struct ieee80211_channel home = {0, 11}, scan = {1, 36}, changed = {1, 44};
static struct ieee80211_channel *channels[] = { &scan };
static struct ieee80211_scan_request req = {{1, channels}};
static int rx_mode_cb(void *s) { return 0; }
static struct sk_buff *rx_alloc_cb(void *s, int n) { return ieee80211_nullfunc_get(NULL, NULL, 0, false); }
static int next_pkt_cb(struct sk_buff *s, void *sc) { return 0; }
static int capture_cb(struct sk_buff *s, void *arg) {
    struct ssv_softc *sc = arg;
    assert(sc->sh->hci.hci_ctrl->rx_capture_mutex.held);
    capture_calls++; received_stamp = READ_ONCE(sc->rx_stamp_chan); free(s); return 0;
}
static struct ssv6xxx_hci_info shi = {NULL, rx_mode_cb, rx_alloc_cb, next_pkt_cb, capture_cb};
static struct ssv6xxx_hci_ctrl hci;
static struct ssv_hw sh;
static struct ssv_softc sc;
static void reset(void) {
    memset(&sc, 0, sizeof(sc)); memset(&hci, 0, sizeof(hci));
    sh.hci.hci_ctrl = &hci; sc.sh = &sh; shi.sc = &sc; hci.shi = &shi; active = &sc;
    sc.hwscan.home = &home; sc.hwscan.req = &req; sc.hwscan.assoc = true;
    sc.hw_chan = 11; sc.hw_chan_type = NL80211_CHAN_NO_HT;
    sc.hwscan.running = true; sc.hwscan.state = SSV_SCAN_LEAVE;
    irq_status = irq_error = reg_error = chip_ids = chip_pages = chip_input = 0;
    null_submits = wake_nulls = retunes = offchannel = completed = sleeps = 0;
    allocation_fail = prepare_fail = final_aborted = queues_stopped = false;
    post_null_busy = false;
    ack_result = 1; radio_channel = 11; radio_type = NL80211_CHAN_NO_HT;
}
static void run(void) { ssv_scan_work(&sc.hwscan.work.work); }
int main(void) {
    reset();
    /* Receiver paused after IF_RECV: even an empty chip must not publish until capture completes. */
    mutex_lock(&hci.rx_capture_mutex);
    assert(!ssv_rx_flush_stale(&sc, 36)); assert(sc.rx_stamp_chan == 0 && sleeps == 25);
    u8 old_buffer_stamp = sc.rx_stamp_chan; /* callback resumes, with no DS/HT element needed to reject it */
    mutex_unlock(&hci.rx_capture_mutex);
    assert(ssv_rx_flush_stale(&sc, 36)); assert(sc.rx_stamp_chan == 36 && old_buffer_stamp == 0);
    reset(); irq_status = SSV6XXX_INT_RX;
    assert(!ssv_rx_flush_stale(&sc, 36) && sc.rx_stamp_chan == 0);
    reset(); irq_error = -5;
    assert(!ssv_rx_flush_stale(&sc, 36) && sc.rx_stamp_chan == 0);
    irq_error = 0; assert(ssv_rx_flush_stale(&sc, 36) && sc.rx_stamp_chan == 36);
    /* Exercise both actual HCI receive implementations, not just the drain helper's lock mock. */
    for (int worker = 0; worker < 2; worker++) {
        reset(); hci.rx_buf = rx_alloc_cb(NULL, 1024); sc.rx_stamp_chan = 11;
        receive_calls = capture_calls = 0;
        if (worker) ssv6xxx_hci_rx_work(&hci.hci_rx_work); else assert(_do_rx(&hci, SSV6XXX_INT_RX) == 0);
        assert(receive_calls == 1 && capture_calls == 1 && received_stamp == 0);
        assert(ssv_rx_flush_stale(&sc, 36) && sc.rx_stamp_chan == 36);
        free(hci.rx_buf);
    }
    puts("ok RX: both real HCI receive paths, paused capture, full-chip timeout, SDIO error, recovery");
    reset(); assert(ssv_tx_idle(&sc));
    mutex_lock(&sc.tx_drain_mutex); assert(!ssv_tx_idle(&sc)); mutex_unlock(&sc.tx_drain_mutex);
    mutex_lock(&hci.tx_drain_mutex); assert(!ssv_tx_idle(&sc)); mutex_unlock(&hci.tx_drain_mutex);
    sc.tx_skb_q.count = 1; assert(!ssv_tx_idle(&sc)); sc.tx_skb_q.count = 0;
    hci.q[4] = 1; assert(!ssv_tx_idle(&sc)); hci.q[4] = 0;
    sc.ampdu_tx_frame = 1; assert(!ssv_tx_idle(&sc)); sc.ampdu_tx_frame = 0;
    chip_ids = 1; assert(!ssv_tx_idle(&sc)); chip_ids = 0;
    chip_pages = 1; assert(!ssv_tx_idle(&sc)); chip_pages = 0;
    chip_input = 1; assert(!ssv_tx_idle(&sc)); chip_input = 0;
    reg_error = 1; assert(!ssv_tx_idle(&sc)); reg_error = 0;
    assert(ssv_tx_idle(&sc));
    puts("ok TX: both dequeued-worker barriers, A-MPDUs, host/chip queues, SDIO error");
    reset(); hci.q[0] = 1; run();
    assert(completed == 1 && final_aborted && !retunes && !offchannel && !null_submits && sleeps >= 40);
    reset(); post_null_busy = true; run();
    assert(completed == 1 && final_aborted && !retunes && !offchannel && null_submits == 1 && wake_nulls == 1);
    reset(); ack_result = 0; run();
    assert(completed == 1 && final_aborted && !retunes && !offchannel && null_submits == 1 && wake_nulls == 1);
    reset(); ack_result = -1; run();
    assert(final_aborted && !offchannel && sc.hwscan.null_result == 1);
    sc.hwscan.running = true; sc.hwscan.req = &req; sc.hwscan.state = SSV_SCAN_LEAVE; sc.hwscan.abort = false; run();
    assert(null_submits == 1 && completed == 2 && final_aborted); /* unresolved old ACK cannot satisfy another null */
    ssv_scan_null_report(&sc, true); assert(sc.hwscan.null_result == 2);
    sc.hwscan.running = true; sc.hwscan.req = &req; sc.hwscan.state = SSV_SCAN_LEAVE; sc.hwscan.abort = false; ack_result = 1; run();
    assert(offchannel == 1 && null_submits == 2); /* old report resolved; a fresh announcement now works */
    reset(); allocation_fail = true; run(); assert(final_aborted && !offchannel);
    reset(); prepare_fail = true; run(); assert(final_aborted && !offchannel && sc.hwscan.null_result == 0);
    reset(); run(); assert(offchannel == 1 && sc.hwscan.running && sc.hwscan.away && !completed);
    puts("ok scan: backlog/failed ACK/missing ACK/allocation/prepare failures stay home; success departs");
    /* Channel+width changed while away: latest request is restored on both completion and cancellation. */
    for (int cancel = 0; cancel < 2; cancel++) {
        reset(); run(); assert(offchannel == 1);
        mutex_lock(&sc.mutex); assert(ssv_hwscan_defer_channel(&sc, &home, NL80211_CHAN_HT20)); assert(ssv_hwscan_defer_channel(&sc, &changed, NL80211_CHAN_HT40PLUS)); mutex_unlock(&sc.mutex);
        sc.hwscan.abort = cancel; run();
        assert(completed == 1 && final_aborted == (bool)cancel && radio_channel == 44 && radio_type == NL80211_CHAN_HT40PLUS);
        assert(!sc.hwscan.pending_home && !sc.hwscan.running);
        mutex_lock(&sc.mutex); assert(!ssv_hwscan_defer_channel(&sc, &home, NL80211_CHAN_HT20)); mutex_unlock(&sc.mutex);
    }
    reset(); sc.hwscan.pending_home = &changed; sc.hwscan.pending_type = NL80211_CHAN_HT20; sc.hwscan.state = SSV_SCAN_HOME; run();
    assert(radio_channel == 44 && radio_type == NL80211_CHAN_HT20 && !sc.hwscan.away);
    puts("ok channel: completion, cancellation and intermediate home trips apply deferred channel and width");
    puts("ssv-scan-sim: all passed");
}
