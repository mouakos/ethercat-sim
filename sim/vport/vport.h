/*
 * vport.h — EtherCAT virtual port driver declarations (NDIS 6.85, Windows 11)
 *
 * Two virtual NICs ("EtherCAT vPort Master" / "EtherCAT vPort Slave") are
 * linked as a virtual crossover cable: frames sent on either NIC arrive as
 * receives on the other.
 *
 * Intended pairing:
 *   NIC-A (VPORT_ROLE_MASTER) — TwinCAT 3 installs its RT-Ethernet driver here
 *   NIC-B (VPORT_ROLE_SLAVE)  — ec-core: --serve \Device\NPF_{GUID-of-NIC-B}
 */

#pragma once

#include <ntddk.h>
#include <ndis.h>
#include <wdmsec.h>

/* ── Build identity ─────────────────────────────────────────────────────────── */

#define VPORT_TAG               'TROP'      /* 'PORT' as little-endian pool tag */
#define VPORT_VENDOR_DESC       "EtherCAT-Sim Virtual Port"
#define VPORT_VENDOR_ID         0x02ECCAFFu /* matches OUI of our synthetic MACs */

/* ── Physical constants ─────────────────────────────────────────────────────── */

#define VPORT_MTU               1500u
#define VPORT_MAX_FRAME_SIZE    1514u       /* MTU + 14-byte Ethernet header     */
#define VPORT_LINK_SPEED_BPS    1000000000ULL  /* reported 1 Gbit/s              */

/* ── Adapter roles ──────────────────────────────────────────────────────────── */

#define VPORT_ROLE_MASTER   0u
#define VPORT_ROLE_SLAVE    1u
#define VPORT_ROLE_COUNT    2u

/*
 * Hardcoded MAC addresses — Locally Administered Address (LAA) range.
 * Second nibble of first byte = 2 → unicast + locally administered.
 * OUI 02:EC:CA is synthetic and will never conflict with an IEEE-assigned vendor.
 */
static const UCHAR kVportMac[VPORT_ROLE_COUNT][6] = {
    { 0x02, 0xEC, 0xCA, 0x00, 0x00, 0x01 },  /* Master (NIC-A) */
    { 0x02, 0xEC, 0xCA, 0x00, 0x00, 0x02 },  /* Slave  (NIC-B) */
};

/* ── Per-adapter context ────────────────────────────────────────────────────── */

typedef struct _VPORT_ADAPTER {
    NDIS_HANDLE     MiniportHandle;
    NDIS_HANDLE     NblPool;        /* pool for cloned receive NBLs              */
    ULONG           Role;           /* VPORT_ROLE_MASTER or VPORT_ROLE_SLAVE     */
    BOOLEAN         Paused;         /* TRUE while NDIS has paused this adapter   */
    ULONG           PacketFilter;   /* current OID_GEN_CURRENT_PACKET_FILTER val */

    /* simple statistics (64-bit, interlocked) */
    LONGLONG        XmitOk;
    LONGLONG        RcvOk;
    LONGLONG        XmitErr;
    LONGLONG        RcvErr;
} VPORT_ADAPTER, *PVPORT_ADAPTER;

/*
 * Context embedded in NET_BUFFER_LIST_MINIPORT_RESERVED for cloned receive NBLs.
 * NET_BUFFER_LIST_MINIPORT_RESERVED is PVOID[2] = 16 bytes on x64 — fits exactly.
 */
typedef struct _VPORT_RX_CTX {
    PVOID   Data;       /* heap buffer holding the copied frame bytes */
    PMDL    Mdl;        /* MDL describing Data (freed in ReturnNetBufferLists) */
} VPORT_RX_CTX, *PVPORT_RX_CTX;

C_ASSERT(sizeof(VPORT_RX_CTX) <= sizeof(((PNET_BUFFER_LIST)0)->MiniportReserved));

/* ── Global pair ────────────────────────────────────────────────────────────── */

typedef struct _VPORT_PAIR {
    PVPORT_ADAPTER  Adapter[VPORT_ROLE_COUNT]; /* [0]=master [1]=slave          */
    KSPIN_LOCK      Lock;       /* guards Adapter[] during init / halt           */
} VPORT_PAIR, *PVPORT_PAIR;

/* Declared in vport.c; visible to all TUs in the driver. */
extern VPORT_PAIR    g_Pair;
extern NDIS_HANDLE   g_DriverHandle;

/* ── Prototypes ─────────────────────────────────────────────────────────────── */

/* DriverEntry / unload */
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     VportDriverUnload;

/* NDIS miniport characteristics */
NDIS_MINIPORT_INITIALIZE                VportInitialize;
NDIS_MINIPORT_HALT                      VportHalt;
NDIS_MINIPORT_PAUSE                     VportPause;
NDIS_MINIPORT_RESTART                   VportRestart;
NDIS_MINIPORT_OID_REQUEST               VportOidRequest;
NDIS_MINIPORT_CANCEL_OID_REQUEST        VportCancelOidRequest;
NDIS_MINIPORT_SEND_NET_BUFFER_LISTS     VportSendNetBufferLists;
NDIS_MINIPORT_RETURN_NET_BUFFER_LISTS   VportReturnNetBufferLists;
NDIS_MINIPORT_CANCEL_SEND               VportCancelSend;
NDIS_MINIPORT_DEVICE_PNP_EVENT_NOTIFY   VportDevicePnpEventNotify;
NDIS_MINIPORT_SHUTDOWN                  VportShutdownEx;
NDIS_MINIPORT_CHECK_FOR_HANG            VportCheckForHangEx;
