/*
** Id: @(#) gl_cfg80211.c@@
*/

/*! \file   gl_cfg80211.c
    \brief  Main routines for supporintg MT6620 cfg80211 control interface

    This file contains the support routines of Linux driver for MediaTek Inc. 802.11
    Wireless LAN Adapters.
*/

/*
** Log: gl_cfg80211.c
**
** 09 05 2013 cp.wu
** correct length to pass to wlanoidSetBssid()
**
** 09 04 2013 cp.wu
** fix typo
**
** 09 03 2013 cp.wu
** add path for reassociation
**
** 11 23 2012 yuche.tsai
** [ALPS00398671] [Tablet] Remove Wi-Fi Direct completely
** Fix bug of WiFi may reboot under user load, when WiFi Direct is removed..
**
** 09 12 2012 wcpadmin
** [ALPS00276400] Remove MTK copyright and legal header on GPL/LGPL related packages
** .
**
** 08 30 2012 chinglan.wang
** [ALPS00349664] [6577JB][WIFI] Phone can not connect to AP secured with AES via WPS in 802.11n Only
** .
 *
**
*/

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/
#include "gl_os.h"
#include "debug.h"
#include "wlan_lib.h"
#include "gl_wext.h"
#include "precomp.h"
#include <linux/can/netlink.h>
#include <net/netlink.h>
#include <net/cfg80211.h>
#include "gl_cfg80211.h"
#include "gl_vendor.h"

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/

#if CFG_SUPPORT_WAPI
extern UINT_8 keyStructBuf[1024];	/* add/remove key shared buffer */
#else
extern UINT_8 keyStructBuf[100];	/* add/remove key shared buffer */
#endif
/* workaround for some ANR CRs. if suppliant is blocked longer than 10s, wifi hal will tell wifiMonitor
to teminate. for the case which can block supplicant 10s is to del key more than 5 times. the root cause
is that there is no resource in TC4, so del key command was not able to set, and then oid
timeout was happed. if we found the root cause why fw couldn't release TC resouce, we will remove this
workaround */
static UINT_8 gucKeyIndex = 255;

P_SW_RFB_T g_arGscnResultsTempBuffer[MAX_BUFFERED_GSCN_RESULTS];
UINT_8 g_GscanResultsTempBufferIndex = 0;
UINT_8 g_arGscanResultsIndicateNumber[MAX_BUFFERED_GSCN_RESULTS] = { 0, 0, 0, 0, 0 };

UINT_8 g_GetResultsBufferedCnt = 0;
UINT_8 g_GetResultsCmdCnt = 0;

/*******************************************************************************
*                           P R I V A T E   D A T A
********************************************************************************
*/

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/

/*******************************************************************************
*                   F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/
extern void wlanHandleSystemResume(void);
extern void wlanHandleSystemSuspend(void);
extern void p2pHandleSystemResume(void);
extern void p2pHandleSystemSuspend(void);

/*******************************************************************************
*                              F U N C T I O N S
********************************************************************************
*/

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for change STA type between
 *        1. Infrastructure Client (Non-AP STA)
 *        2. Ad-Hoc IBSS
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_change_iface(struct wiphy *wiphy,
			  struct net_device *ndev, enum nl80211_iftype type, u32 *flags, struct vif_params *params)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;
	ENUM_PARAM_OP_MODE_T eOpMode;
	UINT_32 u4BufLen;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	if (type == NL80211_IFTYPE_STATION)
		eOpMode = NET_TYPE_INFRA;
	else if (type == NL80211_IFTYPE_ADHOC)
		eOpMode = NET_TYPE_IBSS;
	else
		return -EINVAL;

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetInfrastructureMode,
			   &eOpMode, sizeof(eOpMode), FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS)
		DBGLOG(REQ, WARN, ("set infrastructure mode error:%x\n", rStatus));

	/* reset wpa info */
	prGlueInfo->rWpaInfo.u4WpaVersion = IW_AUTH_WPA_VERSION_DISABLED;
	prGlueInfo->rWpaInfo.u4KeyMgmt = 0;
	prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_NONE;
	prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_NONE;
	prGlueInfo->rWpaInfo.u4AuthAlg = IW_AUTH_ALG_OPEN_SYSTEM;
#if CFG_SUPPORT_802_11W
	prGlueInfo->rWpaInfo.u4Mfp = IW_AUTH_MFP_DISABLED;
#endif

	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for adding key
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_add_key(struct wiphy *wiphy,
		     struct net_device *ndev,
		     u8 key_index, bool pairwise, const u8 *mac_addr, struct key_params *params)
{
	PARAM_KEY_T rKey;
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;
	INT_32 i4Rslt = -EINVAL;
	UINT_32 u4BufLen = 0;
	UINT_8 tmp1[8];
	UINT_8 tmp2[8];

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	kalMemZero(&rKey, sizeof(PARAM_KEY_T));

	rKey.u4KeyIndex = key_index;

	if (mac_addr) {
		COPY_MAC_ADDR(rKey.arBSSID, mac_addr);
		if ((rKey.arBSSID[0] == 0x00) && (rKey.arBSSID[1] == 0x00) && (rKey.arBSSID[2] == 0x00) &&
		    (rKey.arBSSID[3] == 0x00) && (rKey.arBSSID[4] == 0x00) && (rKey.arBSSID[5] == 0x00)) {
			rKey.arBSSID[0] = 0xff;
			rKey.arBSSID[1] = 0xff;
			rKey.arBSSID[2] = 0xff;
			rKey.arBSSID[3] = 0xff;
			rKey.arBSSID[4] = 0xff;
			rKey.arBSSID[5] = 0xff;
		}
		if (rKey.arBSSID[0] != 0xFF) {
			rKey.u4KeyIndex |= BIT(31);
			if ((rKey.arBSSID[0] != 0x00) || (rKey.arBSSID[1] != 0x00) || (rKey.arBSSID[2] != 0x00) ||
			    (rKey.arBSSID[3] != 0x00) || (rKey.arBSSID[4] != 0x00) || (rKey.arBSSID[5] != 0x00))
				rKey.u4KeyIndex |= BIT(30);
		}
	} else {
		rKey.arBSSID[0] = 0xff;
		rKey.arBSSID[1] = 0xff;
		rKey.arBSSID[2] = 0xff;
		rKey.arBSSID[3] = 0xff;
		rKey.arBSSID[4] = 0xff;
		rKey.arBSSID[5] = 0xff;
		/* rKey.u4KeyIndex |= BIT(31);//Enable BIT 31 will make tx use bc key id,should use pairwise key id 0 */
	}

	if (params->key) {
		/* rKey.aucKeyMaterial[0] = kalMemAlloc(params->key_len, VIR_MEM_TYPE); */
		kalMemCopy(rKey.aucKeyMaterial, params->key, params->key_len);
		if (params->key_len == 32) {
			kalMemCopy(tmp1, &params->key[16], 8);
			kalMemCopy(tmp2, &params->key[24], 8);
			kalMemCopy(&rKey.aucKeyMaterial[16], tmp2, 8);
			kalMemCopy(&rKey.aucKeyMaterial[24], tmp1, 8);
		}
	}

	rKey.u4KeyLength = params->key_len;
	rKey.u4Length = ((ULONG) & (((P_P2P_PARAM_KEY_T) 0)->aucKeyMaterial)) + rKey.u4KeyLength;

	rStatus = kalIoctl(prGlueInfo, wlanoidSetAddKey, &rKey, rKey.u4Length, FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus == WLAN_STATUS_SUCCESS)
		i4Rslt = 0;

	return i4Rslt;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for getting key for specified STA
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_get_key(struct wiphy *wiphy,
		     struct net_device *ndev,
		     u8 key_index,
		     bool pairwise,
		     const u8 *mac_addr, void *cookie, void (*callback) (void *cookie, struct key_params *))
{
	P_GLUE_INFO_T prGlueInfo = NULL;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

#if 1
	printk("--> %s()\n", __func__);
#endif

	/* not implemented */

	return -EINVAL;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for removing key for specified STA
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_del_key(struct wiphy *wiphy, struct net_device *ndev, u8 key_index, bool pairwise, const u8 *mac_addr)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;
	PARAM_REMOVE_KEY_T rRemoveKey;
	UINT_32 u4BufLen = 0;
	INT_32 i4Rslt = -EINVAL;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	kalMemZero(&rRemoveKey, sizeof(PARAM_REMOVE_KEY_T));
	if (mac_addr)
		COPY_MAC_ADDR(rRemoveKey.arBSSID, mac_addr);
	else if (key_index > gucKeyIndex) {	/* bypass the next remove key operation */
		gucKeyIndex = key_index;
		return -EBUSY;
	} else			/* new operation, reset gucKeyIndex */
		gucKeyIndex = 255;
	rRemoveKey.u4KeyIndex = key_index;
	rRemoveKey.u4Length = sizeof(PARAM_REMOVE_KEY_T);

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetRemoveKey, &rRemoveKey, rRemoveKey.u4Length, FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, WARN, ("remove key error:%x\n", rStatus));
		if (WLAN_STATUS_FAILURE == rStatus && mac_addr) {
			i4Rslt = -EBUSY;
			gucKeyIndex = key_index;
		}
	} else {
		gucKeyIndex = 255;
		i4Rslt = 0;
	}

	return i4Rslt;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for setting default key on an interface
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_set_default_key(struct wiphy *wiphy, struct net_device *ndev, u8 key_index, bool unicast, bool multicast)
{
	P_GLUE_INFO_T prGlueInfo = NULL;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

#if 1
	printk("--> %s()\n", __func__);
#endif

	/* not implemented */

	return WLAN_STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for setting set_default_mgmt_ke on an interface
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_set_default_mgmt_key(struct wiphy *wiphy, struct net_device *netdev, u8 key_index)
{
	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for getting station information such as RSSI
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/

int mtk_cfg80211_get_station(struct wiphy *wiphy, struct net_device *ndev, u8 *mac, struct station_info *sinfo)
{
#define LINKSPEED_MAX_RANGE_11BGN 3000
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;
	PARAM_MAC_ADDRESS arBssid;
	UINT_32 u4BufLen, u4Rate;
	UINT_32 u8diffTxBad, u8diffRetry;
	INT_32 i4Rssi;
	PARAM_802_11_STATISTICS_STRUCT_T rStatistics;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	kalMemZero(arBssid, MAC_ADDR_LEN);
	wlanQueryInformation(prGlueInfo->prAdapter, wlanoidQueryBssid, &arBssid[0], sizeof(arBssid), &u4BufLen);

	/* 1. check BSSID */
	if (UNEQUAL_MAC_ADDR(arBssid, mac)) {
		/* wrong MAC address */
		DBGLOG(REQ, WARN, ("incorrect BSSID: [" MACSTR "] currently connected BSSID[" MACSTR "]\n",
				   MAC2STR(mac), MAC2STR(arBssid)));
		return -ENOENT;
	}

	/* 2. fill TX rate */
	rStatus = kalIoctl(prGlueInfo,
			   wlanoidQueryLinkSpeed, &u4Rate, sizeof(u4Rate), TRUE, FALSE, FALSE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, WARN, ("unable to retrieve link speed\n"));
	} else {
		sinfo->filled |= STATION_INFO_TX_BITRATE;
		sinfo->txrate.legacy = u4Rate / 1000;	/* convert from 100bps to 100kbps */
        
        if(sinfo->txrate.legacy > LINKSPEED_MAX_RANGE_11BGN){
            DBGLOG(REQ, WARN, ("Disconnected with cached Linkspeed(%d)->(%d)\n",sinfo->txrate.legacy,prGlueInfo->i4LinkSpeedCache));
            sinfo->txrate.legacy = prGlueInfo->i4LinkSpeedCache;
        }
        else{
            prGlueInfo->i4LinkSpeedCache = sinfo->txrate.legacy;            

        }

	}

	if (prGlueInfo->eParamMediaStateIndicated != PARAM_MEDIA_STATE_CONNECTED) {
		/* not connected */
		DBGLOG(REQ, WARN, ("not yet connected\n"));
	} else {
		/* 3. fill RSSI */
		rStatus = kalIoctl(prGlueInfo,
				   wlanoidQueryRssi, &i4Rssi, sizeof(i4Rssi), TRUE, FALSE, FALSE, FALSE, &u4BufLen);

		if (rStatus != WLAN_STATUS_SUCCESS) {
			DBGLOG(REQ, WARN, ("unable to retrieve link speed\n"));
		} else {
			sinfo->filled |= STATION_INFO_SIGNAL;
	    //in the cfg80211 layer, the signal is a signed char variable.
            if(i4Rssi < -128){
                DBGLOG(REQ, TRACE, ("Disconnected with cached RSSI(%d)->(%d)\n",i4Rssi,prGlueInfo->i4RssiCache));
        		sinfo->signal = prGlueInfo->i4RssiCache;
            }
    	    else{
				sinfo->signal = i4Rssi;	/* dBm */
                prGlueInfo->i4RssiCache = i4Rssi;            
            }
		}
		sinfo->rx_packets = prGlueInfo->rNetDevStats.rx_packets;

		/* 4. Fill Tx OK and Tx Bad */

		sinfo->filled |= STATION_INFO_TX_PACKETS;
		sinfo->filled |= STATION_INFO_TX_FAILED;
		{
			WLAN_STATUS rStatus;
			kalMemZero(&rStatistics, sizeof(rStatistics));
			/* Get Tx OK/Fail cnt from AIS statistic counter */
			rStatus = kalIoctl(prGlueInfo,
					   wlanoidQueryStatisticsPL,
					   &rStatistics, sizeof(rStatistics), TRUE, TRUE, TRUE, FALSE, &u4BufLen);

			if (rStatus != WLAN_STATUS_SUCCESS) {
				DBGLOG(REQ, WARN, ("unable to retrieive statistic\n"));
				printk("unable to retrieve statics");
			} else {
				INT_32 i4RssiThreshold = -85;	/* set rssi threshold -85dBm */
				UINT_32 u4LinkspeedThreshold = 55;	/* set link speed threshold 5.5Mbps */
				BOOLEAN fgWeighted = 0;

				/* calculate difference */
				u8diffTxBad = rStatistics.rFailedCount.QuadPart - prGlueInfo->u8Statistic[0];
				u8diffRetry = rStatistics.rRetryCount.QuadPart - prGlueInfo->u8Statistic[1];
				/* restore counters */
				prGlueInfo->u8Statistic[0] = rStatistics.rFailedCount.QuadPart;
				prGlueInfo->u8Statistic[1] = rStatistics.rRetryCount.QuadPart;

				/* check threshold is valid */
				if (prGlueInfo->fgPoorlinkValid) {
					if (prGlueInfo->i4RssiThreshold)
						i4RssiThreshold = prGlueInfo->i4RssiThreshold;
					if (prGlueInfo->u4LinkspeedThreshold)
						u4LinkspeedThreshold = prGlueInfo->u4LinkspeedThreshold;
				}
				/* add weighted to fail counter */
				if (sinfo->txrate.legacy < u4LinkspeedThreshold || sinfo->signal < i4RssiThreshold) {
					prGlueInfo->u8TotalFailCnt += (u8diffTxBad * 16 + u8diffRetry);
					fgWeighted = 1;
				} else {
					prGlueInfo->u8TotalFailCnt += u8diffTxBad;
				}
				/* report counters */
				prGlueInfo->rNetDevStats.tx_packets = rStatistics.rTransmittedFragmentCount.QuadPart;
				prGlueInfo->rNetDevStats.tx_errors = prGlueInfo->u8TotalFailCnt;

				sinfo->tx_packets = prGlueInfo->rNetDevStats.tx_packets;
				sinfo->tx_failed = prGlueInfo->rNetDevStats.tx_errors;
                /* Good Fail Bad Difference retry difference Linkspeed Rate Weighted */
        		DBGLOG(REQ, INFO,("Poorlink State TxOK(%d) TxFail(%d) Bad(%d) Retry(%d) Rate(%d) Signal(%d) Weight(%d) (%d)\n",
				       sinfo->tx_packets,
				       sinfo->tx_failed,
				       (int)u8diffTxBad,
				       (int)u8diffRetry,
				       sinfo->txrate.legacy,
            		sinfo->signal,
            		(int)fgWeighted,
            		(int)rStatistics.rMultipleRetryCount.QuadPart));
			}
		}

	}
	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for adding a station information
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_change_station(struct wiphy *wiphy, struct net_device *ndev, u8 *mac, struct station_parameters *params)
{
#if (CFG_SUPPORT_TDLS == 1)
	/*
	   EX: In supplicant,
	   (Supplicant) wpa_tdls_process_tpk_m3() ->
	   (Supplicant) wpa_tdls_enable_link() ->
	   (Supplicant) wpa_sm_tdls_peer_addset() ->
	   (Supplicant) ..tdls_peer_addset() ->
	   (Supplicant) wpa_supplicant_tdls_peer_addset() ->
	   (Supplicant) wpa_drv_sta_add() ->
	   (Supplicant) ..sta_add() ->
	   (Supplicant) wpa_driver_nl80211_sta_add() ->
	   (NL80211) nl80211_set_station() ->
	   (Driver) mtk_cfg80211_change_station()

	   if nl80211_set_station fails, supplicant will tear down the link.
	 */
	P_GLUE_INFO_T prGlueInfo;
	TDLS_CMD_PEER_UPDATE_T rCmdUpdate;
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen, u4Temp;

	/* sanity check */
	if ((wiphy == NULL) || (mac == NULL) || (params == NULL))
		return -EINVAL;

	DBGLOG(TDLS, INFO, ("%s: 0x%p 0x%x\n", __func__, params->supported_rates, params->sta_flags_set));

	if (!(params->sta_flags_set & BIT(NL80211_STA_FLAG_TDLS_PEER)))
		return -EOPNOTSUPP;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	if (prGlueInfo == NULL)
		return -EINVAL;

	/* TODO: check if we are station mode, not AP mode */

	/* init */
	kalMemZero(&rCmdUpdate, sizeof(rCmdUpdate));
	kalMemCopy(rCmdUpdate.aucPeerMac, mac, 6);

	if (params->supported_rates != NULL) {
		u4Temp = params->supported_rates_len;
		if (u4Temp > TDLS_CMD_PEER_UPDATE_SUP_RATE_MAX) {
			u4Temp = TDLS_CMD_PEER_UPDATE_SUP_RATE_MAX;
			DBGLOG(TDLS, ERROR, ("%s sup rate too long: %d\n", __func__, params->supported_rates_len));
		}
		kalMemCopy(rCmdUpdate.aucSupRate, params->supported_rates, u4Temp);
		rCmdUpdate.u2SupRateLen = u4Temp;
	}

	/*
	   In supplicant, only recognize WLAN_EID_QOS 46, not 0xDD WMM
	   So force to support UAPSD here.
	 */
	rCmdUpdate.UapsdBitmap = 0x0F;	/*params->uapsd_queues; */
	rCmdUpdate.UapsdMaxSp = 0;	/*params->max_sp; */

	DBGLOG(TDLS, INFO, ("%s: UapsdBitmap=0x%x UapsdMaxSp=%d\n",
			    __func__, rCmdUpdate.UapsdBitmap, rCmdUpdate.UapsdMaxSp));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
	rCmdUpdate.u2Capability = params->capability;

	if (params->ext_capab != NULL) {
		u4Temp = params->ext_capab_len;
		if (u4Temp > TDLS_CMD_PEER_UPDATE_EXT_CAP_MAXLEN) {
			u4Temp = TDLS_CMD_PEER_UPDATE_EXT_CAP_MAXLEN;
			DBGLOG(TDLS, ERROR, ("%s ext_capab too long: %d\n", __func__, params->ext_capab_len));
		}
		kalMemCopy(rCmdUpdate.aucExtCap, params->ext_capab, u4Temp);
		rCmdUpdate.u2ExtCapLen = u4Temp;
	}
#endif

	if (params->ht_capa != NULL) {
		DBGLOG(TDLS, INFO, ("%s: peer is 11n device\n", __func__));

		rCmdUpdate.rHtCap.u2CapInfo = params->ht_capa->cap_info;
		rCmdUpdate.rHtCap.ucAmpduParamsInfo = params->ht_capa->ampdu_params_info;
		rCmdUpdate.rHtCap.u2ExtHtCapInfo = params->ht_capa->extended_ht_cap_info;
		rCmdUpdate.rHtCap.u4TxBfCapInfo = params->ht_capa->tx_BF_cap_info;
		rCmdUpdate.rHtCap.ucAntennaSelInfo = params->ht_capa->antenna_selection_info;
		kalMemCopy(rCmdUpdate.rHtCap.rMCS.arRxMask,
			   params->ht_capa->mcs.rx_mask, sizeof(rCmdUpdate.rHtCap.rMCS.arRxMask));
		rCmdUpdate.rHtCap.rMCS.u2RxHighest = params->ht_capa->mcs.rx_highest;
		rCmdUpdate.rHtCap.rMCS.ucTxParams = params->ht_capa->mcs.tx_params;
		rCmdUpdate.fgIsSupHt = TRUE;
	}

	/* update a TDLS peer record */
	rStatus = kalIoctl(prGlueInfo,
			   TdlsexPeerUpdate,
			   &rCmdUpdate, sizeof(TDLS_CMD_PEER_UPDATE_T), FALSE, FALSE, FALSE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(TDLS, ERROR, ("%s update error:%x\n", __func__, rStatus));
		return -EINVAL;
	}
#endif /* CFG_SUPPORT_TDLS */

	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for adding a station information
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_add_station(struct wiphy *wiphy, struct net_device *ndev, u8 *mac, struct station_parameters *params)
{
#if (CFG_SUPPORT_TDLS == 1)
	/* from supplicant -- wpa_supplicant_tdls_peer_addset() */
	P_GLUE_INFO_T prGlueInfo;
	TDLS_CMD_PEER_ADD_T rCmdCreate;
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;

	if ((wiphy == NULL) || (mac == NULL) || (params == NULL))
		return -EINVAL;

	/*
	   wpa_sm_tdls_peer_addset(sm, peer->addr, 1, 0, 0, NULL, 0, NULL, NULL, 0,
	   NULL, 0);

	   wpa_sm_tdls_peer_addset(struct wpa_sm *sm, const u8 *addr, int add,
	   u16 aid, u16 capability, const u8 *supp_rates,
	   size_t supp_rates_len,
	   const struct ieee80211_ht_capabilities *ht_capab,
	   const struct ieee80211_vht_capabilities *vht_capab,
	   u8 qosinfo, const u8 *ext_capab, size_t ext_capab_len)

	   Only MAC address of the peer is valid.
	 */

	DBGLOG(TDLS, INFO, ("%s: 0x%p %d\n", __func__, params->supported_rates, params->supported_rates_len));

	/* sanity check */
	if (!(params->sta_flags_set & BIT(NL80211_STA_FLAG_TDLS_PEER)))
		return -EOPNOTSUPP;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	if (prGlueInfo == NULL)
		return -EINVAL;

	/* TODO: check if we are station mode, not AP mode */

	/* init */
	kalMemZero(&rCmdCreate, sizeof(rCmdCreate));
	kalMemCopy(rCmdCreate.aucPeerMac, mac, 6);

#if 0
	rCmdCreate.eNetTypeIndex = NETWORK_TYPE_AIS_INDEX;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
	rCmdCreate.u2CapInfo = params->capability;
#endif

	DBGLOG(TDLS, INFO, ("<tdls_cmd> %s: capability = 0x%x\n", __func__, rCmdCreate.u2CapInfo));

	if ((params->supported_rates != NULL) && (params->supported_rates_len != 0)) {
		UINT32 u4Idx;

		DBGLOG(TDLS, INFO, ("<tdls_cmd> %s: sup rate = 0x", __func__));

		rIeSup.ucId = ELEM_ID_SUP_RATES;
		rIeSup.ucLength = params->supported_rates_len;
		for (u4Idx = 0; u4Idx < rIeSup.ucLength; u4Idx++) {
			rIeSup.aucSupportedRates[u4Idx] = params->supported_rates[u4Idx];
			DBGLOG(TDLS, INFO, ("%x ", rIeSup.aucSupportedRates[u4Idx]));
		}
		DBGLOG(TDLS, INFO, ("\n"));

		rateGetRateSetFromIEs(&rIeSup,
				      NULL,
				      &rCmdCreate.u2OperationalRateSet,
				      &rCmdCreate.u2BSSBasicRateSet, &rCmdCreate.fgIsUnknownBssBasicRate);
	}

	/* phy type */
#endif

	/* create a TDLS peer record */
	rStatus = kalIoctl(prGlueInfo,
			   TdlsexPeerAdd,
			   &rCmdCreate, sizeof(TDLS_CMD_PEER_ADD_T), FALSE, FALSE, FALSE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(TDLS, ERROR, ("%s create error:%x\n", __func__, rStatus));
		return -EINVAL;
	}
#endif /* CFG_SUPPORT_TDLS */

	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for deleting a station information
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 *
 * @other
 *		must implement if you have add_station().
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_del_station(struct wiphy *wiphy, struct net_device *ndev, u8 *mac)
{
	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to do a scan
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
static PARAM_SCAN_REQUEST_EXT_T rScanRequest;
int mtk_cfg80211_scan(struct wiphy *wiphy,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
		      struct net_device *ndev,
#endif				/* LINUX_VERSION_CODE */
		      struct cfg80211_scan_request *request)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;
/* PARAM_SCAN_REQUEST_EXT_T rScanRequest; */

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	DBGLOG(REQ, INFO, ("mtk_cfg80211_scan\n"));
	kalMemZero(&rScanRequest, sizeof(PARAM_SCAN_REQUEST_EXT_T));

	/* check if there is any pending scan not yet finished */
	if (prGlueInfo->prScanRequest != NULL) {
		DBGLOG(REQ, INFO, ("prGlueInfo->prScanRequest != NULL\n"));
		return -EBUSY;
	}

	if (request->n_ssids == 0) {
		rScanRequest.rSsid.u4SsidLen = 0;
	} else if (request->n_ssids == 1) {
		COPY_SSID(rScanRequest.rSsid.aucSsid, rScanRequest.rSsid.u4SsidLen, request->ssids[0].ssid,
			  request->ssids[0].ssid_len);
	} else {
		DBGLOG(REQ, INFO, ("request->n_ssids:%d\n", request->n_ssids));
		return -EINVAL;
	}

	if (request->ie_len > 0) {
		rScanRequest.u4IELength = request->ie_len;
		rScanRequest.pucIE = (PUINT_8) (request->ie);
	} else {
		rScanRequest.u4IELength = 0;
	}

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetBssidListScanExt,
			   &rScanRequest, sizeof(PARAM_SCAN_REQUEST_EXT_T), FALSE, FALSE, FALSE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, INFO, ("scan error:%x\n", rStatus));
		return -EINVAL;
	}

	prGlueInfo->prScanRequest = request;

	return 0;
}

static UINT_8 wepBuf[48];

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to connect to
 *        the ESS with the specified parameters
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_connect(struct wiphy *wiphy, struct net_device *ndev, struct cfg80211_connect_params *sme)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;
	ENUM_PARAM_ENCRYPTION_STATUS_T eEncStatus;
	ENUM_PARAM_AUTH_MODE_T eAuthMode;
	UINT_32 cipher;
	PARAM_CONNECT_T rNewSsid;
	BOOLEAN fgCarryWPSIE = FALSE;
	ENUM_PARAM_OP_MODE_T eOpMode;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	DBGLOG(REQ, INFO, ("[wlan] mtk_cfg80211_connect %p %zu\n", sme->ie, sme->ie_len));

	if (prGlueInfo->prAdapter->rWifiVar.rConnSettings.eOPMode > NET_TYPE_AUTO_SWITCH)
		eOpMode = NET_TYPE_AUTO_SWITCH;
	else
		eOpMode = prGlueInfo->prAdapter->rWifiVar.rConnSettings.eOPMode;

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetInfrastructureMode,
			   &eOpMode, sizeof(eOpMode), FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(INIT, INFO, ("wlanoidSetInfrastructureMode fail 0x%x\n", rStatus));
		return -EFAULT;
	}

	/* after set operation mode, key table are cleared */

	/* reset wpa info */
	prGlueInfo->rWpaInfo.u4WpaVersion = IW_AUTH_WPA_VERSION_DISABLED;
	prGlueInfo->rWpaInfo.u4KeyMgmt = 0;
	prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_NONE;
	prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_NONE;
	prGlueInfo->rWpaInfo.u4AuthAlg = IW_AUTH_ALG_OPEN_SYSTEM;
#if CFG_SUPPORT_802_11W
	prGlueInfo->rWpaInfo.u4Mfp = IW_AUTH_MFP_DISABLED;
#endif

	if (sme->crypto.wpa_versions & NL80211_WPA_VERSION_1)
		prGlueInfo->rWpaInfo.u4WpaVersion = IW_AUTH_WPA_VERSION_WPA;
	else if (sme->crypto.wpa_versions & NL80211_WPA_VERSION_2)
		prGlueInfo->rWpaInfo.u4WpaVersion = IW_AUTH_WPA_VERSION_WPA2;
	else
		prGlueInfo->rWpaInfo.u4WpaVersion = IW_AUTH_WPA_VERSION_DISABLED;

	switch (sme->auth_type) {
	case NL80211_AUTHTYPE_OPEN_SYSTEM:
		prGlueInfo->rWpaInfo.u4AuthAlg = IW_AUTH_ALG_OPEN_SYSTEM;
		break;
	case NL80211_AUTHTYPE_SHARED_KEY:
		prGlueInfo->rWpaInfo.u4AuthAlg = IW_AUTH_ALG_SHARED_KEY;
		break;
	default:
		prGlueInfo->rWpaInfo.u4AuthAlg = IW_AUTH_ALG_OPEN_SYSTEM | IW_AUTH_ALG_SHARED_KEY;
		break;
	}

	if (sme->crypto.n_ciphers_pairwise) {
		prGlueInfo->prAdapter->rWifiVar.rConnSettings.rRsnInfo.au4PairwiseKeyCipherSuite[0] =
		    sme->crypto.ciphers_pairwise[0];
		switch (sme->crypto.ciphers_pairwise[0]) {
		case WLAN_CIPHER_SUITE_WEP40:
			prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_WEP40;
			break;
		case WLAN_CIPHER_SUITE_WEP104:
			prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_WEP104;
			break;
		case WLAN_CIPHER_SUITE_TKIP:
			prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_TKIP;
			break;
		case WLAN_CIPHER_SUITE_CCMP:
			prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_CCMP;
			break;
		case WLAN_CIPHER_SUITE_AES_CMAC:
			prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_CCMP;
			break;
		default:
			DBGLOG(REQ, WARN, ("invalid cipher pairwise (%d)\n", sme->crypto.ciphers_pairwise[0]));
			return -EINVAL;
		}
	}

	if (sme->crypto.cipher_group) {
		prGlueInfo->prAdapter->rWifiVar.rConnSettings.rRsnInfo.u4GroupKeyCipherSuite = sme->crypto.cipher_group;
		switch (sme->crypto.cipher_group) {
		case WLAN_CIPHER_SUITE_WEP40:
			prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_WEP40;
			break;
		case WLAN_CIPHER_SUITE_WEP104:
			prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_WEP104;
			break;
		case WLAN_CIPHER_SUITE_TKIP:
			prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_TKIP;
			break;
		case WLAN_CIPHER_SUITE_CCMP:
			prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_CCMP;
			break;
		case WLAN_CIPHER_SUITE_AES_CMAC:
			prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_CCMP;
			break;
		default:
			DBGLOG(REQ, WARN, ("invalid cipher group (%d)\n", sme->crypto.cipher_group));
			return -EINVAL;
		}
	}

	if (sme->crypto.n_akm_suites) {
		prGlueInfo->prAdapter->rWifiVar.rConnSettings.rRsnInfo.au4AuthKeyMgtSuite[0] =
		    sme->crypto.akm_suites[0];
		if (prGlueInfo->rWpaInfo.u4WpaVersion == IW_AUTH_WPA_VERSION_WPA) {
			switch (sme->crypto.akm_suites[0]) {
			case WLAN_AKM_SUITE_8021X:
				eAuthMode = AUTH_MODE_WPA;
				break;
			case WLAN_AKM_SUITE_PSK:
				eAuthMode = AUTH_MODE_WPA_PSK;
				break;
			default:
				DBGLOG(REQ, WARN, ("invalid cipher group (%d)\n", sme->crypto.cipher_group));
				return -EINVAL;
			}
		} else if (prGlueInfo->rWpaInfo.u4WpaVersion == IW_AUTH_WPA_VERSION_WPA2) {
			switch (sme->crypto.akm_suites[0]) {
			case WLAN_AKM_SUITE_8021X:
				eAuthMode = AUTH_MODE_WPA2;
				break;
			case WLAN_AKM_SUITE_PSK:
				eAuthMode = AUTH_MODE_WPA2_PSK;
				break;
			default:
				DBGLOG(REQ, WARN, ("invalid cipher group (%d)\n", sme->crypto.cipher_group));
				return -EINVAL;
			}
		}
	}

	if (prGlueInfo->rWpaInfo.u4WpaVersion == IW_AUTH_WPA_VERSION_DISABLED) {
		eAuthMode = (prGlueInfo->rWpaInfo.u4AuthAlg == IW_AUTH_ALG_OPEN_SYSTEM) ?
		    AUTH_MODE_OPEN : AUTH_MODE_AUTO_SWITCH;
	}

	prGlueInfo->rWpaInfo.fgPrivacyInvoke = sme->privacy;

	prGlueInfo->fgWpsActive = FALSE;
#if CFG_SUPPORT_HOTSPOT_2_0
	prGlueInfo->fgConnectHS20AP = FALSE;
#endif

	if (sme->ie && sme->ie_len > 0) {
		WLAN_STATUS rStatus;
		UINT_32 u4BufLen;
		PUINT_8 prDesiredIE = NULL;

#if CFG_SUPPORT_WAPI
		if (wextSrchDesiredWAPIIE(sme->ie, sme->ie_len, (PUINT_8 *) &prDesiredIE)) {
			rStatus = kalIoctl(prGlueInfo,
					   wlanoidSetWapiAssocInfo,
					   prDesiredIE, IE_SIZE(prDesiredIE), FALSE, FALSE, FALSE, FALSE, &u4BufLen);

			if (rStatus != WLAN_STATUS_SUCCESS)
				DBGLOG(SEC, WARN, ("[wapi] set wapi assoc info error:%x\n", rStatus));
		}
#endif

		DBGLOG(REQ, INFO, ("[wlan] wlanoidSetWapiAssocInfo: .fgWapiMode = %d\n",
				   prGlueInfo->prAdapter->rWifiVar.rConnSettings.fgWapiMode));

#if CFG_SUPPORT_WPS2
		if (wextSrchDesiredWPSIE(sme->ie, sme->ie_len, 0xDD, (PUINT_8 *) &prDesiredIE)) {
			prGlueInfo->fgWpsActive = TRUE;
			fgCarryWPSIE = TRUE;

			rStatus = kalIoctl(prGlueInfo,
					   wlanoidSetWSCAssocInfo,
					   prDesiredIE, IE_SIZE(prDesiredIE), FALSE, FALSE, FALSE, FALSE, &u4BufLen);
			if (rStatus != WLAN_STATUS_SUCCESS)
				DBGLOG(SEC, WARN, ("WSC] set WSC assoc info error:%x\n", rStatus));
		}
#endif

#if CFG_SUPPORT_HOTSPOT_2_0
		if (wextSrchDesiredHS20IE(sme->ie, sme->ie_len, (PUINT_8 *) &prDesiredIE)) {
			rStatus = kalIoctl(prGlueInfo,
					   wlanoidSetHS20Info,
					   prDesiredIE, IE_SIZE(prDesiredIE), FALSE, FALSE, TRUE, FALSE, &u4BufLen);
			if (rStatus != WLAN_STATUS_SUCCESS) {
				/* Do nothing */
				/* printk(KERN_INFO "[HS20] set HS20 assoc info error:%lx\n", rStatus); */
			}
		}
		if (wextSrchDesiredInterworkingIE(sme->ie, sme->ie_len, (PUINT_8 *) &prDesiredIE)) {
			rStatus = kalIoctl(prGlueInfo,
					   wlanoidSetInterworkingInfo,
					   prDesiredIE, IE_SIZE(prDesiredIE), FALSE, FALSE, TRUE, FALSE, &u4BufLen);
			if (rStatus != WLAN_STATUS_SUCCESS) {
				/* Do nothing */
				/* printk(KERN_INFO "[HS20] set Interworking assoc info error:%lx\n", rStatus); */
			}
		}
		if (wextSrchDesiredRoamingConsortiumIE(sme->ie, sme->ie_len, (PUINT_8 *) &prDesiredIE)) {
			rStatus = kalIoctl(prGlueInfo,
					   wlanoidSetRoamingConsortiumIEInfo,
					   prDesiredIE, IE_SIZE(prDesiredIE), FALSE, FALSE, TRUE, FALSE, &u4BufLen);
			if (rStatus != WLAN_STATUS_SUCCESS) {
				/* Do nothing */
				/* printk(KERN_INFO "[HS20] set RoamingConsortium assoc info error:%lx\n", rStatus); */
			}
		}
#endif
	}

	/* clear WSC Assoc IE buffer in case WPS IE is not detected */
	if (fgCarryWPSIE == FALSE) {
		kalMemZero(&prGlueInfo->aucWSCAssocInfoIE, 200);
		prGlueInfo->u2WSCAssocInfoIELen = 0;
	}

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetAuthMode, &eAuthMode, sizeof(eAuthMode), FALSE, FALSE, FALSE, FALSE, &u4BufLen);
	if (rStatus != WLAN_STATUS_SUCCESS)
		DBGLOG(REQ, WARN, ("set auth mode error:%x\n", rStatus));

	cipher = prGlueInfo->rWpaInfo.u4CipherGroup | prGlueInfo->rWpaInfo.u4CipherPairwise;

	if (prGlueInfo->rWpaInfo.fgPrivacyInvoke) {
		if (cipher & IW_AUTH_CIPHER_CCMP) {
			eEncStatus = ENUM_ENCRYPTION3_ENABLED;
		} else if (cipher & IW_AUTH_CIPHER_TKIP) {
			eEncStatus = ENUM_ENCRYPTION2_ENABLED;
		} else if (cipher & (IW_AUTH_CIPHER_WEP104 | IW_AUTH_CIPHER_WEP40)) {
			eEncStatus = ENUM_ENCRYPTION1_ENABLED;
		} else if (cipher & IW_AUTH_CIPHER_NONE) {
			if (prGlueInfo->rWpaInfo.fgPrivacyInvoke)
				eEncStatus = ENUM_ENCRYPTION1_ENABLED;
			else
				eEncStatus = ENUM_ENCRYPTION_DISABLED;
		} else {
			eEncStatus = ENUM_ENCRYPTION_DISABLED;
		}
	} else {
		eEncStatus = ENUM_ENCRYPTION_DISABLED;
	}

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetEncryptionStatus,
			   &eEncStatus, sizeof(eEncStatus), FALSE, FALSE, FALSE, FALSE, &u4BufLen);
	if (rStatus != WLAN_STATUS_SUCCESS)
		DBGLOG(REQ, WARN, ("set encryption mode error:%x\n", rStatus));

	if (sme->key_len != 0 && prGlueInfo->rWpaInfo.u4WpaVersion == IW_AUTH_WPA_VERSION_DISABLED) {
		P_PARAM_WEP_T prWepKey = (P_PARAM_WEP_T) wepBuf;

		prWepKey->u4Length = 12 + sme->key_len;
		prWepKey->u4KeyLength = (UINT_32) sme->key_len;
		prWepKey->u4KeyIndex = (UINT_32) sme->key_idx;
		prWepKey->u4KeyIndex |= BIT(31);
		if (prWepKey->u4KeyLength > 32) {
			DBGLOG(REQ, WARN, ("Too long key length (%u)\n", prWepKey->u4KeyLength));
			return -EINVAL;
		}
		kalMemCopy(prWepKey->aucKeyMaterial, sme->key, prWepKey->u4KeyLength);

		rStatus = kalIoctl(prGlueInfo,
				   wlanoidSetAddWep,
				   prWepKey, prWepKey->u4Length, FALSE, FALSE, TRUE, FALSE, &u4BufLen);

		if (rStatus != WLAN_STATUS_SUCCESS) {
			DBGLOG(INIT, INFO, ("wlanoidSetAddWep fail 0x%x\n", rStatus));
			return -EFAULT;
		}
	}

	if (sme->channel)
		rNewSsid.u4CenterFreq = sme->channel->center_freq;
	else
		rNewSsid.u4CenterFreq = 0;
	rNewSsid.pucBssid = sme->bssid;
	rNewSsid.pucSsid = sme->ssid;
	rNewSsid.u4SsidLen = sme->ssid_len;
	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetConnect,
			   (PVOID) & rNewSsid, sizeof(PARAM_CONNECT_T), FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, WARN, ("set SSID:%x\n", rStatus));
		return -EINVAL;
	}

	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to disconnect from
 *        currently connected ESS
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_disconnect(struct wiphy *wiphy, struct net_device *ndev, u16 reason_code)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	rStatus = kalIoctl(prGlueInfo, wlanoidSetDisassociate, NULL, 0, FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, WARN, ("disassociate error:%x\n", rStatus));
		return -EFAULT;
	}

	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to join an IBSS group
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_join_ibss(struct wiphy *wiphy, struct net_device *ndev, struct cfg80211_ibss_params *params)
{
	PARAM_SSID_T rNewSsid;
	P_GLUE_INFO_T prGlueInfo = NULL;
	UINT_32 u4ChnlFreq;	/* Store channel or frequency information */
	UINT_32 u4BufLen = 0;
	WLAN_STATUS rStatus;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
	struct ieee80211_channel *channel = NULL;
#endif

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	/* set channel */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
	if (params->chandef.chan)
		channel = params->chandef.chan;
	if (channel) {
		u4ChnlFreq = nicChannelNum2Freq(channel->hw_value);
#else

	if (params->channel) {
		u4ChnlFreq = nicChannelNum2Freq(params->channel->hw_value);
#endif
		rStatus = kalIoctl(prGlueInfo,
				   wlanoidSetFrequency,
				   &u4ChnlFreq, sizeof(u4ChnlFreq), FALSE, FALSE, FALSE, FALSE, &u4BufLen);
		if (rStatus != WLAN_STATUS_SUCCESS)
			return -EFAULT;
	}

	/* set SSID */
	kalMemCopy(rNewSsid.aucSsid, params->ssid, params->ssid_len);
	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetSsid,
			   (PVOID) & rNewSsid, sizeof(PARAM_SSID_T), FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, WARN, ("set SSID:%x\n", rStatus));
		return -EFAULT;
	}

	return 0;

	return -EINVAL;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to leave from IBSS group
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_leave_ibss(struct wiphy *wiphy, struct net_device *ndev)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	rStatus = kalIoctl(prGlueInfo, wlanoidSetDisassociate, NULL, 0, FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, WARN, ("disassociate error:%x\n", rStatus));
		return -EFAULT;
	}

	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to configure
 *        WLAN power managemenet
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_set_power_mgmt(struct wiphy *wiphy, struct net_device *ndev, bool enabled, int timeout)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;
	PARAM_POWER_MODE ePowerMode;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	if (enabled) {
		if (timeout == -1)
			ePowerMode = Param_PowerModeFast_PSP;
		else
			ePowerMode = Param_PowerModeMAX_PSP;
	} else {
		ePowerMode = Param_PowerModeCAM;
	}

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSet802dot11PowerSaveProfile,
			   &ePowerMode, sizeof(ePowerMode), FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, WARN, ("set_power_mgmt error:%x\n", rStatus));
		return -EFAULT;
	}

	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to cache
 *        a PMKID for a BSSID
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_set_pmksa(struct wiphy *wiphy, struct net_device *ndev, struct cfg80211_pmksa *pmksa)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;
	P_PARAM_PMKID_T prPmkid;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	prPmkid = (P_PARAM_PMKID_T) kalMemAlloc(8 + sizeof(PARAM_BSSID_INFO_T), VIR_MEM_TYPE);
	if (!prPmkid) {
		DBGLOG(INIT, INFO, ("Can not alloc memory for IW_PMKSA_ADD\n"));
		return -ENOMEM;
	}

	prPmkid->u4Length = 8 + sizeof(PARAM_BSSID_INFO_T);
	prPmkid->u4BSSIDInfoCount = 1;
	kalMemCopy(prPmkid->arBSSIDInfo->arBSSID, pmksa->bssid, 6);
	kalMemCopy(prPmkid->arBSSIDInfo->arPMKID, pmksa->pmkid, IW_PMKID_LEN);

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetPmkid, prPmkid, sizeof(PARAM_PMKID_T), FALSE, FALSE, FALSE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS)
		DBGLOG(INIT, INFO, ("add pmkid error:%x\n", rStatus));
	kalMemFree(prPmkid, VIR_MEM_TYPE, 8 + sizeof(PARAM_BSSID_INFO_T));

	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to remove
 *        a cached PMKID for a BSSID
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_del_pmksa(struct wiphy *wiphy, struct net_device *ndev, struct cfg80211_pmksa *pmksa)
{

	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to flush
 *        all cached PMKID
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_flush_pmksa(struct wiphy *wiphy, struct net_device *ndev)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;
	P_PARAM_PMKID_T prPmkid;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	prPmkid = (P_PARAM_PMKID_T) kalMemAlloc(8, VIR_MEM_TYPE);
	if (!prPmkid) {
		DBGLOG(INIT, INFO, ("Can not alloc memory for IW_PMKSA_FLUSH\n"));
		return -ENOMEM;
	}

	prPmkid->u4Length = 8;
	prPmkid->u4BSSIDInfoCount = 0;

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetPmkid, prPmkid, sizeof(PARAM_PMKID_T), FALSE, FALSE, FALSE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS)
		DBGLOG(INIT, INFO, ("flush pmkid error:%x\n", rStatus));
	kalMemFree(prPmkid, VIR_MEM_TYPE, 8);

	return 0;
}

void mtk_cfg80211_mgmt_frame_register(IN struct wiphy *wiphy,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
				      IN struct net_device *dev,
#else
				      IN struct wireless_dev *wdev,
#endif
				      IN u16 frame_type, IN bool reg)
{
#if 0
	P_MSG_P2P_MGMT_FRAME_REGISTER_T prMgmtFrameRegister = (P_MSG_P2P_MGMT_FRAME_REGISTER_T) NULL;
#endif
	P_GLUE_INFO_T prGlueInfo = (P_GLUE_INFO_T) NULL;

	do {

		DBGLOG(INIT, LOUD, ("mtk_cfg80211_mgmt_frame_register\n"));

		prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);

		switch (frame_type) {
		case MAC_FRAME_PROBE_REQ:
			if (reg) {
				prGlueInfo->u4OsMgmtFrameFilter |= PARAM_PACKET_FILTER_PROBE_REQ;
				DBGLOG(INIT, LOUD, ("Open packet filer probe request\n"));
			} else {
				prGlueInfo->u4OsMgmtFrameFilter &= ~PARAM_PACKET_FILTER_PROBE_REQ;
				DBGLOG(INIT, LOUD, ("Close packet filer probe request\n"));
			}
			break;
		case MAC_FRAME_ACTION:
			if (reg) {
				prGlueInfo->u4OsMgmtFrameFilter |= PARAM_PACKET_FILTER_ACTION_FRAME;
				DBGLOG(INIT, LOUD, ("Open packet filer action frame.\n"));
			} else {
				prGlueInfo->u4OsMgmtFrameFilter &= ~PARAM_PACKET_FILTER_ACTION_FRAME;
				DBGLOG(INIT, LOUD, ("Close packet filer action frame.\n"));
			}
			break;
		default:
			printk("Ask frog to add code for mgmt:%x\n", frame_type);
			break;
		}

		if (prGlueInfo->prAdapter != NULL) {
			/* prGlueInfo->ulFlag |= GLUE_FLAG_FRAME_FILTER_AIS; */
			set_bit(GLUE_FLAG_FRAME_FILTER_AIS_BIT, &prGlueInfo->ulFlag);

			/* wake up main thread */
			wake_up_interruptible(&prGlueInfo->waitq);

			if (in_interrupt())
				DBGLOG(INIT, TRACE, ("It is in interrupt level\n"));
		}
#if 0

		prMgmtFrameRegister = (P_MSG_P2P_MGMT_FRAME_REGISTER_T) cnmMemAlloc(prGlueInfo->prAdapter,
										    RAM_TYPE_MSG,
										    sizeof
										    (MSG_P2P_MGMT_FRAME_REGISTER_T));

		if (prMgmtFrameRegister == NULL) {
			ASSERT(FALSE);
			break;
		}

		prMgmtFrameRegister->rMsgHdr.eMsgId = MID_MNY_P2P_MGMT_FRAME_REGISTER;

		prMgmtFrameRegister->u2FrameType = frame_type;
		prMgmtFrameRegister->fgIsRegister = reg;

		mboxSendMsg(prGlueInfo->prAdapter, MBOX_ID_0, (P_MSG_HDR_T) prMgmtFrameRegister, MSG_SEND_METHOD_BUF);

#endif

	} while (FALSE);

	return;
}				/* mtk_cfg80211_mgmt_frame_register */

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to stay on a
 *        specified channel
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_remain_on_channel(struct wiphy *wiphy,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
				   struct net_device *ndev,
#else
				   struct wireless_dev *wdev,
#endif				/* LINUX_VERSION_CODE */
				   struct ieee80211_channel *chan,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
				   enum nl80211_channel_type channel_type,
#endif				/* LINUX_VERSION_CODE */
				   unsigned int duration, u64 *cookie)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	INT_32 i4Rslt = -EINVAL;
	P_MSG_REMAIN_ON_CHANNEL_T prMsgChnlReq = (P_MSG_REMAIN_ON_CHANNEL_T) NULL;

	do {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
		if ((wiphy == NULL) || (ndev == NULL) || (chan == NULL) || (cookie == NULL))
			break;
#else
		if ((wiphy == NULL) || (wdev == NULL) || (chan == NULL) || (cookie == NULL))
			break;
#endif

		prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
		ASSERT(prGlueInfo);

#if 1
		printk("--> %s()\n", __func__);
#endif

		*cookie = prGlueInfo->u8Cookie++;

		prMsgChnlReq = cnmMemAlloc(prGlueInfo->prAdapter, RAM_TYPE_MSG, sizeof(MSG_REMAIN_ON_CHANNEL_T));

		if (prMsgChnlReq == NULL) {
			ASSERT(FALSE);
			i4Rslt = -ENOMEM;
			break;
		}

		prMsgChnlReq->rMsgHdr.eMsgId = MID_MNY_AIS_REMAIN_ON_CHANNEL;
		prMsgChnlReq->u8Cookie = *cookie;
		prMsgChnlReq->u4DurationMs = duration;

		prMsgChnlReq->ucChannelNum = nicFreq2ChannelNum(chan->center_freq * 1000);

		switch (chan->band) {
		case IEEE80211_BAND_2GHZ:
			prMsgChnlReq->eBand = BAND_2G4;
			break;
		case IEEE80211_BAND_5GHZ:
			prMsgChnlReq->eBand = BAND_5G;
			break;
		default:
			prMsgChnlReq->eBand = BAND_2G4;
			break;
		}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
		switch (channel_type) {
		case NL80211_CHAN_NO_HT:
			prMsgChnlReq->eSco = CHNL_EXT_SCN;
			break;
		case NL80211_CHAN_HT20:
			prMsgChnlReq->eSco = CHNL_EXT_SCN;
			break;
		case NL80211_CHAN_HT40MINUS:
			prMsgChnlReq->eSco = CHNL_EXT_SCA;
			break;
		case NL80211_CHAN_HT40PLUS:
			prMsgChnlReq->eSco = CHNL_EXT_SCB;
			break;
		default:
			ASSERT(FALSE);
			prMsgChnlReq->eSco = CHNL_EXT_SCN;
			break;
		}
#endif
		mboxSendMsg(prGlueInfo->prAdapter, MBOX_ID_0, (P_MSG_HDR_T) prMsgChnlReq, MSG_SEND_METHOD_BUF);

		i4Rslt = 0;
	} while (FALSE);

	return i4Rslt;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to cancel staying
 *        on a specified channel
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_cancel_remain_on_channel(struct wiphy *wiphy,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
					  struct net_device *ndev,
#else
					  struct wireless_dev *wdev,
#endif				/* LINUX_VERSION_CODE */
					  u64 cookie)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	INT_32 i4Rslt = -EINVAL;
	P_MSG_CANCEL_REMAIN_ON_CHANNEL_T prMsgChnlAbort = (P_MSG_CANCEL_REMAIN_ON_CHANNEL_T) NULL;

	do {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
		if ((wiphy == NULL) || (ndev == NULL))
			break;
#else
		if ((wiphy == NULL) || (wdev == NULL))
			break;
#endif

		prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
		ASSERT(prGlueInfo);

#if 1
		printk("--> %s()\n", __func__);
#endif

		prMsgChnlAbort =
		    cnmMemAlloc(prGlueInfo->prAdapter, RAM_TYPE_MSG, sizeof(MSG_CANCEL_REMAIN_ON_CHANNEL_T));

		if (prMsgChnlAbort == NULL) {
			ASSERT(FALSE);
			i4Rslt = -ENOMEM;
			break;
		}

		prMsgChnlAbort->rMsgHdr.eMsgId = MID_MNY_AIS_CANCEL_REMAIN_ON_CHANNEL;
		prMsgChnlAbort->u8Cookie = cookie;

		mboxSendMsg(prGlueInfo->prAdapter, MBOX_ID_0, (P_MSG_HDR_T) prMsgChnlAbort, MSG_SEND_METHOD_BUF);

		i4Rslt = 0;
	} while (FALSE);

	return i4Rslt;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to send a management frame
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
int
mtk_cfg80211_mgmt_tx(struct wiphy *wiphy,
		     struct net_device *ndev,
		     struct ieee80211_channel *channel,
		     bool offscan,
		     enum nl80211_channel_type channel_type,
		     bool channel_type_valid,
		     unsigned int wait, const u8 *buf, size_t len, bool no_cck, bool dont_wait_for_ack, u64 *cookie)
#else
int
mtk_cfg80211_mgmt_tx(struct wiphy *wiphy,
		     struct wireless_dev *wdev,
		     struct ieee80211_channel *channel,
		     bool offscan,
		     unsigned int wait, const u8 *buf, size_t len, bool no_cck, bool dont_wait_for_ack, u64 *cookie)
#endif				/* LINUX_VERSION_CODE */
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	INT_32 i4Rslt = -EINVAL;
	P_MSG_MGMT_TX_REQUEST_T prMsgTxReq = (P_MSG_MGMT_TX_REQUEST_T) NULL;
	P_MSDU_INFO_T prMgmtFrame = (P_MSDU_INFO_T) NULL;
	PUINT_8 pucFrameBuf = (PUINT_8) NULL;

	do {
#if 1
		printk("--> %s()\n", __func__);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
		if ((wiphy == NULL) || (buf == NULL) || (len == 0) || (ndev == NULL) || (cookie == NULL))
			break;
#else
		if ((wiphy == NULL) || (buf == NULL) || (len == 0) || (wdev == NULL) || (cookie == NULL))
			break;
#endif

		prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
		ASSERT(prGlueInfo);

		*cookie = prGlueInfo->u8Cookie++;

		/* Channel & Channel Type & Wait time are ignored. */
		prMsgTxReq = cnmMemAlloc(prGlueInfo->prAdapter, RAM_TYPE_MSG, sizeof(MSG_MGMT_TX_REQUEST_T));

		if (prMsgTxReq == NULL) {
			ASSERT(FALSE);
			i4Rslt = -ENOMEM;
			break;
		}

		prMsgTxReq->fgNoneCckRate = FALSE;
		prMsgTxReq->fgIsWaitRsp = TRUE;

		prMgmtFrame = cnmMgtPktAlloc(prGlueInfo->prAdapter, (UINT_32) (len + MAC_TX_RESERVED_FIELD));
		prMsgTxReq->prMgmtMsduInfo = prMgmtFrame;
		if (prMsgTxReq->prMgmtMsduInfo == NULL) {
			ASSERT(FALSE);
			i4Rslt = -ENOMEM;
			break;
		}

		prMsgTxReq->u8Cookie = *cookie;
		prMsgTxReq->rMsgHdr.eMsgId = MID_MNY_AIS_MGMT_TX;

		pucFrameBuf = (PUINT_8) ((ULONG) prMgmtFrame->prPacket + MAC_TX_RESERVED_FIELD);

		kalMemCopy(pucFrameBuf, buf, len);

		prMgmtFrame->u2FrameLength = len;

		mboxSendMsg(prGlueInfo->prAdapter, MBOX_ID_0, (P_MSG_HDR_T) prMsgTxReq, MSG_SEND_METHOD_BUF);

		i4Rslt = 0;
	} while (FALSE);

	if ((i4Rslt != 0) && (prMsgTxReq != NULL)) {
		if (prMsgTxReq->prMgmtMsduInfo != NULL)
			cnmMgtPktFree(prGlueInfo->prAdapter, prMsgTxReq->prMgmtMsduInfo);

		cnmMemFree(prGlueInfo->prAdapter, prMsgTxReq);
	}

	return i4Rslt;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to cancel the wait time
 *        from transmitting a management frame on another channel
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_mgmt_tx_cancel_wait(struct wiphy *wiphy,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
				     struct net_device *ndev,
#else
				     struct wireless_dev *wdev,
#endif				/* LINUX_VERSION_CODE */
				     u64 cookie)
{
	P_GLUE_INFO_T prGlueInfo = NULL;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

#if 1
	printk("--> %s()\n", __func__);
#endif

	/* not implemented */

	return -EINVAL;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for handling sched_scan start/stop request
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/

int
mtk_cfg80211_sched_scan_start(IN struct wiphy *wiphy,
			      IN struct net_device *ndev, IN struct cfg80211_sched_scan_request *request)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;
	UINT_32 i, u4BufLen;
	P_PARAM_SCHED_SCAN_REQUEST prSchedScanRequest;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);

	DBGLOG(INIT, INFO, ("mtk_cfg80211_sched_scan_start \n"));
	DBGLOG(INIT, TRACE, ("mtk_cfg80211_sched_scan_start \n"));

	ASSERT(prGlueInfo);

	/* check if there is any pending scan/sched_scan not yet finished */
	if (prGlueInfo->prScanRequest != NULL || prGlueInfo->prSchedScanRequest != NULL) {
		return -EBUSY;
	} else if (request == NULL || request->n_match_sets > CFG_SCAN_SSID_MATCH_MAX_NUM) {
		/* invalid scheduled scan request */
		return -EINVAL;
	} else if (!request->n_ssids || !request->n_match_sets) {
		/* invalid scheduled scan request */
		return -EINVAL;
	}

	prSchedScanRequest = (P_PARAM_SCHED_SCAN_REQUEST) kalMemAlloc(sizeof(PARAM_SCHED_SCAN_REQUEST), VIR_MEM_TYPE);
	if (prSchedScanRequest == NULL)
		return -ENOMEM;

	prSchedScanRequest->u4SsidNum = request->n_match_sets;
	for (i = 0; i < request->n_match_sets; i++) {
		if (request->match_sets == NULL || &(request->match_sets[i]) == NULL) {
			prSchedScanRequest->arSsid[i].u4SsidLen = 0;
		} else {
			COPY_SSID(prSchedScanRequest->arSsid[i].aucSsid,
				  prSchedScanRequest->arSsid[i].u4SsidLen,
				  request->match_sets[i].ssid.ssid, request->match_sets[i].ssid.ssid_len);
		}
	}

	prSchedScanRequest->u4IELength = request->ie_len;
	if (request->ie_len > 0)
		prSchedScanRequest->pucIE = (PUINT_8) (request->ie);

	prSchedScanRequest->u2ScanInterval = (UINT_16) (request->interval);

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetStartSchedScan,
			   prSchedScanRequest, sizeof(PARAM_SCHED_SCAN_REQUEST), FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	kalMemFree(prSchedScanRequest, VIR_MEM_TYPE, sizeof(PARAM_SCHED_SCAN_REQUEST));

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, WARN, ("scheduled scan error:%x\n", rStatus));
		return -EINVAL;
	}

	prGlueInfo->prSchedScanRequest = request;

	return 0;
}

int mtk_cfg80211_sched_scan_stop(IN struct wiphy *wiphy, IN struct net_device *ndev)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	/* check if there is any pending scan/sched_scan not yet finished */
	if (prGlueInfo->prSchedScanRequest == NULL)
		return -EBUSY;

	rStatus = kalIoctl(prGlueInfo, wlanoidSetStopSchedScan, NULL, 0, FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, WARN, ("scheduled scan error:%x\n", rStatus));
		return -EINVAL;
	}

	return 0;
}

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for handling association request
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int mtk_cfg80211_assoc(struct wiphy *wiphy, struct net_device *ndev, struct cfg80211_assoc_request *req)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	PARAM_MAC_ADDRESS arBssid;
#if CFG_SUPPORT_HOTSPOT_2_0
	PUINT_8 prDesiredIE = NULL;
#endif
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	kalMemZero(arBssid, MAC_ADDR_LEN);
	wlanQueryInformation(prGlueInfo->prAdapter, wlanoidQueryBssid, &arBssid[0], sizeof(arBssid), &u4BufLen);

	/* 1. check BSSID */
	if (UNEQUAL_MAC_ADDR(arBssid, req->bss->bssid)) {
		/* wrong MAC address */
		DBGLOG(REQ, WARN, ("incorrect BSSID: [" MACSTR "] currently connected BSSID[" MACSTR "]\n",
				   MAC2STR(req->bss->bssid), MAC2STR(arBssid)));
		return -ENOENT;
	}

	if (req->ie && req->ie_len > 0) {
#if CFG_SUPPORT_HOTSPOT_2_0
		if (wextSrchDesiredHS20IE((PUINT_8) req->ie, req->ie_len, (PUINT_8 *) &prDesiredIE)) {
			rStatus = kalIoctl(prGlueInfo,
					   wlanoidSetHS20Info,
					   prDesiredIE, IE_SIZE(prDesiredIE), FALSE, FALSE, TRUE, FALSE, &u4BufLen);
			if (rStatus != WLAN_STATUS_SUCCESS) {
				/* Do nothing */
				/* printk(KERN_INFO "[HS20] set HS20 assoc info error:%lx\n", rStatus); */
			}
		}

		if (wextSrchDesiredInterworkingIE((PUINT_8) req->ie, req->ie_len, (PUINT_8 *) &prDesiredIE)) {
			rStatus = kalIoctl(prGlueInfo,
					   wlanoidSetInterworkingInfo,
					   prDesiredIE, IE_SIZE(prDesiredIE), FALSE, FALSE, TRUE, FALSE, &u4BufLen);
			if (rStatus != WLAN_STATUS_SUCCESS) {
				/* Do nothing */
				/* printk(KERN_INFO "[HS20] set Interworking assoc info error:%lx\n", rStatus); */
			}
		}

		if (wextSrchDesiredRoamingConsortiumIE((PUINT_8) req->ie, req->ie_len, (PUINT_8 *) &prDesiredIE)) {
			rStatus = kalIoctl(prGlueInfo,
					   wlanoidSetRoamingConsortiumIEInfo,
					   prDesiredIE, IE_SIZE(prDesiredIE), FALSE, FALSE, TRUE, FALSE, &u4BufLen);
			if (rStatus != WLAN_STATUS_SUCCESS) {
				/* Do nothing */
				/* printk(KERN_INFO "[HS20] set RoamingConsortium assoc info error:%lx\n", rStatus); */
			}
		}
#endif
	}

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetBssid,
			   (PVOID) req->bss->bssid, MAC_ADDR_LEN, FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(REQ, WARN, ("set BSSID:%x\n", rStatus));
		return -EINVAL;
	}

	return 0;
}

#if CONFIG_NL80211_TESTMODE
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
#define NLA_PUT(skb, attrtype, attrlen, data) \
do { \
	if (unlikely(nla_put(skb, attrtype, attrlen, data) < 0)) \
		goto nla_put_failure; \
} while (0)

#define NLA_PUT_TYPE(skb, type, attrtype, value) \
do { \
	type __tmp = value; \
	NLA_PUT(skb, attrtype, sizeof(type), &__tmp); \
} while (0)

#define NLA_PUT_U8(skb, attrtype, value) \
	 NLA_PUT_TYPE(skb, u8, attrtype, value)

#define NLA_PUT_U16(skb, attrtype, value) \
	 NLA_PUT_TYPE(skb, u16, attrtype, value)

#define NLA_PUT_U32(skb, attrtype, value) \
	 NLA_PUT_TYPE(skb, u32, attrtype, value)

#define NLA_PUT_U64(skb, attrtype, value) \
	 NLA_PUT_TYPE(skb, u64, attrtype, value)

#endif

#if CFG_SUPPORT_WAPI
int mtk_cfg80211_testmode_set_key_ext(IN struct wiphy *wiphy, IN void *data, IN int len)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	P_NL80211_DRIVER_SET_KEY_EXTS prParams = (P_NL80211_DRIVER_SET_KEY_EXTS) NULL;
	struct iw_encode_exts *prIWEncExt = (struct iw_encode_exts *)NULL;
	WLAN_STATUS rstatus = WLAN_STATUS_SUCCESS;
	int fgIsValid = 0;
	UINT_32 u4BufLen = 0;

	P_PARAM_WPI_KEY_T prWpiKey = (P_PARAM_WPI_KEY_T) keyStructBuf;
	memset(keyStructBuf, 0, sizeof(keyStructBuf));

	ASSERT(wiphy);

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);

#if 1
	printk("--> %s()\n", __func__);
#endif

	if (data && len)
		prParams = (P_NL80211_DRIVER_SET_KEY_EXTS) data;

	if (prParams)
		prIWEncExt = (struct iw_encode_exts *)&prParams->ext;

	if (prIWEncExt->alg == IW_ENCODE_ALG_SMS4) {
		/* KeyID */
		prWpiKey->ucKeyID = prParams->key_index;
		prWpiKey->ucKeyID--;
		if (prWpiKey->ucKeyID > 1) {
			/* key id is out of range */
			/* printk(KERN_INFO "[wapi] add key error: key_id invalid %d\n", prWpiKey->ucKeyID); */
			return -EINVAL;
		}

		if (prIWEncExt->key_len != 32) {
			/* key length not valid */
			/* printk(KERN_INFO "[wapi] add key error: key_len invalid %d\n", prIWEncExt->key_len); */
			return -EINVAL;
		}
		/* printk(KERN_INFO "[wapi] %d ext_flags %d\n", prEnc->flags, prIWEncExt->ext_flags); */

		if (prIWEncExt->ext_flags & IW_ENCODE_EXT_GROUP_KEY) {
			prWpiKey->eKeyType = ENUM_WPI_GROUP_KEY;
			prWpiKey->eDirection = ENUM_WPI_RX;
		} else if (prIWEncExt->ext_flags & IW_ENCODE_EXT_SET_TX_KEY) {
			prWpiKey->eKeyType = ENUM_WPI_PAIRWISE_KEY;
			prWpiKey->eDirection = ENUM_WPI_RX_TX;
		}
/* #if CFG_SUPPORT_WAPI */
		/* handle_sec_msg_final(prIWEncExt->key, 32, prIWEncExt->key, NULL); */
/* #endif */
		/* PN */
		memcpy(prWpiKey->aucPN, prIWEncExt->tx_seq, IW_ENCODE_SEQ_MAX_SIZE * 2);

		/* BSSID */
		memcpy(prWpiKey->aucAddrIndex, prIWEncExt->addr, 6);

		memcpy(prWpiKey->aucWPIEK, prIWEncExt->key, 16);
		prWpiKey->u4LenWPIEK = 16;

		memcpy(prWpiKey->aucWPICK, &prIWEncExt->key[16], 16);
		prWpiKey->u4LenWPICK = 16;

		rstatus = kalIoctl(prGlueInfo,
				   wlanoidSetWapiKey,
				   prWpiKey, sizeof(PARAM_WPI_KEY_T), FALSE, FALSE, TRUE, FALSE, &u4BufLen);

		if (rstatus != WLAN_STATUS_SUCCESS) {
			/* printk(KERN_INFO "[wapi] add key error:%lx\n", rStatus); */
			fgIsValid = -EFAULT;
		}

	}
	return fgIsValid;
}
#endif

int
mtk_cfg80211_testmode_get_sta_statistics(IN struct wiphy *wiphy, IN void *data, IN int len, IN P_GLUE_INFO_T prGlueInfo)
{
	WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;
	INT_32 i4Status = -EINVAL;
	UINT_32 u4BufLen;
	UINT_32 u4LinkScore;
	UINT_32 u4TotalError;
	UINT_32 u4TxExceedThresholdCount;
	UINT_32 u4TxTotalCount;

	P_NL80211_DRIVER_GET_STA_STATISTICS_PARAMS prParams = NULL;
	PARAM_GET_STA_STA_STATISTICS rQueryStaStatistics;
	struct sk_buff *skb;

	ASSERT(wiphy);
	ASSERT(prGlueInfo);

	if (data && len)
		prParams = (P_NL80211_DRIVER_GET_STA_STATISTICS_PARAMS) data;

	if (!prParams->aucMacAddr) {
		DBGLOG(QM, TRACE, ("%s MAC Address is NULL\n", __func__));
		return -EINVAL;
	}

	skb = cfg80211_testmode_alloc_reply_skb(wiphy, sizeof(PARAM_GET_STA_STA_STATISTICS) + 1);

	if (!skb) {
		DBGLOG(QM, TRACE, ("%s allocate skb failed:%x\n", __func__, rStatus));
		return -ENOMEM;
	}

	DBGLOG(QM, TRACE, ("Get [" MACSTR "] STA statistics\n", MAC2STR(prParams->aucMacAddr)));

	kalMemZero(&rQueryStaStatistics, sizeof(rQueryStaStatistics));
	COPY_MAC_ADDR(rQueryStaStatistics.aucMacAddr, prParams->aucMacAddr);

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidQueryStaStatistics,
			   &rQueryStaStatistics, sizeof(rQueryStaStatistics), TRUE, FALSE, TRUE, TRUE, &u4BufLen);

	/* Calcute Link Score */
	u4TxExceedThresholdCount = rQueryStaStatistics.u4TxExceedThresholdCount;
	u4TxTotalCount = rQueryStaStatistics.u4TxTotalCount;
	u4TotalError = rQueryStaStatistics.u4TxFailCount + rQueryStaStatistics.u4TxLifeTimeoutCount;

	/* u4LinkScore 10~100 , ExceedThreshold ratio 0~90 only */
	/* u4LinkScore 0~9    , Drop packet ratio 0~9 and all packets exceed threshold */
	if (u4TxTotalCount) {
		if (u4TxExceedThresholdCount <= u4TxTotalCount)
			u4LinkScore = (90 - ((u4TxExceedThresholdCount * 90) / u4TxTotalCount));
		else
			u4LinkScore = 0;
	} else {
		u4LinkScore = 90;
	}

	u4LinkScore += 10;

	if (u4LinkScore == 10) {

		if (u4TotalError <= u4TxTotalCount)
			u4LinkScore = (10 - ((u4TotalError * 10) / u4TxTotalCount));
		else
			u4LinkScore = 0;

	}

	if (u4LinkScore > 100)
		u4LinkScore = 100;

	NLA_PUT_U8(skb, NL80211_TESTMODE_STA_STATISTICS_INVALID, 0);
	NLA_PUT_U8(skb, NL80211_TESTMODE_STA_STATISTICS_VERSION, NL80211_DRIVER_TESTMODE_VERSION);
	NLA_PUT(skb, NL80211_TESTMODE_STA_STATISTICS_MAC, MAC_ADDR_LEN, prParams->aucMacAddr);
	NLA_PUT_U8(skb, NL80211_TESTMODE_STA_STATISTICS_LINK_SCORE, u4LinkScore);
	NLA_PUT_U32(skb, NL80211_TESTMODE_STA_STATISTICS_FLAG, rQueryStaStatistics.u4Flag);

	/* FW part STA link status */
	NLA_PUT_U8(skb, NL80211_TESTMODE_STA_STATISTICS_PER, rQueryStaStatistics.ucPer);
	NLA_PUT_U8(skb, NL80211_TESTMODE_STA_STATISTICS_RSSI, rQueryStaStatistics.ucRcpi);
	NLA_PUT_U32(skb, NL80211_TESTMODE_STA_STATISTICS_PHY_MODE, rQueryStaStatistics.u4PhyMode);
	NLA_PUT_U16(skb, NL80211_TESTMODE_STA_STATISTICS_TX_RATE, rQueryStaStatistics.u2LinkSpeed);
	NLA_PUT_U32(skb, NL80211_TESTMODE_STA_STATISTICS_FAIL_CNT, rQueryStaStatistics.u4TxFailCount);
	NLA_PUT_U32(skb, NL80211_TESTMODE_STA_STATISTICS_TIMEOUT_CNT, rQueryStaStatistics.u4TxLifeTimeoutCount);
	NLA_PUT_U32(skb, NL80211_TESTMODE_STA_STATISTICS_AVG_AIR_TIME, rQueryStaStatistics.u4TxAverageAirTime);

	/* Driver part link status */
	NLA_PUT_U32(skb, NL80211_TESTMODE_STA_STATISTICS_TOTAL_CNT, rQueryStaStatistics.u4TxTotalCount);
	NLA_PUT_U32(skb, NL80211_TESTMODE_STA_STATISTICS_THRESHOLD_CNT, rQueryStaStatistics.u4TxExceedThresholdCount);
	NLA_PUT_U32(skb, NL80211_TESTMODE_STA_STATISTICS_AVG_PROCESS_TIME, rQueryStaStatistics.u4TxAverageProcessTime);

	/* Network counter */
	NLA_PUT(skb,
		NL80211_TESTMODE_STA_STATISTICS_TC_EMPTY_CNT_ARRAY,
		sizeof(rQueryStaStatistics.au4TcResourceEmptyCount), rQueryStaStatistics.au4TcResourceEmptyCount);

	/* Sta queue length */
	NLA_PUT(skb,
		NL80211_TESTMODE_STA_STATISTICS_TC_QUE_LEN_ARRAY,
		sizeof(rQueryStaStatistics.au4TcQueLen), rQueryStaStatistics.au4TcQueLen);

	/* Global QM counter */
	NLA_PUT(skb,
		NL80211_TESTMODE_STA_STATISTICS_TC_AVG_QUE_LEN_ARRAY,
		sizeof(rQueryStaStatistics.au4TcAverageQueLen), rQueryStaStatistics.au4TcAverageQueLen);

	NLA_PUT(skb,
		NL80211_TESTMODE_STA_STATISTICS_TC_CUR_QUE_LEN_ARRAY,
		sizeof(rQueryStaStatistics.au4TcCurrentQueLen), rQueryStaStatistics.au4TcCurrentQueLen);

	/* Reserved field */
	NLA_PUT(skb,
		NL80211_TESTMODE_STA_STATISTICS_RESERVED_ARRAY,
		sizeof(rQueryStaStatistics.au4Reserved), rQueryStaStatistics.au4Reserved);

	i4Status = cfg80211_testmode_reply(skb);

nla_put_failure:
	return i4Status;
}

int
mtk_cfg80211_testmode_get_link_detection(IN struct wiphy *wiphy, IN void *data, IN int len, IN P_GLUE_INFO_T prGlueInfo)
{

	WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;
	INT_32 i4Status = -EINVAL;
	UINT_32 u4BufLen;

	PARAM_802_11_STATISTICS_STRUCT_T rStatistics;
	struct sk_buff *skb;

	ASSERT(wiphy);
	ASSERT(prGlueInfo);

	skb = cfg80211_testmode_alloc_reply_skb(wiphy, sizeof(PARAM_GET_STA_STA_STATISTICS) + 1);

	if (!skb) {
		DBGLOG(QM, TRACE, ("%s allocate skb failed:%x\n", __func__, rStatus));
		return -ENOMEM;
	}

	kalMemZero(&rStatistics, sizeof(rStatistics));

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidQueryStatistics,
			   &rStatistics, sizeof(rStatistics), TRUE, TRUE, TRUE, FALSE, &u4BufLen);

	NLA_PUT_U8(skb, NL80211_TESTMODE_STA_STATISTICS_INVALID, 0);
	NLA_PUT_U64(skb, NL80211_TESTMODE_LINK_TX_FAIL_CNT, rStatistics.rFailedCount.QuadPart);
	NLA_PUT_U64(skb, NL80211_TESTMODE_LINK_TX_RETRY_CNT, rStatistics.rRetryCount.QuadPart);
	NLA_PUT_U64(skb, NL80211_TESTMODE_LINK_TX_MULTI_RETRY_CNT, rStatistics.rMultipleRetryCount.QuadPart);
	NLA_PUT_U64(skb, NL80211_TESTMODE_LINK_ACK_FAIL_CNT, rStatistics.rACKFailureCount.QuadPart);
	NLA_PUT_U64(skb, NL80211_TESTMODE_LINK_FCS_ERR_CNT, rStatistics.rFCSErrorCount.QuadPart);

	i4Status = cfg80211_testmode_reply(skb);

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_testmode_sw_cmd(IN struct wiphy *wiphy, IN void *data, IN int len)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	P_NL80211_DRIVER_SW_CMD_PARAMS prParams = (P_NL80211_DRIVER_SW_CMD_PARAMS) NULL;
	WLAN_STATUS rstatus = WLAN_STATUS_SUCCESS;
	int fgIsValid = 0;
	UINT_32 u4SetInfoLen = 0;

	ASSERT(wiphy);

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);

#if 1
	printk("--> %s()\n", __func__);
#endif

	if (data && len)
		prParams = (P_NL80211_DRIVER_SW_CMD_PARAMS) data;

	if (prParams) {
		if (prParams->set == 1) {
			rstatus = kalIoctl(prGlueInfo,
					   (PFN_OID_HANDLER_FUNC) wlanoidSetSwCtrlWrite,
					   &prParams->adr, (UINT_32) 8, FALSE, FALSE, TRUE, FALSE, &u4SetInfoLen);
		}
	}

	if (WLAN_STATUS_SUCCESS != rstatus)
		fgIsValid = -EFAULT;

	return fgIsValid;
}

#if CFG_SUPPORT_HOTSPOT_2_0
int mtk_cfg80211_testmode_hs20_cmd(IN struct wiphy *wiphy, IN void *data, IN int len)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	struct wpa_driver_hs20_data_s *prParams = NULL;
	WLAN_STATUS rstatus = WLAN_STATUS_SUCCESS;
	int fgIsValid = 0;
	UINT_32 u4SetInfoLen = 0;

	ASSERT(wiphy);

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);

#if 1
	printk("--> %s()\n", __func__);
#endif

	if (data && len)
		prParams = (struct wpa_driver_hs20_data_s *)data;

	printk("[%s] Cmd Type (%d)\n", __func__, prParams->CmdType);

	if (prParams) {
		int i;

		switch (prParams->CmdType) {
		case HS20_CMD_ID_SET_BSSID_POOL:
			printk("[%s] fgBssidPoolIsEnable (%d)\n", __func__,
			       prParams->hs20_set_bssid_pool.fgBssidPoolIsEnable);
			printk("[%s] ucNumBssidPool (%d)\n", __func__, prParams->hs20_set_bssid_pool.ucNumBssidPool);

			for (i = 0; i < prParams->hs20_set_bssid_pool.ucNumBssidPool; i++) {
				printk("[%s][%d][" MACSTR "]\n", __func__, i,
				       MAC2STR(prParams->hs20_set_bssid_pool.arBssidPool[i]));
			}
			rstatus = kalIoctl(prGlueInfo,
					   (PFN_OID_HANDLER_FUNC) wlanoidSetHS20BssidPool,
					   &prParams->hs20_set_bssid_pool,
					   sizeof(struct param_hs20_set_bssid_pool),
					   FALSE, FALSE, TRUE, FALSE, &u4SetInfoLen);
			break;
		default:
			printk("[%s] Unknown Cmd Type (%d)\n", __func__, prParams->CmdType);
			rstatus = WLAN_STATUS_FAILURE;

		}

	}

	if (WLAN_STATUS_SUCCESS != rstatus)
		fgIsValid = -EFAULT;

	return fgIsValid;
}

#endif
int
mtk_cfg80211_testmode_set_poorlink_param(IN struct wiphy *wiphy, IN void *data, IN int len, IN P_GLUE_INFO_T prGlueInfo)
{
	int fgIsValid = 0;
	P_NL80211_DRIVER_POORLINK_PARAMS prParams = NULL;

	ASSERT(wiphy);
	ASSERT(prGlueInfo);

	if (data && len)
		prParams = (P_NL80211_DRIVER_POORLINK_PARAMS) data;
	if (prParams->ucLinkSpeed)
		prGlueInfo->u4LinkspeedThreshold = prParams->ucLinkSpeed * 10;
	if (prParams->cRssi)
		prGlueInfo->i4RssiThreshold = prParams->cRssi;
	if (!prGlueInfo->fgPoorlinkValid)
		prGlueInfo->fgPoorlinkValid = 1;
#if 0
	printk("poorlink set param valid(%d)rssi(%d)linkspeed(%d)\n",
	       prGlueInfo->fgPoorlinkValid, prGlueInfo->i4RssiThreshold, prGlueInfo->u4LinkspeedThreshold);
#endif

	return fgIsValid;

}

int mtk_cfg80211_testmode_cmd(IN struct wiphy *wiphy, IN void *data, IN int len)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	P_NL80211_DRIVER_TEST_MODE_PARAMS prParams = (P_NL80211_DRIVER_TEST_MODE_PARAMS) NULL;
	INT_32 i4Status = -EINVAL;
#if CFG_SUPPORT_HOTSPOT_2_0
	BOOLEAN fgIsValid = 0;
#endif

	ASSERT(wiphy);

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);

	if (data && len) {
		prParams = (P_NL80211_DRIVER_TEST_MODE_PARAMS) data;
	} else {
		DBGLOG(REQ, ERROR, ("mtk_cfg80211_testmode_cmd, data is NULL\n"));
		return i4Status;
	}

	/* Clear the version byte */
	prParams->index = prParams->index & ~BITS(24, 31);

	if (prParams) {
		switch (prParams->index) {
		case TESTMODE_CMD_ID_SW_CMD:	/* SW cmd */
			i4Status = mtk_cfg80211_testmode_sw_cmd(wiphy, data, len);
			break;
		case TESTMODE_CMD_ID_WAPI:	/* WAPI */
#if CFG_SUPPORT_WAPI
			i4Status = mtk_cfg80211_testmode_set_key_ext(wiphy, data, len);
#endif
			break;
		case TESTMODE_CMD_ID_SUSPEND:
			{
				P_NL80211_DRIVER_SUSPEND_PARAMS prParams = (P_NL80211_DRIVER_SUSPEND_PARAMS) data;

				if (prParams->suspend == 1) {
					wlanHandleSystemSuspend();
					if (prGlueInfo->prAdapter->fgIsP2PRegistered)
						p2pHandleSystemSuspend();
					i4Status = 0;
				} else if (prParams->suspend == 0) {
					wlanHandleSystemResume();
					if (prGlueInfo->prAdapter->fgIsP2PRegistered)
						p2pHandleSystemResume();
					i4Status = 0;
				}
				break;
			}
		case TESTMODE_CMD_ID_STATISTICS:
			i4Status = mtk_cfg80211_testmode_get_sta_statistics(wiphy, data, len, prGlueInfo);
			break;
		case TESTMODE_CMD_ID_LINK_DETECT:
			i4Status = mtk_cfg80211_testmode_get_link_detection(wiphy, data, len, prGlueInfo);
			break;
		case TESTMODE_CMD_ID_POORLINK:
			i4Status = mtk_cfg80211_testmode_set_poorlink_param(wiphy, data, len, prGlueInfo);
			break;

#if CFG_SUPPORT_HOTSPOT_2_0
		case TESTMODE_CMD_ID_HS20:
			if (mtk_cfg80211_testmode_hs20_cmd(wiphy, data, len))
				fgIsValid = TRUE;
			break;
#endif
		default:
			i4Status = -EINVAL;
			break;
		}
	}

	return i4Status;
}

int mtk_cfg80211_testmode_get_scan_done(IN struct wiphy *wiphy, IN void *data, IN int len, IN P_GLUE_INFO_T prGlueInfo)
{
#define NL80211_TESTMODE_P2P_SCANDONE_INVALID 0
#define NL80211_TESTMODE_P2P_SCANDONE_STATUS 1
	WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;
	INT_32 i4Status = -EINVAL, READY_TO_BEAM = 0;

/* P_NL80211_DRIVER_GET_STA_STATISTICS_PARAMS prParams = NULL; */
	struct sk_buff *skb;

	ASSERT(wiphy);
	ASSERT(prGlueInfo);

	skb = cfg80211_testmode_alloc_reply_skb(wiphy, sizeof(UINT_32));
	READY_TO_BEAM =
	    (UINT_32) (prGlueInfo->prAdapter->rWifiVar.prP2pFsmInfo->rScanReqInfo.
		       fgIsGOInitialDone) &
	    (!prGlueInfo->prAdapter->rWifiVar.prP2pFsmInfo->rScanReqInfo.fgIsScanRequest);
	DBGLOG(QM, TRACE,
	       ("NFC:GOInitialDone[%d] and P2PScanning[%d]\n",
		prGlueInfo->prAdapter->rWifiVar.prP2pFsmInfo->rScanReqInfo.fgIsGOInitialDone,
		prGlueInfo->prAdapter->rWifiVar.prP2pFsmInfo->rScanReqInfo.fgIsScanRequest));

	if (!skb) {
		DBGLOG(QM, TRACE, ("%s allocate skb failed:%x\n", __func__, rStatus));
		return -ENOMEM;
	}

	NLA_PUT_U8(skb, NL80211_TESTMODE_P2P_SCANDONE_INVALID, 0);
	NLA_PUT_U32(skb, NL80211_TESTMODE_P2P_SCANDONE_STATUS, READY_TO_BEAM);

	i4Status = cfg80211_testmode_reply(skb);

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_vendor_get_capabilities(struct wiphy *wiphy, struct wireless_dev *wdev, const void *data, int data_len)
{
	P_GLUE_INFO_T prGlueInfo = NULL;
	INT_32 i4Status = -EINVAL;
	PARAM_WIFI_GSCAN_CAPABILITIES_STRUCT_T rGscanCapabilities;
	struct sk_buff *skb;
	/* UINT_32 u4BufLen; */

	printk("%s for vendor command \r\n", __func__);

	ASSERT(wiphy);
	ASSERT(wdev);
	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, sizeof(rGscanCapabilities));
	if (!skb) {
		DBGLOG(INIT, TRACE, ("%s allocate skb failed:%x\n", __func__, i4Status));
		return -ENOMEM;
	}

	kalMemZero(&rGscanCapabilities, sizeof(rGscanCapabilities));

	/*rStatus = kalIoctl(prGlueInfo,
	   wlanoidQueryStatistics,
	   &rGscanCapabilities,
	   sizeof(rGscanCapabilities),
	   TRUE,
	   TRUE,
	   TRUE,
	   FALSE,
	   &u4BufLen); */
	rGscanCapabilities.max_scan_cache_size = PSCAN_MAX_SCAN_CACHE_SIZE;
	rGscanCapabilities.max_scan_buckets = GSCAN_MAX_BUCKETS;
	rGscanCapabilities.max_ap_cache_per_scan = PSCAN_MAX_AP_CACHE_PER_SCAN;
	rGscanCapabilities.max_rssi_sample_size = 10;
	rGscanCapabilities.max_scan_reporting_threshold = GSCAN_MAX_REPORT_THRESHOLD;
	rGscanCapabilities.max_hotlist_aps = MAX_HOTLIST_APS;
	rGscanCapabilities.max_significant_wifi_change_aps = MAX_SIGNIFICANT_CHANGE_APS;
	rGscanCapabilities.max_bssid_history_entries = PSCAN_MAX_AP_CACHE_PER_SCAN * PSCAN_MAX_SCAN_CACHE_SIZE;

	/* NLA_PUT_U8(skb, NL80211_TESTMODE_STA_STATISTICS_INVALID, 0); */
	/* NLA_PUT_U32(skb, NL80211_ATTR_VENDOR_ID, GOOGLE_OUI); */
	/* NLA_PUT_U32(skb, NL80211_ATTR_VENDOR_SUBCMD, GSCAN_SUBCMD_GET_CAPABILITIES); */
	NLA_PUT(skb, NL80211_ATTR_VENDOR_CAPABILITIES, sizeof(rGscanCapabilities), &rGscanCapabilities);

	i4Status = cfg80211_vendor_cmd_reply(skb);

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_vendor_set_config(struct wiphy *wiphy, struct wireless_dev *wdev, const void *data, int data_len)
{
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;
	P_GLUE_INFO_T prGlueInfo = NULL;
	/* CMD_GSCN_REQ_T rCmdGscnParam; */

	/* INT_32 i4Status = -EINVAL; */
	P_PARAM_WIFI_GSCAN_CMD_PARAMS prWifiScanCmd;
	struct nlattr *attr[GSCAN_ATTRIBUTE_REPORT_EVENTS + 1];
	struct nlattr *pbucket, *pchannel;
	UINT_32 len_basic, len_bucket, len_channel;
	int i, j, k;
	static struct nla_policy policy[GSCAN_ATTRIBUTE_REPORT_EVENTS + 1] = {
		[GSCAN_ATTRIBUTE_NUM_BUCKETS] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_BASE_PERIOD] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_BUCKETS_BAND] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_BUCKET_ID] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_BUCKET_PERIOD] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_BUCKET_NUM_CHANNELS] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_BUCKET_CHANNELS] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_REPORT_EVENTS] = {.type = NLA_U32},
	};

	ASSERT(wiphy);
	ASSERT(wdev);
	if ((data == NULL) || !data_len)
		goto nla_put_failure;

	prWifiScanCmd = (P_PARAM_WIFI_GSCAN_CMD_PARAMS) kalMemAlloc(sizeof(PARAM_WIFI_GSCAN_CMD_PARAMS), VIR_MEM_TYPE);
	if (!prWifiScanCmd) {
		DBGLOG(INIT, INFO, ("Can not alloc memory for PARAM_WIFI_GSCAN_CMD_PARAMS\n"));
		return -ENOMEM;
	}

	printk("%s for vendor command: data_len=%d \r\n", __func__, data_len);
	for (i = 0; i < 13; i++)
		printk("0x%x 0x%x 0x%x 0x%x \r\n", *((UINT_32 *) data + i * 4),
		       *((UINT_32 *) data + i * 4 + 1), *((UINT_32 *) data + i * 4 + 2),
		       *((UINT_32 *) data + i * 4 + 3));
	kalMemZero(prWifiScanCmd, sizeof(PARAM_WIFI_GSCAN_CMD_PARAMS));
	kalMemZero(attr, sizeof(struct nlattr *) * (GSCAN_ATTRIBUTE_REPORT_EVENTS + 1));

	nla_parse_nested(attr, GSCAN_ATTRIBUTE_REPORT_EVENTS, (struct nlattr *)(data - NLA_HDRLEN), policy);
	len_basic = 0;
	for (k = GSCAN_ATTRIBUTE_NUM_BUCKETS; k <= GSCAN_ATTRIBUTE_REPORT_EVENTS; k++) {
		if (attr[k]) {
			switch (k) {
			case GSCAN_ATTRIBUTE_BASE_PERIOD:
				prWifiScanCmd->base_period = nla_get_u32(attr[k]);
				len_basic += NLA_ALIGN(attr[k]->nla_len);
				break;
			case GSCAN_ATTRIBUTE_NUM_BUCKETS:
				prWifiScanCmd->num_buckets = nla_get_u32(attr[k]);
				len_basic += NLA_ALIGN(attr[k]->nla_len);
				printk("attr=0x%x, num_buckets=%d nla_len=%d, \r\n",
				       *(UINT_32 *) attr[k], prWifiScanCmd->num_buckets, attr[k]->nla_len);
				break;
			}
		}
	}
	pbucket = (struct nlattr *)((UINT_8 *) data + len_basic);
	/* printk("+++basic attribute size=%d pbucket=%p\r\n", len_basic, pbucket); */

	for (i = 0; i < prWifiScanCmd->num_buckets; i++) {
		nla_parse_nested(attr, GSCAN_ATTRIBUTE_REPORT_EVENTS, (struct nlattr *)pbucket, policy);
		len_bucket = 0;
		for (k = GSCAN_ATTRIBUTE_NUM_BUCKETS; k <= GSCAN_ATTRIBUTE_REPORT_EVENTS; k++) {
			if (attr[k]) {
				switch (k) {
				case GSCAN_ATTRIBUTE_BUCKETS_BAND:
					prWifiScanCmd->buckets[i].band = nla_get_u32(attr[k]);
					len_bucket += NLA_ALIGN(attr[k]->nla_len);
					break;
				case GSCAN_ATTRIBUTE_BUCKET_ID:
					prWifiScanCmd->buckets[i].bucket = nla_get_u32(attr[k]);
					len_bucket += NLA_ALIGN(attr[k]->nla_len);
					break;
				case GSCAN_ATTRIBUTE_BUCKET_PERIOD:
					prWifiScanCmd->buckets[i].period = nla_get_u32(attr[k]);
					len_bucket += NLA_ALIGN(attr[k]->nla_len);
					break;
				case GSCAN_ATTRIBUTE_REPORT_EVENTS:
					prWifiScanCmd->buckets[i].report_events = nla_get_u32(attr[k]);
					len_bucket += NLA_ALIGN(attr[k]->nla_len);
					break;
				case GSCAN_ATTRIBUTE_BUCKET_NUM_CHANNELS:
					prWifiScanCmd->buckets[i].num_channels = nla_get_u32(attr[k]);
					len_bucket += NLA_ALIGN(attr[k]->nla_len);
					printk("bucket%d: attr=0x%x, num_channels=%d nla_len = %d, \r\n",
					       i, *(UINT_32 *) attr[k], nla_get_u32(attr[k]), attr[k]->nla_len);
					break;
				}
			}
		}
		pbucket = (struct nlattr *)((UINT_8 *) pbucket + NLA_HDRLEN);
		/* request.attr_start(i) as nested attribute */
		/* printk("+++pure bucket size=%d pbucket=%p \r\n", len_bucket, pbucket); */
		pbucket = (struct nlattr *)((UINT_8 *) pbucket + len_bucket);
		/* pure bucket payload, not include channels */

		/*don't need to use nla_parse_nested to parse channels */
		/* the header of channel in bucket i */
		pchannel = (struct nlattr *)((UINT_8 *) pbucket + NLA_HDRLEN);
		for (j = 0; j < prWifiScanCmd->buckets[i].num_channels; j++) {
			prWifiScanCmd->buckets[i].channels[j].channel = nla_get_u32(pchannel);
			len_channel = NLA_ALIGN(pchannel->nla_len);
			printk("attr=0x%x, channel=%d, \r\n", *(UINT_32 *) pchannel, nla_get_u32(pchannel));

			pchannel = (struct nlattr *)((UINT_8 *) pchannel + len_channel);
		}
		pbucket = pchannel;
	}

	printk("base_period=%d, num_buckets=%d, bucket0: %d %d %d %d, num_channels=%d, "
		"channel0=%d, channel1=%d; bucket1: num_channels=%d, channel0=%d, channel1=%d; "
		"bucket2: num_channels=%d, channel0=%d, channel1=%d \r\n",
		prWifiScanCmd->base_period, prWifiScanCmd->num_buckets,
		prWifiScanCmd->buckets[0].bucket, prWifiScanCmd->buckets[0].period,
		prWifiScanCmd->buckets[0].band, prWifiScanCmd->buckets[0].report_events,
		prWifiScanCmd->buckets[0].num_channels,
		prWifiScanCmd->buckets[0].channels[0].channel, prWifiScanCmd->buckets[0].channels[1].channel,
		prWifiScanCmd->buckets[1].num_channels,
		prWifiScanCmd->buckets[1].channels[0].channel, prWifiScanCmd->buckets[1].channels[1].channel,
		prWifiScanCmd->buckets[2].num_channels,
		prWifiScanCmd->buckets[2].channels[0].channel, prWifiScanCmd->buckets[2].channels[1].channel);

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetGSCNAParam,
			   prWifiScanCmd, sizeof(PARAM_WIFI_GSCAN_CMD_PARAMS), FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	return 0;

nla_put_failure:
	return -1;
}

int mtk_cfg80211_vendor_set_scan_config(struct wiphy *wiphy, struct wireless_dev *wdev, const void *data, int data_len)
{
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;
	P_GLUE_INFO_T prGlueInfo = NULL;

	INT_32 i4Status = -EINVAL;
	PARAM_WIFI_GSCAN_CMD_PARAMS rWifiScanCmd;
	struct nlattr *attr[GSCAN_ATTRIBUTE_NUM_SCANS_TO_CACHE + 1];
	/* UINT_32 num_scans = 0; */	/* another attribute */
	int i, k;
	static struct nla_policy policy[GSCAN_ATTRIBUTE_NUM_SCANS_TO_CACHE + 1] = {
		[GSCAN_ATTRIBUTE_NUM_AP_PER_SCAN] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_REPORT_THRESHOLD] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_NUM_SCANS_TO_CACHE] = {.type = NLA_U32},
	};

	ASSERT(wiphy);
	ASSERT(wdev);
	if ((data == NULL) || !data_len)
		goto nla_put_failure;
	printk("%s for vendor command: data_len=%d \r\n", __func__, data_len);
	for (i = 0; i < 2; i++)
		printk("0x%x 0x%x 0x%x 0x%x \r\n", *((UINT_32 *) data + i * 4), *((UINT_32 *) data + i * 4 + 1),
		       *((UINT_32 *) data + i * 4 + 2), *((UINT_32 *) data + i * 4 + 3));
	kalMemZero(&rWifiScanCmd, sizeof(rWifiScanCmd));
	kalMemZero(attr, sizeof(struct nlattr *) * (GSCAN_ATTRIBUTE_NUM_SCANS_TO_CACHE + 1));

	nla_parse_nested(attr, GSCAN_ATTRIBUTE_NUM_SCANS_TO_CACHE, (struct nlattr *)(data - NLA_HDRLEN), policy);
	for (k = GSCAN_ATTRIBUTE_NUM_AP_PER_SCAN; k <= GSCAN_ATTRIBUTE_NUM_SCANS_TO_CACHE; k++) {
		if (attr[k]) {
			switch (k) {
			case GSCAN_ATTRIBUTE_NUM_AP_PER_SCAN:
				rWifiScanCmd.max_ap_per_scan = nla_get_u32(attr[k]);
				break;
			case GSCAN_ATTRIBUTE_REPORT_THRESHOLD:
				rWifiScanCmd.report_threshold = nla_get_u32(attr[k]);
				break;
			case GSCAN_ATTRIBUTE_NUM_SCANS_TO_CACHE:
				rWifiScanCmd.num_scans = nla_get_u32(attr[k]);
				break;
			}
		}
	}
	printk("attr=0x%x, attr2=0x%x ", *(UINT_32 *) attr[GSCAN_ATTRIBUTE_NUM_AP_PER_SCAN],
	       *(UINT_32 *) attr[GSCAN_ATTRIBUTE_REPORT_THRESHOLD]);

	printk("max_ap_per_scan=%d, report_threshold=%d num_scans=%d \r\n",
	       rWifiScanCmd.max_ap_per_scan, rWifiScanCmd.report_threshold, rWifiScanCmd.num_scans);

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetGSCNAConfig,
			   &rWifiScanCmd, sizeof(PARAM_WIFI_GSCAN_CMD_PARAMS), FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	return 0;

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_vendor_set_significant_change(struct wiphy *wiphy, struct wireless_dev *wdev,
					       const void *data, int data_len)
{
	INT_32 i4Status = -EINVAL;
	PARAM_WIFI_SIGNIFICANT_CHANGE rWifiChangeCmd;
	UINT_8 flush = 0;
	struct nlattr *attr[GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH + 1];
	struct nlattr *paplist;
	int i, k;
	UINT_32 len_basic, len_aplist;
	static struct nla_policy policy[GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH + 1] = {
		[GSCAN_ATTRIBUTE_BSSID] = {.type = NLA_UNSPEC},
		[GSCAN_ATTRIBUTE_RSSI_LOW] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_RSSI_HIGH] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_RSSI_SAMPLE_SIZE] = {.type = NLA_U16},
		[GSCAN_ATTRIBUTE_LOST_AP_SAMPLE_SIZE] = {.type = NLA_U16},
		[GSCAN_ATTRIBUTE_MIN_BREACHING] = {.type = NLA_U16},
		[GSCAN_ATTRIBUTE_NUM_AP] = {.type = NLA_U16},
		[GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH] = {.type = NLA_U8},
	};

	ASSERT(wiphy);
	ASSERT(wdev);
	if ((data == NULL) || !data_len)
		goto nla_put_failure;
	printk("%s for vendor command: data_len=%d \r\n", __func__, data_len);
	for (i = 0; i < 6; i++)
		printk("0x%x 0x%x 0x%x 0x%x \r\n", *((UINT_32 *) data + i * 4), *((UINT_32 *) data + i * 4 + 1),
		       *((UINT_32 *) data + i * 4 + 2), *((UINT_32 *) data + i * 4 + 3));
	kalMemZero(&rWifiChangeCmd, sizeof(rWifiChangeCmd));
	kalMemZero(attr, sizeof(struct nlattr *) * (GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH + 1));

	nla_parse_nested(attr, GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH, (struct nlattr *)(data - NLA_HDRLEN), policy);
	len_basic = 0;
	for (k = GSCAN_ATTRIBUTE_RSSI_SAMPLE_SIZE; k <= GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH; k++) {
		if (attr[k]) {
			switch (k) {
			case GSCAN_ATTRIBUTE_RSSI_SAMPLE_SIZE:
				rWifiChangeCmd.rssi_sample_size = nla_get_u16(attr[k]);
				len_basic += NLA_ALIGN(attr[k]->nla_len);
				break;
			case GSCAN_ATTRIBUTE_LOST_AP_SAMPLE_SIZE:
				rWifiChangeCmd.lost_ap_sample_size = nla_get_u16(attr[k]);
				len_basic += NLA_ALIGN(attr[k]->nla_len);
				break;
			case GSCAN_ATTRIBUTE_MIN_BREACHING:
				rWifiChangeCmd.min_breaching = nla_get_u16(attr[k]);
				len_basic += NLA_ALIGN(attr[k]->nla_len);
				break;
			case GSCAN_ATTRIBUTE_NUM_AP:
				rWifiChangeCmd.num_ap = nla_get_u16(attr[k]);
				len_basic += NLA_ALIGN(attr[k]->nla_len);
				printk("attr=0x%x, num_ap=%d nla_len=%d, \r\n",
				       *(UINT_32 *) attr[k], rWifiChangeCmd.num_ap, attr[k]->nla_len);
				break;
			case GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH:
				flush = nla_get_u8(attr[k]);
				len_basic += NLA_ALIGN(attr[k]->nla_len);
				break;
			}
		}
	}
	paplist = (struct nlattr *)((UINT_8 *) data + len_basic);
	printk("+++basic attribute size=%d flush=%d\r\n", len_basic, flush);

	if (paplist->nla_type == GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_BSSIDS)
		paplist = (struct nlattr *)((UINT_8 *) paplist + NLA_HDRLEN);

	for (i = 0; i < rWifiChangeCmd.num_ap; i++) {
		nla_parse_nested(attr, GSCAN_ATTRIBUTE_RSSI_HIGH, (struct nlattr *)paplist, policy);
		paplist = (struct nlattr *)((UINT_8 *) paplist + NLA_HDRLEN);
		/* request.attr_start(i) as nested attribute */
		len_aplist = 0;
		for (k = GSCAN_ATTRIBUTE_BSSID; k <= GSCAN_ATTRIBUTE_RSSI_HIGH; k++) {
			if (attr[k]) {
				switch (k) {
				case GSCAN_ATTRIBUTE_BSSID:
					kalMemCopy(rWifiChangeCmd.ap[i].bssid, nla_data(attr[k]), sizeof(mac_addr));
					len_aplist += NLA_ALIGN(attr[k]->nla_len);
					break;
				case GSCAN_ATTRIBUTE_RSSI_LOW:
					rWifiChangeCmd.ap[i].low = nla_get_u32(attr[k]);
					len_aplist += NLA_ALIGN(attr[k]->nla_len);
					break;
				case GSCAN_ATTRIBUTE_RSSI_HIGH:
					rWifiChangeCmd.ap[i].high = nla_get_u32(attr[k]);
					len_aplist += NLA_ALIGN(attr[k]->nla_len);
					break;
				}
			}
		}
		if (((i + 1) % 4 == 0) || (i == rWifiChangeCmd.num_ap - 1))
			printk("ap[%d], len_aplist=%d\n", i, len_aplist);
		else
			printk("ap[%d], len_aplist=%d \t", i, len_aplist);
		paplist = (struct nlattr *)((UINT_8 *) paplist + len_aplist);
	}

	printk("flush=%d, rssi_sample_size=%d lost_ap_sample_size=%d min_breaching=%d,"
		"ap[0].channel=%d low=%d high=%d, ap[1].channel=%d low=%d high=%d", flush,
		rWifiChangeCmd.rssi_sample_size, rWifiChangeCmd.lost_ap_sample_size, rWifiChangeCmd.min_breaching,
		rWifiChangeCmd.ap[0].channel, rWifiChangeCmd.ap[0].low, rWifiChangeCmd.ap[0].high,
		rWifiChangeCmd.ap[1].channel, rWifiChangeCmd.ap[1].low, rWifiChangeCmd.ap[1].high);

	return 0;

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_vendor_set_hotlist(struct wiphy *wiphy, struct wireless_dev *wdev, const void *data, int data_len)
{
	/*WLAN_STATUS rStatus;*/
	P_GLUE_INFO_T prGlueInfo = NULL;
	CMD_SET_PSCAN_ADD_HOTLIST_BSSID rCmdPscnAddHotlist;

	INT_32 i4Status = -EINVAL;
	PARAM_WIFI_BSSID_HOTLIST rWifiHotlistCmd;
	UINT_8 flush = 0;
	struct nlattr *attr[GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH + 1];
	struct nlattr *paplist;
	int i, k;
	UINT_32 len_basic, len_aplist;
	static struct nla_policy policy[GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH + 1] = {
		[GSCAN_ATTRIBUTE_BSSID] = {.type = NLA_UNSPEC},
		[GSCAN_ATTRIBUTE_RSSI_LOW] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_RSSI_HIGH] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_LOST_AP_SAMPLE_SIZE] = {.type = NLA_U32},
		[GSCAN_ATTRIBUTE_NUM_AP] = {.type = NLA_U16},
		[GSCAN_ATTRIBUTE_HOTLIST_FLUSH] = {.type = NLA_U8},
	};

	ASSERT(wiphy);
	ASSERT(wdev);
	if ((data == NULL) || !data_len)
		goto nla_put_failure;
	printk("%s for vendor command: data_len=%d \r\n", __func__, data_len);
	for (i = 0; i < 5; i++)
		printk("0x%x 0x%x 0x%x 0x%x \r\n", *((UINT_32 *) data + i * 4), *((UINT_32 *) data + i * 4 + 1),
		       *((UINT_32 *) data + i * 4 + 2), *((UINT_32 *) data + i * 4 + 3));
	kalMemZero(&rWifiHotlistCmd, sizeof(rWifiHotlistCmd));
	kalMemZero(attr, sizeof(struct nlattr *) * (GSCAN_ATTRIBUTE_SIGNIFICANT_CHANGE_FLUSH + 1));

	nla_parse_nested(attr, GSCAN_ATTRIBUTE_NUM_AP, (struct nlattr *)(data - NLA_HDRLEN), policy);
	len_basic = 0;
	for (k = GSCAN_ATTRIBUTE_HOTLIST_FLUSH; k <= GSCAN_ATTRIBUTE_NUM_AP; k++) {
		if (attr[k]) {
			switch (k) {
			case GSCAN_ATTRIBUTE_LOST_AP_SAMPLE_SIZE:
				rWifiHotlistCmd.lost_ap_sample_size = nla_get_u32(attr[k]);
				len_basic += NLA_ALIGN(attr[k]->nla_len);
				break;
			case GSCAN_ATTRIBUTE_NUM_AP:
				rWifiHotlistCmd.num_ap = nla_get_u16(attr[k]);
				len_basic += NLA_ALIGN(attr[k]->nla_len);
				printk("attr=0x%x, num_ap=%d nla_len=%d, \r\n",
				       *(UINT_32 *) attr[k], rWifiHotlistCmd.num_ap, attr[k]->nla_len);
				break;
			case GSCAN_ATTRIBUTE_HOTLIST_FLUSH:
				flush = nla_get_u8(attr[k]);
				len_basic += NLA_ALIGN(attr[k]->nla_len);
				break;
			}
		}
	}
	paplist = (struct nlattr *)((UINT_8 *) data + len_basic);
	printk("+++basic attribute size=%d flush=%d\r\n", len_basic, flush);

	if (paplist->nla_type == GSCAN_ATTRIBUTE_HOTLIST_BSSIDS)
		paplist = (struct nlattr *)((UINT_8 *) paplist + NLA_HDRLEN);

	for (i = 0; i < rWifiHotlistCmd.num_ap; i++) {
		nla_parse_nested(attr, GSCAN_ATTRIBUTE_RSSI_HIGH, (struct nlattr *)paplist, policy);
		paplist = (struct nlattr *)((UINT_8 *) paplist + NLA_HDRLEN);
		/* request.attr_start(i) as nested attribute */
		len_aplist = 0;
		for (k = GSCAN_ATTRIBUTE_BSSID; k <= GSCAN_ATTRIBUTE_RSSI_HIGH; k++) {
			if (attr[k]) {
				switch (k) {
				case GSCAN_ATTRIBUTE_BSSID:
					kalMemCopy(rWifiHotlistCmd.ap[i].bssid, nla_data(attr[k]), sizeof(mac_addr));
					len_aplist += NLA_ALIGN(attr[k]->nla_len);
					break;
				case GSCAN_ATTRIBUTE_RSSI_LOW:
					rWifiHotlistCmd.ap[i].low = nla_get_u32(attr[k]);
					len_aplist += NLA_ALIGN(attr[k]->nla_len);
					break;
				case GSCAN_ATTRIBUTE_RSSI_HIGH:
					rWifiHotlistCmd.ap[i].high = nla_get_u32(attr[k]);
					len_aplist += NLA_ALIGN(attr[k]->nla_len);
					break;
				}
			}
		}
		if (((i + 1) % 4 == 0) || (i == rWifiHotlistCmd.num_ap - 1))
			printk("ap[%d], len_aplist=%d\n", i, len_aplist);
		else
			printk("ap[%d], len_aplist=%d \t", i, len_aplist);
		paplist = (struct nlattr *)((UINT_8 *) paplist + len_aplist);
	}

	printk("flush=%d, lost_ap_sample_size=%d, "
		"Hotlist: ap[0].channel=%d low=%d high=%d, ap[1].channel=%d low=%d high=%d",
		flush, rWifiHotlistCmd.lost_ap_sample_size,
		rWifiHotlistCmd.ap[0].channel, rWifiHotlistCmd.ap[0].low, rWifiHotlistCmd.ap[0].high,
		rWifiHotlistCmd.ap[1].channel, rWifiHotlistCmd.ap[1].low, rWifiHotlistCmd.ap[1].high);

	memcpy(&(rCmdPscnAddHotlist.aucMacAddr), &(rWifiHotlistCmd.ap[0].bssid), 6 * sizeof(UINT_8));
	rCmdPscnAddHotlist.ucFlags = (UINT_8) TRUE;
	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	return 0;

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_vendor_enable_scan(struct wiphy *wiphy, struct wireless_dev *wdev, const void *data, int data_len)
{
	WLAN_STATUS rStatus;
	UINT_32 u4BufLen;
	P_GLUE_INFO_T prGlueInfo = NULL;
	PARAM_WIFI_GSCAN_ACTION_CMD_PARAMS rWifiScanActionCmd;

	INT_32 i4Status = -EINVAL;
	struct nlattr *attr;
	UINT_8 gGScanEn = 0;

	static UINT_8 k; /* only for test */

	ASSERT(wiphy);
	ASSERT(wdev);
	if ((data == NULL) || !data_len)
		goto nla_put_failure;
	printk("%s for vendor command: data_len=%d, data=0x%x 0x%x\r\n",
	       __func__, data_len, *((UINT_32 *) data), *((UINT_32 *) data + 1));

	attr = (struct nlattr *)data;
	if (attr->nla_type == GSCAN_ATTRIBUTE_ENABLE_FEATURE)
		gGScanEn = nla_get_u32(attr);
	printk("gGScanEn=%d, \r\n", gGScanEn);

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);
	if (gGScanEn == TRUE)
		rWifiScanActionCmd.ucPscanAct = ENABLE;
	else
		rWifiScanActionCmd.ucPscanAct = DISABLE;

	rStatus = kalIoctl(prGlueInfo,
			   wlanoidSetGSCNAction,
			   &rWifiScanActionCmd,
			   sizeof(PARAM_WIFI_GSCAN_ACTION_CMD_PARAMS), FALSE, FALSE, TRUE, FALSE, &u4BufLen);

	/* mtk_cfg80211_vendor_get_scan_results(wiphy, wdev, data, data_len ); */

	return 0;

	/* only for test */
	if (k % 3 == 1) {
		mtk_cfg80211_vendor_event_significant_change_results(wiphy, wdev, NULL, 0);
		mtk_cfg80211_vendor_event_hotlist_ap_found(wiphy, wdev, NULL, 0);
		mtk_cfg80211_vendor_event_hotlist_ap_lost(wiphy, wdev, NULL, 0);
	}
	k++;

	return 0;

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_vendor_enable_full_scan_results(struct wiphy *wiphy, struct wireless_dev *wdev,
						 const void *data, int data_len)
{
	INT_32 i4Status = -EINVAL;
	struct nlattr *attr;
	UINT_8 gFullScanResultsEn = 0;

	ASSERT(wiphy);
	ASSERT(wdev);
	if ((data == NULL) || !data_len)
		goto nla_put_failure;
	printk("%s for vendor command: data_len=%d, data=0x%x 0x%x\r\n",
	       __func__, data_len, *((UINT_32 *) data), *((UINT_32 *) data + 1));

	attr = (struct nlattr *)data;
	if (attr->nla_type == GSCAN_ENABLE_FULL_SCAN_RESULTS)
		gFullScanResultsEn = nla_get_u32(attr);
	printk("gFullScanResultsEn=%d, \r\n", gFullScanResultsEn);

	return 0;

	/* only for test */
	mtk_cfg80211_vendor_event_complete_scan(wiphy, wdev, WIFI_SCAN_COMPLETE);
	mtk_cfg80211_vendor_event_scan_results_avaliable(wiphy, wdev, 4);
	if (gFullScanResultsEn == TRUE)
		mtk_cfg80211_vendor_event_full_scan_results(wiphy, wdev, NULL, 0);

	return 0;

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_vendor_get_scan_results(struct wiphy *wiphy, struct wireless_dev *wdev, const void *data, int data_len)
{
	/*WLAN_STATUS rStatus;*/
	UINT_32 u4BufLen;
	P_GLUE_INFO_T prGlueInfo = NULL;
	PARAM_WIFI_GSCAN_GET_RESULT_PARAMS rGSscnResultParm;

	INT_32 i4Status = -EINVAL;
	struct nlattr *attr;
	UINT_32 get_num = 0, real_num = 0;
	UINT_8 flush = 0;
	/*PARAM_WIFI_GSCAN_RESULT result[4], *pResult;
	struct sk_buff *skb;*/
	int i; /*int j;*/
	/*UINT_32 scan_id;*/

	ASSERT(wiphy);
	ASSERT(wdev);
	if ((data == NULL) || !data_len)
		goto nla_put_failure;
	printk("%s for vendor command: data_len=%d \r\n", __func__, data_len);
	for (i = 0; i < 2; i++)
		printk("0x%x 0x%x 0x%x 0x%x \r\n", *((UINT_32 *) data + i * 4), *((UINT_32 *) data + i * 4 + 1),
		       *((UINT_32 *) data + i * 4 + 2), *((UINT_32 *) data + i * 4 + 3));

	attr = (struct nlattr *)data;
	if (attr->nla_type == GSCAN_ATTRIBUTE_NUM_OF_RESULTS) {
		get_num = nla_get_u32(attr);
		attr = (struct nlattr *)((UINT_8 *) attr + attr->nla_len);
	}
	if (attr->nla_type == GSCAN_ATTRIBUTE_FLUSH_RESULTS) {
		flush = nla_get_u8(attr);
		attr = (struct nlattr *)((UINT_8 *) attr + attr->nla_len);
	}
	printk("number=%d, flush=%d \r\n", get_num, flush);

	prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
	ASSERT(prGlueInfo);

	real_num = (get_num < PSCAN_MAX_SCAN_CACHE_SIZE) ? get_num : PSCAN_MAX_SCAN_CACHE_SIZE;
	get_num = real_num;

#if 0	/* driver buffer FW results and reports by buffer workaround for FW mismatch with hal results numbers */
	g_GetResultsCmdCnt++;
	DBGLOG(INIT, INFO,
	       ("(g_GetResultsCmdCnt [%d], g_GetResultsBufferedCnt [%d]\n", g_GetResultsCmdCnt,
		g_GetResultsBufferedCnt));

	BOOLEAN fgIsGetResultFromBuffer = FALSE;
	UINT_8 BufferedResultReportIndex = 0;
	if (g_GetResultsBufferedCnt > 0) {

		DBGLOG(INIT, INFO,
		       ("(g_GetResultsBufferedCnt > 0), report buffered results instead of ask from FW\n"));

		/* reply the results to wifi_hal  */
		for (i = 0; i < MAX_BUFFERED_GSCN_RESULTS; i++) {

			if (g_arGscanResultsIndicateNumber[i] > 0) {
				real_num = g_arGscanResultsIndicateNumber[i];
				get_num = real_num;
				g_arGscanResultsIndicateNumber[i] = 0;
				fgIsGetResultFromBuffer = TRUE;
				BufferedResultReportIndex = i;
				break;
			}
		}
		if (i == MAX_BUFFERED_GSCN_RESULTS)
			printk("all buffered results are invalid, unexpected case \r\n");
		printk("BufferedResultReportIndex[%d] i = %d real_num[%d] get_num[%d] \r\n", BufferedResultReportIndex,
		       i, real_num, get_num);
	}
#endif

	rGSscnResultParm.get_num = get_num;
	rGSscnResultParm.flush = flush;
#if 0/* //driver buffer FW results and reports by buffer workaround for FW results mismatch with hal results number */
	if (fgIsGetResultFromBuffer) {
		nicRxProcessGSCNEvent(prGlueInfo->prAdapter, g_arGscnResultsTempBuffer[BufferedResultReportIndex]);
		g_GetResultsBufferedCnt--;
		g_GetResultsCmdCnt--;
		nicRxReturnRFB(prGlueInfo->prAdapter, g_arGscnResultsTempBuffer[BufferedResultReportIndex]);
	} else
#endif
	{
		DBGLOG(INIT, INFO, ("no buffered gscn results, ask form FW\n"));
		kalIoctl(prGlueInfo,
			 wlanoidGetGSCNResult,
			 &rGSscnResultParm,
			 sizeof(PARAM_WIFI_GSCAN_GET_RESULT_PARAMS), FALSE, FALSE, TRUE, FALSE, &u4BufLen);
	}
	return 0;

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_vendor_get_channel_list(struct wiphy *wiphy, struct wireless_dev *wdev, const void *data, int data_len)
{
	INT_32 i4Status = -EINVAL;
	struct nlattr *attr;
	UINT_32 band = 0;
	UINT_32 num_channel;
	wifi_channel channels[4];
	struct sk_buff *skb;

	ASSERT(wiphy);
	ASSERT(wdev);
	if ((data == NULL) || !data_len)
		goto nla_put_failure;
	printk("%s for vendor command: data_len=%d \r\n", __func__, data_len);

	attr = (struct nlattr *)data;
	if (attr->nla_type == GSCAN_ATTRIBUTE_BAND)
		band = nla_get_u32(attr);
	printk("get channel list: band=%d \r\n", band);

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, sizeof(wifi_channel) * 4);
	if (!skb) {
		DBGLOG(INIT, TRACE, ("%s allocate skb failed:%x\n", __func__, i4Status));
		return -ENOMEM;
	}

	kalMemZero(channels, sizeof(wifi_channel) * 4);
	/*rStatus = kalIoctl(prGlueInfo,
	   wlanoidQueryStatistics,
	   &channel,
	   sizeof(channel),
	   TRUE,
	   TRUE,
	   TRUE,
	   FALSE,
	   &u4BufLen); */

	/* only for test */
	num_channel = 3;
	channels[0] = 2412;
	channels[1] = 2413;
	channels[2] = 2414;
	NLA_PUT_U32(skb, GSCAN_ATTRIBUTE_NUM_CHANNELS, num_channel);
	NLA_PUT(skb, GSCAN_ATTRIBUTE_CHANNEL_LIST, (sizeof(wifi_channel) * num_channel), channels);

	i4Status = cfg80211_vendor_cmd_reply(skb);

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_vendor_llstats_get_info(struct wiphy *wiphy, struct wireless_dev *wdev, const void *data, int data_len)
{
	INT_32 i4Status = -EINVAL;
	WIFI_RADIO_STAT *pRadioStat;
	struct sk_buff *skb;
	UINT_32 u4BufLen;

	printk("%s for vendor command \r\n", __func__);

	ASSERT(wiphy);
	ASSERT(wdev);

	u4BufLen = sizeof(WIFI_RADIO_STAT) + sizeof(WIFI_IFACE_STAT);
	pRadioStat = kalMemAlloc(u4BufLen, VIR_MEM_TYPE);
	kalMemZero(pRadioStat, u4BufLen);

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, u4BufLen);
	if (!skb) {
		DBGLOG(INIT, TRACE, ("%s allocate skb failed:%x\n", __func__, i4Status));
		return -ENOMEM;
	}

	/*rStatus = kalIoctl(prGlueInfo,
	   wlanoidQueryStatistics,
	   &rRadioStat,
	   sizeof(rRadioStat),
	   TRUE,
	   TRUE,
	   TRUE,
	   FALSE,
	   &u4BufLen); */
	/* only for test */
	pRadioStat->radio = 10;
	pRadioStat->on_time = 11;
	pRadioStat->tx_time = 12;
	pRadioStat->num_channels = 4;

	NLA_PUT(skb, NL80211_ATTR_VENDOR_LLSTAT, u4BufLen, pRadioStat);

	i4Status = cfg80211_vendor_cmd_reply(skb);

	kalMemFree(pRadioStat, VIR_MEM_TYPE, u4BufLen);

nla_put_failure:
	return i4Status;
}

int mtk_cfg80211_vendor_event_complete_scan(struct wiphy *wiphy, struct wireless_dev *wdev, WIFI_SCAN_EVENT complete)
{
	struct sk_buff *skb;
	ASSERT(wiphy);
	ASSERT(wdev);
	/* WIFI_SCAN_EVENT complete_scan; */

	printk("%s for vendor command \r\n", __func__);

	skb = cfg80211_vendor_event_alloc(wiphy, sizeof(complete), GSCAN_EVENT_COMPLETE_SCAN, GFP_KERNEL);
	if (!skb) {
		DBGLOG(INIT, TRACE, ("%s allocate skb failed\n", __func__));
		return -ENOMEM;
	}
	/* complete_scan = WIFI_SCAN_COMPLETE; */
	NLA_PUT_U32(skb, GSCAN_EVENT_COMPLETE_SCAN, complete);

	cfg80211_vendor_event(skb, GFP_KERNEL);
	return 0;

nla_put_failure:
	return -1;
}

int mtk_cfg80211_vendor_event_scan_results_avaliable(struct wiphy *wiphy, struct wireless_dev *wdev, UINT_32 num)
{
	struct sk_buff *skb;
	ASSERT(wiphy);
	ASSERT(wdev);
	/* UINT_32 scan_result; */

	printk("%s for vendor command %d  \r\n", __func__, num);

	skb = cfg80211_vendor_event_alloc(wiphy, sizeof(num), GSCAN_EVENT_SCAN_RESULTS_AVAILABLE, GFP_KERNEL);
	if (!skb) {
		DBGLOG(INIT, TRACE, ("%s allocate skb failed\n", __func__));
		return -ENOMEM;
	}
	/* scan_result = 2; */
	NLA_PUT_U32(skb, GSCAN_EVENT_SCAN_RESULTS_AVAILABLE, num);

	cfg80211_vendor_event(skb, GFP_KERNEL);
	return 0;

nla_put_failure:
	return -1;
}

int mtk_cfg80211_vendor_event_full_scan_results(struct wiphy *wiphy, struct wireless_dev *wdev,
						P_PARAM_WIFI_GSCAN_RESULT pdata, UINT_32 data_len)
{
	struct sk_buff *skb;
	PARAM_WIFI_GSCAN_RESULT result;

	ASSERT(wiphy);
	ASSERT(wdev);
	printk("%s for vendor command \r\n", __func__);

	skb = cfg80211_vendor_event_alloc(wiphy, sizeof(result), GSCAN_EVENT_FULL_SCAN_RESULTS, GFP_KERNEL);

#if 1
	if (!skb) {
		DBGLOG(INIT, TRACE, ("%s allocate skb failed\n", __func__));
		return -ENOMEM;
	}

	kalMemZero(&result, sizeof(result));
	kalMemCopy(result.ssid, "Gscan_full_test", sizeof("Gscan_full_test"));
	result.channel = 2437;
#endif

	/* kalMemCopy(&result, pdata, sizeof(PARAM_WIFI_GSCAN_RESULT); */
	NLA_PUT(skb, GSCAN_EVENT_FULL_SCAN_RESULTS, sizeof(result), &result);

	cfg80211_vendor_event(skb, GFP_KERNEL);
	return 0;

nla_put_failure:
	return -1;
}

int mtk_cfg80211_vendor_event_significant_change_results(struct wiphy *wiphy, struct wireless_dev *wdev,
							 P_PARAM_WIFI_CHANGE_RESULT pdata, UINT_32 data_len)
{
	struct sk_buff *skb;
	PARAM_WIFI_CHANGE_RESULT result[2], *presult;

	ASSERT(wiphy);
	ASSERT(wdev);
	printk("%s for vendor command \r\n", __func__);

	skb = cfg80211_vendor_event_alloc(wiphy, sizeof(PARAM_WIFI_CHANGE_RESULT),
					  GSCAN_EVENT_SIGNIFICANT_CHANGE_RESULTS, GFP_KERNEL);
	if (!skb) {
		DBGLOG(INIT, TRACE, ("%s allocate skb failed\n", __func__));
		return -ENOMEM;
	}

	presult = result;
	kalMemZero(presult, (sizeof(PARAM_WIFI_CHANGE_RESULT) * 2));
	/* only for test */
	kalMemCopy(presult->bssid, "213123", sizeof(mac_addr));
	presult->channel = 2437;
	presult->rssi[0] = -45;
	presult->rssi[1] = -46;
	presult++;
	presult->channel = 2439;
	presult->rssi[0] = -47;
	presult->rssi[1] = -48;
	NLA_PUT(skb, GSCAN_EVENT_SIGNIFICANT_CHANGE_RESULTS, (sizeof(PARAM_WIFI_CHANGE_RESULT) * 2), result);

	cfg80211_vendor_event(skb, GFP_KERNEL);
	return 0;

nla_put_failure:
	return -1;
}

int mtk_cfg80211_vendor_event_hotlist_ap_found(struct wiphy *wiphy, struct wireless_dev *wdev,
					       P_PARAM_WIFI_GSCAN_RESULT pdata, UINT_32 data_len)
{
	struct sk_buff *skb;
	PARAM_WIFI_GSCAN_RESULT result[2], *presult;

	ASSERT(wiphy);
	ASSERT(wdev);
	printk("%s for vendor command \r\n", __func__);

	skb = cfg80211_vendor_event_alloc(wiphy, sizeof(PARAM_WIFI_GSCAN_RESULT),
					  GSCAN_EVENT_HOTLIST_RESULTS_FOUND, GFP_KERNEL);
	if (!skb) {
		DBGLOG(INIT, TRACE, ("%s allocate skb failed\n", __func__));
		return -ENOMEM;
	}

	presult = result;
	kalMemZero(presult, (sizeof(PARAM_WIFI_GSCAN_RESULT) * 2));
	/* only for test */
	kalMemCopy(presult->bssid, "123123", sizeof(mac_addr));
	presult->channel = 2441;
	presult->rssi = -45;
	presult++;
	presult->channel = 2443;
	presult->rssi = -47;
	NLA_PUT(skb, GSCAN_EVENT_HOTLIST_RESULTS_FOUND, (sizeof(PARAM_WIFI_GSCAN_RESULT) * 2), result);

	cfg80211_vendor_event(skb, GFP_KERNEL);
	return 0;

nla_put_failure:
	return -1;
}

int mtk_cfg80211_vendor_event_hotlist_ap_lost(struct wiphy *wiphy, struct wireless_dev *wdev,
					      P_PARAM_WIFI_GSCAN_RESULT pdata, UINT_32 data_len)
{
	struct sk_buff *skb;
	PARAM_WIFI_GSCAN_RESULT result[2], *presult;

	ASSERT(wiphy);
	ASSERT(wdev);
	printk("%s for vendor command \r\n", __func__);

	skb = cfg80211_vendor_event_alloc(wiphy, sizeof(PARAM_WIFI_GSCAN_RESULT),
					  GSCAN_EVENT_HOTLIST_RESULTS_LOST, GFP_KERNEL);
	if (!skb) {
		DBGLOG(INIT, TRACE, ("%s allocate skb failed\n", __func__));
		return -ENOMEM;
	}

	presult = result;
	kalMemZero(presult, (sizeof(PARAM_WIFI_GSCAN_RESULT) * 2));
	/* only for test */
	kalMemCopy(presult->bssid, "321321", sizeof(mac_addr));
	presult->channel = 2445;
	presult->rssi = -46;
	presult++;
	presult->channel = 2447;
	presult->rssi = -48;
	NLA_PUT(skb, GSCAN_EVENT_HOTLIST_RESULTS_LOST, (sizeof(PARAM_WIFI_GSCAN_RESULT) * 2), result);

	cfg80211_vendor_event(skb, GFP_KERNEL);
	return 0;

nla_put_failure:
	return -1;
}

#if CFG_AUTO_CHANNEL_SEL_SUPPORT
int
mtk_cfg80211_testmode_get_lte_channel(IN struct wiphy *wiphy, IN void *data, IN int len, IN P_GLUE_INFO_T prGlueInfo)
{
#define MAXMUN_2_4G_CHA_NUM 14
#define CHN_DIRTY_WEIGHT_UPPERBOUND 4

	BOOLEAN fgIsReady = FALSE, fgIsFistRecord = TRUE;
	BOOLEAN fgIsPureAP, fgIsLteSafeChn = FALSE;

	WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;
	UINT_8 ucIdx = 0, ucMax_24G_Chn_List = 11, ucDefaultIdx = 0, ucArrayIdx = 0;
	UINT_16 u2APNumScore = 0, u2UpThreshold = 0, u2LowThreshold = 0, ucInnerIdx = 0;
	INT_32 i4Status = -EINVAL;
	UINT_32 u4BufLen, u4LteSafeChnBitMask_2_4G = 0;
	UINT32 AcsChnReport[4];
	/*RF_CHANNEL_INFO_T aucChannelList[MAXMUN_2_4G_CHA_NUM];*/

	struct sk_buff *skb;

	PARAM_GET_CHN_LOAD rQueryLTEChn;
	PARAM_PREFER_CHN_INFO rPreferChannels[2], ar2_4G_ChannelLoadingWeightScore[MAXMUN_2_4G_CHA_NUM];
	P_PARAM_CHN_LOAD_INFO prChnLoad;
	P_PARAM_GET_CHN_LOAD prGetChnLoad;

	P_DOMAIN_INFO_ENTRY prDomainInfo;

/*
	 P_PARAM_GET_CHN_LOAD prParams = NULL;
*/
	ASSERT(wiphy);
	ASSERT(prGlueInfo);

	kalMemZero(rPreferChannels, sizeof(rPreferChannels));
	fgIsPureAP = prGlueInfo->prAdapter->rWifiVar.prP2pFsmInfo->fgIsApMode;
#if 0
	if (data && len)
		prParams = (P_NL80211_DRIVER_GET_LTE_PARAMS) data;
#endif
	skb = cfg80211_testmode_alloc_reply_skb(wiphy, sizeof(AcsChnReport) + sizeof(UINT8) + 1);
	if (!skb) {
		DBGLOG(QM, TRACE, ("%s allocate skb failed:%x\n", __func__, rStatus));
		return -ENOMEM;
	}

	DBGLOG(P2P, INFO, ("[Auto Channel]Get LTE Channels\n"));

	kalMemZero(&rQueryLTEChn, sizeof(rQueryLTEChn));

	/* Query LTE Safe Channels */
	rQueryLTEChn.rLteSafeChnList.au4SafeChannelBitmask[NL80211_TESTMODE_AVAILABLE_CHAN_2G_BASE_1 - 1] = 0xFFFFFFFF;

	rQueryLTEChn.rLteSafeChnList.au4SafeChannelBitmask[NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_34 - 1] = 0xFFFFFFFF;

	rQueryLTEChn.rLteSafeChnList.au4SafeChannelBitmask[NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_149 - 1] =
	    0xFFFFFFFF;

	rQueryLTEChn.rLteSafeChnList.au4SafeChannelBitmask[NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_184 - 1] =
	    0xFFFFFFFF;
	rStatus =
	    kalIoctl(prGlueInfo, wlanoidQueryACSChannelList, &rQueryLTEChn, sizeof(rQueryLTEChn), TRUE, FALSE, TRUE,
		     TRUE, &u4BufLen);
#if 0
	if (fgIsPureAP) {

		AcsChnRepot[NL80211_TESTMODE_AVAILABLE_CHAN_2G_BASE_1 - 1] = 0x20;	/* Channel 6 */
	} else
#endif
	{
		fgIsReady = prGlueInfo->prAdapter->rWifiVar.rChnLoadInfo.fgDataReadyBit;
		rPreferChannels[0].u2APNum = 0xFFFF;
		rPreferChannels[1].u2APNum = 0xFFFF;

		/* 4 In LTE Mode, Hotspot pick up channels from ch4. */
		ucDefaultIdx = 0;
		/*
		   if (fgIsPureAP) {
		   ucDefaultIdx=3; //SKIP LTE Channels 1~3
		   }
		 */

		/* 4 Get the Maximun channel List in 2.4G Bands */

		prDomainInfo = rlmDomainGetDomainInfo(prGlueInfo->prAdapter);
		ASSERT(prDomainInfo);

		/* 4 ToDo: Enable Step 2 only if we could get Country Code from framework */
		/* 4 2. Get current domain channel list */

#if 0
		rlmDomainGetChnlList(prGlueInfo->prAdapter,
				     BAND_2G4, MAXMUN_2_4G_CHA_NUM, &ucMax_24G_Chn_List, aucChannelList);
#endif

		prGetChnLoad = (P_PARAM_GET_CHN_LOAD) &(prGlueInfo->prAdapter->rWifiVar.rChnLoadInfo);
		for (ucIdx = 0; ucIdx < ucMax_24G_Chn_List; ucIdx++) {
			DBGLOG(P2P, INFO,
			       ("[Auto Channel] ch[%d]=%d\n", ucIdx,
				prGetChnLoad->rEachChnLoad[ucIdx + ucInnerIdx].u2APNum));
		}

		/*Calculate Each Channel Direty Score */
		for (ucIdx = ucDefaultIdx; ucIdx < ucMax_24G_Chn_List; ucIdx++) {

#if 1
			u2APNumScore = prGetChnLoad->rEachChnLoad[ucIdx].u2APNum * CHN_DIRTY_WEIGHT_UPPERBOUND;
			u2UpThreshold = u2LowThreshold = 3;

			if (ucIdx < 3) {
				u2UpThreshold = ucIdx;
				u2LowThreshold = 3;
			} else if (ucIdx >= (ucMax_24G_Chn_List - 3)) {
				u2UpThreshold = 3;
				u2LowThreshold = ucMax_24G_Chn_List - (ucIdx + 1);

			}

			/*Calculate Lower Channel Dirty Score */
			for (ucInnerIdx = 0; ucInnerIdx < u2LowThreshold; ucInnerIdx++) {
				ucArrayIdx = ucIdx + ucInnerIdx + 1;
				if ((ucArrayIdx >= 0) && (ucArrayIdx < MAX_AUTO_CHAL_NUM)) {
					u2APNumScore +=
					    (prGetChnLoad->rEachChnLoad[ucArrayIdx].u2APNum *
					     (CHN_DIRTY_WEIGHT_UPPERBOUND - 1 - ucInnerIdx));
				}
			}

			/*Calculate Upper Channel Dirty Score */
			for (ucInnerIdx = 0; ucInnerIdx < u2UpThreshold; ucInnerIdx++) {
				ucArrayIdx = ucIdx - ucInnerIdx - 1;
				if ((ucArrayIdx >= 0) && (ucArrayIdx < MAX_AUTO_CHAL_NUM)) {
					u2APNumScore +=
					    (prGetChnLoad->rEachChnLoad[ucArrayIdx].u2APNum *
					     (CHN_DIRTY_WEIGHT_UPPERBOUND - 1 - ucInnerIdx));
				}
			}

			ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum = u2APNumScore;

			DBGLOG(P2P, INFO, ("[Auto Channel]chn=%d score=%d\n", ucIdx, u2APNumScore));
#else
			if (ucIdx == 0) {
				/* ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum =
				(prGetChnLoad->rEachChnLoad[ucIdx].u2APNum +
				prGetChnLoad->rEachChnLoad[ucIdx+1].u2APNum*0.75); */
				u2APNumScore = (prGetChnLoad->rEachChnLoad[ucIdx].u2APNum + ((UINT_16)
											     ((3 *
											       (prGetChnLoad->
												rEachChnLoad[ucIdx +
													     1].
												u2APNum +
												prGetChnLoad->
												rEachChnLoad[ucIdx +
													     2].
												u2APNum)) / 4)));

				ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum = u2APNumScore;
				DBGLOG(P2P, INFO,
				       ("[Auto Channel]ucIdx=%d score=%d=%d+0.75*%d\n", ucIdx,
					ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum,
					prGetChnLoad->rEachChnLoad[ucIdx].u2APNum,
					prGetChnLoad->rEachChnLoad[ucIdx + 1].u2APNum));
			}
			if ((ucIdx > 0) && (ucIdx < (MAXMUN_2_4G_CHA_NUM - 1))) {
				u2APNumScore = (prGetChnLoad->rEachChnLoad[ucIdx].u2APNum + ((UINT_16)
											     ((3 *
											       (prGetChnLoad->
												rEachChnLoad[ucIdx +
													     1].
												u2APNum +
												prGetChnLoad->
												rEachChnLoad[ucIdx -
													     1].
												u2APNum)) / 4)));

				ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum = u2APNumScore;
				DBGLOG(P2P, INFO,
				       ("[Auto Channel]ucIdx=%d score=%d=%d+0.75*%d+0.75*%d\n", ucIdx,
					ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum,
					prGetChnLoad->rEachChnLoad[ucIdx].u2APNum,
					prGetChnLoad->rEachChnLoad[ucIdx + 1].u2APNum,
					prGetChnLoad->rEachChnLoad[ucIdx - 1].u2APNum));
			}

			if (ucIdx == (MAXMUN_2_4G_CHA_NUM - 1)) {
				u2APNumScore = (prGetChnLoad->rEachChnLoad[ucIdx].u2APNum +
						((UINT_16) ((3 * prGetChnLoad->rEachChnLoad[ucIdx - 1].u2APNum) / 4)));

				ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum = u2APNumScore;
				DBGLOG(P2P, INFO,
				       ("[Auto Channel]ucIdx=%d score=%d=%d+0.75*%d\n", ucIdx,
					ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum,
					prGetChnLoad->rEachChnLoad[ucIdx].u2APNum,
					prGetChnLoad->rEachChnLoad[ucIdx - 1].u2APNum));
			}
#endif

		}

		u4LteSafeChnBitMask_2_4G =
		    rQueryLTEChn.rLteSafeChnList.au4SafeChannelBitmask[NL80211_TESTMODE_AVAILABLE_CHAN_2G_BASE_1 - 1];

		/*Find out the best channel */
		for (ucIdx = ucDefaultIdx; ucIdx < ucMax_24G_Chn_List; ucIdx++) {
			/* 4 Skip LTE Unsafe Channel */
			fgIsLteSafeChn = ((u4LteSafeChnBitMask_2_4G & BIT(ucIdx + 1)) >> ucIdx);
			if (!fgIsLteSafeChn)
				continue;

			prChnLoad =
			    (P_PARAM_CHN_LOAD_INFO) &(prGlueInfo->prAdapter->rWifiVar.
						       rChnLoadInfo.rEachChnLoad[ucIdx]);
			if (rPreferChannels[0].u2APNum >= ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum) {
				rPreferChannels[1].ucChannel = rPreferChannels[0].ucChannel;
				rPreferChannels[1].u2APNum = rPreferChannels[0].u2APNum;

				rPreferChannels[0].ucChannel = ucIdx;
				rPreferChannels[0].u2APNum = ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum;
			} else {
				if (rPreferChannels[1].u2APNum >= ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum
				    || fgIsFistRecord == 1) {
					fgIsFistRecord = FALSE;
					rPreferChannels[1].ucChannel = ucIdx;
					rPreferChannels[1].u2APNum = ar2_4G_ChannelLoadingWeightScore[ucIdx].u2APNum;
				}
			}
		}
		/* AcsChnRepot[NL80211_TESTMODE_AVAILABLE_CHAN_2G_BASE_1-1] =
		BITS((rQueryLTEChn.rLteSafeChnList.ucChannelLow-1),(rQueryLTEChn.rLteSafeChnList.ucChannelHigh-1)); */
		AcsChnReport[NL80211_TESTMODE_AVAILABLE_CHAN_2G_BASE_1 - 1] = fgIsReady ? BIT(31) : 0;
		AcsChnReport[NL80211_TESTMODE_AVAILABLE_CHAN_2G_BASE_1 - 1] |= BIT(rPreferChannels[0].ucChannel);
	}

	/* ToDo: Support 5G Channel Selection */
	AcsChnReport[NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_34 - 1] = 0x11223344;
	AcsChnReport[NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_149 - 1] = 0x55667788;
	AcsChnReport[NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_184 - 1] = 0x99AABBCC;

	NLA_PUT_U8(skb, NL80211_TESTMODE_AVAILABLE_CHAN_INVALID, 0);
	NLA_PUT_U32(skb, NL80211_TESTMODE_AVAILABLE_CHAN_2G_BASE_1,
		    AcsChnReport[NL80211_TESTMODE_AVAILABLE_CHAN_2G_BASE_1 - 1]);
	NLA_PUT_U32(skb, NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_34,
		    AcsChnReport[NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_34 - 1]);
	NLA_PUT_U32(skb, NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_149,
		    AcsChnReport[NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_149 - 1]);
	NLA_PUT_U32(skb, NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_184,
		    AcsChnReport[NL80211_TESTMODE_AVAILABLE_CHAN_5G_BASE_184 - 1]);

	DBGLOG(P2P, INFO,
	       ("[Auto Channel]Relpy AcsChanInfo[%x:%x:%x:%x]\n", AcsChnReport[0], AcsChnReport[1], AcsChnReport[2],
		AcsChnReport[3]));

	i4Status = cfg80211_testmode_reply(skb);

nla_put_failure:
	return i4Status;

}
#endif
#endif
