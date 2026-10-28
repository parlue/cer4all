#include "bridge.hpp"
#include "gateway_log.hpp"
#include "log_protocol.hpp"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "nvs.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <memory>

namespace cer4all {
namespace {
// Existing eChessGw protocol: the phone is the GATT server, the module is its client.
const ble_uuid128_t serviceUuid = BLE_UUID128_INIT(1,0,0x0c,0x7f,0x1a,0x6b,0x2e,0x9d,0x3b,0x4a,0x4f,0x6c,0,0,0xc4,0xe5);
const ble_uuid128_t logUuid = BLE_UUID128_INIT(1,0,0x0c,0x7f,0x1a,0x6b,0x2e,0x9d,0x3b,0x4a,0x4f,0x6c,1,0,0xc4,0xe5);
const ble_uuid128_t cmdUuid = BLE_UUID128_INIT(1,0,0x0c,0x7f,0x1a,0x6b,0x2e,0x9d,0x3b,0x4a,0x4f,0x6c,2,0,0xc4,0xe5);
constexpr size_t ringSize = 12288, fileMax = 49152;
uint8_t ring[ringSize];
uint64_t total = 0, persisted = 0;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
LogCommands commands;
std::atomic<bool> ready{false}, found{false}, recording{false};
std::atomic<uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
ble_addr_t target{};
uint8_t ownAddressType = 0;
uint16_t svcStart = 0, svcEnd = 0, logHandle = 0, cmdHandle = 0, cmdEnd = 0, cccd = 0;
SemaphoreHandle_t completed = nullptr;
std::atomic<int> result{0};
std::atomic<uintptr_t> operation{0};
bool fsReady = false;
unsigned fileIndex = 0;
uint32_t sequence = 0;
uint64_t recordingSince = 0, lastFlush = 0;
std::atomic<uint32_t> lastTraffic{0};
const char *paths[] = {"/gwlog/gwlog0.txt", "/gwlog/gwlog1.txt"};
uint64_t millis() { return uint64_t(esp_timer_get_time() / 1000); }
void preferences() {
    nvs_handle_t h;
    if (nvs_open("gwlog", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "rec", recording ? 1 : 0);
    nvs_set_u8(h, "idx", fileIndex);
    nvs_set_u32(h, "seq", sequence);
    nvs_commit(h);
    nvs_close(h);
}
size_t fileSize(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return size > 0 ? size_t(size) : 0;
}
// Only this low-priority worker touches files/NVS. USB and application BLE never wait for it.
bool flushLog(bool force = false) {
    if (!recording || !fsReady) return false;
    uint8_t buffer[4096];
    portENTER_CRITICAL(&lock);
    if (total - persisted > ringSize) persisted = total - ringSize;
    size_t n = size_t(total - persisted);
    portEXIT_CRITICAL(&lock);
    if (!n || (!force && n < 3072 && millis() - lastFlush < 60000)) return true;
    if (!force && uint32_t(uint32_t(millis()) - lastTraffic.load()) < 150 &&
        n < 8192 && millis() - lastFlush < 300000)
        return true;
    n = std::min(n, sizeof(buffer));
    portENTER_CRITICAL(&lock);
    // A producer may have wrapped since the first check.
    if (total - persisted > ringSize) persisted = total - ringSize;
    const uint64_t begin = persisted;
    for (size_t i = 0; i < n; ++i) buffer[i] = ring[(begin + i) % ringSize];
    portEXIT_CRITICAL(&lock);
    if (fileSize(paths[fileIndex]) >= fileMax) {
        fileIndex ^= 1; ++sequence;
        remove(paths[fileIndex]);
        preferences();
    }
    FILE *f = fopen(paths[fileIndex], "ab");
    bool ok = false;
    if (f) {
        if (fileSize(paths[fileIndex]) == 0) fprintf(f, "# gwlog seq=%lu\n", (unsigned long)sequence);
        ok = fwrite(buffer, 1, n, f) == n;
        ok = fclose(f) == 0 && ok;
    }
    if (ok) {
        portENTER_CRITICAL(&lock);
        persisted = begin + n;
        portEXIT_CRITICAL(&lock);
        lastFlush = millis();
    } else {
        recording = false; preferences();
        debug("[GWLOG] flash write failed; recording switched off");
    }
    return ok;
}
bool setRecording(bool on) {
    if (on && !fsReady) return false;
    if (!on && recording) {
        // At most three ring chunks; no unbounded drain while producers run.
        for (int i = 0; i < 3; ++i) if (!flushLog(true)) break;
    }
    recording = on;
    recordingSince = lastFlush = millis();
    preferences();
    nvs_handle_t h;
    if (nvs_open("gwlog", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "boots", 0); nvs_commit(h); nvs_close(h);
    }
    debug("[GWLOG] recording %s", on ? "ON" : "OFF");
    return true;
}
void finish(int rc, void *token) {
    if (uintptr_t(token) != operation.load()) return;
    result = rc;
    xSemaphoreGive(completed);
}
void *beginOperation() {
    ++operation;
    while (xSemaphoreTake(completed, 0) == pdTRUE) {}
    result = BLE_HS_ETIMEOUT;
    return reinterpret_cast<void *>(operation.load());
}
bool waitOperation(int rc, uint32_t timeout = 6000) {
    if (rc) return false;
    return xSemaphoreTake(completed, pdMS_TO_TICKS(timeout)) == pdTRUE && result == 0;
}
int svcCallback(uint16_t, const ble_gatt_error *e, const ble_gatt_svc *s, void *token) {
    if (uintptr_t(token) != operation.load()) return 0;
    if (!e->status && s) { svcStart = s->start_handle; svcEnd = s->end_handle; }
    else finish(e->status == BLE_HS_EDONE ? 0 : e->status, token);
    return 0;
}
int chrCallback(uint16_t, const ble_gatt_error *e, const ble_gatt_chr *c, void *token) {
    if (uintptr_t(token) != operation.load()) return 0;
    if (!e->status && c) {
        if (cmdHandle && !cmdEnd && c->def_handle > cmdHandle) cmdEnd = c->def_handle - 1;
        if (!ble_uuid_cmp(&c->uuid.u, &logUuid.u) && (c->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP)) logHandle = c->val_handle;
        if (!ble_uuid_cmp(&c->uuid.u, &cmdUuid.u) && (c->properties & BLE_GATT_CHR_PROP_NOTIFY)) cmdHandle = c->val_handle;
    } else finish(e->status == BLE_HS_EDONE ? 0 : e->status, token);
    return 0;
}
int dscCallback(uint16_t, const ble_gatt_error *e, uint16_t, const ble_gatt_dsc *d, void *token) {
    if (uintptr_t(token) != operation.load()) return 0;
    if (!e->status && d) {
        if (ble_uuid_u16(&d->uuid.u) == 0x2902) cccd = d->handle;
    } else finish(e->status == BLE_HS_EDONE ? 0 : e->status, token);
    return 0;
}
int written(uint16_t, const ble_gatt_error *e, ble_gatt_attr *, void *token) {
    finish(e->status, token); return 0;
}
int mtuChanged(uint16_t, const ble_gatt_error *e, uint16_t, void *token) {
    finish(e->status, token); return 0;
}
int gap(ble_gap_event *e, void *token) {
    if (e->type == BLE_GAP_EVENT_DISC && !applicationConnected() && !found) {
        ble_hs_adv_fields fields{};
        if (!ble_hs_adv_parse_fields(&fields, e->disc.data, e->disc.length_data)) {
            char name[32]{};
            if (fields.name) memcpy(name, fields.name, std::min(size_t(fields.name_len), sizeof(name)-1));
            bool tag = strstr(name, "eChessGateway") != nullptr;
            for (unsigned i = 0; i < fields.num_uuids128; ++i)
                tag |= !ble_uuid_cmp(&fields.uuids128[i].u, &serviceUuid.u);
            if (tag) {
                target = e->disc.addr; found = true; ble_gap_disc_cancel();
            }
        }
    } else if (e->type == BLE_GAP_EVENT_CONNECT) {
        if (uintptr_t(token) != operation.load()) {
            if (!e->connect.status) ble_gap_terminate(e->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (!e->connect.status) connection = e->connect.conn_handle;
        finish(e->connect.status, token);
    } else if (e->type == BLE_GAP_EVENT_DISCONNECT) {
        if (connection != e->disconnect.conn.conn_handle) return 0;
        connection = BLE_HS_CONN_HANDLE_NONE;
        finish(BLE_HS_ENOTCONN, reinterpret_cast<void *>(operation.load()));
        bleBoardChanged();
        wakeBleTx();
    } else if (e->type == BLE_GAP_EVENT_NOTIFY_RX && e->notify_rx.attr_handle == cmdHandle) {
        uint8_t bytes[256]; uint16_t n = 0;
        if (OS_MBUF_PKTLEN(e->notify_rx.om) <= sizeof(bytes) &&
            !ble_hs_mbuf_to_flat(e->notify_rx.om, bytes, sizeof(bytes), &n)) {
            portENTER_CRITICAL(&lock); commands.feed(bytes, n); portEXIT_CRITICAL(&lock);
        }
    }
    return 0;
}
bool send(const uint8_t *data, size_t n) {
    while (n && connection != BLE_HS_CONN_HANDLE_NONE && !applicationConnected()) {
        uint16_t h = connection.load();
        size_t chunk = std::min(n, std::min(size_t(180), size_t(ble_att_mtu(h) - 3)));
        bool sent = false;
        for (int attempt = 0; attempt < 20 && connection == h; ++attempt) {
            if (!ble_gattc_write_no_rsp_flat(h, logHandle, data, chunk)) { sent = true; break; }
            vTaskDelay(pdMS_TO_TICKS(15));
        }
        if (!sent) return false;
        data += chunk; n -= chunk;
        // Same upload-only pacing as the gateway; never in the board data path.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return n == 0;
}
bool sendText(const char *text) { return send(reinterpret_cast<const uint8_t *>(text), strlen(text)); }
bool upload() {
    svcStart = svcEnd = logHandle = cmdHandle = cmdEnd = cccd = 0;
    portENTER_CRITICAL(&lock); commands = LogCommands{}; portEXIT_CRITICAL(&lock);
    void *token = beginOperation();
    if (!waitOperation(ble_gattc_exchange_mtu(connection, mtuChanged, token))) return false;
    token = beginOperation();
    if (!waitOperation(ble_gattc_disc_svc_by_uuid(connection, &serviceUuid.u, svcCallback, token)) || !svcStart) return false;
    token = beginOperation();
    if (!waitOperation(ble_gattc_disc_all_chrs(connection, svcStart, svcEnd, chrCallback, token)) || !logHandle || !cmdHandle) return false;
    if (!cmdEnd) cmdEnd = svcEnd;
    token = beginOperation();
    if (!waitOperation(ble_gattc_disc_all_dscs(connection, cmdHandle, cmdEnd, dscCallback, token)) || !cccd) return false;
    uint8_t notify[] = {1, 0};
    token = beginOperation();
    if (!waitOperation(ble_gattc_write_flat(connection, cccd, notify, sizeof(notify), written, token))) return false;

    // Freeze the pending RAM tail. Only this worker persists/rotates files.
    std::unique_ptr<uint8_t[]> tail(new (std::nothrow) uint8_t[ringSize]);
    if (!tail) return false;
    portENTER_CRITICAL(&lock);
    size_t pending = size_t(std::min(uint64_t(ringSize), total - persisted));
    for (size_t i = 0; i < pending; ++i) tail[i] = ring[(total - pending + i) % ringSize];
    portEXIT_CRITICAL(&lock);
    size_t sizes[] = {fileSize(paths[fileIndex ^ 1]), fileSize(paths[fileIndex])};
    char header[128];
    logHeader(header, sizeof(header), sizes[0] + sizes[1] + pending, recording,
              esp_app_get_description()->version, (unsigned long)millis());
    if (!sendText(header)) return false;
    uint8_t buffer[180];
    for (unsigned i = 0; i < 2; ++i) {
        if (!sizes[i]) continue;
        FILE *f = fopen(paths[i ? fileIndex : fileIndex ^ 1], "rb");
        if (!f) return false;
        size_t remaining = sizes[i]; bool ok = true;
        while (remaining && ok) {
            size_t n = fread(buffer, 1, std::min(remaining, sizeof(buffer)), f);
            ok = n && send(buffer, n); remaining -= n;
        }
        fclose(f);
        if (!ok) return false;
    }
    if (!send(tail.get(), pending) || !sendText("\n#GWLOG-END\n")) return false;
    debug("[GWLOG] log upload finished");
    uint64_t until = millis() + 20000;
    while (connection != BLE_HS_CONN_HANDLE_NONE && !applicationConnected() && millis() < until) {
        char cmd[32];
        portENTER_CRITICAL(&lock); bool got = commands.next(cmd); portEXIT_CRITICAL(&lock);
        if (got) {
            until = millis() + 10000;
            bool ok = true;
            if (!strcmp(cmd, "REC1")) ok = setRecording(true);
            else if (!strcmp(cmd, "REC0")) ok = setRecording(false);
            else if (!strcmp(cmd, "CLR")) {
                if (fsReady) { remove(paths[0]); remove(paths[1]); }
                fileIndex = 0; sequence = 0; preferences();
                debug("[GWLOG] stored log files deleted");
            } else if (!strcmp(cmd, "BYE")) break;
            else continue; // PING keeps the same command window alive.
            if (!ok) return false;
            char reply[48]; snprintf(reply, sizeof(reply), "#OK %s\n", cmd);
            if (!sendText(reply)) return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}
void worker(void *) {
    for (;;) {
        if (recording && millis() - recordingSince > 7200000) setRecording(false);
        flushLog();
        if (ready && !applicationConnected() && connection == BLE_HS_CONN_HANDLE_NONE) {
            found = false;
            ble_gap_disc_params p{};
            p.passive = 0; p.itvl = 160; p.window = 48; p.filter_duplicates = 1;
            if (!ble_hs_id_infer_auto(0, &ownAddressType) &&
                !ble_gap_disc(ownAddressType, 4000, &p, gap, nullptr)) {
                for (int i = 0; i < 40 && !found && !applicationConnected() && ready; ++i)
                    vTaskDelay(pdMS_TO_TICKS(100));
                ble_gap_disc_cancel();
                if (found && !applicationConnected() && ready) {
                    void *token = beginOperation();
                    if (waitOperation(ble_gap_connect(ownAddressType, &target, 8000, nullptr, gap, token), 9000)) {
                        debug("[GWLOG] tag found; uploading stored log");
                        if (!upload()) debug("[GWLOG] log upload failed");
                    } else ble_gap_conn_cancel();
                    if (connection != BLE_HS_CONN_HANDLE_NONE) {
                        ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
                        // Do not reuse session state before DISCONNECT is delivered.
                        while (connection != BLE_HS_CONN_HANDLE_NONE && ready)
                            vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
} // namespace
void appendGatewayLog(const char *text, size_t length) {
    portENTER_CRITICAL(&lock);
    for (size_t i = 0; i < length; ++i) ring[(total + i) % ringSize] = uint8_t(text[i]);
    total += length;
    portEXIT_CRITICAL(&lock);
}
void gatewayLogReady(bool on) {
    ready = on;
    if (!on) {
        connection = BLE_HS_CONN_HANDLE_NONE;
        ++operation;
        result = BLE_HS_ENOTCONN;
        if (completed) xSemaphoreGive(completed);
    }
}
void gatewayLogTraffic() {
    lastTraffic = millis();
}
void startGatewayLog() {
    completed = xSemaphoreCreateBinary(); assert(completed);
    esp_vfs_spiffs_conf_t config{};
    config.base_path = "/gwlog"; config.partition_label = "gwlog";
    config.max_files = 3; config.format_if_mount_failed = true;
    fsReady = esp_vfs_spiffs_register(&config) == ESP_OK;
    nvs_handle_t h;
    if (nvs_open("gwlog", NVS_READWRITE, &h) == ESP_OK) {
        uint8_t rec = 0, boots = 0, idx = 0;
        nvs_get_u8(h, "rec", &rec); nvs_get_u8(h, "boots", &boots);
        nvs_get_u8(h, "idx", &idx); nvs_get_u32(h, "seq", &sequence);
        fileIndex = idx & 1;
        if (rec && ++boots > 3) { rec = 0; boots = 0; }
        recording = rec && fsReady;
        nvs_set_u8(h, "rec", recording ? 1 : 0); nvs_set_u8(h, "boots", rec ? boots : 0);
        nvs_commit(h); nvs_close(h);
    }
    recordingSince = lastFlush = millis();
    debug("=== BOOT rec=%d fw=%s ===", recording ? 1 : 0, esp_app_get_description()->version);
    if (!fsReady) debug("[GWLOG] log storage unavailable");
    if (xTaskCreate(worker, "gwlog", 8192, nullptr, 2, nullptr) != pdTRUE) abort();
}
} // namespace cer4all
