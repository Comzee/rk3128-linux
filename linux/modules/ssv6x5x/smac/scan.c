// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * The driver's own scan (development record #492, docs/RK3128-LINUX.md "Wi-Fi scan while associated (#492)").
 *
 * Without hw_scan, mac80211 scans for this driver in software: while associated it goes back to the operating channel after
 * nearly every scanned channel, for 200 ms, and then waits another 100 ms there with its queues stopped (no .flush) before the
 * next one: 16-18 s for the 36-39 channels of the world domain on the board, and half the pings lost, against Android's 7.6 s
 * with none lost. The vendor module (3.10, read back in #492) has no scan of its own either; its mac80211 scheduled the scan.
 * So the schedule is ours:
 *  - associated: tell the AP we sleep (a plain power-save null, the frame the vendor's mac80211 sent), stop mac80211's queues,
 *    then visit as many channels in a row as fit in scan_away_ms off the operating channel, go back for scan_home_ms (null
 *    with the power-save bit clear, queues woken: the AP sends what it held), and so on. The AP holds our frames while we
 *    are away, so a ping is late, never lost.
 *  - not associated: the channels one after the other, nothing to go back to.
 *  - a channel the regulatory domain allows to transmit on gets one probe request per SSID right after the retune and
 *    scan_active_ms; any other channel (No-IR, radar) is listened to for scan_passive_ms (above one 102.4 ms beacon interval
 *    plus the retune, so one beacon of every BSS there is heard).
 * Frames received during the scan carry the channel the radio was on when they came off SDIO (the RX stamp, smac/dev.c),
 * which is what cfg80211 lists them at; mac80211's hw->conf stays on the operating channel throughout.
 * hw_scan_mode=0 hands every scan back to mac80211's software scan (the comparison).
 * Open (#492): while associated, a passive channel rarely delivers another BSS's beacon to the host (probe responses come
 * through), whatever the dwell, the BSSID register or the nulls; not associated, every one. Android never lists those BSSes
 * either. docs/RK3128-LINUX.md has the measurements.
 */
#include <linux/version.h>
#include <linux/module.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <net/mac80211.h>
#include <ssv6200.h>
#include <hci/hctrl.h>
#include "dev.h"

static int hw_scan_mode = 1;
module_param(hw_scan_mode, int, 0644);
MODULE_PARM_DESC(hw_scan_mode, "1: the driver's own scan (default), 0: mac80211's software scan");
static int scan_away_ms = 300;
module_param(scan_away_ms, int, 0644);
MODULE_PARM_DESC(scan_away_ms, "Longest time off the operating channel per trip while associated (ms)");
static int scan_home_ms = 100;
module_param(scan_home_ms, int, 0644);
MODULE_PARM_DESC(scan_home_ms, "Time back on the operating channel between trips (ms)");
static int scan_active_ms = 50;
module_param(scan_active_ms, int, 0644);
MODULE_PARM_DESC(scan_active_ms, "Dwell on a channel where probe requests are sent (ms)");
static int scan_passive_ms = 130;
module_param(scan_passive_ms, int, 0644);
MODULE_PARM_DESC(scan_passive_ms, "Dwell on a channel that is only listened to (ms)");
/* the retune and its RX drain, both ways, for the trip budget (measured: chan_sw_stats and the unassociated scan, ~15 ms) */
#define SSV_SCAN_SWITCH_MS 15
/* scan_stats: scans, aborted scans, trips off the operating channel, channels, and the last scan's duration in ms */
static unsigned int scan_stats[5];
module_param_array(scan_stats, uint, NULL, 0644);
MODULE_PARM_DESC(scan_stats, "Driver scans, aborted, trips, channels, last scan's ms");

/* Failure stage: queued data, PM announcement (allocation/preparation/ACK), or post-announcement drain. */
static unsigned int scan_failures[3];
module_param_array(scan_failures, uint, NULL, 0644);
MODULE_PARM_DESC(scan_failures, "Scans aborted at home: data drain, PM announcement, PM drain");

enum { SSV_SCAN_LEAVE, SSV_SCAN_TUNE, SSV_SCAN_DWELT, SSV_SCAN_HOME, SSV_SCAN_DONE };

static bool ssv_scan_passive(struct cfg80211_scan_request *req, struct ieee80211_channel *chan)
{
    return (chan->flags & (IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR)) || req->n_ssids == 0;
}
static int ssv_scan_dwell_ms(struct cfg80211_scan_request *req, struct ieee80211_channel *chan)
{
    int ms = ssv_scan_passive(req, chan) ? scan_passive_ms : scan_active_ms;
    int want = req->duration ? (int)req->duration * 1024 / 1000 : 0;
    return max(ms, want);
}
/* Config and the final scan restore share sc->mutex. A callback never takes s->lock (the opposite lock order). */
bool ssv_hwscan_defer_channel(struct ssv_softc *sc, struct ieee80211_channel *chan, enum nl80211_channel_type type)
{
    struct ssv_hwscan *s = &sc->hwscan;
    lockdep_assert_held(&sc->mutex);
    if (!s->running)
        return false;
    s->pending_home = chan;
    s->pending_type = type;
    return true;
}
void ssv_scan_null_report(struct ssv_softc *sc, bool ack)
{
    /* Keep a timed-out announcement outstanding until its own report arrives. Never reuse an unresolved token. */
    if (atomic_cmpxchg(&sc->hwscan.null_result, 1, ack ? 2 : 3) == 1)
        complete(&sc->hwscan.null_done);
}
/* the power-save null to the AP: pm true before leaving the operating channel, false on return */
static bool ssv_scan_null(struct ssv_softc *sc, bool pm)
{
    struct ssv_hwscan *s = &sc->hwscan;
    struct sk_buff *skb;
    struct ieee80211_hdr *hdr;
    skb = ieee80211_nullfunc_get(sc->hw, s->vif, -1, false);
    if (skb == NULL)
        return false;
    hdr = (struct ieee80211_hdr *)skb->data;
    if (pm)
        hdr->frame_control |= cpu_to_le16(IEEE80211_FCTL_PM);
    if (pm) {
        /* A previous timeout can still have a report in flight. Do not let it acknowledge a different announcement. */
        if (atomic_read(&s->null_result) == 1) {
            ieee80211_free_txskb(sc->hw, skb);
            return false;
        }
        reinit_completion(&s->null_done);
        atomic_set(&s->null_result, 1);
        if (!ssv_tx_scan_null(sc, s->vif, skb, s->home->band)) {
            atomic_set(&s->null_result, 0); /* never submitted */
            return false;
        }
        if (!wait_for_completion_timeout(&s->null_done, msecs_to_jiffies(40)))
            return false;
        return atomic_read(&s->null_result) == 2;
    }
    ssv_tx_frame(sc, s->vif, skb, s->home->band);
    return true;
}
static void ssv_scan_probes(struct ssv_softc *sc, struct ieee80211_channel *chan)
{
    struct ssv_hwscan *s = &sc->hwscan;
    struct cfg80211_scan_request *req = &s->req->req;
    struct ieee80211_scan_ies *ies = &s->req->ies;
    int band = chan->band, i;
    for (i = 0; i < req->n_ssids; i++) {
        struct sk_buff *skb;
        struct ieee80211_hdr *hdr;
        skb = ieee80211_probereq_get(sc->hw, s->vif->addr, req->ssids[i].ssid, req->ssids[i].ssid_len,
                                     ies->len[band] + ies->common_ie_len);
        if (skb == NULL)
            continue;
        if (ies->len[band])
            skb_put_data(skb, ies->ies[band], ies->len[band]);
        if (ies->common_ie_len)
            skb_put_data(skb, ies->common_ies, ies->common_ie_len);
        hdr = (struct ieee80211_hdr *)skb->data;
        if (!is_broadcast_ether_addr(req->bssid)) {
            memcpy(hdr->addr1, req->bssid, ETH_ALEN);
            memcpy(hdr->addr3, req->bssid, ETH_ALEN);
        }
        if (req->no_cck)
            IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_CTL_NO_CCK_RATE;
        ssv_tx_frame(sc, s->vif, skb, band);
    }
}
static void ssv_scan_queue(struct ssv_softc *sc, int ms)
{
    mod_delayed_work(sc->hwscan.wq, &sc->hwscan.work, ms > 0 ? msecs_to_jiffies(ms) : 0);
}
/* Consume configuration changes only under sc->mutex, also used by .config. s->lock is held by the scan worker. */
static void ssv_scan_home_channel(struct ssv_softc *sc)
{
    struct ssv_hwscan *s = &sc->hwscan;
    if (s->pending_home) {
        s->home = s->pending_home;
        s->home_type = s->pending_type;
        s->pending_home = NULL;
    }
    /* A failed departure already is home: do not interrupt its still-draining TX with an unnecessary RF rewrite. */
    if (s->home && ((sc->sc_flags & SC_OP_OFFCHAN) || sc->hw_chan != s->home->hw_value || sc->hw_chan_type != s->home_type))
        ssv_retune(sc, s->home, s->home_type, false);
}
/* back on the operating channel: retune, tell the AP we are awake, let mac80211 send again */
static void ssv_scan_go_home(struct ssv_softc *sc)
{
    struct ssv_hwscan *s = &sc->hwscan;
    mutex_lock(&sc->mutex);
    ssv_scan_home_channel(sc);
    ssv_scan_null(sc, false);
    mutex_unlock(&sc->mutex);
    ieee80211_wake_queues(sc->hw);
    s->away = false;
}
static void ssv_scan_work(struct work_struct *work)
{
    struct ssv_hwscan *s = container_of(to_delayed_work(work), struct ssv_hwscan, work);
    struct ssv_softc *sc = container_of(s, struct ssv_softc, hwscan);
    struct cfg80211_scan_request *req;
    struct ieee80211_channel *chan;
    struct cfg80211_scan_info info = {};
    int dwell, failure;
    mutex_lock(&s->lock);
    if (!s->running) {
        mutex_unlock(&s->lock);
        return;
    }
    req = &s->req->req;
again:
    switch (s->state) {
    case SSV_SCAN_LEAVE:
        if (s->abort || s->idx >= req->n_channels) {
            s->state = SSV_SCAN_DONE;
            goto again;
        }
        /*
         * hold mac80211's frames and send what the driver already has (a data frame after the null would tell the AP we
         * are awake again: they carry no power-save bit), then tell the AP we sleep and let the null leave before the radio
         */
        ieee80211_stop_queues(sc->hw);
        synchronize_net(); /* complete TX callbacks that entered before stop_queues */
        failure = !ssv_tx_wait_idle(sc, 40) ? 0 : !ssv_scan_null(sc, true) ? 1 : !ssv_tx_wait_idle(sc, 40) ? 2 : -1;
        if (failure >= 0) {
            scan_failures[failure]++;
            /* Stay home. PM=1 might have succeeded even when its report/drain timed out: explicitly wake the AP. */
            ssv_scan_null(sc, false);
            ieee80211_wake_queues(sc->hw);
            s->abort = true;
            s->state = SSV_SCAN_DONE;
            goto again;
        }
        s->away = true;
        s->left_home = jiffies;
        s->trips++;
        s->state = SSV_SCAN_TUNE;
        goto again;
    case SSV_SCAN_TUNE:
        if (s->abort || s->idx >= req->n_channels) {
            s->state = SSV_SCAN_DONE;
            goto again;
        }
        chan = req->channels[s->idx++];
        dwell = ssv_scan_dwell_ms(req, chan);
        mutex_lock(&sc->mutex);
        ssv_retune(sc, chan, NL80211_CHAN_NO_HT, true);
        mutex_unlock(&sc->mutex);
        if (!ssv_scan_passive(req, chan))
            ssv_scan_probes(sc, chan);
        s->state = SSV_SCAN_DWELT;
        ssv_scan_queue(sc, dwell);
        break;
    case SSV_SCAN_DWELT:
        if (s->abort || s->idx >= req->n_channels) {
            s->state = SSV_SCAN_DONE;
            goto again;
        }
        if (s->assoc) {
            int away = jiffies_to_msecs(jiffies - s->left_home);
            dwell = ssv_scan_dwell_ms(req, req->channels[s->idx]);
            if (away + 2 * SSV_SCAN_SWITCH_MS + dwell > scan_away_ms) {
                s->state = SSV_SCAN_HOME;
                goto again;
            }
        }
        s->state = SSV_SCAN_TUNE;
        goto again;
    case SSV_SCAN_HOME:
        ssv_scan_go_home(sc);
        s->state = SSV_SCAN_LEAVE;
        ssv_scan_queue(sc, scan_home_ms);
        break;
    case SSV_SCAN_DONE:
    default:
        /* .config cannot slip a request between this final restore and relinquishing ownership of the radio. */
        mutex_lock(&sc->mutex);
        ssv_scan_home_channel(sc);
        if (s->assoc && s->away)
            ssv_scan_null(sc, false);
        s->away = false;
        ieee80211_wake_queues(sc->hw);
        ssv_scan_leave(sc);
        WRITE_ONCE(s->running, false);
        mutex_unlock(&sc->mutex);
        info.aborted = s->abort;
        scan_stats[0]++;
        scan_stats[1] += s->abort;
        scan_stats[2] += s->trips;
        scan_stats[3] += s->idx;
        scan_stats[4] = (unsigned int)ktime_ms_delta(ktime_get(), s->t0);
        dev_dbg(sc->dev, "scan: %d channels, %d trips, %u ms%s\n", s->idx, s->trips, scan_stats[4],
                s->abort ? ", aborted" : "");
        s->req = NULL;
        s->vif = NULL;
        ieee80211_scan_completed(sc->hw, &info);
        break;
    }
    mutex_unlock(&s->lock);
}
int ssv_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_scan_request *hw_req)
{
    struct ssv_softc *sc = hw->priv;
    struct ssv_hwscan *s = &sc->hwscan;
    struct ieee80211_channel *home = hw->conf.chandef.chan;
    if (!hw_scan_mode || s->wq == NULL || vif->type != NL80211_IFTYPE_STATION || vif->p2p)
        return 1;
    if (hw_req->req.n_channels == 0)
        return 1;
    mutex_lock(&s->lock);
    if (s->running) {
        mutex_unlock(&s->lock);
        return -EBUSY;
    }
    mutex_lock(&sc->mutex);
    s->vif = vif;
    s->req = hw_req;
    s->assoc = vif->cfg.assoc && home != NULL;
    s->home = home;
    s->home_type = home ? cfg80211_get_chandef_type(&hw->conf.chandef) : NL80211_CHAN_NO_HT;
    s->pending_home = NULL;
    s->idx = 0;
    s->trips = 0;
    s->abort = false;
    s->away = false;
    s->t0 = ktime_get();
    s->state = s->assoc ? SSV_SCAN_LEAVE : SSV_SCAN_TUNE;
    ssv_scan_enter(sc);
    WRITE_ONCE(s->running, true);
    mutex_unlock(&sc->mutex);
    ssv_scan_queue(sc, 0);
    mutex_unlock(&s->lock);
    return 0;
}
void ssv_cancel_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
    struct ssv_softc *sc = hw->priv;
    struct ssv_hwscan *s = &sc->hwscan;
    mutex_lock(&s->lock);
    if (s->running) {
        s->abort = true;
        if (s->state != SSV_SCAN_HOME && s->state != SSV_SCAN_LEAVE)
            s->state = SSV_SCAN_DONE;
        ssv_scan_queue(sc, 0);
    }
    mutex_unlock(&s->lock);
}
/* the interface stops (ssv6200_stop): end a scan that is still running, reported as aborted */
void ssv_hwscan_stop(struct ssv_softc *sc)
{
    struct ssv_hwscan *s = &sc->hwscan;
    if (s->wq == NULL)
        return;
    mutex_lock(&s->lock);
    if (s->running) {
        s->abort = true;
        s->state = SSV_SCAN_DONE;
        ssv_scan_queue(sc, 0);
    }
    mutex_unlock(&s->lock);
    flush_delayed_work(&s->work);
}
int ssv_hwscan_init(struct ssv_softc *sc)
{
    struct ssv_hwscan *s = &sc->hwscan;
    mutex_init(&s->lock);
    init_completion(&s->null_done);
    atomic_set(&s->null_result, 0);
    INIT_DELAYED_WORK(&s->work, ssv_scan_work);
    s->wq = alloc_ordered_workqueue("ssv6x5x_scan", WQ_HIGHPRI | WQ_MEM_RECLAIM);
    if (s->wq == NULL)
        return -ENOMEM;
    /* what mac80211 sets up itself for a software scan (main.c), and one call for every band */
    sc->hw->wiphy->max_scan_ssids = 4;
    sc->hw->wiphy->max_scan_ie_len = 1024;
    ieee80211_hw_set(sc->hw, SINGLE_SCAN_ON_ALL_BANDS);
    return 0;
}
void ssv_hwscan_deinit(struct ssv_softc *sc)
{
    struct ssv_hwscan *s = &sc->hwscan;
    if (s->wq == NULL)
        return;
    cancel_delayed_work_sync(&s->work);
    destroy_workqueue(s->wq);
    s->wq = NULL;
}
