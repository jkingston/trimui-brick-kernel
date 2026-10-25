// SPDX-License-Identifier: GPL-2.0-only
#include "xradio.h"
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include "bh.h"
#include "station-runtime.h"
#include "monitor-frame.h"
#include "recovery.h"
#include "station-address.h"
#include "station-power.h"
#include "station-plan.h"
#include "station-tx-state.h"
#include "station-bus.h"
#include "station-feedback.h"
#include "station-rate.h"
#include "station-edca.h"
#include "station-ht.h"
#include "station-policy.h"
#include "key-transaction.h"
#include "ccmp-rx.h"

bool allow_ht = true;
module_param(allow_ht,bool,0400);
MODULE_PARM_DESC(allow_ht, "Enable single-stream HT20 long-GI station mode");
const bool allow_station = true;
#define XRS_SLOTS 16
#define XRS_BUS_BUFFER 4096
struct xrs_runtime {
 struct xradio_common *hw;
 struct dentry *debugfs;
 u64 policy_uploads,policy_hits,policy_waits,protection_requests;
 u64 rate_attempts[28],rate_success[28],feedback_rejected,feedback_success_no_rate;
 u64 tx_queued[4],tx_acked[4],tx_failed[4],tx_tid[8],rx_tid[8];
 spinlock_t lock;
 struct mutex bus_lock; /* held across get_tx -> SDIO write -> written */
 struct mutex control_lock;
 struct delayed_work timeout;
 struct work_struct policy_work;
 struct xpc_cache policies;
 struct xpc_handle policy_lease[XRS_SLOTS],management_lease;
 struct xpc_key policy_key[XRS_SLOTS];
 bool policy_held[XRS_SLOTS],policy_ready[XRS_SLOTS];
 struct xrs_tx_slot slots[XRS_SLOTS];
 struct sk_buff *skb[XRS_SLOTS];
 struct xrs_tx_confirm feedback[XRS_SLOTS];
 bool feedback_valid[XRS_SLOTS];
 u8 *wire[XRS_SLOTS];
 size_t length[XRS_SLOTS];
 bool credit[XRS_SLOTS], acked[XRS_SLOTS], poisoned, joined, associated;
 u8 command_buffer[XRS_BUS_BUFFER] __aligned(4);
 struct xrs_keys keys;
 struct ieee80211_key_conf *keyconf[11]; /* identity cookies only */
 bool keypairwise[11];
 u8 keyid[11];
 bool keychanging,closing,powerchanging;
 int power_min,power_max,power_confirmed,bss_power;
 unsigned long long next_key_owner;
 unsigned int rx_reject_reports, rx_beacon_reports;
 u32 enqueue_order;
 unsigned int next_slot;
 int sending;
 struct xrs_ht_link ht;
	struct xrs_ba_state ba;
 struct wsm_edca_params edca;
 struct xrs_station_settings settings;
 struct xrs_address address;
};

/* Research counters contain no packet bytes or key material. RX counts are
 * deliveries to mac80211, before its replay checks, not accepted IP packets. */
static int xrs_stats_show(struct seq_file *file,void *unused)
{
 struct xrs_runtime *s=file->private;unsigned long flags;
 u64 queued[4],acked[4],failed[4],tx_tid[8],rx_tid[8],uploads,hits,waits;
 unsigned int i,refs=0,ready=0;u64 attempts[28],success[28],rejected,protection,no_rate;
 spin_lock_irqsave(&s->lock,flags);
 memcpy(queued,s->tx_queued,sizeof(queued));memcpy(acked,s->tx_acked,sizeof(acked));
 memcpy(failed,s->tx_failed,sizeof(failed));memcpy(tx_tid,s->tx_tid,sizeof(tx_tid));
 memcpy(rx_tid,s->rx_tid,sizeof(rx_tid));
 memcpy(attempts,s->rate_attempts,sizeof(attempts));memcpy(success,s->rate_success,sizeof(success));
 rejected=s->feedback_rejected;protection=s->protection_requests;
 no_rate=s->feedback_success_no_rate;
 uploads=s->policy_uploads;hits=s->policy_hits;waits=s->policy_waits;
 for(i=0;i<XPC_SLOTS;i++) {refs+=s->policies.slots[i].refs;ready+=s->policies.slots[i].state==XPC_READY;}
 spin_unlock_irqrestore(&s->lock,flags);
 seq_printf(file,"policy uploads=%llu hits=%llu capacity_waits=%llu refs=%u ready_slots=%u\n",
            uploads,hits,waits,refs,ready);
 seq_printf(file,"validated_feedback_rejected=%llu per_rate_protection_requests=%llu\n",rejected,protection);
 seq_printf(file,"firmware_success_without_rate=%llu\n",no_rate);
 seq_puts(file,"rate attempts successes (0..11=legacy;12..19=HT20LGI;20..27=HT20SGI)\n");
 for(i=0;i<28;i++)if(attempts[i])seq_printf(file,"%u %llu %llu\n",i,attempts[i],success[i]);
 seq_puts(file,"ac queued acked failed (0=VO 1=VI 2=BE 3=BK)\n");
 for(i=0;i<4;i++)seq_printf(file,"%u %llu %llu %llu\n",i,queued[i],acked[i],failed[i]);
 seq_puts(file,"tid tx_queued rx_delivered_to_mac80211\n");
 for(i=0;i<8;i++)seq_printf(file,"%u %llu %llu\n",i,tx_tid[i],rx_tid[i]);
 return 0;
}
DEFINE_SHOW_ATTRIBUTE(xrs_stats);

static_assert(offsetof(struct xrs_runtime,command_buffer)%4==0);

u8 *xrs_runtime_command_buffer(struct xradio_common *hw)
{
 return hw->station ? hw->station->command_buffer : NULL;
}

static int station_policy_prepare(struct xrs_runtime *s);
static void station_policy_work(struct work_struct *work);

static void report_slot(struct xrs_runtime *s, unsigned int i, bool acked)
{
 struct sk_buff *skb;
 struct ieee80211_tx_info *info;
 struct xrs_feedback feedback={0};
 struct xrs_tx_confirm rejected_feedback={0};
 int feedback_error=0;
 unsigned int rate;
 u8 *wire;
 unsigned long flags;
 spin_lock_irqsave(&s->lock, flags);
 if (s->slots[i].phase != XRS_DONE) {
  spin_unlock_irqrestore(&s->lock, flags); return;
 }
 skb=s->skb[i]; wire=s->wire[i]; s->skb[i]=NULL; s->wire[i]=NULL;
 if(s->feedback_valid[i]) {
  if(xrs_tx_success_no_rate(&s->feedback[i]))s->feedback_success_no_rate++;
  feedback_error=xrs_feedback_decode(&feedback,&s->feedback[i]);
  if(feedback_error) {
   s->feedback_rejected++;
   rejected_feedback=s->feedback[i];
  } else for(rate=0;rate<feedback.used;rate++) {
   const struct xrs_rate_entry *r=&feedback.entry[rate];
   unsigned int index=r->ht?12+r->index+8*r->short_gi:r->index;
   if(index<28 && !r->width40 && !r->greenfield && !r->stbc) {
    s->rate_attempts[index]+=feedback.count[rate];
    if(acked && rate+1==feedback.used)s->rate_success[index]++;
   }
  }
 }
 s->feedback_valid[i]=false;
 if(acked)s->tx_acked[(s->slots[i].cookie>>16)&3]++;
 else s->tx_failed[(s->slots[i].cookie>>16)&3]++;
 if(s->policy_held[i]) {xpc_release(&s->policies,s->policy_lease[i]);s->policy_held[i]=false;}
 s->policy_ready[i]=false;
 xrs_tx_released(&s->slots[i]);
 if(!s->poisoned && !s->closing)queue_work(s->hw->workqueue,&s->policy_work);
 spin_unlock_irqrestore(&s->lock, flags);
 if(feedback_error) {
  unsigned int fc=0,tid=255;
  bool no_ack=false;
  if(skb) {
   no_ack=!!(IEEE80211_SKB_CB(skb)->flags&IEEE80211_TX_CTL_NO_ACK);
   if(skb->len>=2)fc=skb->data[0]|((unsigned int)skb->data[1]<<8);
   /* Accepted QoS data has a three-address header; do not inspect payload. */
   if(skb->len>=26 && (fc&0x008c)==0x0088 && (fc&0x0300)!=0x0300)
    tid=skb->data[24]&15;
  }
  dev_warn_ratelimited(s->hw->pdev,
   "XR829 rejected TX feedback cookie=%08x ac=%u err=%d status=%u flags=%02x rate=%04x retries=%04x,%04x,%04x,%04x,%04x,%04x ack_failures=%u fc=%04x tid=%u no_ack=%u\n",
   rejected_feedback.packet_id,(rejected_feedback.packet_id>>16)&3,
   feedback_error,rejected_feedback.status,rejected_feedback.flags,
   rejected_feedback.rate,rejected_feedback.retries[0],rejected_feedback.retries[1],
   rejected_feedback.retries[2],rejected_feedback.retries[3],
   rejected_feedback.retries[4],rejected_feedback.retries[5],
   rejected_feedback.ack_failures,fc,tid,no_ack);
 }
 kfree(wire);
 if (skb) {
  info=IEEE80211_SKB_CB(skb);
  ieee80211_tx_info_clear_status(info);
  for(rate=0;rate<IEEE80211_TX_MAX_RATES;rate++) {
   info->status.rates[rate].idx=-1;
   info->status.rates[rate].count=0;
   info->status.rates[rate].flags=0;
   if(rate<feedback.used) {
    info->status.rates[rate].idx=feedback.index[rate];
    info->status.rates[rate].count=feedback.count[rate];
    if(feedback.entry[rate].ht)info->status.rates[rate].flags|=IEEE80211_TX_RC_MCS;
    if(feedback.entry[rate].width40)info->status.rates[rate].flags|=IEEE80211_TX_RC_40_MHZ_WIDTH;
    if(feedback.entry[rate].short_gi)info->status.rates[rate].flags|=IEEE80211_TX_RC_SHORT_GI;
    if(feedback.entry[rate].greenfield)info->status.rates[rate].flags|=IEEE80211_TX_RC_GREEN_FIELD;
    if(feedback.short_preamble[rate])
     info->status.rates[rate].flags|=IEEE80211_TX_RC_USE_SHORT_PREAMBLE;
   }
  }
  if (acked) info->flags |= IEEE80211_TX_STAT_ACK;
  ieee80211_tx_status_irqsafe(s->hw->hw,skb);
 }
 if(allow_recovery && s->slots[i].generation >=
    clamp_t(unsigned int,recovery_test_limit?recovery_test_limit:4094,8,4094))
  xrs_recovery_request(s->hw);
 if (!READ_ONCE(s->poisoned) && !READ_ONCE(s->closing) &&
     READ_ONCE(s->hw->recovery_state)==XRS_RUNNING) {
  if(!READ_ONCE(s->keychanging) && !READ_ONCE(s->powerchanging))ieee80211_wake_queues(s->hw->hw);
 }
}

/* No identity update may overlap keys, TX, JOIN or an uncertain scan. */
static int station_address_send(void *context,const u8 *address)
{
 struct xrs_runtime *s=context;
 return wsm_write_mib(s->hw,0x1046,(void *)address,6,0);
}
static int station_address_apply(struct xrs_runtime *s,const u8 *address,int install)
{
 unsigned long flags;unsigned int i;int ret=0;bool idle;
 spin_lock_irqsave(&s->hw->scan_only_lock,flags);
 idle=s->hw->scan_only_state.phase==XR_SCAN_IDLE;
 spin_unlock_irqrestore(&s->hw->scan_only_lock,flags);
 if(!idle)return -EBUSY;
 spin_lock_irqsave(&s->lock,flags);
 if(s->poisoned || s->closing || READ_ONCE(s->hw->bh_error))ret=-EIO;
 else if(s->joined || s->associated)ret=-EBUSY;
 for(i=0;!ret && i<XRS_SLOTS;i++)
  if(s->slots[i].phase!=XRS_FREE)ret=-EBUSY;
 for(i=0;!ret && i<11;i++)if(s->keyconf[i])ret=-EBUSY;
 spin_unlock_irqrestore(&s->lock,flags);
 if(ret)return ret;
 ret=xrs_address_apply(&s->address,address,install,station_address_send,s);
 if(s->address.poisoned) {
  spin_lock_irqsave(&s->lock,flags);
  s->poisoned=true;s->policies.poisoned=true;WRITE_ONCE(s->hw->bh_error,1);
  spin_unlock_irqrestore(&s->lock,flags);
  ieee80211_stop_queues(s->hw->hw);
  dev_err(s->hw->pdev,"XR829 MAC update uncertain; fresh boot required\n");
 }
 return ret;
}
int xrs_runtime_add_address(struct xradio_common *hw,const u8 *address)
{
 struct xrs_runtime *s=hw->station;int ret;
 if(!s || !hw->scan_only_configured)return -EIO;
 mutex_lock(&s->control_lock);
 ret=station_address_apply(s,address,1);
 mutex_unlock(&s->control_lock);
 return ret;
}
void xrs_runtime_remove_address(struct xradio_common *hw)
{
 struct xrs_runtime *s=hw->station;if(!s)return;
 mutex_lock(&s->control_lock);xrs_address_remove(&s->address);
 mutex_unlock(&s->control_lock);
}

static int station_key_drain(struct xrs_runtime *s);
static int station_key_send(void *context,u16 command,const u8 *payload,size_t size);

void xrs_runtime_stop(struct xradio_common *hw)
{
 struct xrs_runtime *s=hw->station;
 struct wsm_reset reset={.link_id=0,.reset_statistics=false};
 u8 active[4]={0x10,0,0,0},rcpi[4]={0x0d,0,0,16};
 unsigned long flags;unsigned int i;int ret=0;
 if(!s)return;
 mutex_lock(&s->control_lock);
 if(!s->joined && !s->poisoned)goto out;
 if(s->poisoned || READ_ONCE(hw->bh_error)) {ret=-EIO;goto failed;}
 ret=station_key_drain(s);if(ret)goto failed;
 /* Exclude all receive delivery during teardown; queued TX has completed. */
 spin_lock_irqsave(&s->lock,flags);
 s->joined=false;s->associated=false;
 spin_unlock_irqrestore(&s->lock,flags);
	ret = wsm_set_block_ack_policy(hw, 0, 0, 0);
	if (ret)
		goto failed;
 /* Explicitly remove every live key; RESET is not assumed to erase keys. */
 for(i=0;i<11;i++)if(s->keyconf[i]) {
  ret=xrs_key_remove(&s->keys,i,s->keys.slots[i].owner,station_key_send,hw);
  if(ret)goto failed;
  s->keyconf[i]=NULL;
 }
 xrs_address_reset(&s->address);
 ret=wsm_reset(hw,&reset,0);if(ret)goto failed;
 /* Restore per-VIF active and RCPI policy, as vendor unjoin does. */
 ret=wsm_write_mib(hw,0x1006,active,sizeof(active),0);if(ret)goto failed;
 ret=wsm_write_mib(hw,0x1009,rcpi,sizeof(rcpi),0);if(ret)goto failed;
 spin_lock_irqsave(&s->lock,flags);
 ret=xpc_reset(&s->policies);
	memset(&s->settings, 0, sizeof(s->settings));
	memset(&s->ht, 0, sizeof(s->ht));
	memset(&s->ba, 0, sizeof(s->ba));
	s->keychanging = false;
 spin_unlock_irqrestore(&s->lock,flags);
 if(ret)goto failed;
 /* Retain TX generations and key owner counter across reconnects. */
 if(!READ_ONCE(s->closing) && READ_ONCE(hw->recovery_state)==XRS_RUNNING)ieee80211_wake_queues(hw->hw);
 dev_info(hw->pdev,"XR829 station reset confirmed; ready for new association\n");
 goto out;
failed:
 mutex_lock(&s->bus_lock);
 spin_lock_irqsave(&s->lock,flags);
 s->poisoned=true;s->policies.poisoned=true;WRITE_ONCE(hw->bh_error,1);
 s->joined=false;s->associated=false;s->keychanging=true;
 memset(s->keyconf,0,sizeof(s->keyconf));
 for(i=0;i<XRS_SLOTS;i++)xrs_tx_abort(&s->slots[i],1);
 spin_unlock_irqrestore(&s->lock,flags);
 mutex_unlock(&s->bus_lock);
 for(i=0;i<XRS_SLOTS;i++)report_slot(s,i,false);
 ieee80211_stop_queues(hw->hw);
 dev_err(hw->pdev,"XR829 station teardown failed%d; fresh boot required\n",ret);
out:
 mutex_unlock(&s->control_lock);
}

static void tx_timeout(struct work_struct *work)
{
 struct xrs_runtime *s=container_of(to_delayed_work(work),struct xrs_runtime,timeout);
 unsigned long flags; bool expired=false, pending=false; unsigned int i;
 spin_lock_irqsave(&s->lock,flags);
 for(i=0;i<XRS_SLOTS;i++) {
  if(xrs_tx_pending(&s->slots[i])) {
   pending=true;
   if(xrs_tx_expired(&s->slots[i],jiffies,5*HZ)) expired=true;
  }
 }
 if(expired) s->poisoned=true;
 if(!expired && pending && !s->closing)queue_delayed_work(s->hw->workqueue,&s->timeout,HZ);
 spin_unlock_irqrestore(&s->lock,flags);
 if(expired) {
  dev_err(s->hw->pdev,"XR829 station queued/inflight TX timeout; recovery required\n");
  xrs_runtime_stop(s->hw);
 }
}

int xrs_runtime_init(struct xradio_common *hw)
{
 struct xrs_runtime *s;
 if(!allow_station) return 0;
 s=kzalloc(sizeof(*s),GFP_KERNEL); if(!s) return -ENOMEM;
 for(unsigned int i=0;i<4;i++) {
  s->edca.params[i].cwMin=31;s->edca.params[i].cwMax=1023;
  s->edca.params[i].aifns=2;
 }
 s->hw=hw;s->sending=-1;spin_lock_init(&s->lock);
 mutex_init(&s->bus_lock);mutex_init(&s->control_lock);
 INIT_DELAYED_WORK(&s->timeout,tx_timeout);
 INIT_WORK(&s->policy_work,station_policy_work);xpc_init(&s->policies);hw->station=s;
 s->debugfs=debugfs_create_dir("xr829-station",NULL);
 debugfs_create_file("traffic",0400,s->debugfs,s,&xrs_stats_fops);
 return 0;
}
void xrs_runtime_free(struct xradio_common *hw)
{
 struct xrs_runtime *s=hw->station;unsigned long flags;if(!s)return;
 spin_lock_irqsave(&s->lock,flags);s->closing=true;spin_unlock_irqrestore(&s->lock,flags);
 ieee80211_stop_queues(hw->hw);
 debugfs_remove_recursive(s->debugfs);
 cancel_delayed_work_sync(&s->timeout);xrs_runtime_stop(hw);
 cancel_work_sync(&s->policy_work);cancel_delayed_work_sync(&s->timeout);
 kfree(s);hw->station=NULL;
}
bool xrs_runtime_joined(struct xradio_common *hw)
{
 return hw->station && smp_load_acquire(&hw->station->joined);
}

/* control_lock held. Block new host frames across the confirmed power MIB.
 * Already submitted firmware frames cannot be recalled by this command. */
static int station_power_update(struct xrs_runtime *s,int configured_power)
{
 struct xradio_common *hw=s->hw;struct ieee80211_channel *ch=hw->hw->conf.chandef.chan;
 struct wsm_reset reset={.link_id=0,.reset_statistics=false};
 unsigned long flags;int ret,power,reset_ret;
 if(s->poisoned || READ_ONCE(hw->bh_error))return -EIO;
 ret=ch && !(ch->flags&IEEE80211_CHAN_DISABLED)?
  xrs_power_limit(ch->max_power,configured_power,s->bss_power,
                  s->power_min,s->power_max,&power):-EINVAL;
 if(!ret && power==s->power_confirmed)return 0;
 ieee80211_stop_queues(hw->hw);
 /* Barrier against get_tx -> written; never hold bus_lock over a command. */
 mutex_lock(&s->bus_lock);
 spin_lock_irqsave(&s->lock,flags);s->powerchanging=true;
 spin_unlock_irqrestore(&s->lock,flags);mutex_unlock(&s->bus_lock);
 if(!ret)ret=wsm_set_output_power(hw,power,0);
 /* BH/timeout/free can change state while the synchronous command waits.
  * Never reopen the gate or report success for an invalidated lifetime. */
 spin_lock_irqsave(&s->lock,flags);
 if(!ret && (s->poisoned || READ_ONCE(hw->bh_error) || s->closing))ret=-EIO;
 if(!ret) {s->power_confirmed=power;s->powerchanging=false;}
 spin_unlock_irqrestore(&s->lock,flags);
 if(ret) {
  /* No more host data; ask firmware to leave the BSS before quarantine.
   * Failed RESET is uncertainty, not evidence that the radio is silent. */
  reset_ret=READ_ONCE(s->poisoned) || READ_ONCE(hw->bh_error)?
            -EIO:wsm_reset(hw,&reset,0);
  spin_lock_irqsave(&s->lock,flags);
  s->poisoned=true;s->policies.poisoned=true;s->joined=false;s->associated=false;
  WRITE_ONCE(hw->bh_error,1);
  spin_unlock_irqrestore(&s->lock,flags);
  dev_err(hw->pdev,"XR829 power update failed %d, reset %d; reload required\n",ret,reset_ret);
  return ret;
 }
 dev_info(hw->pdev,"XR829 power setting confirmed %d/10dBm\n",power);
 if(!s->closing && !s->keychanging)ieee80211_wake_queues(hw->hw);
 xradio_bh_wakeup(hw);
 return 0;
}

/* Report the firmware-confirmed setting, not mac80211's requested ceiling.
 * This is a configured limit, not a measurement of RF output. */
int xrs_get_txpower(struct ieee80211_hw *dev,struct ieee80211_vif *vif,
                   unsigned int link_id,int *dbm)
{
 struct xradio_common *hw=dev->priv;struct xrs_runtime *s=hw->station;
 int ret=0,power;unsigned long flags;
 if(link_id || vif->type!=NL80211_IFTYPE_STATION)return -EOPNOTSUPP;
 if(!s)return -ENODATA;
 mutex_lock(&s->control_lock);
 spin_lock_irqsave(&s->lock,flags);
 if(!s->joined || s->poisoned || s->closing || s->powerchanging ||
    READ_ONCE(hw->bh_error))ret=-ENODATA;
 else {
  power=s->power_confirmed;
  /* The API exposes whole dBm; round down, including negative fractions. */
  *dbm=power/10-(power<0 && power%10);
 }
 spin_unlock_irqrestore(&s->lock,flags);
 mutex_unlock(&s->control_lock);return ret;
}

int xrs_runtime_config(struct xradio_common *hw,u32 changed)
{
 struct xrs_runtime *s=hw->station;
 if(s && smp_load_acquire(&s->joined) &&
    (!hw->hw->conf.chandef.chan ||
     hw->hw->conf.chandef.chan->hw_value!=s->settings.channel)) {
  xrs_runtime_stop(hw);return -EOPNOTSUPP;
 }
 if(s && (changed&(IEEE80211_CONF_CHANGE_POWER|IEEE80211_CONF_CHANGE_CHANNEL))) {
  int ret=0;
  mutex_lock(&s->control_lock);
  if(s->joined)ret=station_power_update(s,hw->hw->conf.power_level);
  mutex_unlock(&s->control_lock);return ret;
 }
 return 0;
}

void xrs_runtime_tx(struct xradio_common *hw,struct ieee80211_sta *sta,struct sk_buff *skb)
{
 struct xrs_runtime *s=hw->station;
 unsigned long flags;unsigned int i,ac;int n;u8 *wire;
 struct ieee80211_key_conf *key=IEEE80211_SKB_CB(skb)->control.hw_key;
 bool protected,rollover=false;
 struct xrs_policy_rate chain[XRS_SELECTED_RATES]={0};unsigned int rate_count=0;
 struct xpc_key policy;bool data_frame=false,protection_requested=false;
 struct xrs_tx tx={.rate=7,.queue=0,.priority=5,.policy=0};
 if(!s || READ_ONCE(hw->recovery_state)!=XRS_RUNNING ||
    !smp_load_acquire(&s->joined) || READ_ONCE(s->poisoned)) goto drop;
 /* CCMP requires a confirmed key and mac80211-generated IV. */
 if(skb_linearize(skb)) goto drop;
 if(skb->len<24)goto drop;
	/* Firmware owns ADDBA/DELBA exchange, including its own responses.
	 * Do not send mac80211's duplicate BA action frames to the controller.
	 */
	if ((skb->data[0] & 0xfc) == 0xd0 && skb->len >= 25 &&
	    skb->data[24] == WLAN_CATEGORY_BACK)
		goto drop;
 ac=skb_get_queue_mapping(skb);if(ac>=4)goto drop;
 tx.queue=wsm_queue_id_to_wsm(ac);
 protected=!!(skb->data[1]&0x40);
 if(protected != !!key)goto drop;
	if ((skb->data[0] & 0x0c) == 8) {
		int header;

		data_frame = true;
		if (!smp_load_acquire(&s->associated) || !sta)
			goto drop;
		header = xrs_data_header_size(skb->data, skb->len);

		if (header < 0)
			goto drop;
		tx.priority = header == 26 ? skb->data[24] & 7 : 0;
  if(sta) {
   struct ieee80211_tx_rate selected[XRS_SELECTED_RATES];unsigned int j;
   const u16 represented=IEEE80211_TX_RC_MCS|IEEE80211_TX_RC_40_MHZ_WIDTH|
     IEEE80211_TX_RC_SHORT_GI|IEEE80211_TX_RC_GREEN_FIELD|IEEE80211_TX_RC_USE_SHORT_PREAMBLE|
     IEEE80211_TX_RC_USE_RTS_CTS|IEEE80211_TX_RC_USE_CTS_PROTECT;
   ieee80211_get_tx_rates(IEEE80211_SKB_CB(skb)->control.vif,sta,skb,selected,XRS_SELECTED_RATES);
   for(j=0;j<XRS_SELECTED_RATES;j++) {
    struct xrs_rate_entry *r=&chain[j].rate;
    if(selected[j].idx<0 || !selected[j].count)break;
    /* Vendor and cw1200 do not encode per-rate RTS/CTS: firmware controls
     * protection globally. Retain requests in diagnostics, not invented wire
     * bits. Explicit per-rate protection remains an integration limitation. */
    if(selected[j].flags&(IEEE80211_TX_RC_USE_RTS_CTS|IEEE80211_TX_RC_USE_CTS_PROTECT))
     protection_requested=true;
    if(selected[j].flags&~represented)goto drop;
    r->index=selected[j].idx;r->ht=!!(selected[j].flags&IEEE80211_TX_RC_MCS);
    r->width40=!!(selected[j].flags&IEEE80211_TX_RC_40_MHZ_WIDTH);
    r->short_gi=!!(selected[j].flags&IEEE80211_TX_RC_SHORT_GI);
    r->greenfield=!!(selected[j].flags&IEEE80211_TX_RC_GREEN_FIELD);
    r->short_preamble=!!(selected[j].flags&IEEE80211_TX_RC_USE_SHORT_PREAMBLE);
    chain[j].count=selected[j].count;rate_count++;
   }
  }
  if((skb->data[1]&3)!=1 || memcmp(skb->data+4,s->settings.bssid,6))goto drop;
 } else if((skb->data[0]&0x0c)==0) {
  u8 subtype=skb->data[0]&0xf0;
  if(subtype==0x40) {
   struct ieee80211_vif *vif=IEEE80211_SKB_CB(skb)->control.vif;
   if(!vif || xrs_monitor_probe(skb->data,skb->len,vif->addr,
       s->settings.bssid,s->settings.ssid,s->settings.ssid_len,
       smp_load_acquire(&s->associated)))goto drop;
  } else {
   if(subtype!=0 && subtype!=0x20 && subtype!=0xb0 && subtype!=0xa0 && subtype!=0xc0)goto drop;
   if(memcmp(skb->data+4,s->settings.bssid,6))goto drop;
  }
 } else goto drop;
 wire=kzalloc(XRS_BUS_BUFFER,GFP_ATOMIC);if(!wire)goto drop;
 spin_lock_irqsave(&s->lock,flags);
 if(s->poisoned || !s->joined || s->keychanging || s->closing ||
    READ_ONCE(hw->recovery_state)!=XRS_RUNNING)goto unlock_free;
 if(data_frame) {
  if(xrs_policy_chain(&policy,chain,rate_count,s->settings.operational_rates,&s->ht))goto unlock_free;
 } else if(xrs_policy_management(&policy,s->settings.basic_rates))goto unlock_free;
 tx.rate=policy.rates[0];
 if(protected) {
  unsigned int k=key->hw_key_idx;
  if(key->cipher!=WLAN_CIPHER_SUITE_CCMP || key->keyidx ||
     !(key->flags&IEEE80211_KEY_FLAG_PAIRWISE) ||
     !(key->flags&IEEE80211_KEY_FLAG_GENERATE_IV) ||
     key->iv_len!=8 || key->icv_len!=8 || k>=11 || s->keyconf[k]!=key ||
     !xrs_key_usable(&s->keys,k,s->keys.slots[k].owner))goto unlock_free;
 }

 i=xrs_tx_available(s->slots,XRS_SLOTS,s->next_slot);
 if(i==XRS_SLOTS) {ieee80211_stop_queues(hw->hw);goto unlock_free;}
 if(xrs_tx_reserve(&s->slots[i],i,ac))goto unlock_free;
 s->next_slot=(i+1)%XRS_SLOTS;
 s->slots[i].enqueue_order=++s->enqueue_order;
 tx.packet_id=s->slots[i].cookie;
 n=xrs_tx_encode_mode(wire,XRS_BUS_BUFFER,&tx,skb->data,skb->len,
                 min_t(size_t,hw->wsm_caps.sizeInpChBuf,XRS_BUS_BUFFER-512),protected);
 if(n<0) {xrs_tx_abort(&s->slots[i],0);xrs_tx_released(&s->slots[i]);goto unlock_free;}
 s->policy_key[i]=policy;s->policy_held[i]=false;s->policy_ready[i]=false;
 if(protection_requested)s->protection_requests++;
 s->tx_queued[ac]++;
 if((skb->data[0]&0xfc)==0x88)s->tx_tid[skb->data[24]&7]++;
 s->feedback_valid[i]=false;s->skb[i]=skb;s->wire[i]=wire;s->length[i]=n;s->credit[i]=false;s->acked[i]=false;
 s->slots[i].queued_at=jiffies;
 {
  unsigned int free_slot;
  for(free_slot=0;free_slot<XRS_SLOTS;free_slot++)
   if(s->slots[free_slot].phase==XRS_FREE && s->slots[free_slot].generation<4095)break;
  if(free_slot==XRS_SLOTS)ieee80211_stop_queues(hw->hw);
 }
 if(!s->closing) {
  queue_delayed_work(hw->workqueue,&s->timeout,HZ);
  queue_work(hw->workqueue,&s->policy_work);
 }
 spin_unlock_irqrestore(&s->lock,flags);return;
unlock_free:
 if(allow_recovery)
  rollover=xrs_tx_epoch_due(s->slots,XRS_SLOTS,
   clamp_t(unsigned int,recovery_test_limit?recovery_test_limit:4094,8,4094));
 spin_unlock_irqrestore(&s->lock,flags);kfree(wire);
 if(rollover)xrs_recovery_request(hw);
drop:
 ieee80211_free_txskb(hw->hw,skb);
}

/* Caller holds control_lock across selection, command submission and result.
 * That prevents an old upload being sent after reset starts a new generation. */
static int station_policy_prepare(struct xrs_runtime *s)
{
 struct xradio_common *hw=s->hw;struct xpc_ticket ticket;
 struct xpc_handle lease;struct xpc_key key;u8 payload[XRS_POLICY_SIZE];
 unsigned long flags;unsigned int i,slot;u32 cookie;int ret;
 spin_lock_irqsave(&s->lock,flags);
 if(s->poisoned || READ_ONCE(hw->bh_error)) {ret=-EIO;goto unlock;}
 if(!s->joined) {ret=0;goto unlock;}
 i=xrs_tx_oldest(s->slots,XRS_SLOTS);
 if(i==XRS_SLOTS) {ret=0;goto unlock;}
 if(s->policy_ready[i]) {ret=0;goto wake;}
 if(!s->policy_held[i]) {
  ret=xpc_reserve(&s->policies,&s->policy_key[i],&s->policy_lease[i]);
  if(ret==XPC_BUSY) {s->policy_waits++;ret=0;goto unlock;}
  if(ret<0)goto failed;
  s->policy_held[i]=true;
 }
 lease=s->policy_lease[i];cookie=s->slots[i].cookie;
 ret=xpc_publish(&s->policies,lease,&slot);
 if(ret==XPC_OK) {s->policy_hits++;goto ready;}
 if(ret!=XPC_WAIT)goto failed;
 ret=xpc_begin_upload(&s->policies,lease,&ticket,&key);
 if(ret)goto failed; /* control_lock permits only one uploader */
 spin_unlock_irqrestore(&s->lock,flags);
 ret=xrs_policy_wire(payload,sizeof(payload),ticket.slot,&key);
 if(ret>=0)ret=wsm_write_mib(hw,0x1016,payload,sizeof(payload),0);
 spin_lock_irqsave(&s->lock,flags);
 if(xpc_finish_upload(&s->policies,ticket,!ret)) {ret=-EIO;goto failed;}
 s->policy_uploads++;
 if(s->poisoned || s->slots[i].cookie!=cookie || s->slots[i].phase!=XRS_QUEUED) {
  ret=-EIO;goto failed;
 }
 ret=xpc_publish(&s->policies,lease,&slot);if(ret)goto failed;
ready:
 /* Policy byte is immutable after BH takes transport ownership. */
 s->wire[i][14]=slot;s->policy_ready[i]=true;ret=0;
wake:
 spin_unlock_irqrestore(&s->lock,flags);xradio_bh_wakeup(hw);return ret;
failed:
 s->poisoned=true;s->policies.poisoned=true;WRITE_ONCE(hw->bh_error,1);ret=-EIO;
unlock:
 spin_unlock_irqrestore(&s->lock,flags);return ret;
}
static void station_policy_work(struct work_struct *work)
{
 struct xrs_runtime *s=container_of(work,struct xrs_runtime,policy_work);int ret;
 mutex_lock(&s->control_lock);
 ret=s->closing?0:station_policy_prepare(s);mutex_unlock(&s->control_lock);
 if(ret<0)xrs_runtime_stop(s->hw);
}

int xrs_runtime_get_tx(struct xradio_common *hw,u8 **data,size_t *len)
{
 struct xrs_runtime *s=hw->station;unsigned long flags;unsigned int i;
 if(!s)return 0;
 mutex_lock(&s->bus_lock);
 spin_lock_irqsave(&s->lock,flags);
 if(s->poisoned || s->powerchanging || hw->hw_bufs_used>=hw->wsm_caps.numInpChBufs)goto none;
 i=xrs_tx_oldest(s->slots,XRS_SLOTS);
 if(i==XRS_SLOTS)goto none;
 /* A later ready frame must never overtake the oldest unprepared frame. */
 if(!s->policy_ready[i]) {if(!s->closing)queue_work(hw->workqueue,&s->policy_work);goto none;}
 xrs_tx_begin(&s->slots[i]);s->credit[i]=true;s->sending=i;
 *data=s->wire[i];*len=s->length[i];
 if(!s->closing) {
  queue_delayed_work(hw->workqueue,&s->timeout,HZ);
  queue_work(hw->workqueue,&s->policy_work);
 }
 spin_unlock_irqrestore(&s->lock,flags);
 return 1; /* bus_lock intentionally held until written */
none:
 spin_unlock_irqrestore(&s->lock,flags);mutex_unlock(&s->bus_lock);return 0;
}
int xrs_runtime_written(struct xradio_common *hw,u8 *data,bool failed)
{
 struct xrs_runtime *s=hw->station;unsigned long flags;int i,credits;bool done,acked;
 if(!s)return -1;
 spin_lock_irqsave(&s->lock,flags);i=s->sending;
 if(i<0 || s->wire[i]!=data) {spin_unlock_irqrestore(&s->lock,flags);return -1;}
 credits=xrs_bus_written(&s->slots[i],&s->credit[i],failed,&done);
 acked=s->acked[i] && !failed;
 if(failed) {s->poisoned=true;WRITE_ONCE(hw->bh_error,1);}
 s->sending=-1;spin_unlock_irqrestore(&s->lock,flags);
 mutex_unlock(&s->bus_lock);
 if(done)report_slot(s,i,acked);
 return credits;
}

/* Called only by BH: validated unique cookies, not message count, return credits. */
int xrs_runtime_confirm(struct xradio_common *hw,int id,const u8 *data,size_t size)
{
 struct xrs_runtime *s=hw->station;
 struct xrs_tx_confirm confirm[XRS_SLOTS];unsigned long flags;
 int count,k,credits;u16 completed,acked,accepted,copied=0;
 if(!s)return -EOPNOTSUPP;
 if(id==0x0404) {count=xrs_tx_confirm_decode(confirm,data,size);if(count>=0)count=1;}
 else count=xrs_tx_batch_decode(confirm,XRS_SLOTS,data,size);
 if(count<0)return count;
 spin_lock_irqsave(&s->lock,flags);
 credits=xrs_bus_confirm_many(s->slots,s->credit,XRS_SLOTS,confirm,count,
                              &completed,&acked,&accepted);
 for(k=0;k<XRS_SLOTS;k++)if(accepted & BIT(k))s->acked[k]=!!(acked & BIT(k));
 for(k=0;k<count;k++) {
  unsigned int slot=confirm[k].packet_id&255;
  if(slot>=XRS_SLOTS || !(accepted&BIT(slot)) || (copied&BIT(slot)) ||
     confirm[k].packet_id!=s->slots[slot].cookie)continue;
  s->feedback[slot]=confirm[k];s->feedback_valid[slot]=true;copied|=BIT(slot);
 }
 spin_unlock_irqrestore(&s->lock,flags);
 if(credits>hw->hw_bufs_used) {
  WRITE_ONCE(s->poisoned,true);WRITE_ONCE(hw->bh_error,1);
  for(k=0;k<XRS_SLOTS;k++)if(completed & BIT(k))report_slot(s,k,false);
  return -EINVAL;
 }
 if(credits) {
  wsm_release_tx_buffer(hw,credits);
  dev_info_ratelimited(hw->pdev,"XR829 TX confirmations%u unique-credits%d status%u\n",count,credits,confirm[0].status);
 }
 for(k=0;k<XRS_SLOTS;k++)if(completed & BIT(k))report_slot(s,k,!!(acked & BIT(k)));
 return 0;
}

int xrs_runtime_rx(struct xradio_common *hw,const u8 *data,size_t size)
{
 struct xrs_runtime *s=hw->station;struct xrs_rx_view view;struct sk_buff *skb;
 struct xrs_ccmp_rx_view secure;struct ieee80211_rx_status *status;
 unsigned long flags;unsigned int i;bool pairwise=false,decrypted=false;
 u8 groups=0;int decoded=0;
 if(!s || size<16+24)return 0;
 spin_lock_irqsave(&s->lock,flags);
 if(!s->joined || s->poisoned || s->keychanging)goto out;
 if(data[17]&0x40) {
  if(!s->associated)goto out;
  for(i=0;i<11;i++)if(s->keyconf[i] &&
      xrs_key_usable(&s->keys,i,s->keys.slots[i].owner)) {
   if(s->keypairwise[i])pairwise=true;
   else groups|=BIT(s->keyid[i]);
  }
  decoded=xrs_ccmp_rx_decode(&secure,data,size,s->settings.bssid,pairwise,groups);
  if(decoded<0)goto rejected;
  view=secure.frame;decrypted=true;
 } else {
  decoded=xrs_rx_open_decode(&view,data,size,s->settings.bssid);
  if(decoded<0)goto rejected;
 }
 if(data[16]==0x80 && s->rx_beacon_reports++<2)
  dev_info(hw->pdev,"XR829 beacon accepted rate=%04x flags=%08x len=%zu\n",
           view.radio.rate,view.radio.flags,size);
 if(view.amsdu && !s->ht.enabled)goto out;
	if (view.radio.flags & 0x08) {
		unsigned int tid;

		if (!s->ba.enabled || !view.rate.ht || (data[16] & 0xfc) != 0x88 ||
		    size < 16 + 26 || (data[16 + 4] & 1))
			goto out;
		tid = data[16 + 24] & 15;
		if (tid >= 8 || !(s->ba.rx & BIT(tid)))
			goto out;
	}
 if(view.radio.channel!=s->settings.channel || !xrs_ht_rx_allowed(&s->ht,&view.rate))goto out;
 skb=dev_alloc_skb(view.frame_size);if(!skb)goto out;
 skb_put_data(skb,data+16,view.frame_size);status=IEEE80211_SKB_RXCB(skb);
 memset(status,0,sizeof(*status));status->band=NL80211_BAND_2GHZ;
 status->freq=ieee80211_channel_to_frequency(view.radio.channel,NL80211_BAND_2GHZ);
 status->signal=view.signal_dbm;status->rate_idx=view.rate_index;
 status->encoding=view.rate.ht?RX_ENC_HT:RX_ENC_LEGACY;
 status->bw=view.rate.width40?RATE_INFO_BW_40:RATE_INFO_BW_20;
 if(view.rate.short_preamble)status->enc_flags|=RX_ENC_FLAG_SHORTPRE;
 if(view.rate.short_gi)status->enc_flags|=RX_ENC_FLAG_SHORT_GI;
 if(view.rate.greenfield)status->enc_flags|=RX_ENC_FLAG_HT_GF;
 if(view.rate.stbc)status->enc_flags|=1<<RX_ENC_FLAG_STBC_SHIFT;
 /* Retain IV, PN and MIC for mac80211 replay validation and stripping. */
 if(decrypted)status->flag|=RX_FLAG_DECRYPTED;
 if((data[16]&0xfc)==0x88)s->rx_tid[data[40]&7]++;
 ieee80211_rx_irqsafe(hw->hw,skb);
 goto out;
rejected:
 if(s->rx_reject_reports++<16) {
  struct xrw_rx radio;
  xrw_rx_decode(&radio,data,size);
  dev_info(hw->pdev,"XR829 RX rejected err=%d fc=%04x status=%u rate=%04x flags=%08x len=%zu\n",
           decoded,xrw_get16(data+16),radio.status,radio.rate,radio.flags,size);
 }
out:
 spin_unlock_irqrestore(&s->lock,flags);return 0;
}

static int station_mib32(struct xradio_common *hw,u16 mib,u32 value)
{
 __le32 v=cpu_to_le32(value);return wsm_write_mib(hw,mib,&v,4,0);
}
static int snapshot_bss(struct xradio_common *hw,struct ieee80211_vif *vif,
                        struct xrs_station_settings *settings)
{
 struct ieee80211_channel *ch=hw->hw->conf.chandef.chan;
 struct cfg80211_bss *bss;const struct cfg80211_bss_ies *ies;const u8 *ie;
 struct ieee80211_supported_band *band=hw->hw->wiphy->bands[NL80211_BAND_2GHZ];
 int n,j,ret=0;u8 ids[]={WLAN_EID_SUPP_RATES,WLAN_EID_EXT_SUPP_RATES};
 if(!ch || ch->band!=NL80211_BAND_2GHZ ||
    ch->flags&(IEEE80211_CHAN_DISABLED|IEEE80211_CHAN_NO_IR) ||
    !vif->bss_conf.bssid)return -EINVAL;
 bss=cfg80211_get_bss(hw->hw->wiphy,ch,vif->bss_conf.bssid,NULL,0,
                     IEEE80211_BSS_TYPE_ESS,IEEE80211_PRIVACY_ANY);
 if(!bss)return -ENOENT;
 memset(settings,0,sizeof(*settings));settings->channel=ch->hw_value;
 settings->privacy=!!(bss->capability&WLAN_CAPABILITY_PRIVACY);
 memcpy(settings->bssid,bss->bssid,6);settings->beacon_interval=bss->beacon_interval;
 settings->dtim=1;settings->aid=1; /* preassociation plan validation only */
 rcu_read_lock();ies=rcu_dereference(bss->ies);
 if(!ies) {ret=-EINVAL;goto out;}
 ie=cfg80211_find_ie(WLAN_EID_SSID,ies->data,ies->len);
 if(!ie || ie[1]>32) {ret=-EINVAL;goto out;}
 settings->ssid_len=ie[1];memcpy(settings->ssid,ie+2,ie[1]);
 ie=cfg80211_find_ie(WLAN_EID_TIM,ies->data,ies->len);
 if(ie && ie[1]>=4 && ie[3]) settings->dtim=ie[3];
 for(n=0;n<ARRAY_SIZE(ids);n++) {
  ie=cfg80211_find_ie(ids[n],ies->data,ies->len);if(!ie)continue;
  for(j=0;j<ie[1];j++) {
   unsigned int k;u8 rate=ie[2+j];
   for(k=0;k<band->n_bitrates;k++)if(band->bitrates[k].bitrate==(rate&127)*5) {
    settings->operational_rates|=BIT(band->bitrates[k].hw_value);
    if(rate&128)settings->basic_rates|=BIT(band->bitrates[k].hw_value);
   }
  }
 }
out:
 rcu_read_unlock();cfg80211_put_bss(hw->hw->wiphy,bss);return ret;
}

static int station_join(struct xradio_common *hw,struct ieee80211_vif *vif,
                        bool *setup_started)
{
 struct xrs_runtime *s=hw->station;struct xrs_station_settings settings;
 struct xrs_station_plan plan;struct wsm_join join={};
 struct wsm_set_tx_queue_params queue={};
 struct wsm_beacon_filter_control beacons={.enabled=0,.bcn_count=1};
 struct wsm_rx_filter filter={};
 int i,ret,power;unsigned long flags;bool scan_idle;
 *setup_started=false;
 if(!s || s->poisoned)return -EIO;
 if(hw->wsm_caps.numInpChBufs < 2)return -EOPNOTSUPP;
 spin_lock_irqsave(&hw->scan_only_lock,flags);
 scan_idle=hw->scan_only_state.phase==XR_SCAN_IDLE;
 spin_unlock_irqrestore(&hw->scan_only_lock,flags);
 if(!scan_idle)return -EBUSY;
 if(s->joined)return 0;
 /* Reject invalid/unsupported BSS settings before any firmware mutation. */
 ret=snapshot_bss(hw,vif,&settings);if(ret)return ret;
 ret=xrs_station_prepare(&plan,&settings);if(ret)return ret;
 /* From here a failed command can leave partially applied radio state. */
 *setup_started=true;
 ret=station_address_apply(s,vif->addr,0);if(ret)return ret;
 for(i=0;i<4;i++) {
  ret=wsm_set_tx_queue_params(hw,&queue,i,0);if(ret)return ret;
 }
 ret=wsm_set_edca_params(hw,&s->edca,0);if(ret)return ret;
 {
  struct xpc_key key;
  struct xpc_ticket ticket;u8 payload[XRS_POLICY_SIZE];
  ret=xrs_policy_management(&key,settings.basic_rates);if(ret)return ret;
  spin_lock_irqsave(&s->lock,flags);
  ret=xpc_reserve(&s->policies,&key,&s->management_lease);
  if(ret==XPC_WAIT)ret=xpc_begin_upload(&s->policies,s->management_lease,&ticket,&key);
  else ret=-EIO; /* first JOIN requires a fresh confirmed-reset cache */
  spin_unlock_irqrestore(&s->lock,flags);if(ret)return ret;
  ret=xrs_policy_wire(payload,sizeof(payload),ticket.slot,&key);
  if(ret>=0)ret=wsm_write_mib(hw,0x1016,payload,sizeof(payload),0);
  spin_lock_irqsave(&s->lock,flags);
  if(xpc_finish_upload(&s->policies,ticket,!ret))ret=-EIO;
  else s->policy_uploads++;
  spin_unlock_irqrestore(&s->lock,flags);if(ret)return ret;
 }
 ret=wsm_set_block_ack_policy(hw,0,0,0);if(ret)return ret;
 ret=station_mib32(hw,0x1020,2);if(ret)return ret; /* unprotected management */
 join.mode=1;join.band=0;join.channelNumber=settings.channel;
 join.dtimPeriod=settings.dtim;join.beaconInterval=settings.beacon_interval;
 join.basicRateSet=settings.basic_rates;join.ssidLength=settings.ssid_len;
 memcpy(join.ssid,settings.ssid,settings.ssid_len);memcpy(join.bssid,settings.bssid,6);
	/* Apply limits before JOIN can transmit association management frames. */
	ret = xrs_power_limit(hw->hw->conf.chandef.chan->max_power,
			      hw->hw->conf.power_level, vif->bss_conf.txpower,
			      hw->scan_power.min_power_level,
			      hw->scan_power.max_power_level, &power);
	if (ret)
		return ret;
	ret = wsm_set_output_power(hw, power, 0);
	if (ret)
		return ret;
 ret=wsm_join(hw,&join,0);if(ret)return ret;
 /* Vendor applies receive filters after JOIN, with one beacon forwarded. */
 ret=wsm_beacon_filter_control(hw,&beacons,0);if(ret)return ret;
 ret=wsm_set_rx_filter(hw,&filter,0);if(ret)return ret;
 ret=wsm_set_bssid_filtering(hw,true,0);if(ret)return ret;

	/* JOIN may narrow, but must not widen, the calibrated configuration range. */
	join.minPowerLevel = max(join.minPowerLevel, hw->scan_power.min_power_level);
	join.maxPowerLevel = min(join.maxPowerLevel, hw->scan_power.max_power_level);
 ret=xrs_power_limit(hw->hw->conf.chandef.chan->max_power,hw->hw->conf.power_level,
                     vif->bss_conf.txpower,join.minPowerLevel,join.maxPowerLevel,&power);
 if(ret)return ret;
 ret=wsm_set_output_power(hw,power,0);if(ret)return ret;
 s->power_min=join.minPowerLevel;s->power_max=join.maxPowerLevel;
 s->power_confirmed=power;s->bss_power=vif->bss_conf.txpower;
 s->settings=settings;smp_store_release(&s->joined,true);
 dev_info(hw->pdev,"XR829 joined BSS on channel%u at%d/10dBm; management TX enabled\n",
          settings.channel,power);
 return 0;
}
void xrs_mgd_prepare(struct ieee80211_hw *dev,struct ieee80211_vif *vif,
                      struct ieee80211_prep_tx_info *info)
{
 struct xradio_common *hw=dev->priv;struct xrs_runtime *s=hw->station;int ret;bool setup_started;
 if(!s || READ_ONCE(hw->recovery_state)!=XRS_RUNNING)return;
 mutex_lock(&s->control_lock);ret=station_join(hw,vif,&setup_started);
 if(ret && setup_started) {
  WRITE_ONCE(s->poisoned,true);WRITE_ONCE(hw->bh_error,1);
  dev_err(hw->pdev,"XR829 JOIN/setup failed%d\n",ret);
 } else if(ret) {
  /* mgd_prepare_tx has no error return. TX remains gated by !joined and
   * mac80211 completes its normal authentication timeout/retry lifecycle. */
  dev_dbg(hw->pdev,"XR829 BSS preparation rejected%d\n",ret);
 }
 mutex_unlock(&s->control_lock);
}
void xrs_bss_changed(struct ieee80211_hw *dev,struct ieee80211_vif *vif,
                     struct ieee80211_bss_conf *info,u64 changed)
{
 struct xradio_common *hw=dev->priv;struct xrs_runtime *s=hw->station;
 struct xrs_station_plan plan;struct wsm_set_bss_params params={};int ret=0;
 struct xrs_ht_link negotiated={0};struct ieee80211_sta *sta;
 struct ieee80211_supported_band *band=dev->wiphy->bands[NL80211_BAND_2GHZ];
 unsigned long irqflags;
 if(s && (changed&BSS_CHANGED_TXPOWER)) {
  mutex_lock(&s->control_lock);s->bss_power=info->txpower;
  /* link txpower already combines AP, channel and user limits. conf may
   * still contain the previous AP limit until the next hw_config call. */
  if(s->joined)ret=station_power_update(s,info->txpower==INT_MIN?
                                      dev->conf.power_level:info->txpower);
  mutex_unlock(&s->control_lock);
  if(ret) {ieee80211_connection_loss(vif);return;}
 }
 if(!s || !(changed&(BSS_CHANGED_ASSOC|BSS_CHANGED_HT|BSS_CHANGED_ERP_PREAMBLE|BSS_CHANGED_ERP_SLOT)))return;
 if(!vif->cfg.assoc) {if((changed&BSS_CHANGED_ASSOC) && s->joined)xrs_runtime_stop(hw);return;}
 mutex_lock(&s->control_lock);
 if(!s->joined || s->poisoned) {ret=-EIO;goto out;}
 if(dev->conf.chandef.width!=NL80211_CHAN_WIDTH_20 &&
    dev->conf.chandef.width!=NL80211_CHAN_WIDTH_20_NOHT) {ret=-EOPNOTSUPP;goto out;}
 rcu_read_lock();sta=ieee80211_find_sta(vif,s->settings.bssid);
 if(sta && band->ht_cap.ht_supported && sta->deflink.ht_cap.ht_supported &&
    dev->conf.chandef.width==NL80211_CHAN_WIDTH_20) {
  negotiated.enabled=1;
  /* This driver advertises one TX stream explicitly; peer RX constrains TX. */
  negotiated.tx_mcs=0xff&sta->deflink.ht_cap.mcs.rx_mask[0];
  negotiated.rx_mcs=band->ht_cap.mcs.rx_mask[0];
  negotiated.rx_short_gi=!!(band->ht_cap.cap&IEEE80211_HT_CAP_SGI_20);
  negotiated.short_gi=!!(band->ht_cap.cap&sta->deflink.ht_cap.cap&IEEE80211_HT_CAP_SGI_20);
  negotiated.spacing=sta->deflink.ht_cap.ampdu_density;
 }
 rcu_read_unlock();
 s->settings.aid=vif->cfg.aid;
 ret=xrs_station_prepare(&plan,&s->settings);if(ret)goto out;
 ret=xrs_ht_association(plan.association,sizeof(plan.association),&negotiated,
                         s->settings.basic_rates,info->use_short_preamble);
 if(ret<0)goto out;
 ret=wsm_write_mib(hw,0x1010,plan.association,sizeof(plan.association),0);if(ret)goto out;
 ret=station_mib32(hw,0x1021,xrs_ht_protection(info->ht_operation_mode));if(ret)goto out;
 params.aid=vif->cfg.aid;params.beaconLostCount=20;
 params.operationalRateSet=s->settings.operational_rates;
 ret=wsm_set_bss_params(hw,&params,0);if(ret)goto out;
 /* Match the current BSS slot timing; HT protection was set above. */
 ret=station_mib32(hw,0x0003,info->use_short_slot?9:20);if(ret)goto out;
	ret = wsm_set_block_ack_policy(hw, negotiated.enabled ? XRS_BA_TIDS : 0,
				       negotiated.enabled ? XRS_BA_TIDS : 0, 0);
	if (ret)
		goto out;
	spin_lock_irqsave(&s->lock, irqflags);
	s->ht = negotiated;
	s->ba.enabled = negotiated.enabled;
	if (!negotiated.enabled)
		s->ba.rx = 0;
 smp_store_release(&s->associated,true);spin_unlock_irqrestore(&s->lock,irqflags);
out:
 if(ret) {WRITE_ONCE(s->poisoned,true);WRITE_ONCE(hw->bh_error,1);dev_err(hw->pdev,"XR829 association setup failed%d\n",ret);}
 mutex_unlock(&s->control_lock);
 if(ret)ieee80211_connection_loss(vif);
}

/* Firmware owns TX sessions; RX callbacks only install/remove host reorder
 * state. SSN and window are consumed by mac80211, not a fictitious WSM command.
 */
int xrs_ampdu_action(struct ieee80211_hw *dev, struct ieee80211_vif *vif,
		     struct ieee80211_ampdu_params *params)
{
	struct xradio_common *hw = dev->priv;
	struct xrs_runtime *s = hw->station;
	unsigned long flags;
	int ret = -EOPNOTSUPP;

	if (!s || !params->sta || vif->type != NL80211_IFTYPE_STATION)
		return -EINVAL;
	mutex_lock(&s->control_lock);
	spin_lock_irqsave(&s->lock, flags);
	if (params->action == IEEE80211_AMPDU_RX_STOP) {
		/* A late stop for an old AP must not clear the replacement BSS. */
		ret = 0;
		if (!s->associated ||
		    !memcmp(params->sta->addr, s->settings.bssid, ETH_ALEN))
			ret = xrs_ba_stop(&s->ba, params->tid);
	} else if (params->action == IEEE80211_AMPDU_RX_START &&
		   s->joined && s->associated && !s->poisoned && !s->closing &&
		   READ_ONCE(hw->recovery_state) == XRS_RUNNING &&
		   !READ_ONCE(hw->bh_error) &&
		   !memcmp(params->sta->addr, s->settings.bssid, ETH_ALEN)) {
		ret = xrs_ba_start(&s->ba, params->tid, params->ssn,
				   params->buf_size);
	}
	spin_unlock_irqrestore(&s->lock, flags);
	mutex_unlock(&s->control_lock);
	return ret;
}

/* Caller holds control_lock. Stop new frames; let BH finish prior frames.
 * WSM commands use the independently owned command buffer, not data slots.
 */
static int station_key_drain(struct xrs_runtime *s)
{
 unsigned long flags,deadline=jiffies+5*HZ;
 unsigned int i;bool pending;
 spin_lock_irqsave(&s->lock,flags);s->keychanging=true;
 spin_unlock_irqrestore(&s->lock,flags);
 ieee80211_stop_queues(s->hw->hw);
 for(;;) {
  pending=false;
  spin_lock_irqsave(&s->lock,flags);
  for(i=0;i<XRS_SLOTS;i++)if(s->slots[i].phase!=XRS_FREE)pending=true;
  spin_unlock_irqrestore(&s->lock,flags);
  if(READ_ONCE(s->poisoned) || READ_ONCE(s->hw->bh_error))return -EIO;
  if(!pending)return 0;
  /* control_lock is held: service pending uploads here instead of waiting
   * for the worker that requires the same mutex. BH remains independent. */
  if(station_policy_prepare(s)<0)return -EIO;
  if(time_after(jiffies,deadline))return -ETIMEDOUT;
  msleep(1);
 }
}
static int station_key_send(void *context,u16 command,const u8 *payload,size_t size)
{
 return wsm_station_key_command(context,command,payload,size);
}
/* CCMP only; never request software fallback for an unverified raw path. */
int xrs_set_key(struct ieee80211_hw *dev,enum set_key_cmd command,
                struct ieee80211_vif *vif,struct ieee80211_sta *sta,
                struct ieee80211_key_conf *key)
{
 struct xradio_common *hw=dev->priv;struct xrs_runtime *s=hw->station;
 struct ieee80211_key_seq seq;u8 wire_sequence[8]={0};
 bool pairwise=key->flags&IEEE80211_KEY_FLAG_PAIRWISE;
 unsigned long flags;unsigned int i;int ret=-EINVAL,slot=-1;
 unsigned long long owner;
 if(!s || key->cipher!=WLAN_CIPHER_SUITE_CCMP || key->keylen!=16 ||
    key->keyidx>3 || (pairwise && (!sta || key->keyidx)))return -EOPNOTSUPP;
 if(command!=SET_KEY && command!=DISABLE_KEY)return -EINVAL;
 mutex_lock(&s->control_lock);
 for(i=0;i<11;i++)if(s->keyconf[i]==key) {slot=i;break;}
 if(command==DISABLE_KEY && slot<0) {ret=0;goto out;}
 /* During teardown mac80211 may already clear bssid; recorded key identity
  * remains sufficient for removal. New installation still needs live BSS. */
 if(s->poisoned || (command==SET_KEY &&
    (!s->joined || !vif->bss_conf.bssid ||
     memcmp(vif->bss_conf.bssid,s->settings.bssid,6) ||
     (pairwise && memcmp(sta->addr,s->settings.bssid,6))))) {
  ret=-ENOTCONN;
  if(command==DISABLE_KEY)goto failed;
  goto out;
 }
 if(command==SET_KEY && slot>=0) {
  /* Same mac80211 object: preserve PN and never reissue install. */
  ret=xrs_key_usable(&s->keys,slot,s->keys.slots[slot].owner)?0:-EIO;goto out;
 }
 /* One firmware identity per peer/key ID. Replacement must remove the
  * previous mac80211 key first; overlapping identities are unqualified.
  * This runtime has exactly one BSSID, checked above.
  */
 if(command==SET_KEY)for(i=0;i<11;i++)if(s->keyconf[i] &&
    s->keypairwise[i]==pairwise && s->keyid[i]==key->keyidx) {ret=-EBUSY;goto out;}
 ret=station_key_drain(s);if(ret)goto failed;
 if(command==SET_KEY) {
  if(s->next_key_owner==~0ULL) {ret=-EOVERFLOW;goto failed;}
  owner=++s->next_key_owner;
  if(!pairwise) {
   ieee80211_get_key_rx_seq(key,0,&seq);
   for(i=0;i<6;i++)wire_sequence[i]=seq.ccmp.pn[5-i];
  }
  ret=xrs_key_install(&s->keys,owner,pairwise?sta->addr:NULL,key->keyidx,
                      key->key,key->keylen,wire_sequence,station_key_send,hw);
  if(ret<0)goto failed;
  slot=ret;s->keyconf[slot]=key;s->keypairwise[slot]=pairwise;
  s->keyid[slot]=key->keyidx;key->hw_key_idx=slot;
  key->flags|=IEEE80211_KEY_FLAG_GENERATE_IV;
  ret=0;
 } else {
  ret=xrs_key_remove(&s->keys,slot,s->keys.slots[slot].owner,station_key_send,hw);
  if(ret)goto failed;
  s->keyconf[slot]=NULL;
 }
 spin_lock_irqsave(&s->lock,flags);s->keychanging=false;
 spin_unlock_irqrestore(&s->lock,flags);
 ieee80211_wake_queues(dev);
 goto out;
failed:
 spin_lock_irqsave(&s->lock,flags);
 s->poisoned=true;WRITE_ONCE(hw->bh_error,1);
 /* mac80211 can free the key as soon as DISABLE_KEY returns, even when
  * firmware removal failed. Quarantine hardware, always detach the cookie.
  */
 if(command==DISABLE_KEY && slot>=0) {s->keyconf[slot]=NULL;ret=0;}
 spin_unlock_irqrestore(&s->lock,flags);
 ieee80211_stop_queues(dev);
 /* Keep queues stopped; uncertain firmware ownership requires fresh boot. */
out:
 memzero_explicit(wire_sequence,sizeof(wire_sequence));
 mutex_unlock(&s->control_lock);return ret;
}

int xrs_conf_tx(struct ieee80211_hw *dev,struct ieee80211_vif *vif,
                unsigned int link_id,u16 ac,
                const struct ieee80211_tx_queue_params *params)
{
 struct xradio_common *hw=dev->priv;struct xrs_runtime *s=hw->station;
 struct wsm_edca_params next;int ret=0;
 if(!s || vif->type!=NL80211_IFTYPE_STATION)return -EOPNOTSUPP;
 ret=xrs_edca_validate(link_id,ac,params->aifs,params->cw_min,
                       params->cw_max,params->txop,params->uapsd);
 if(ret)return ret;
 mutex_lock(&s->control_lock);
 if(s->poisoned || READ_ONCE(hw->bh_error)) {ret=-EIO;goto out;}
 next=s->edca;
 WSM_EDCA_SET(&next,ac,params->aifs,params->cw_min,params->cw_max,
              params->txop,0xc8,false);
 /* Cache pre-JOIN settings; after JOIN firmware must confirm before commit. */
 if(s->joined)ret=wsm_set_edca_params(hw,&next,0);
 if(!ret)s->edca=next;
 else {
  WRITE_ONCE(s->poisoned,true);WRITE_ONCE(hw->bh_error,1);
  ieee80211_stop_queues(dev);
 }
out:
 mutex_unlock(&s->control_lock);
 return ret;
}

/* Called from external recovery work under the wiphy mutex, never from BH or
 * the ordered runtime queue. Leave the command/BH path available for drain. */
void xrs_runtime_quiesce(struct xradio_common *hw)
{
 struct xrs_runtime *s=hw->station;unsigned long flags;
 if(!s)return;
 spin_lock_irqsave(&s->lock,flags);s->closing=true;
 spin_unlock_irqrestore(&s->lock,flags);
 ieee80211_stop_queues(hw->hw);
 synchronize_net();
 cancel_delayed_work_sync(&s->timeout);
 cancel_work_sync(&s->policy_work);
}
/* No firmware commands: IRQ/BH and runtime workers are already joined. */
int xrs_runtime_detached(struct xradio_common *hw)
{
 struct xrs_runtime *s=hw->station;unsigned int i;int ret=0;unsigned long flags;
 if(!s)return 0;
 mutex_lock(&s->control_lock);
 for(i=0;i<XRS_SLOTS;i++) {
  spin_lock_irqsave(&s->lock,flags);
  xrs_tx_abort(&s->slots[i],1);
  spin_unlock_irqrestore(&s->lock,flags);
  report_slot(s,i,false);
 }
 s->sending=-1;
 memset(s->credit,0,sizeof(s->credit));
 /* A stale mac80211 key may never be replayed into a fresh epoch. */
 for(i=0;i<11;i++)if(s->keyconf[i])ret=-EBUSY;
 mutex_unlock(&s->control_lock);
 return ret;
}
void xrs_runtime_new_epoch(struct xradio_common *hw)
{
 struct xrs_runtime *s=hw->station;unsigned long flags;
 if(!s)return;
 mutex_lock(&s->control_lock);
 spin_lock_irqsave(&s->lock,flags);
 /* Explicit mutable protocol state only: keep locks, work and debugfs users. */
 memset(s->slots,0,sizeof(s->slots));
 memset(s->feedback_valid,0,sizeof(s->feedback_valid));
 memset(s->policy_held,0,sizeof(s->policy_held));
 memset(s->policy_ready,0,sizeof(s->policy_ready));
 memset(&s->keys,0,sizeof(s->keys));
 memset(&s->settings,0,sizeof(s->settings));
	memset(&s->ht, 0, sizeof(s->ht));
	memset(&s->ba, 0, sizeof(s->ba));
 memset(&s->address,0,sizeof(s->address));
 xpc_init(&s->policies);
 s->enqueue_order=0;s->next_slot=0;s->sending=-1;
 s->joined=false;s->associated=false;s->poisoned=false;
 s->keychanging=false;s->powerchanging=false;s->closing=false;
 s->power_min=0;s->power_max=0;s->power_confirmed=0;s->bss_power=0;
 spin_unlock_irqrestore(&s->lock,flags);
 mutex_unlock(&s->control_lock);
}
