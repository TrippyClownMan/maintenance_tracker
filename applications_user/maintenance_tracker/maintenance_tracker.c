#include <furi.h>
#include <furi_hal_rtc.h>
#include <datetime/datetime.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller_sync.h>
#include <storage/storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "MaintenanceTracker"
#define ASSET_LIMIT 24
#define SERVICE_LIMIT 12
#define SERVICE_NAME_SIZE 24
#define ASSET_NAME_SIZE 32
#define DATE_SIZE 12
#define HISTORY_SIZE 4096
#define URI_SIZE 96
#define NFC_NDEF_BUFFER_SIZE (URI_SIZE + 8U)
#define ASSET_UID_TEXT_SIZE 25
#define TRACKER_LOG_PATH EXT_PATH("apps_data/maintenance_tracker/log.txt")

typedef enum {
    ViewMain,
    ViewEquipment,
    ViewProfile,
    ViewInput,
    ViewHistory,
    ViewChecklist,
    ViewNotice,
    ViewTracker,
} MaintenanceViewId;

typedef enum {
    TrackerStateIdle,
    TrackerStateRunning,
} TrackerState;

typedef enum {
    InputNewAsset,
    InputNewService,
} InputKind;

typedef enum {
    NfcWriteSuccess,
    NfcWriteNoTag,
    NfcWriteUnsupportedTag,
    NfcWriteReadOnly,
    NfcWriteInsufficientSpace,
    NfcWriteFailed,
    NfcWriteVerifyFailed,
    NfcWriteSnapshotFailed,
} NfcWriteResult;

typedef struct {
    uint32_t id;
    uint32_t lifetime_seconds;
    char name[ASSET_NAME_SIZE];
    char last_service[DATE_SIZE];
} Asset;

typedef struct {
    ViewDispatcher* dispatcher;
    Submenu* main_menu;
    Submenu* profile_menu;
    TextInput* text_input;
    TextBox* text_box;
    View* equipment_view;
    View* checklist_view;
    View* notice_view;
    View* tracker_view;
    Storage* storage;
    Asset assets[ASSET_LIMIT];
    uint8_t asset_count;
    int8_t selected_asset;
    bool storage_ready;
    char input_buffer[ASSET_NAME_SIZE];
    InputKind input_kind;
    char services[SERVICE_LIMIT][SERVICE_NAME_SIZE];
    bool service_checked[SERVICE_LIMIT];
    uint8_t service_count;
    uint8_t checklist_selection;
    TrackerState tracker_state;
    uint32_t session_start_timestamp;
    uint32_t chip_accumulated_seconds;
    char session_uid[ASSET_UID_TEXT_SIZE];
    char session_asset_name[ASSET_NAME_SIZE];
    bool checklist_log_saved;
    uint32_t current_view;
    char notice[96];
    bool notice_dismiss_exits;
    MaintenanceViewId notice_return_view;
    char history[HISTORY_SIZE];
} MaintenanceApp;

static const char* const standard_services[] = {
    "Oil",
    "Filter",
    "Inspection",
    "Lubrication",
};

static const uint8_t standard_service_count = COUNT_OF(standard_services);

static void text_input_callback(void* context);
static void profile_menu_callback(void* context, uint32_t index);
static void rebuild_profile_menu(MaintenanceApp* app);
static void show_notice(MaintenanceApp* app, const char* message, bool exits);
static bool tracker_input(InputEvent* event, void* context);

// Returns the shared SD-card path for the asset database.
static const char* asset_db_path(void) {
    return APP_DATA_PATH("assets.tsv");
}

// Builds a stable per-asset path for its maintenance history.
static void asset_log_path(const Asset* asset, char* path, size_t size) {
    snprintf(path, size, APP_DATA_PATH("%lu.txt"), (unsigned long)asset->id);
}

// Builds a stable per-asset path for its custom service list.
static void asset_services_path(const Asset* asset, char* path, size_t size) {
    snprintf(path, size, APP_DATA_PATH("%lu.svc"), (unsigned long)asset->id);
}

// Builds the path where a scanned and written NFC tag snapshot is stored.
static void asset_nfc_path(const Asset* asset, char* path, size_t size) {
    snprintf(path, size, APP_DATA_PATH("%lu.nfc"), (unsigned long)asset->id);
}

// Updates the active view and keeps the current-view state in sync for callbacks.
static void switch_view(MaintenanceApp* app, MaintenanceViewId view_id) {
    app->current_view = view_id;
    view_dispatcher_switch_to_view(app->dispatcher, view_id);
}

// Formats the current device uptime as a short timestamp (not wall-clock time).
static void format_uptime(char timestamp[DATE_SIZE]) {
    uint32_t seconds = furi_get_tick() / furi_kernel_get_tick_frequency();
    snprintf(timestamp, DATE_SIZE, "T+%lus", (unsigned long)seconds);
}

// Ensures the app's SD-card data directory exists before files are accessed.
static bool ensure_data_directory(MaintenanceApp* app) {
    FS_Error error = storage_common_mkdir(app->storage, "/ext/apps_data/maintenance_tracker");
    if((error != FSE_OK) && (error != FSE_EXIST)) {
        FURI_LOG_E(TAG, "Cannot create app data directory: %s", filesystem_api_error_get_desc(error));
        return false;
    }

    File* file = storage_file_alloc(app->storage);
    bool ready = storage_file_open(file, TRACKER_LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ready && !storage_file_close(file)) ready = false;
    if(!ready) FURI_LOG_E(TAG, "Cannot initialize clock-in/out log");
    storage_file_free(file);
    return ready;
}

// Restores the lifetime total and open session state represented by one log record.
static void restore_tracker_log_line(MaintenanceApp* app, char* line) {
    char* total_field = strrchr(line, '|');
    if(total_field) {
        char* end = NULL;
        unsigned long total = strtoul(total_field + 1, &end, 10);
        if((end != total_field + 1) && (total <= UINT32_MAX)) {
            app->chip_accumulated_seconds = (uint32_t)total;
        }
    }

    char* uid_field = strchr(line, '|');
    if(!uid_field) return;
    char* event_field = strchr(uid_field + 1, '|');
    if(!event_field) return;
    *uid_field++ = '\0';
    *event_field++ = '\0';
    while(*uid_field == ' ') uid_field++;
    while(*event_field == ' ') event_field++;
    char* uid_end = uid_field + strlen(uid_field);
    while((uid_end > uid_field) && (uid_end[-1] == ' ')) *--uid_end = '\0';
    char* event_end = strchr(event_field, '|');
    if(event_end) {
        while((event_end > event_field) && (event_end[-1] == ' ')) *--event_end = '\0';
    }

    if(strcmp(event_field, "CLOCK OUT") == 0) {
        app->tracker_state = TrackerStateIdle;
        app->session_start_timestamp = 0;
        app->session_uid[0] = '\0';
        app->session_asset_name[0] = '\0';
    } else if(strcmp(event_field, "CLOCK IN") == 0) {
        unsigned int year, month, day, hour, minute, second;
        if(sscanf(
               line,
               "%4u-%2u-%2u %2u:%2u:%2u",
               &year,
               &month,
               &day,
               &hour,
               &minute,
               &second) != 6) {
            return;
        }
        DateTime datetime = {
            .year = (uint16_t)year,
            .month = (uint8_t)month,
            .day = (uint8_t)day,
            .hour = (uint8_t)hour,
            .minute = (uint8_t)minute,
            .second = (uint8_t)second,
            .weekday = 1,
        };
        if(!datetime_validate_datetime(&datetime) ||
           (strlen(uid_field) >= sizeof(app->session_uid))) {
            return;
        }
        app->session_start_timestamp = datetime_datetime_to_timestamp(&datetime);
        snprintf(app->session_uid, sizeof(app->session_uid), "%s", uid_field);
        snprintf(
            app->session_asset_name,
            sizeof(app->session_asset_name),
            "%s",
            "Recovered session");
        app->tracker_state = TrackerStateRunning;
    }
}

// Restores the lifetime runtime and any unfinished session from the event log.
static bool load_tracker_total(MaintenanceApp* app) {
    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, TRACKER_LOG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Cannot read clock-in/out log");
        storage_file_free(file);
        return false;
    }

    char line[160];
    size_t line_length = 0;
    bool line_overflow = false;
    char chunk[64];
    uint16_t count;
    app->chip_accumulated_seconds = 0;
    while((count = storage_file_read(file, chunk, sizeof(chunk))) > 0) {
        for(uint16_t i = 0; i < count; i++) {
            if(chunk[i] == '\n') {
                if(line_length && !line_overflow) {
                    line[line_length] = '\0';
                    restore_tracker_log_line(app, line);
                }
                line_length = 0;
                line_overflow = false;
            } else if((line_length + 1U) < sizeof(line)) {
                line[line_length++] = chunk[i];
            } else {
                line_overflow = true;
            }
        }
    }
    if(line_length && !line_overflow) {
        line[line_length] = '\0';
        restore_tracker_log_line(app, line);
    }

    bool success = storage_file_close(file);
    storage_file_free(file);
    return success;
}

// Appends one clock event with its local time, UID, session duration, and new total.
static bool append_tracker_event(
    MaintenanceApp* app,
    const DateTime* datetime,
    const char* uid,
    const char* event_type,
    uint32_t session_duration,
    uint32_t grand_total) {
    File* file = storage_file_alloc(app->storage);
    bool success = false;
    if(storage_file_open(file, TRACKER_LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char line[160];
        int length = snprintf(
            line,
            sizeof(line),
            "%04u-%02u-%02u %02u:%02u:%02u | %s | %s | %lu s | %lu s\n",
            (unsigned)datetime->year,
            (unsigned)datetime->month,
            (unsigned)datetime->day,
            (unsigned)datetime->hour,
            (unsigned)datetime->minute,
            (unsigned)datetime->second,
            uid,
            event_type,
            (unsigned long)session_duration,
            (unsigned long)grand_total);
        success = (length > 0) && ((size_t)length < sizeof(line)) &&
                  (storage_file_write(file, line, (uint16_t)length) == (uint16_t)length);
        if(!storage_file_close(file)) success = false;
    } else {
        FURI_LOG_E(TAG, "Cannot open clock-in/out log for append");
    }
    storage_file_free(file);
    return success;
}

// Rewrites all asset records to the tab-separated SD-card database.
static bool write_asset_database(MaintenanceApp* app) {
    File* file = storage_file_alloc(app->storage);
    bool success = false;
    if(storage_file_open(file, asset_db_path(), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        success = true;
        for(uint8_t i = 0; i < app->asset_count; i++) {
            Asset* asset = &app->assets[i];
            char line[128];
            int length = snprintf(
                line,
                sizeof(line),
                "%lu\t%lu\t%s\t%s\n",
                (unsigned long)asset->id,
                (unsigned long)asset->lifetime_seconds,
                asset->last_service,
                asset->name);
            if((length < 0) || ((size_t)length >= sizeof(line)) ||
               (storage_file_write(file, line, (uint16_t)length) != (uint16_t)length)) {
                success = false;
                break;
            }
        }
        if(!storage_file_close(file)) success = false;
    } else {
        FURI_LOG_E(TAG, "Cannot open asset database for writing");
    }
    storage_file_free(file);
    return success;
}

// Parses one database row, ignoring malformed records and enforcing field limits.
static void parse_asset_line(MaintenanceApp* app, char* line) {
    if(app->asset_count >= ASSET_LIMIT) return;

    char* id_text = line;
    char* seconds_text = strchr(id_text, '\t');
    if(!seconds_text) return;
    *seconds_text++ = '\0';

    char* date_text = strchr(seconds_text, '\t');
    if(!date_text) return;
    *date_text++ = '\0';

    char* name_text = strchr(date_text, '\t');
    if(!name_text) return;
    *name_text++ = '\0';
    name_text[strcspn(name_text, "\r\n")] = '\0';

    char* end = NULL;
    unsigned long id = strtoul(id_text, &end, 10);
    if((end == id_text) || (*end != '\0') || (id > UINT32_MAX)) return;
    unsigned long seconds = strtoul(seconds_text, &end, 10);
    if((end == seconds_text) || (*end != '\0') || (seconds > UINT32_MAX)) return;
    if(strlen(name_text) >= ASSET_NAME_SIZE) return;

    Asset* asset = &app->assets[app->asset_count++];
    asset->id = (uint32_t)id;
    asset->lifetime_seconds = (uint32_t)seconds;
    snprintf(asset->last_service, sizeof(asset->last_service), "%s", date_text);
    snprintf(asset->name, sizeof(asset->name), "%s", name_text);
}

// Loads asset records from SD, creating an empty database on first launch.
static bool load_assets(MaintenanceApp* app) {
    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, asset_db_path(), FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        file = storage_file_alloc(app->storage);
        bool created = storage_file_open(file, asset_db_path(), FSAM_WRITE, FSOM_CREATE_NEW);
        if(created) created = storage_file_close(file);
        storage_file_free(file);
        if(!created) FURI_LOG_E(TAG, "Cannot create asset database");
        return created;
    }

    char line[128];
    uint8_t line_length = 0;
    char chunk[64];
    uint16_t count;
    while((count = storage_file_read(file, chunk, sizeof(chunk))) > 0) {
        for(uint16_t i = 0; i < count; i++) {
            if(chunk[i] == '\n') {
                line[line_length] = '\0';
                if(line_length) parse_asset_line(app, line);
                line_length = 0;
            } else if((line_length + 1U) < sizeof(line)) {
                line[line_length++] = chunk[i];
            } else {
                line_length = 0;
            }
        }
    }
    if(line_length) {
        line[line_length] = '\0';
        parse_asset_line(app, line);
    }

    bool success = storage_file_close(file);
    storage_file_free(file);
    return success;
}

// Resets the service list to defaults, then appends this asset's custom entries.
static void load_services(MaintenanceApp* app) {
    app->service_count = standard_service_count;
    for(uint8_t i = 0; i < standard_service_count; i++) {
        snprintf(app->services[i], SERVICE_NAME_SIZE, "%s", standard_services[i]);
    }
    if(app->selected_asset < 0) return;

    Asset* asset = &app->assets[(uint8_t)app->selected_asset];
    char path[64];
    asset_services_path(asset, path, sizeof(path));
    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        return;
    }

    char line[SERVICE_NAME_SIZE];
    uint8_t line_length = 0;
    char chunk[32];
    uint16_t count;
    while((count = storage_file_read(file, chunk, sizeof(chunk))) > 0) {
        for(uint16_t i = 0; i < count; i++) {
            if(chunk[i] == '\n') {
                line[line_length] = '\0';
                if(line_length && app->service_count < SERVICE_LIMIT) {
                    line[strcspn(line, "\r")] = '\0';
                    if(line[0]) {
                        snprintf(
                            app->services[app->service_count++],
                            SERVICE_NAME_SIZE,
                            "%s",
                            line);
                    }
                }
                line_length = 0;
            } else if((line_length + 1U) < sizeof(line)) {
                line[line_length++] = chunk[i];
            } else {
                line_length = 0;
            }
        }
    }
    if(line_length && app->service_count < SERVICE_LIMIT) {
        line[line_length] = '\0';
        line[strcspn(line, "\r")] = '\0';
        if(line[0]) {
            snprintf(app->services[app->service_count++], SERVICE_NAME_SIZE, "%s", line);
        }
    }
    storage_file_close(file);
    storage_file_free(file);
}

// Appends a user-defined service name to the selected asset's service file.
static bool append_custom_service(MaintenanceApp* app, const char* service) {
    Asset* asset = &app->assets[(uint8_t)app->selected_asset];
    char path[64];
    asset_services_path(asset, path, sizeof(path));
    File* file = storage_file_alloc(app->storage);
    bool success = false;
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        size_t length = strlen(service);
        success = (storage_file_write(file, service, (uint16_t)length) == length) &&
                  (storage_file_write(file, "\n", 1) == 1);
        if(!storage_file_close(file)) success = false;
    } else {
        FURI_LOG_E(TAG, "Cannot open custom service file");
    }
    storage_file_free(file);
    return success;
}

// Appends the selected maintenance items and uptime timestamp to the asset log.
static bool append_service_log(MaintenanceApp* app) {
    Asset* asset = &app->assets[(uint8_t)app->selected_asset];
    char path[64];
    asset_log_path(asset, path, sizeof(path));
    File* file = storage_file_alloc(app->storage);
    bool success = false;
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char line[256];
        int length = snprintf(
            line,
            sizeof(line),
            "T+%lu s  ",
            (unsigned long)(furi_get_tick() / furi_kernel_get_tick_frequency()));
        for(uint8_t i = 0; (i < app->service_count) && (length > 0) &&
                           ((size_t)length < sizeof(line) - 2);
            i++) {
            if(app->service_checked[i]) {
                int added = snprintf(
                    line + length,
                    sizeof(line) - (size_t)length,
                    "%s%s",
                    (length > 22) ? ", " : "",
                    app->services[i]);
                if(added < 0) {
                    length = -1;
                    break;
                }
                length += added;
            }
        }
        if(length > 0) {
            if((size_t)length < sizeof(line) - 1) line[length++] = '\n';
            success = storage_file_write(file, line, (uint16_t)length) == (uint16_t)length;
        }
        if(!storage_file_close(file)) success = false;
    } else {
        FURI_LOG_E(TAG, "Cannot open service log");
    }
    storage_file_free(file);
    return success;
}

// Adds a uniquely numbered asset and persists it, rolling back if saving fails.
static bool add_asset(MaintenanceApp* app, const char* name) {
    if(app->asset_count >= ASSET_LIMIT) return false;
    Asset* asset = &app->assets[app->asset_count];
    memset(asset, 0, sizeof(*asset));
    uint32_t next_id = 1;
    for(uint8_t i = 0; i < app->asset_count; i++) {
        if(app->assets[i].id >= next_id) next_id = app->assets[i].id + 1;
    }
    asset->id = next_id;
    snprintf(asset->name, sizeof(asset->name), "%s", name);
    snprintf(asset->last_service, sizeof(asset->last_service), "Never");
    app->asset_count++;
    if(!write_asset_database(app)) {
        app->asset_count--;
        return false;
    }
    return true;
}

// Builds the compact web URI containing lifetime hours and an optional service bitmask.
static bool build_service_uri(
    MaintenanceApp* app,
    char* buffer,
    size_t size,
    bool include_services) {
    uint32_t hours = app->chip_accumulated_seconds / 3600U;
    uint32_t mask = 0;
    if(include_services) {
        for(uint8_t i = 0; i < app->service_count; i++) {
            if(app->service_checked[i]) mask |= 1U << i;
        }
    }
    int length = snprintf(
        buffer,
        size,
        "https://m.io/?h=%lu&s=%04lx",
        (unsigned long)hours,
        (unsigned long)mask);
    return (length > 0) && ((size_t)length < size);
}

// Encodes an https URI as an NFC Forum Type 2 NDEF message with a URI record.
static bool encode_uri_ndef(
    const char* uri,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* encoded_size) {
    static const char uri_scheme[] = "https://";
    if(strncmp(uri, uri_scheme, sizeof(uri_scheme) - 1U) != 0) return false;

    const char* uri_suffix = uri + sizeof(uri_scheme) - 1U;
    size_t uri_suffix_size = strlen(uri_suffix);
    size_t payload_size = uri_suffix_size + 1U;
    size_t record_size = payload_size + 4U;
    size_t total_size = record_size + 3U;
    if((record_size > UINT8_MAX) || (total_size > buffer_size)) return false;

    buffer[0] = 0x03;
    buffer[1] = (uint8_t)record_size;
    buffer[2] = 0xD1;
    buffer[3] = 0x01;
    buffer[4] = (uint8_t)payload_size;
    buffer[5] = 'U';
    buffer[6] = 0x04;
    memcpy(buffer + 7, uri_suffix, uri_suffix_size);
    buffer[7U + uri_suffix_size] = 0xFE;
    *encoded_size = total_size;
    return true;
}

// Reads the tag after writing and saves its protocol data as a Flipper NFC file.
static bool save_nfc_snapshot(MaintenanceApp* app, Nfc* nfc) {
    MfUltralightData* tag_data = mf_ultralight_alloc();
    bool saved = false;
    if(mf_ultralight_poller_sync_read_card(nfc, tag_data, NULL) == MfUltralightErrorNone) {
        NfcDevice* device = nfc_device_alloc();
        nfc_device_set_data(device, NfcProtocolMfUltralight, tag_data);
        char path[64];
        asset_nfc_path(&app->assets[(uint8_t)app->selected_asset], path, sizeof(path));
        saved = nfc_device_save(device, path);
        nfc_device_free(device);
    }
    mf_ultralight_free(tag_data);
    return saved;
}

// Detects a Type 2 tag, validates writable capacity, writes and verifies the NDEF URI.
static NfcWriteResult write_uri_to_tag(MaintenanceApp* app, const char* uri) {
    uint8_t ndef[NFC_NDEF_BUFFER_SIZE];
    size_t ndef_size = 0;
    if(!encode_uri_ndef(uri, ndef, sizeof(ndef), &ndef_size)) return NfcWriteFailed;

    NfcWriteResult result = NfcWriteFailed;
    Nfc* nfc = nfc_alloc();
    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolMfUltralight);
    if(!nfc_poller_detect(poller)) {
        result = NfcWriteNoTag;
        goto cleanup;
    }
    nfc_poller_free(poller);
    poller = NULL;

    MfUltralightPage capability_page;
    MfUltralightPage lock_page;
    if((mf_ultralight_poller_sync_read_page(nfc, 3, &capability_page) !=
        MfUltralightErrorNone) ||
       (mf_ultralight_poller_sync_read_page(nfc, 2, &lock_page) != MfUltralightErrorNone)) {
        goto cleanup;
    }
    if(capability_page.data[0] != 0xE1) {
        result = NfcWriteUnsupportedTag;
        goto cleanup;
    }
    if(capability_page.data[3] || lock_page.data[2] || lock_page.data[3]) {
        result = NfcWriteReadOnly;
        goto cleanup;
    }

    size_t user_memory_size = (size_t)capability_page.data[2] * 8U;
    if(ndef_size > user_memory_size) {
        result = NfcWriteInsufficientSpace;
        goto cleanup;
    }

    MfUltralightPage invalidated_page = {.data = {0x03, 0x00, 0xFE, 0x00}};
    if(mf_ultralight_poller_sync_write_page(nfc, 4, &invalidated_page) !=
       MfUltralightErrorNone) {
        goto cleanup;
    }

    for(size_t offset = MF_ULTRALIGHT_PAGE_SIZE; offset < ndef_size;
        offset += MF_ULTRALIGHT_PAGE_SIZE) {
        MfUltralightPage page = {};
        size_t remaining = ndef_size - offset;
        size_t chunk_size =
            remaining < MF_ULTRALIGHT_PAGE_SIZE ? remaining : MF_ULTRALIGHT_PAGE_SIZE;
        memcpy(page.data, ndef + offset, chunk_size);
        uint16_t page_number = (uint16_t)(4U + offset / MF_ULTRALIGHT_PAGE_SIZE);
        if(mf_ultralight_poller_sync_write_page(nfc, page_number, &page) !=
           MfUltralightErrorNone) {
            goto cleanup;
        }
    }

    MfUltralightPage first_page = {};
    memcpy(first_page.data, ndef, sizeof(first_page.data));
    if(mf_ultralight_poller_sync_write_page(nfc, 4, &first_page) != MfUltralightErrorNone) {
        goto cleanup;
    }

    for(size_t offset = 0; offset < ndef_size; offset += MF_ULTRALIGHT_PAGE_SIZE) {
        MfUltralightPage actual_page;
        MfUltralightPage expected_page = {};
        size_t remaining = ndef_size - offset;
        size_t chunk_size =
            remaining < MF_ULTRALIGHT_PAGE_SIZE ? remaining : MF_ULTRALIGHT_PAGE_SIZE;
        memcpy(expected_page.data, ndef + offset, chunk_size);
        uint16_t page_number = (uint16_t)(4U + offset / MF_ULTRALIGHT_PAGE_SIZE);
        if((mf_ultralight_poller_sync_read_page(nfc, page_number, &actual_page) !=
            MfUltralightErrorNone) ||
           (memcmp(actual_page.data, expected_page.data, sizeof(actual_page.data)) != 0)) {
            result = NfcWriteVerifyFailed;
            goto cleanup;
        }
    }

    result = save_nfc_snapshot(app, nfc) ? NfcWriteSuccess : NfcWriteSnapshotFailed;

cleanup:
    if(poller) nfc_poller_free(poller);
    nfc_free(nfc);
    return result;
}

// Writes the requested payload to a tag, then displays status and the encoded URI.
static bool show_payload(MaintenanceApp* app, bool include_services) {
    char uri[URI_SIZE];
    if(!build_service_uri(app, uri, sizeof(uri), include_services)) {
        show_notice(app, "Could not build URI payload", false);
        return false;
    }

    NfcWriteResult result = write_uri_to_tag(app, uri);
    if((result != NfcWriteSuccess) && (result != NfcWriteSnapshotFailed)) {
        const char* message = "NFC write failed";
        if(result == NfcWriteNoTag) message = "No compatible NFC tag found";
        else if(result == NfcWriteUnsupportedTag) message = "Unsupported NFC tag";
        else if(result == NfcWriteReadOnly) message = "NFC tag is locked";
        else if(result == NfcWriteInsufficientSpace) message = "NFC tag has insufficient space";
        else if(result == NfcWriteVerifyFailed) message = "NFC write verification failed";
        show_notice(app, message, false);
        return false;
    }

    char payload[160];
    snprintf(
        payload,
        sizeof(payload),
        result == NfcWriteSuccess ? "NDEF tag written and saved to SD.\n\n%s" :
                                    "Tag written; SD snapshot failed.\n\n%s",
        uri);
    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, payload);
    text_box_set_focus(app->text_box, TextBoxFocusStart);
    switch_view(app, ViewHistory);
    return true;
}

// Reads a supported NTAG/Ultralight asset tag and returns its UID as uppercase hex.
static bool scan_asset_uid(char uid_text[ASSET_UID_TEXT_SIZE]) {
    Nfc* nfc = nfc_alloc();
    MfUltralightData* tag_data = mf_ultralight_alloc();
    bool success = false;
    if(mf_ultralight_poller_sync_read_card(nfc, tag_data, NULL) == MfUltralightErrorNone) {
        size_t uid_length = 0;
        const uint8_t* uid = mf_ultralight_get_uid(tag_data, &uid_length);
        if(uid && (uid_length > 0) && (uid_length <= (ASSET_UID_TEXT_SIZE - 1U) / 2U)) {
            for(size_t i = 0; i < uid_length; i++) {
                snprintf(uid_text + (i * 2U), 3, "%02X", (unsigned)uid[i]);
            }
            success = true;
        }
    }
    mf_ultralight_free(tag_data);
    nfc_free(nfc);
    return success;
}

// Captures a validated wall-clock time and converts it to the firmware timestamp.
static bool get_current_timestamp(DateTime* datetime, uint32_t* timestamp) {
    furi_hal_rtc_get_datetime(datetime);
    if(!datetime_validate_datetime(datetime)) return false;
    *timestamp = datetime_datetime_to_timestamp(datetime);
    return true;
}

// Applies a scanned tag as either Clock In or Clock Out and logs the transition.
static bool process_asset_scan(MaintenanceApp* app) {
    char uid[ASSET_UID_TEXT_SIZE] = {0};
    if(!scan_asset_uid(uid)) {
        show_notice(app, "Could not read NTAG/Ultralight tag", false);
        return false;
    }

    DateTime datetime = {0};
    uint32_t timestamp = 0;
    if(!get_current_timestamp(&datetime, &timestamp)) {
        show_notice(app, "Device date/time is invalid", false);
        return false;
    }

    if(app->tracker_state == TrackerStateIdle) {
        Asset* asset = &app->assets[(uint8_t)app->selected_asset];
        if(!append_tracker_event(
               app, &datetime, uid, "CLOCK IN", 0, app->chip_accumulated_seconds)) {
            show_notice(app, "Could not save Clock In event", false);
            return false;
        }
        app->session_start_timestamp = timestamp;
        snprintf(app->session_uid, sizeof(app->session_uid), "%s", uid);
        snprintf(app->session_asset_name, sizeof(app->session_asset_name), "%s", asset->name);
        app->tracker_state = TrackerStateRunning;
        return true;
    }

    if(strcmp(uid, app->session_uid) != 0) {
        show_notice(app, "Scan the same asset tag to Clock Out", false);
        return false;
    }
    if(timestamp < app->session_start_timestamp) {
        show_notice(app, "Clock is earlier than Clock In time", false);
        return false;
    }

    uint32_t duration = timestamp - app->session_start_timestamp;
    if((UINT32_MAX - app->chip_accumulated_seconds) < duration) {
        show_notice(app, "Accumulated runtime limit reached", false);
        return false;
    }
    uint32_t new_total = app->chip_accumulated_seconds + duration;
    if(!append_tracker_event(
           app, &datetime, uid, "CLOCK OUT", duration, new_total)) {
        show_notice(app, "Could not save Clock Out event", false);
        return false;
    }

    app->chip_accumulated_seconds = new_total;
    app->session_start_timestamp = 0;
    app->session_uid[0] = '\0';
    app->session_asset_name[0] = '\0';
    app->tracker_state = TrackerStateIdle;
    return true;
}

// Draws only the tracker state, stored asset/UID text, and accumulated runtime.
static void draw_tracker(Canvas* canvas, void* context) {
    MaintenanceApp* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Clock In / Clock Out");
    canvas_set_font(canvas, FontSecondary);
    if(app->tracker_state == TrackerStateRunning) {
        canvas_draw_str(canvas, 3, 25, "State: RUNNING");
        canvas_draw_str(canvas, 3, 36, app->session_asset_name);
        canvas_draw_str(canvas, 3, 47, app->session_uid);
        canvas_draw_str(canvas, 3, 58, "OK: scan same tag to out");
    } else {
        Asset* asset = &app->assets[(uint8_t)app->selected_asset];
        char total[32];
        snprintf(total, sizeof(total), "Total: %lu sec", (unsigned long)app->chip_accumulated_seconds);
        canvas_draw_str(canvas, 3, 25, "State: IDLE");
        canvas_draw_str(canvas, 3, 36, asset->name);
        canvas_draw_str(canvas, 3, 47, total);
        canvas_draw_str(canvas, 3, 58, "OK: scan tag to clock in");
    }
}

// Scans a tag when OK is pressed; Back returns to the selected asset profile.
static bool tracker_input(InputEvent* event, void* context) {
    MaintenanceApp* app = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyBack) {
        switch_view(app, ViewProfile);
        return true;
    }
    if(event->key == InputKeyOk) {
        if(process_asset_scan(app)) view_commit_model(app->tracker_view, true);
        return true;
    }
    return false;
}

// Draws the scrollable equipment selector and the selected asset's last-service date.
static void draw_equipment(Canvas* canvas, void* context) {
    MaintenanceApp* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Equipment");
    if(app->asset_count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 4, 30, "No profiles saved");
        canvas_draw_str(canvas, 4, 43, "Back to add one");
        return;
    }

    uint8_t first = app->selected_asset >= 2 ? (uint8_t)app->selected_asset - 1U : 0U;
    for(uint8_t row = 0; (row < 2) && ((first + row) < app->asset_count); row++) {
        uint8_t index = first + row;
        uint8_t y = 27 + row * 18;
        if(index == (uint8_t)app->selected_asset) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y - 8, 127, 17);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 3, y, app->assets[index].name);
        char date[24];
        snprintf(date, sizeof(date), "Last: %s", app->assets[index].last_service);
        canvas_draw_str(canvas, 3, y + 8, date);
        canvas_set_color(canvas, ColorBlack);
    }
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 62, "Up/Down  OK: open");
}

// Draws the service checklist, selection state, and final write action.
static void draw_checklist(Canvas* canvas, void* context) {
    MaintenanceApp* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, "Service checklist");
    uint8_t item_count = app->service_count + 1U;
    uint8_t first = app->checklist_selection >= 4 ? app->checklist_selection - 3U : 0U;
    for(uint8_t row = 0; (row < 4) && ((first + row) < item_count); row++) {
        uint8_t index = first + row;
        uint8_t y = 22 + row * 10;
        bool selected = index == app->checklist_selection;
        if(selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y - 8, 127, 10);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }
        canvas_set_font(canvas, FontSecondary);
        if(index < app->service_count) {
            char line[32];
            snprintf(
                line,
                sizeof(line),
                "%c %s",
                app->service_checked[index] ? 'x' : ' ',
                app->services[index]);
            canvas_draw_str(canvas, 3, y, line);
        } else {
            canvas_draw_str(canvas, 3, y, "Write NFC + save log");
        }
        canvas_set_color(canvas, ColorBlack);
    }
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 62, "OK: toggle/write  Back");
}

// Renders the app's short status or error message.
static void draw_notice(Canvas* canvas, void* context) {
    MaintenanceApp* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Maintenance");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 3, 29, app->notice);
    canvas_draw_str(canvas, 3, 57, "OK/Back to continue");
}

// Handles equipment-list navigation and opens the selected asset profile.
static bool equipment_input(InputEvent* event, void* context) {
    MaintenanceApp* app = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyUp) {
        if(app->selected_asset > 0) app->selected_asset--;
        view_commit_model(app->equipment_view, true);
        return true;
    }
    if(event->key == InputKeyDown) {
        if((app->asset_count > 0) && ((uint8_t)(app->selected_asset + 1) < app->asset_count)) {
            app->selected_asset++;
        }
        view_commit_model(app->equipment_view, true);
        return true;
    }
    if(event->key == InputKeyOk) {
        if((app->asset_count > 0) && (app->selected_asset >= 0)) {
            load_services(app);
            rebuild_profile_menu(app);
            switch_view(app, ViewProfile);
        }
        return true;
    }
    return false;
}

// Toggles service selections, saves the log once, and retries NFC writing as needed.
static bool checklist_input(InputEvent* event, void* context) {
    MaintenanceApp* app = context;
    if(event->type != InputTypeShort) return false;
    uint8_t item_count = app->service_count + 1U;
    if(event->key == InputKeyUp) {
        if(app->checklist_selection > 0) app->checklist_selection--;
        view_commit_model(app->checklist_view, true);
        return true;
    }
    if(event->key == InputKeyDown) {
        if(app->checklist_selection + 1U < item_count) app->checklist_selection++;
        view_commit_model(app->checklist_view, true);
        return true;
    }
    if(event->key == InputKeyOk) {
        if(app->checklist_selection < app->service_count) {
            if(app->checklist_log_saved) {
                show_notice(app, "Log saved; retry NFC or go Back", false);
                return true;
            }
            app->service_checked[app->checklist_selection] =
                !app->service_checked[app->checklist_selection];
            view_commit_model(app->checklist_view, true);
        } else {
            bool selected_any = false;
            for(uint8_t i = 0; i < app->service_count; i++) {
                selected_any |= app->service_checked[i];
            }
            if(!selected_any) {
                show_notice(app, "Select a service first", false);
                return true;
            }
            if(!app->checklist_log_saved) {
                if(!append_service_log(app)) {
                    show_notice(app, "Could not save SD log", false);
                    return true;
                }
                app->checklist_log_saved = true;
            }
            format_uptime(app->assets[(uint8_t)app->selected_asset].last_service);
            if(!write_asset_database(app)) {
                FURI_LOG_E(TAG, "Could not update asset database");
            }
            if(show_payload(app, true)) app->checklist_log_saved = false;
        }
        return true;
    }
    return false;
}

// Dismisses a notice and restores its recorded return view.
static bool notice_input(InputEvent* event, void* context) {
    MaintenanceApp* app = context;
    if((event->type != InputTypeShort) || (event->key != InputKeyOk)) return false;
    if(app->notice_dismiss_exits) {
        view_dispatcher_stop(app->dispatcher);
    } else {
        switch_view(app, app->notice_return_view);
    }
    return true;
}

// Displays a notice and records where the user should return when it is dismissed.
static void show_notice(MaintenanceApp* app, const char* message, bool exits) {
    snprintf(app->notice, sizeof(app->notice), "%s", message);
    app->notice_dismiss_exits = exits;
    app->notice_return_view = app->current_view;
    if(app->current_view == ViewInput) {
        app->notice_return_view =
            app->input_kind == InputNewAsset ? ViewMain : ViewProfile;
    }
    switch_view(app, ViewNotice);
}

// Rebuilds the selected asset menu with its clock-tracking action.
static void rebuild_profile_menu(MaintenanceApp* app) {
    submenu_reset(app->profile_menu);
    Asset* asset = &app->assets[(uint8_t)app->selected_asset];
    char header[64];
    snprintf(header, sizeof(header), "%s", asset->name);
    submenu_set_header(app->profile_menu, header);
    submenu_add_item(app->profile_menu, "View History/Log", 0, profile_menu_callback, app);
    submenu_add_item(app->profile_menu, "Log Service Action", 1, profile_menu_callback, app);
    submenu_add_item(
        app->profile_menu, "Add Custom Service Type", 2, profile_menu_callback, app);
    submenu_add_item(app->profile_menu, "Clock In / Out: Scan Tag", 3, profile_menu_callback, app);
    submenu_add_item(app->profile_menu, "Write hours to NFC tag", 4, profile_menu_callback, app);
}

// Routes main-menu selections to equipment browsing or new-asset input.
static void main_menu_callback(void* context, uint32_t index) {
    MaintenanceApp* app = context;
    if(index == 0) {
        app->selected_asset = app->asset_count ? 0 : -1;
        switch_view(app, ViewEquipment);
    } else if(index == 1) {
        app->input_kind = InputNewAsset;
        memset(app->input_buffer, 0, sizeof(app->input_buffer));
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Equipment name");
        text_input_set_result_callback(
            app->text_input,
            text_input_callback,
            app,
            app->input_buffer,
            sizeof(app->input_buffer),
            true);
        switch_view(app, ViewInput);
    }
}

// Validates text input, then creates an asset or appends a custom service.
static void text_input_callback(void* context) {
    MaintenanceApp* app = context;
    if((app->input_buffer[0] == '\0') ||
       (strchr(app->input_buffer, '\t') != NULL) ||
       (strchr(app->input_buffer, '\n') != NULL) ||
       (strchr(app->input_buffer, '\r') != NULL)) {
        show_notice(app, "Name cannot be empty", false);
        return;
    }

    if(app->input_kind == InputNewAsset) {
        if(add_asset(app, app->input_buffer)) {
            app->selected_asset = (int8_t)(app->asset_count - 1U);
            switch_view(app, ViewMain);
        } else {
            show_notice(app, "Could not save equipment", false);
        }
    } else if(app->input_kind == InputNewService) {
        if(strlen(app->input_buffer) >= SERVICE_NAME_SIZE) {
            show_notice(app, "Service name is too long", false);
        } else if(app->service_count >= SERVICE_LIMIT) {
            show_notice(app, "Service list is full", false);
        } else if(append_custom_service(app, app->input_buffer)) {
            load_services(app);
            rebuild_profile_menu(app);
            switch_view(app, ViewProfile);
        } else {
            show_notice(app, "Could not save service", false);
        }
    }
}

// Handles profile actions: history, checklist, custom service, clock tracking, and NFC write.
static void profile_menu_callback(void* context, uint32_t index) {
    MaintenanceApp* app = context;
    Asset* asset = &app->assets[(uint8_t)app->selected_asset];
    if(index == 0) {
        char path[64];
        asset_log_path(asset, path, sizeof(path));
        File* file = storage_file_alloc(app->storage);
        size_t used = 0;
        app->history[0] = '\0';
        if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            while((used + 1 < sizeof(app->history)) &&
                  !storage_file_eof(file)) {
                uint16_t read = storage_file_read(
                    file, app->history + used, (uint16_t)(sizeof(app->history) - used - 1));
                if(read == 0) break;
                used += read;
            }
            app->history[used] = '\0';
            storage_file_close(file);
        } else {
            snprintf(app->history, sizeof(app->history), "No service log saved yet.");
        }
        storage_file_free(file);
        text_box_reset(app->text_box);
        text_box_set_text(app->text_box, app->history);
        text_box_set_focus(app->text_box, TextBoxFocusEnd);
        switch_view(app, ViewHistory);
    } else if(index == 1) {
        memset(app->service_checked, 0, sizeof(app->service_checked));
        app->checklist_log_saved = false;
        app->checklist_selection = 0;
        load_services(app);
        view_commit_model(app->checklist_view, true);
        switch_view(app, ViewChecklist);
    } else if(index == 2) {
        app->input_kind = InputNewService;
        memset(app->input_buffer, 0, sizeof(app->input_buffer));
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "New service type");
        text_input_set_result_callback(
            app->text_input,
            text_input_callback,
            app,
            app->input_buffer,
            sizeof(app->input_buffer),
            true);
        switch_view(app, ViewInput);
    } else if(index == 3) {
        switch_view(app, ViewTracker);
    } else if(index == 4) {
        show_payload(app, false);
    }
}

// Implements Back navigation and prevents exiting with an unfinished clocked-in session.
static bool navigation_callback(void* context) {
    MaintenanceApp* app = context;
    uint32_t current = app->current_view;
    if(current == ViewMain) {
        if(app->tracker_state == TrackerStateRunning) {
            show_notice(app, "Clock Out before exiting", false);
            return true;
        }
        return false;
    }
    if(current == ViewEquipment) {
        switch_view(app, ViewMain);
    } else if(current == ViewProfile) {
        switch_view(app, ViewEquipment);
    } else if(current == ViewHistory) {
        switch_view(app, ViewProfile);
    } else if(current == ViewChecklist) {
        app->checklist_log_saved = false;
        switch_view(app, ViewProfile);
    } else if(current == ViewTracker) {
        switch_view(app, ViewProfile);
    } else if(current == ViewInput) {
        switch_view(app, app->input_kind == InputNewAsset ? ViewMain : ViewProfile);
    } else if(current == ViewNotice) {
        if(app->notice_dismiss_exits) {
            view_dispatcher_stop(app->dispatcher);
        } else {
            switch_view(app, app->notice_return_view);
        }
    } else {
        return false;
    }
    return true;
}

// Allocates app state, opens storage, builds views, and attaches the dispatcher to the GUI.
static MaintenanceApp* maintenance_tracker_alloc(void) {
    MaintenanceApp* app = malloc(sizeof(MaintenanceApp));
    furi_check(app);
    memset(app, 0, sizeof(*app));
    app->selected_asset = -1;
    app->storage = furi_record_open(RECORD_STORAGE);
    app->storage_ready =
        ensure_data_directory(app) && load_assets(app) && load_tracker_total(app);
    if(!app->storage_ready) {
        FURI_LOG_E(TAG, "Failed to initialize app data");
    }

    Gui* gui = furi_record_open(RECORD_GUI);
    app->dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->dispatcher);
    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->dispatcher, navigation_callback);
    view_dispatcher_attach_to_gui(app->dispatcher, gui, ViewDispatcherTypeFullscreen);

    app->main_menu = submenu_alloc();
    submenu_set_header(app->main_menu, "Maintenance Tracker");
    submenu_add_item(app->main_menu, "Show Equipment", 0, main_menu_callback, app);
    submenu_add_item(app->main_menu, "Add Equipment", 1, main_menu_callback, app);
    view_dispatcher_add_view(
        app->dispatcher, ViewMain, submenu_get_view(app->main_menu));

    app->profile_menu = submenu_alloc();
    view_dispatcher_add_view(
        app->dispatcher, ViewProfile, submenu_get_view(app->profile_menu));

    app->text_input = text_input_alloc();
    text_input_set_result_callback(
        app->text_input,
        text_input_callback,
        app,
        app->input_buffer,
        sizeof(app->input_buffer),
        true);
    view_dispatcher_add_view(
        app->dispatcher, ViewInput, text_input_get_view(app->text_input));

    app->text_box = text_box_alloc();
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_add_view(
        app->dispatcher, ViewHistory, text_box_get_view(app->text_box));

    app->equipment_view = view_alloc();
    view_set_context(app->equipment_view, app);
    view_set_draw_callback(app->equipment_view, draw_equipment);
    view_set_input_callback(app->equipment_view, equipment_input);
    view_allocate_model(app->equipment_view, ViewModelTypeLockFree, 1);
    view_dispatcher_add_view(app->dispatcher, ViewEquipment, app->equipment_view);

    app->checklist_view = view_alloc();
    view_set_context(app->checklist_view, app);
    view_set_draw_callback(app->checklist_view, draw_checklist);
    view_set_input_callback(app->checklist_view, checklist_input);
    view_allocate_model(app->checklist_view, ViewModelTypeLockFree, 1);
    view_dispatcher_add_view(app->dispatcher, ViewChecklist, app->checklist_view);

    app->notice_view = view_alloc();
    view_set_context(app->notice_view, app);
    view_set_draw_callback(app->notice_view, draw_notice);
    view_set_input_callback(app->notice_view, notice_input);
    view_allocate_model(app->notice_view, ViewModelTypeLockFree, 1);
    view_dispatcher_add_view(app->dispatcher, ViewNotice, app->notice_view);

    app->tracker_view = view_alloc();
    view_set_context(app->tracker_view, app);
    view_set_draw_callback(app->tracker_view, draw_tracker);
    view_set_input_callback(app->tracker_view, tracker_input);
    view_allocate_model(app->tracker_view, ViewModelTypeLockFree, 1);
    view_dispatcher_add_view(app->dispatcher, ViewTracker, app->tracker_view);

    furi_record_close(RECORD_GUI);
    return app;
}

// Releases every view, module, dispatcher, and firmware record acquired during allocation.
static void maintenance_tracker_free(MaintenanceApp* app) {
    view_dispatcher_remove_view(app->dispatcher, ViewMain);
    view_dispatcher_remove_view(app->dispatcher, ViewEquipment);
    view_dispatcher_remove_view(app->dispatcher, ViewProfile);
    view_dispatcher_remove_view(app->dispatcher, ViewInput);
    view_dispatcher_remove_view(app->dispatcher, ViewHistory);
    view_dispatcher_remove_view(app->dispatcher, ViewChecklist);
    view_dispatcher_remove_view(app->dispatcher, ViewNotice);
    view_dispatcher_remove_view(app->dispatcher, ViewTracker);
    submenu_free(app->main_menu);
    submenu_free(app->profile_menu);
    text_input_free(app->text_input);
    text_box_free(app->text_box);
    view_free(app->equipment_view);
    view_free(app->checklist_view);
    view_free(app->notice_view);
    view_free(app->tracker_view);
    view_dispatcher_free(app->dispatcher);
    furi_record_close(RECORD_STORAGE);
    free(app);
}

// Application entry point: initialize, run the view dispatcher, then release app resources.
int32_t maintenance_tracker_app(void* context) {
    UNUSED(context);
    MaintenanceApp* app = maintenance_tracker_alloc();
    switch_view(app, ViewMain);
    if(!app->storage_ready) {
        show_notice(app, "Storage unavailable; check SD card", false);
        app->notice_return_view = ViewMain;
    }
    view_dispatcher_run(app->dispatcher);
    maintenance_tracker_free(app);
    return 0;
}
