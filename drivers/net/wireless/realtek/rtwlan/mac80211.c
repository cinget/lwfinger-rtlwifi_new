// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2018  Realtek Corporation.
 */

#include "main.h"
#include "sec.h"
#include "tx.h"
#include "fw.h"
#include "mac.h"
#include "ps.h"
#include "reg.h"
#include "debug.h"

static void rtw_ops_tx(struct ieee80211_hw *hw,
		       struct ieee80211_tx_control *control,
		       struct sk_buff *skb)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_tx_pkt_info pkt_info;

	if (!rtw_flag_check(rtwdev, RTW_FLAG_RUNNING))
		goto out;

	memset(&pkt_info, 0, sizeof(pkt_info));
	rtw_tx_pkt_info_update(rtwdev, &pkt_info, control, skb);
	if (rtw_hci_tx(rtwdev, &pkt_info, skb))
		goto out;

	return;

out:
	dev_kfree_skb_any(skb);
}

static int rtw_ops_start(struct ieee80211_hw *hw)
{
	struct rtw_dev *rtwdev = hw->priv;

	return rtw_core_start(rtwdev);
}

static void rtw_ops_stop(struct ieee80211_hw *hw)
{
	struct rtw_dev *rtwdev = hw->priv;

	rtw_core_stop(rtwdev);
}

static int rtw_ops_config(struct ieee80211_hw *hw, u32 changed)
{
	struct rtw_dev *rtwdev = hw->priv;
	int ret;

	if (changed & IEEE80211_CONF_CHANGE_IDLE) {
		if (hw->conf.flags & IEEE80211_CONF_IDLE) {
			rtw_enter_ips(rtwdev);
		} else {
			ret = rtw_leave_ips(rtwdev);
			if (ret) {
				rtw_err(rtwdev, "failed to leave idle state\n");
				return ret;
			}
		}
	}

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL)
		rtw_set_channel(rtwdev);

	return 0;
}

static struct rtw_vif_port rtw_vif_port[] = {
	[0] = {
		.mac_addr	= {.addr = 0x0610},
		.bssid		= {.addr = 0x0618},
		.net_type	= {.addr = 0x0100, .mask = 0x30000},
		.aid		= {.addr = 0x06a8, .mask = 0x7ff},
	},
	[1] = {
		.mac_addr	= {.addr = 0x0700},
		.bssid		= {.addr = 0x0708},
		.net_type	= {.addr = 0x0100, .mask = 0xc0000},
		.aid		= {.addr = 0x0710, .mask = 0x7ff},
	},
	[2] = {
		.mac_addr	= {.addr = 0x1620},
		.bssid		= {.addr = 0x1628},
		.net_type	= {.addr = 0x1100, .mask = 0x3},
		.aid		= {.addr = 0x1600, .mask = 0x7ff},
	},
	[3] = {
		.mac_addr	= {.addr = 0x1630},
		.bssid		= {.addr = 0x1638},
		.net_type	= {.addr = 0x1100, .mask = 0xc},
		.aid		= {.addr = 0x1604, .mask = 0x7ff},
	},
	[4] = {
		.mac_addr	= {.addr = 0x1640},
		.bssid		= {.addr = 0x1648},
		.net_type	= {.addr = 0x1100, .mask = 0x30},
		.aid		= {.addr = 0x1608, .mask = 0x7ff},
	},
};

static int rtw_ops_add_interface(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
	enum rtw_net_type net_type;
	u32 config = 0;
	u8 port = 0;

	vif->driver_flags |= IEEE80211_VIF_BEACON_FILTER;
	rtwvif->port = port;
	rtwvif->vif = vif;
	rtwvif->stats.tx_unicast = 0;
	rtwvif->stats.rx_unicast = 0;
	rtwvif->stats.tx_cnt = 0;
	rtwvif->stats.rx_cnt = 0;
	rtwvif->in_lps = false;
	list_add(&rtwvif->list, &rtwdev->vif_list);
	INIT_LIST_HEAD(&rtwvif->sta_list);

	rtwvif->conf = &rtw_vif_port[port];

	switch (vif->type) {
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_MESH_POINT:
		net_type = RTW_NET_AP_MODE;
		break;
	case NL80211_IFTYPE_ADHOC:
		net_type = RTW_NET_AD_HOC;
		break;
	default:
		net_type = RTW_NET_NO_LINK;
		break;
	}

	ether_addr_copy(rtwvif->mac_addr, vif->addr);
	config |= PORT_SET_MAC_ADDR;
	rtwvif->net_type = net_type;
	config |= PORT_SET_NET_TYPE;
	rtw_vif_port_config(rtwdev, rtwvif, config);

	rtw_info(rtwdev, "start vif %pM on port %d", vif->addr, rtwvif->port);

	return 0;
}

static void rtw_ops_remove_interface(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
	u32 config = 0;

	rtw_info(rtwdev, "stop vif %pM on port %d", vif->addr, rtwvif->port);

	eth_zero_addr(rtwvif->mac_addr);
	config |= PORT_SET_MAC_ADDR;
	rtwvif->net_type = RTW_NET_NO_LINK;
	config |= PORT_SET_NET_TYPE;
	rtw_vif_port_config(rtwdev, rtwvif, config);

	list_del(&rtwvif->list);
}

static void rtw_ops_configure_filter(struct ieee80211_hw *hw,
				     unsigned int changed_flags,
				     unsigned int *new_flags,
				     u64 multicast)
{
	struct rtw_dev *rtwdev = hw->priv;

	*new_flags &= (FIF_ALLMULTI | FIF_OTHER_BSS | FIF_FCSFAIL |
		       FIF_BCN_PRBRESP_PROMISC);

	if (changed_flags & FIF_ALLMULTI) {
		if (*new_flags & FIF_ALLMULTI)
			rtwdev->hal.rcr |= BIT_AM | BIT_AB;
		else
			rtwdev->hal.rcr &= ~(BIT_AM | BIT_AB);
	}
	if (changed_flags & FIF_FCSFAIL) {
		if (*new_flags & FIF_FCSFAIL)
			rtwdev->hal.rcr |= BIT_ACRC32;
		else
			rtwdev->hal.rcr &= ~(BIT_ACRC32);
	}
	if (changed_flags & FIF_OTHER_BSS) {
		if (*new_flags & FIF_OTHER_BSS)
			rtwdev->hal.rcr |= BIT_AAP;
		else
			rtwdev->hal.rcr &= ~(BIT_AAP);
	}
	if (changed_flags & FIF_BCN_PRBRESP_PROMISC) {
		if (*new_flags & FIF_BCN_PRBRESP_PROMISC)
			rtwdev->hal.rcr &= ~(BIT_CBSSID_BCN | BIT_CBSSID_DATA);
		else
			rtwdev->hal.rcr |= BIT_CBSSID_BCN;
	}

	rtw_dbg(rtwdev, "config rx filter, changed=0x%08x, new=0x%08x, rcr=0x%08x",
		changed_flags, *new_flags, rtwdev->hal.rcr);

	rtw_write32(rtwdev, REG_RCR, rtwdev->hal.rcr);
}

static void rtw_ops_bss_info_changed(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *conf,
				     u32 changed)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
	u32 config = 0;

	if (changed & BSS_CHANGED_ASSOC) {
		struct rtw_sta_info *ap;
		struct rtw_chip_info *chip = rtwdev->chip;
		enum rtw_net_type net_type;

		ap = list_first_entry(&rtwvif->sta_list,
				      struct rtw_sta_info, list);
		if (conf->assoc) {
			net_type = RTW_NET_MGD_LINKED;
			chip->ops->do_iqk(rtwdev);

			rtwvif->aid = conf->aid;
			rtw_add_rsvd_page(rtwdev, RSVD_PS_POLL, true);
			rtw_add_rsvd_page(rtwdev, RSVD_QOS_NULL, true);
			rtw_add_rsvd_page(rtwdev, RSVD_NULL, true);
			rtw_fw_download_rsvd_page(rtwdev, vif);
		} else {
			net_type = RTW_NET_NO_LINK;
			rtwvif->aid = 0;
			rtw_reset_rsvd_page(rtwdev);
		}

		rtwvif->net_type = net_type;
		config |= PORT_SET_NET_TYPE;
		config |= PORT_SET_AID;
		rtw_fw_media_status_report(rtwdev, ap->mac_id, conf->assoc);
	}

	if (changed & BSS_CHANGED_BSSID) {
		ether_addr_copy(rtwvif->bssid, conf->bssid);
		config |= PORT_SET_BSSID;
	}

	if (changed & BSS_CHANGED_BEACON)
		rtw_fw_download_rsvd_page(rtwdev, vif);

	rtw_vif_port_config(rtwdev, rtwvif, config);
}

static u8 rtw_acquire_macid(struct rtw_dev *rtwdev)
{
	u8 i;

	for (i = 0; i < RTW_MAX_MAC_ID_NUM; i++) {
		if (!rtwdev->macid_used[i]) {
			rtwdev->macid_used[i] = true;
			return i;
		}
	}

	return i;
}

static void rtw_release_macid(struct rtw_dev *rtwdev, u8 mac_id)
{
	rtwdev->macid_used[mac_id] = false;
}

static int rtw_ops_sta_add(struct ieee80211_hw *hw,
			   struct ieee80211_vif *vif,
			   struct ieee80211_sta *sta)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_sta_info *si = (struct rtw_sta_info *)sta->drv_priv;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;

	si->mac_id = rtw_acquire_macid(rtwdev);
	if (si->mac_id >= RTW_MAX_MAC_ID_NUM)
		return -ENOSPC;

	si->sta = sta;
	si->vif = vif;
	si->init_ra_lv = 1;
	ewma_rssi_init(&si->avg_rssi);

	rtw_update_sta_info(rtwdev, si);

	list_add_tail(&si->list, &rtwvif->sta_list);

	rtw_info(rtwdev, "sta %pM joined with macid %d", sta->addr, si->mac_id);

	return 0;
}

static int rtw_ops_sta_remove(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      struct ieee80211_sta *sta)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_sta_info *si = (struct rtw_sta_info *)sta->drv_priv;

	rtw_release_macid(rtwdev, si->mac_id);

	list_del(&si->list);

	rtw_info(rtwdev, "sta %pM with macid %d left", sta->addr, si->mac_id);

	return 0;
}

static int rtw_ops_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			   struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			   struct ieee80211_key_conf *key)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_sec_desc *sec = &rtwdev->sec;
	u8 hw_key_type;
	u8 hw_key_idx;
	int ret = 0;

	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		hw_key_type = RTW_CAM_WEP40;
		break;
	case WLAN_CIPHER_SUITE_WEP104:
		hw_key_type = RTW_CAM_WEP104;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		hw_key_type = RTW_CAM_TKIP;
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_MMIC;
		break;
	case WLAN_CIPHER_SUITE_CCMP:
		hw_key_type = RTW_CAM_AES;
		key->flags |= IEEE80211_KEY_FLAG_SW_MGMT_TX;
		break;
	default:
		return -ENOTSUPP;
	}

	mutex_lock(&sec->mutex);

	if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE) {
		hw_key_idx = rtw_sec_get_free_cam(sec);
	} else {
		/* multiple interfaces? */
		hw_key_idx = key->keyidx;
	}

	if (hw_key_idx > sec->total_cam_num) {
		ret = -ENOSPC;
		goto out;
	}

	switch (cmd) {
	case SET_KEY:
		/* need sw generated IV */
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_IV;
		key->hw_key_idx = hw_key_idx;
		rtw_sec_write_cam(rtwdev, sec, sta, key,
				  hw_key_type, hw_key_idx);
		break;
	case DISABLE_KEY:
		rtw_sec_clear_cam(rtwdev, sec, key->hw_key_idx);
		break;
	}

out:
	mutex_unlock(&sec->mutex);

	return ret;
}

static int rtw_ops_ampdu_action(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_ampdu_params *params)
{
	struct ieee80211_sta *sta = params->sta;
	u16 tid = params->tid;

	switch (params->action) {
	case IEEE80211_AMPDU_TX_START:
		ieee80211_start_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	case IEEE80211_AMPDU_TX_STOP_CONT:
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
	case IEEE80211_AMPDU_RX_START:
	case IEEE80211_AMPDU_RX_STOP:
		break;
	default:
		WARN_ON(1);
		return -ENOTSUPP;
	}

	return 0;
}

static void rtw_ops_sw_scan_start(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  const u8 *mac_addr)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;

	rtw_leave_lps(rtwdev, rtwvif);

	rtw_flag_set(rtwdev, RTW_FLAG_DIG_DISABLE);
	rtw_flag_set(rtwdev, RTW_FLAG_SCANNING);
}

static void rtw_ops_sw_scan_complete(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	struct rtw_dev *rtwdev = hw->priv;

	rtw_flag_clear(rtwdev, RTW_FLAG_SCANNING);
	rtw_flag_clear(rtwdev, RTW_FLAG_DIG_DISABLE);
}

const struct ieee80211_ops rtw_ops = {
	.tx			= rtw_ops_tx,
	.start			= rtw_ops_start,
	.stop			= rtw_ops_stop,
	.config			= rtw_ops_config,
	.add_interface		= rtw_ops_add_interface,
	.remove_interface	= rtw_ops_remove_interface,
	.configure_filter	= rtw_ops_configure_filter,
	.bss_info_changed	= rtw_ops_bss_info_changed,
	.sta_add		= rtw_ops_sta_add,
	.sta_remove		= rtw_ops_sta_remove,
	.set_key		= rtw_ops_set_key,
	.ampdu_action		= rtw_ops_ampdu_action,
	.sw_scan_start		= rtw_ops_sw_scan_start,
	.sw_scan_complete	= rtw_ops_sw_scan_complete,
};
EXPORT_SYMBOL(rtw_ops);