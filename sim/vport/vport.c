/*
 * vport.c — EtherCAT virtual port NDIS 6.85 miniport driver (Windows 11)
 *
 * Creates two virtual Ethernet NICs that act as a virtual crossover cable.
 * The INF installs two device instances (ROOT\ETHERCAT_VPORT_MASTER and
 * ROOT\ETHERCAT_VPORT_SLAVE); each reads its role from the registry key
 * set by the INF (HKR,,Role,0x10001,<N>).
 *
 * Frame path (send on A → receive on B):
 *   VportSendNetBufferLists(A)
 *     → VportIndicateToP eer(B, frame)           clone frame bytes
 *       → NdisMIndicateReceiveNetBufferLists(B)  upper layer sees receive
 *     → NdisMSendNetBufferListsComplete(A)       sender is released
 *   Upper layer done with receive NBL
 *     → VportReturnNetBufferLists(B)             free clone
 */

#include "vport.h"

/* ── Globals ──────────────────────────────────────────────────────────────── */

NDIS_HANDLE  g_DriverHandle = NULL;
VPORT_PAIR   g_Pair;

/* Supported OID list returned by OID_GEN_SUPPORTED_LIST */
static const NDIS_OID kSupportedOids[] = {
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_PHYSICAL_MEDIUM_EX,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_VENDOR_DRIVER_VERSION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_STATISTICS,
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
};

/* ── DriverEntry ─────────────────────────────────────────────────────────── */

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NdisZeroMemory(&g_Pair, sizeof(g_Pair));
    KeInitializeSpinLock(&g_Pair.Lock);

    NDIS_MINIPORT_DRIVER_CHARACTERISTICS chars;
    NdisZeroMemory(&chars, sizeof(chars));

    chars.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    chars.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_3;
    chars.Header.Size     = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_3;

    chars.MajorNdisVersion   = 6;
    chars.MinorNdisVersion   = 85;
    chars.MajorDriverVersion = 1;
    chars.MinorDriverVersion = 0;

    chars.InitializeHandlerEx         = VportInitialize;
    chars.HaltHandlerEx               = VportHalt;
    chars.PauseHandler                = VportPause;
    chars.RestartHandler              = VportRestart;
    chars.OidRequestHandler           = VportOidRequest;
    chars.CancelOidRequestHandler     = VportCancelOidRequest;
    chars.SendNetBufferListsHandler   = VportSendNetBufferLists;
    chars.ReturnNetBufferListsHandler = VportReturnNetBufferLists;
    chars.CancelSendHandler           = VportCancelSend;
    chars.DevicePnpEventNotifyHandler = VportDevicePnpEventNotify;
    chars.ShutdownHandlerEx           = VportShutdownEx;
    chars.CheckForHangHandlerEx       = VportCheckForHangEx;

    NDIS_STATUS status = NdisMRegisterMiniportDriver(
        DriverObject, RegistryPath, NULL, &chars, &g_DriverHandle);
    if (status != NDIS_STATUS_SUCCESS) {
        return (NTSTATUS)status;
    }

    DriverObject->DriverUnload = VportDriverUnload;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID VportDriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    NdisMDeregisterMiniportDriver(g_DriverHandle);
}

/* ── MiniportInitializeEx ─────────────────────────────────────────────────── */

_Use_decl_annotations_
NDIS_STATUS VportInitialize(
    NDIS_HANDLE                     MiniportAdapterHandle,
    NDIS_HANDLE                     MiniportDriverContext,
    PNDIS_MINIPORT_INIT_PARAMETERS  InitParameters)
{
    UNREFERENCED_PARAMETER(MiniportDriverContext);
    UNREFERENCED_PARAMETER(InitParameters);

    /* Read Role from the registry key set by the INF. */
    NDIS_CONFIGURATION_OBJECT cfgObj;
    NdisZeroMemory(&cfgObj, sizeof(cfgObj));
    cfgObj.Header.Type     = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
    cfgObj.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
    cfgObj.Header.Size     = NDIS_SIZEOF_CONFIGURATION_OBJECT_REVISION_1;
    cfgObj.NdisHandle      = MiniportAdapterHandle;

    NDIS_HANDLE hCfg   = NULL;
    NDIS_STATUS status = NdisOpenConfigurationEx(&cfgObj, &hCfg);
    if (status != NDIS_STATUS_SUCCESS) return status;

    NDIS_STRING roleKey = NDIS_STRING_CONST("Role");
    PNDIS_CONFIGURATION_PARAMETER pParam = NULL;
    NdisReadConfiguration(&status, &pParam, hCfg, &roleKey, NdisParameterInteger);
    ULONG role = (status == NDIS_STATUS_SUCCESS && pParam)
                     ? pParam->ParameterData.IntegerData
                     : VPORT_ROLE_MASTER;
    NdisCloseConfiguration(hCfg);

    if (role >= VPORT_ROLE_COUNT) role = VPORT_ROLE_MASTER;

    /* Allocate adapter context. */
    PVPORT_ADAPTER adapter = (PVPORT_ADAPTER)NdisAllocateMemoryWithTagPriority(
        MiniportAdapterHandle, sizeof(VPORT_ADAPTER), VPORT_TAG, NormalPoolPriority);
    if (!adapter) return NDIS_STATUS_RESOURCES;
    NdisZeroMemory(adapter, sizeof(VPORT_ADAPTER));

    adapter->MiniportHandle = MiniportAdapterHandle;
    adapter->Role           = role;
    adapter->Paused         = TRUE; /* NDIS starts adapters in Paused state */

    /* Allocate NBL pool for receive indications (one NB per NBL, external MDL). */
    NET_BUFFER_LIST_POOL_PARAMETERS poolParams;
    NdisZeroMemory(&poolParams, sizeof(poolParams));
    poolParams.Header.Type     = NDIS_OBJECT_TYPE_DEFAULT;
    poolParams.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    poolParams.Header.Size     = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    poolParams.fAllocateNetBuffer = TRUE;
    poolParams.PoolTag            = VPORT_TAG;
    poolParams.DataSize           = 0; /* data lives in our own heap buffer via MDL */

    adapter->NblPool = NdisAllocateNetBufferListPool(MiniportAdapterHandle, &poolParams);
    if (!adapter->NblPool) {
        NdisFreeMemory(adapter, sizeof(VPORT_ADAPTER), 0);
        return NDIS_STATUS_RESOURCES;
    }

    /* ── Registration attributes ── */

    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES regAttribs;
    NdisZeroMemory(&regAttribs, sizeof(regAttribs));
    regAttribs.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    regAttribs.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    regAttribs.Header.Size     = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    regAttribs.MiniportAdapterContext   = adapter;
    regAttribs.AttributeFlags           = NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND |
                                          NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK;
    regAttribs.CheckForHangTimeInSeconds = 4;
    regAttribs.InterfaceType             = NdisInterfaceInternal;

    status = NdisMSetMiniportAttributes(MiniportAdapterHandle,
        (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&regAttribs);
    if (status != NDIS_STATUS_SUCCESS) goto cleanup;

    /* ── General (media / link) attributes ── */

    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES genAttribs;
    NdisZeroMemory(&genAttribs, sizeof(genAttribs));
    genAttribs.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    genAttribs.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    genAttribs.Header.Size     = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    genAttribs.MediaType            = NdisMedium802_3;
    genAttribs.PhysicalMediumType   = NdisPhysicalMediumUnspecified;
    genAttribs.MtuSize              = VPORT_MTU;
    genAttribs.MaxXmitLinkSpeed     = VPORT_LINK_SPEED_BPS;
    genAttribs.MaxRcvLinkSpeed      = VPORT_LINK_SPEED_BPS;
    genAttribs.XmitLinkSpeed        = VPORT_LINK_SPEED_BPS;
    genAttribs.RcvLinkSpeed         = VPORT_LINK_SPEED_BPS;
    genAttribs.MediaConnectState    = MediaConnectStateConnected;
    genAttribs.MediaDuplexState     = MediaDuplexStateFull;
    genAttribs.LookaheadSize        = VPORT_MAX_FRAME_SIZE;
    genAttribs.MaxMulticastListSize = 32;
    genAttribs.MacAddressLength     = 6;
    NdisMoveMemory(genAttribs.PermanentMacAddress, kVportMac[role], 6);
    NdisMoveMemory(genAttribs.CurrentMacAddress,   kVportMac[role], 6);
    genAttribs.AccessType           = NET_IF_ACCESS_BROADCAST;
    genAttribs.DirectionType        = NET_IF_DIRECTION_SENDRECEIVE;
    genAttribs.ConnectionType       = NET_IF_CONNECTION_DEDICATED;
    genAttribs.IfType               = IF_TYPE_ETHERNET_CSMACD;
    genAttribs.IfConnectorPresent   = FALSE; /* no physical connector */
    genAttribs.SupportedStatistics  = NDIS_STATISTICS_XMIT_OK_SUPPORTED |
                                      NDIS_STATISTICS_RCV_OK_SUPPORTED   |
                                      NDIS_STATISTICS_XMIT_ERROR_SUPPORTED |
                                      NDIS_STATISTICS_RCV_ERROR_SUPPORTED;
    genAttribs.SupportedPauseFunctions  = NdisPauseFunctionsUnsupported;
    genAttribs.AutoNegotiationFlags     = NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED;
    genAttribs.SupportedOidList         = (PNDIS_OID)kSupportedOids;
    genAttribs.SupportedOidListLength   = sizeof(kSupportedOids);

    status = NdisMSetMiniportAttributes(MiniportAdapterHandle,
        (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&genAttribs);
    if (status != NDIS_STATUS_SUCCESS) goto cleanup;

    /* Register this adapter in the global pair. */
    KLOCK_QUEUE_HANDLE lqh;
    KeAcquireInStackQueuedSpinLock(&g_Pair.Lock, &lqh);
    g_Pair.Adapter[role] = adapter;
    KeReleaseInStackQueuedSpinLock(&lqh);

    return NDIS_STATUS_SUCCESS;

cleanup:
    NdisFreeNetBufferListPool(adapter->NblPool);
    NdisFreeMemory(adapter, sizeof(VPORT_ADAPTER), 0);
    return status;
}

/* ── MiniportHaltEx ──────────────────────────────────────────────────────── */

_Use_decl_annotations_
VOID VportHalt(NDIS_HANDLE MiniportAdapterContext, NDIS_HALT_ACTION HaltAction)
{
    UNREFERENCED_PARAMETER(HaltAction);
    PVPORT_ADAPTER adapter = (PVPORT_ADAPTER)MiniportAdapterContext;

    KLOCK_QUEUE_HANDLE lqh;
    KeAcquireInStackQueuedSpinLock(&g_Pair.Lock, &lqh);
    g_Pair.Adapter[adapter->Role] = NULL;
    KeReleaseInStackQueuedSpinLock(&lqh);

    NdisFreeNetBufferListPool(adapter->NblPool);
    NdisFreeMemory(adapter, sizeof(VPORT_ADAPTER), 0);
}

/* ── Pause / Restart ─────────────────────────────────────────────────────── */

_Use_decl_annotations_
NDIS_STATUS VportPause(NDIS_HANDLE MiniportAdapterContext,
                       PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters)
{
    UNREFERENCED_PARAMETER(PauseParameters);
    PVPORT_ADAPTER adapter = (PVPORT_ADAPTER)MiniportAdapterContext;
    adapter->Paused = TRUE;
    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
NDIS_STATUS VportRestart(NDIS_HANDLE MiniportAdapterContext,
                         PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters)
{
    UNREFERENCED_PARAMETER(RestartParameters);
    PVPORT_ADAPTER adapter = (PVPORT_ADAPTER)MiniportAdapterContext;
    adapter->Paused = FALSE;
    return NDIS_STATUS_SUCCESS;
}

/* ── OID handling ────────────────────────────────────────────────────────── */

/* Writes a ULONG into an OID request's InformationBuffer if it fits. */
static NDIS_STATUS OidSetUlong(PNDIS_OID_REQUEST req, ULONG value)
{
    if (req->DATA.QUERY_INFORMATION.InformationBufferLength < sizeof(ULONG)) {
        req->DATA.QUERY_INFORMATION.BytesNeeded = sizeof(ULONG);
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    *(ULONG*)req->DATA.QUERY_INFORMATION.InformationBuffer = value;
    req->DATA.QUERY_INFORMATION.BytesWritten = sizeof(ULONG);
    return NDIS_STATUS_SUCCESS;
}

/* Copies bytes into an OID request's InformationBuffer if it fits. */
static NDIS_STATUS OidSetBytes(PNDIS_OID_REQUEST req, const VOID* src, ULONG len)
{
    if (req->DATA.QUERY_INFORMATION.InformationBufferLength < len) {
        req->DATA.QUERY_INFORMATION.BytesNeeded = len;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    NdisMoveMemory(req->DATA.QUERY_INFORMATION.InformationBuffer, src, len);
    req->DATA.QUERY_INFORMATION.BytesWritten = len;
    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
NDIS_STATUS VportOidRequest(NDIS_HANDLE           MiniportAdapterContext,
                            PNDIS_OID_REQUEST     OidRequest)
{
    PVPORT_ADAPTER adapter = (PVPORT_ADAPTER)MiniportAdapterContext;
    const ULONG role = adapter->Role;

    if (OidRequest->RequestType == NdisRequestQueryInformation ||
        OidRequest->RequestType == NdisRequestQueryStatistics)
    {
        switch (OidRequest->DATA.QUERY_INFORMATION.Oid)
        {
        case OID_GEN_SUPPORTED_LIST:
            return OidSetBytes(OidRequest, kSupportedOids, sizeof(kSupportedOids));

        case OID_GEN_HARDWARE_STATUS:
            return OidSetUlong(OidRequest, NdisHardwareStatusReady);

        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            return OidSetUlong(OidRequest, NdisMedium802_3);

        case OID_GEN_PHYSICAL_MEDIUM_EX:
            return OidSetUlong(OidRequest, NdisPhysicalMediumUnspecified);

        case OID_GEN_CURRENT_LOOKAHEAD:
        case OID_GEN_MAXIMUM_LOOKAHEAD:
        case OID_GEN_MAXIMUM_FRAME_SIZE:
            return OidSetUlong(OidRequest, VPORT_MAX_FRAME_SIZE);

        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            return OidSetUlong(OidRequest, VPORT_MAX_FRAME_SIZE);

        case OID_GEN_LINK_SPEED:
            /* Legacy OID: units are 100 bps → 1 Gbps = 10 000 000 */
            return OidSetUlong(OidRequest, 10000000u);

        case OID_GEN_TRANSMIT_BUFFER_SPACE:
        case OID_GEN_RECEIVE_BUFFER_SPACE:
            return OidSetUlong(OidRequest, VPORT_MAX_FRAME_SIZE * 16u);

        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            return OidSetUlong(OidRequest, VPORT_MAX_FRAME_SIZE);

        case OID_GEN_VENDOR_ID:
            return OidSetUlong(OidRequest, VPORT_VENDOR_ID);

        case OID_GEN_VENDOR_DESCRIPTION: {
            static const CHAR desc[] = VPORT_VENDOR_DESC;
            return OidSetBytes(OidRequest, desc, sizeof(desc));
        }

        case OID_GEN_VENDOR_DRIVER_VERSION:
            return OidSetUlong(OidRequest, 0x00010000u);

        case OID_GEN_DRIVER_VERSION:
            /* High byte = NDIS major, low byte = NDIS minor */
            return OidSetUlong(OidRequest, 0x0655u); /* 6.85 */

        case OID_GEN_CURRENT_PACKET_FILTER:
            return OidSetUlong(OidRequest, adapter->PacketFilter);

        case OID_GEN_MAC_OPTIONS:
            return OidSetUlong(OidRequest,
                NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                NDIS_MAC_OPTION_TRANSFERS_NOT_PEND  |
                NDIS_MAC_OPTION_NO_LOOPBACK);

        case OID_GEN_MEDIA_CONNECT_STATUS:
            return OidSetUlong(OidRequest, NdisMediaStateConnected);

        case OID_GEN_MAXIMUM_SEND_PACKETS:
            return OidSetUlong(OidRequest, 16u);

        case OID_GEN_XMIT_OK:
            return OidSetUlong(OidRequest, (ULONG)adapter->XmitOk);
        case OID_GEN_RCV_OK:
            return OidSetUlong(OidRequest, (ULONG)adapter->RcvOk);
        case OID_GEN_XMIT_ERROR:
            return OidSetUlong(OidRequest, (ULONG)adapter->XmitErr);
        case OID_GEN_RCV_ERROR:
            return OidSetUlong(OidRequest, (ULONG)adapter->RcvErr);
        case OID_GEN_RCV_NO_BUFFER:
            return OidSetUlong(OidRequest, 0u);

        case OID_GEN_STATISTICS: {
            if (OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength <
                sizeof(NDIS_STATISTICS_INFO))
            {
                OidRequest->DATA.QUERY_INFORMATION.BytesNeeded =
                    sizeof(NDIS_STATISTICS_INFO);
                return NDIS_STATUS_BUFFER_TOO_SHORT;
            }
            PNDIS_STATISTICS_INFO pStat =
                (PNDIS_STATISTICS_INFO)OidRequest->DATA.QUERY_INFORMATION.InformationBuffer;
            NdisZeroMemory(pStat, sizeof(NDIS_STATISTICS_INFO));
            pStat->Header.Revision = NDIS_OBJECT_TYPE_DEFAULT;
            pStat->Header.Type     = NDIS_OBJECT_TYPE_DEFAULT;
            pStat->Header.Size     = sizeof(NDIS_STATISTICS_INFO);
            pStat->SupportedStatistics =
                NDIS_STATISTICS_XMIT_OK_SUPPORTED  |
                NDIS_STATISTICS_RCV_OK_SUPPORTED    |
                NDIS_STATISTICS_XMIT_ERROR_SUPPORTED |
                NDIS_STATISTICS_RCV_ERROR_SUPPORTED;
            pStat->ifHCOutUcastPkts = (ULONG64)adapter->XmitOk;
            pStat->ifHCInUcastPkts  = (ULONG64)adapter->RcvOk;
            pStat->ifOutErrors      = (ULONG64)adapter->XmitErr;
            pStat->ifInErrors       = (ULONG64)adapter->RcvErr;
            OidRequest->DATA.QUERY_INFORMATION.BytesWritten = sizeof(NDIS_STATISTICS_INFO);
            return NDIS_STATUS_SUCCESS;
        }

        case OID_802_3_PERMANENT_ADDRESS:
        case OID_802_3_CURRENT_ADDRESS:
            return OidSetBytes(OidRequest, kVportMac[role], 6);

        case OID_802_3_MULTICAST_LIST:
            OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
            return NDIS_STATUS_SUCCESS;

        case OID_802_3_MAXIMUM_LIST_SIZE:
            return OidSetUlong(OidRequest, 32u);

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
        }
    }

    if (OidRequest->RequestType == NdisRequestSetInformation)
    {
        switch (OidRequest->DATA.SET_INFORMATION.Oid)
        {
        case OID_GEN_CURRENT_PACKET_FILTER:
            if (OidRequest->DATA.SET_INFORMATION.InformationBufferLength >= sizeof(ULONG)) {
                adapter->PacketFilter =
                    *(ULONG*)OidRequest->DATA.SET_INFORMATION.InformationBuffer;
            }
            OidRequest->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;

        case OID_GEN_CURRENT_LOOKAHEAD:
            OidRequest->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS; /* we always look ahead the full frame */

        case OID_802_3_MULTICAST_LIST:
            OidRequest->DATA.SET_INFORMATION.BytesRead =
                OidRequest->DATA.SET_INFORMATION.InformationBufferLength;
            return NDIS_STATUS_SUCCESS; /* accept but ignore — deliver all frames */

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
        }
    }

    return NDIS_STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
VOID VportCancelOidRequest(NDIS_HANDLE MiniportAdapterContext, PVOID RequestId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RequestId);
    /* OID requests complete synchronously; nothing to cancel. */
}

/* ── Frame forwarding ────────────────────────────────────────────────────── */

/*
 * Clones one Ethernet frame from srcNb and indicates it as a receive on peer.
 * Called at DISPATCH_LEVEL or below.
 */
static VOID VportIndicateToPeer(PVPORT_ADAPTER peer, PNET_BUFFER srcNb)
{
    ULONG dataLen = NET_BUFFER_DATA_LENGTH(srcNb);
    if (dataLen == 0 || dataLen > VPORT_MAX_FRAME_SIZE) {
        InterlockedIncrement64(&peer->RcvErr);
        return;
    }

    /* Allocate a contiguous heap buffer for the frame copy. */
    PVOID pData = NdisAllocateMemoryWithTagPriority(
        peer->MiniportHandle, dataLen, VPORT_TAG, NormalPoolPriority);
    if (!pData) {
        InterlockedIncrement64(&peer->RcvErr);
        return;
    }

    /*
     * NdisGetDataBuffer: if data is contiguous it returns a pointer into the MDL
     * chain; if fragmented it copies into pData and returns pData.
     * Either way, if the return value differs from pData, we need to copy.
     */
    PVOID pSrc = NdisGetDataBuffer(srcNb, dataLen, pData, 1, 0);
    if (!pSrc) {
        NdisFreeMemory(pData, dataLen, 0);
        InterlockedIncrement64(&peer->RcvErr);
        return;
    }
    if (pSrc != pData) {
        NdisMoveMemory(pData, pSrc, dataLen);
    }

    /* Wrap our buffer in an MDL. */
    PMDL pMdl = NdisAllocateMdl(peer->MiniportHandle, pData, dataLen);
    if (!pMdl) {
        NdisFreeMemory(pData, dataLen, 0);
        InterlockedIncrement64(&peer->RcvErr);
        return;
    }

    /* Allocate a NET_BUFFER_LIST + embedded NET_BUFFER from peer's pool. */
    PNET_BUFFER_LIST pNbl = NdisAllocateNetBufferAndNetBufferList(
        peer->NblPool, 0, 0, pMdl, 0, dataLen);
    if (!pNbl) {
        NdisFreeMdl(pMdl);
        NdisFreeMemory(pData, dataLen, 0);
        InterlockedIncrement64(&peer->RcvErr);
        return;
    }

    /* Stash pointers in the NBL's miniport-reserved area for cleanup. */
    PVPORT_RX_CTX ctx = (PVPORT_RX_CTX)pNbl->MiniportReserved;
    ctx->Data = pData;
    ctx->Mdl  = pMdl;

    NET_BUFFER_LIST_NEXT_NBL(pNbl) = NULL;
    pNbl->SourceHandle             = g_DriverHandle;
    NET_BUFFER_LIST_STATUS(pNbl)   = NDIS_STATUS_SUCCESS;

    InterlockedIncrement64(&peer->RcvOk);

    NdisMIndicateReceiveNetBufferLists(
        peer->MiniportHandle,
        pNbl,
        NDIS_DEFAULT_PORT_NUMBER,
        1,
        NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);
}

/* ── MiniportSendNetBufferLists ──────────────────────────────────────────── */

_Use_decl_annotations_
VOID VportSendNetBufferLists(
    NDIS_HANDLE       MiniportAdapterContext,
    PNET_BUFFER_LIST  NetBufferLists,
    NDIS_PORT_NUMBER  PortNumber,
    ULONG             SendFlags)
{
    UNREFERENCED_PARAMETER(PortNumber);
    PVPORT_ADAPTER adapter = (PVPORT_ADAPTER)MiniportAdapterContext;
    BOOLEAN dispatchLevel  = (SendFlags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) != 0;

    /* Look up the peer adapter under the spinlock. */
    KLOCK_QUEUE_HANDLE lqh;
    KeAcquireInStackQueuedSpinLock(&g_Pair.Lock, &lqh);
    PVPORT_ADAPTER peer = g_Pair.Adapter[1u - adapter->Role];
    KeReleaseInStackQueuedSpinLock(&lqh);

    for (PNET_BUFFER_LIST pNbl = NetBufferLists; pNbl; ) {
        PNET_BUFFER_LIST pNext = NET_BUFFER_LIST_NEXT_NBL(pNbl);
        NET_BUFFER_LIST_NEXT_NBL(pNbl) = NULL;

        if (peer && !peer->Paused) {
            /* Forward every NET_BUFFER in this NBL to the peer. */
            for (PNET_BUFFER pNb = NET_BUFFER_LIST_FIRST_NB(pNbl); pNb;
                 pNb = NET_BUFFER_NEXT_NB(pNb))
            {
                VportIndicateToPeer(peer, pNb);
            }
            InterlockedIncrement64(&adapter->XmitOk);
        } else {
            InterlockedIncrement64(&adapter->XmitErr);
        }

        NET_BUFFER_LIST_STATUS(pNbl) = NDIS_STATUS_SUCCESS;
        NdisMSendNetBufferListsComplete(
            adapter->MiniportHandle, pNbl,
            dispatchLevel ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);

        pNbl = pNext;
    }
}

/* ── MiniportReturnNetBufferLists ────────────────────────────────────────── */

_Use_decl_annotations_
VOID VportReturnNetBufferLists(
    NDIS_HANDLE       MiniportAdapterContext,
    PNET_BUFFER_LIST  NetBufferLists,
    ULONG             ReturnFlags)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ReturnFlags);

    for (PNET_BUFFER_LIST pNbl = NetBufferLists; pNbl; ) {
        PNET_BUFFER_LIST pNext = NET_BUFFER_LIST_NEXT_NBL(pNbl);

        /* Retrieve heap pointers before freeing the NBL (which owns the ctx memory). */
        PVPORT_RX_CTX ctx  = (PVPORT_RX_CTX)pNbl->MiniportReserved;
        PVOID pData        = ctx->Data;
        PMDL  pMdl         = ctx->Mdl;
        ULONG dataLen      = MmGetMdlByteCount(pMdl);

        NdisFreeNetBufferList(pNbl);  /* frees NBL + embedded NB */
        NdisFreeMdl(pMdl);
        NdisFreeMemory(pData, dataLen, 0);

        pNbl = pNext;
    }
}

/* ── Trivial mandatory callbacks ─────────────────────────────────────────── */

_Use_decl_annotations_
VOID VportCancelSend(NDIS_HANDLE MiniportAdapterContext, PVOID CancelId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(CancelId);
    /* Sends complete synchronously in VportSendNetBufferLists; nothing to cancel. */
}

_Use_decl_annotations_
VOID VportDevicePnpEventNotify(NDIS_HANDLE                    MiniportAdapterContext,
                               PNET_DEVICE_PNP_EVENT           NetDevicePnpEvent)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(NetDevicePnpEvent);
}

_Use_decl_annotations_
VOID VportShutdownEx(NDIS_HANDLE            MiniportAdapterContext,
                     NDIS_SHUTDOWN_ACTION   ShutdownAction)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ShutdownAction);
}

_Use_decl_annotations_
BOOLEAN VportCheckForHangEx(NDIS_HANDLE MiniportAdapterContext)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    return FALSE; /* never signal a hang */
}
