#include "bridge.hpp"
#include "gateway_log.hpp"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace cer4all {
namespace {
// UUID octets in NimBLE's little-endian order.
const ble_uuid128_t nus = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93,
                                           0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
const ble_uuid128_t rxUuid = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93,
                                              0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
const ble_uuid128_t txUuid = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93,
                                              0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);
uint16_t txHandle = 0;
std::atomic<uint16_t> app{BLE_HS_CONN_HANDLE_NONE}, peer{BLE_HS_CONN_HANDLE_NONE};
std::atomic<bool> subscribed{false}, synced{false};
uint8_t addrType = 0;
// Serialize transmission with GAP callbacks on NimBLE's own host event queue.
// An old queued USB snapshot must never reach a newly connected client.
ble_npl_event txEvent;
SemaphoreHandle_t txDone = nullptr;
struct {
    uint8_t bytes[253];
    size_t n;
    unsigned epoch;
    int result;
} request{};
void sendOnHost(ble_npl_event *) {
    request.result = BLE_HS_ENOTCONN;
    if (request.epoch == rxEpoch() && subscribed && app != BLE_HS_CONN_HANDLE_NONE) {
        os_mbuf *b = ble_hs_mbuf_from_flat(request.bytes, request.n);
        request.result = b ? ble_gatts_notify_custom(app.load(), txHandle, b) : BLE_HS_ENOMEM;
    }
    xSemaphoreGive(txDone);
}

void logLink(uint16_t h, const char *reason) {
    ble_gap_conn_desc d{};
    if (ble_gap_conn_find(h, &d)) return;
    uint8_t tx = 0, rx = 0;
    int rc = ble_gap_read_le_phy(h, &tx, &rx);
    debug("BLE link %s h=%u interval_us=%lu latency=%u mtu=%u phy_tx=%u phy_rx=%u phy_rc=%d",
          reason, h, (unsigned long)d.conn_itvl * 1250, d.conn_latency,
          ble_att_mtu(h), tx, rx, rc);
}
void requestFastInterval(uint16_t h) {
    ble_gap_upd_params params{};
    params.itvl_min = 6; // 7.5 ms; the central selects the actual interval.
    params.itvl_max = 12; // 15 ms
    params.supervision_timeout = 400;
    int rc = ble_gap_update_params(h, &params);
    debug("BLE interval request h=%u rc=%d", h, rc);
}
int gap(ble_gap_event *, void *);
void advertise() {
    if (!synced || ble_gap_adv_active())
        return;
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = const_cast<ble_uuid128_t *>(&nus);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.name = reinterpret_cast<const uint8_t *>("Certabo");
    fields.name_len = 7;
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc)
        return;
    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 0x30;
    params.itvl_max = 0x60;
    rc = ble_gap_adv_start(addrType, nullptr, BLE_HS_FOREVER, &params, gap, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_ENOMEM)
        debug("BLE advertising rc=%d", rc);
}
int access(uint16_t connection, uint16_t, ble_gatt_access_ctxt *ctx, void *) {
    if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    if (app.load() != BLE_HS_CONN_HANDLE_NONE && app.load() != connection)
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    uint8_t bytes[512];
    uint16_t len = 0;
    if (OS_MBUF_PKTLEN(ctx->om) > sizeof(bytes) ||
        ble_hs_mbuf_to_flat(ctx->om, bytes, sizeof(bytes), &len) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    // A client may write before enabling notifications. Reserve its ownership.
    uint16_t expected = BLE_HS_CONN_HANDLE_NONE;
    app.compare_exchange_strong(expected, connection);
    return receiveApp(bytes, len) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
ble_gatt_chr_def nusCharacteristics[3]{};

ble_gatt_svc_def services[2]{};
int notifyAccess(uint16_t, uint16_t, ble_gatt_access_ctxt *ctx, void *) {
    // NimBLE requires a callback even for notify-only characteristics.
    return ctx->op == BLE_GATT_ACCESS_OP_WRITE_CHR
               ? BLE_ATT_ERR_WRITE_NOT_PERMITTED
               : BLE_ATT_ERR_READ_NOT_PERMITTED;
}
void configureServices() {
    nusCharacteristics[0].uuid = &rxUuid.u;
    nusCharacteristics[0].access_cb = access;
    nusCharacteristics[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
    nusCharacteristics[1].uuid = &txUuid.u;
    nusCharacteristics[1].access_cb = notifyAccess;
    nusCharacteristics[1].flags = BLE_GATT_CHR_F_NOTIFY;
    nusCharacteristics[1].val_handle = &txHandle;
    services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    services[0].uuid = &nus.u;
    services[0].characteristics = nusCharacteristics;

}
int gap(ble_gap_event *e, void *) {
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            peer = e->connect.conn_handle;
            // Stop any log discovery immediately; keep radio time for the board.
            ble_gap_disc_cancel();
            requestFastInterval(e->connect.conn_handle);
            int dle = ble_gap_set_data_len(e->connect.conn_handle, 251, 2120);
            int phy = ble_gap_set_prefered_le_phy(e->connect.conn_handle,
                                                BLE_GAP_LE_PHY_2M_MASK,
                                                BLE_GAP_LE_PHY_2M_MASK, 0);
            // Unsupported/rejected optimizations leave the existing link usable.
            debug("BLE transport request h=%u dle_rc=%d phy_rc=%d",
                  e->connect.conn_handle, dle, phy);
            logLink(e->connect.conn_handle, "connect");
            debug("BLE connected handle=%u", e->connect.conn_handle);
        }
        advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = e->disconnect.conn.conn_handle;
        if (peer == h) peer = BLE_HS_CONN_HANDLE_NONE;
        if (app == h) {
            subscribed = false;
            app = BLE_HS_CONN_HANDLE_NONE;
            appSubscribed(false);
        }
        debug("BLE disconnected handle=%u reason=%d", h, e->disconnect.reason);
        advertise();
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == txHandle) {
            uint16_t h = e->subscribe.conn_handle;
            if (e->subscribe.cur_notify) {
                if (app != BLE_HS_CONN_HANDLE_NONE && app != h) {
                    ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                app = h;
                subscribed = true;
                appSubscribed(true);
                // Some centrals negotiate services before honoring our parameters.
                ble_gap_conn_desc d{};
                if (!ble_gap_conn_find(h, &d) && (d.conn_itvl > 12 || d.conn_latency))
                    requestFastInterval(h);
                logLink(h, "subscribe");
            } else if (app == h) {
                subscribed = false;
                app = BLE_HS_CONN_HANDLE_NONE;
                appSubscribed(false);
            }
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (e->notify_tx.attr_handle == txHandle && e->notify_tx.status != 0) {
            debug("BLE notify failure rc=%d", e->notify_tx.status);
            disconnectApp();
        }
        // Retry pending application data when notification buffers become available.
        wakeBleTx();
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        debug("BLE interval result h=%u status=%d", e->conn_update.conn_handle,
              e->conn_update.status);
        logLink(e->conn_update.conn_handle, "update");
        break;
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        debug("BLE DLE h=%u tx_octets=%u rx_octets=%u tx_us=%u rx_us=%u",
              e->data_len_chg.conn_handle, e->data_len_chg.max_tx_octets,
              e->data_len_chg.max_rx_octets, e->data_len_chg.max_tx_time,
              e->data_len_chg.max_rx_time);
        break;
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        debug("BLE PHY h=%u status=%d tx=%u rx=%u", e->phy_updated.conn_handle,
              e->phy_updated.status, e->phy_updated.tx_phy, e->phy_updated.rx_phy);
        break;
    case BLE_GAP_EVENT_MTU:
        debug("BLE MTU handle=%u mtu=%u", e->mtu.conn_handle, e->mtu.value);
        wakeBleTx();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}
void sync() {
    if (ble_hs_id_infer_auto(0, &addrType) == 0) {
        synced = true;
        gatewayLogReady(true);
        advertise();
    }
}
void reset(int reason) {
    synced = false;
    gatewayLogReady(false);
    subscribed = false;
    app = BLE_HS_CONN_HANDLE_NONE;
    peer = BLE_HS_CONN_HANDLE_NONE;
    appSubscribed(false);
    debug("BLE host reset %d", reason);
}
void host(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}
} // namespace
void startBle() {
    ESP_ERROR_CHECK(nimble_port_init());
    txDone = xSemaphoreCreateBinary();
    assert(txDone);
    ble_npl_event_init(&txEvent, sendOnHost, nullptr);
    ble_hs_cfg.sync_cb = sync;
    ble_hs_cfg.reset_cb = reset;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("Certabo");
    ble_att_set_preferred_mtu(256);
    configureServices();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
    nimble_port_freertos_init(host);
}
bool bleSubscribed() {
    return subscribed.load() && app != BLE_HS_CONN_HANDLE_NONE;
}
size_t blePayload() {
    uint16_t connection = app.load();
    uint16_t mtu = connection == BLE_HS_CONN_HANDLE_NONE ? 23 : ble_att_mtu(connection);
    return mtu >= 23 ? size_t(mtu - 3) : 20;
}
int notifyApp(const uint8_t *p, size_t n, unsigned epoch) {
    if (n > sizeof(request.bytes) || !bleSubscribed())
        return BLE_HS_ENOTCONN;
    std::memcpy(request.bytes, p, n);
    request.n = n;
    request.epoch = epoch;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &txEvent);
    xSemaphoreTake(txDone, portMAX_DELAY);
    return request.result;
}
void disconnectApp() {
    uint16_t h = app.load();
    if (h != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
}
void bleBoardChanged() {
    advertise();
}
bool applicationConnected() {
    return app.load() != BLE_HS_CONN_HANDLE_NONE || peer.load() != BLE_HS_CONN_HANDLE_NONE;
}
} // namespace cer4all
