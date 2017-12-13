/*
 * Copyright (c) 2017 Snowflake Computing, Inc. All rights reserved.
 */

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <snowflake_client.h>
#include <openssl/crypto.h>
#include "constants.h"
#include "snowflake_client_int.h"
#include "connection.h"
#include "snowflake_memory.h"
#include <log.h>
#include "results.h"
#include "error.h"

#define curl_easier_escape(curl, string) curl_easy_escape(curl, string, 0)

// Define internal constants
sf_bool DISABLE_VERIFY_PEER;
char *CA_BUNDLE_FILE;
int32 SSL_VERSION;
sf_bool DEBUG;


static char *LOG_PATH = NULL;
static FILE *LOG_FP = NULL;

#define _SF_STMT_TYPE_DML 0x3000
#define _SF_STMT_TYPE_INSERT (_SF_STMT_TYPE_DML + 0x100)
#define _SF_STMT_TYPE_UPDATE (_SF_STMT_TYPE_DML + 0x200)
#define _SF_STMT_TYPE_DELETE (_SF_STMT_TYPE_DML + 0x300)
#define _SF_STMT_TYPE_MERGE (_SF_STMT_TYPE_DML + 0x400)
#define _SF_STMT_TYPE_MULTI_TABLE_INSERT (_SF_STMT_TYPE_DML + 0x500)

static sf_bool detect_stmt_type(int64 stmt_type_id) {
    sf_bool ret;
    switch (stmt_type_id) {
        case _SF_STMT_TYPE_DML:
        case _SF_STMT_TYPE_INSERT:
        case _SF_STMT_TYPE_UPDATE:
        case _SF_STMT_TYPE_DELETE:
        case _SF_STMT_TYPE_MERGE:
        case _SF_STMT_TYPE_MULTI_TABLE_INSERT:
            ret = SF_BOOLEAN_TRUE;
            break;
        default:
            ret = SF_BOOLEAN_FALSE;
            break;
    }
    return ret;
}

/*
 * Convenience method to find string size, create buffer, copy over, and return.
 */
void alloc_buffer_and_copy(char **var, const char *str) {
    size_t str_size;
    SF_FREE(*var);
    // If passed in string is null, then return since *var is already null from being freed
    if (str) {
        str_size = strlen(str) + 1; // For null terminator
        *var = (char *) SF_CALLOC(1, str_size);
        strncpy(*var, str, str_size);
    }
}

int mkpath(char* file_path, mode_t mode) {
    assert(file_path && *file_path);
    char* p;
    for (p=strchr(file_path+1, '/'); p; p=strchr(p+1, '/')) {
        *p='\0';
        if (mkdir(file_path, mode)==-1) {
            if (errno!=EEXIST) { *p='/'; return -1; }
        }
        *p='/';
    }
    return 0;
}

/*
 * Initializes logging file
 */
sf_bool STDCALL log_init(const char *log_path) {
    sf_bool ret = SF_BOOLEAN_FALSE;
    time_t current_time;
    struct tm * time_info;
    char time_str[15];
    time(&current_time);
    time_info = localtime(&current_time);
    strftime(time_str, sizeof(time_str), "%Y%m%d%H%M%S", time_info);
    const char *sf_log_path;
    size_t log_path_size = 1; //Start with 1 to include null terminator
    log_path_size += strlen(time_str);
    if (log_path) {
        sf_log_path = log_path;
    } else {
        sf_log_path = getenv("SNOWFLAKE_LOG_PATH");
    }
    // Set logging level
    if (DEBUG) {
        log_set_quiet(SF_BOOLEAN_FALSE);
    } else {
        log_set_quiet(SF_BOOLEAN_TRUE);
    }
    log_set_level(LOG_TRACE);
    // If log path is specified, use absolute path. Otherwise set logging dir to be relative to current directory
    if (sf_log_path) {
        log_path_size += strlen(sf_log_path);
        log_path_size += 16; // Size of static format characters
        LOG_PATH = (char *) SF_CALLOC(1, log_path_size);
        snprintf(LOG_PATH, log_path_size, "%s/.capi/logs/%s.txt", sf_log_path, (char *)time_str);
    } else {
        log_path_size += 9; // Size of static format characters
        LOG_PATH = (char *) SF_CALLOC(1, log_path_size);
        snprintf(LOG_PATH, log_path_size, "logs/%s.txt", (char *)time_str);
    }
    if (LOG_PATH != NULL) {
        // Create log file path (if it already doesn't exist)
        if (mkpath(LOG_PATH, 0755) == -1) {
            fprintf(stderr, "Error creating log directory. Error code: %s\n", strerror(errno));
            goto cleanup;
        }
        // Open log file
        LOG_FP = fopen(LOG_PATH, "w+");
        if (LOG_FP) {
            // Set log file
            log_set_fp(LOG_FP);
        } else {
            fprintf(stderr, "Error opening file from file path: %s\nError code: %s\n", LOG_PATH, strerror(errno));
            goto cleanup;
        }

    } else {
        fprintf(stderr, "Log path is NULL. Was there an error during path construction?\n");
        goto cleanup;
    }

    ret = SF_BOOLEAN_TRUE;

cleanup:
    return ret;
}

/*
 * Cleans up memory allocated for log init and closes log file.
 */
void STDCALL log_term() {
    SF_FREE(LOG_PATH);
    if (LOG_FP) {
        fclose(LOG_FP);
        log_set_fp(NULL);
    }
}

SF_STATUS STDCALL snowflake_global_init(const char *log_path) {
    SF_STATUS ret = SF_STATUS_ERROR;
    CURLcode curl_ret;

    // Initialize constants
    DISABLE_VERIFY_PEER = SF_BOOLEAN_FALSE;
    CA_BUNDLE_FILE = NULL;
    SSL_VERSION = CURL_SSLVERSION_TLSv1_2;
    DEBUG = SF_BOOLEAN_FALSE;

    // TODO Add log init error handling
    if (!log_init(log_path)) {
        log_fatal("Error during log initialization");
        goto cleanup;
    }
    curl_ret = curl_global_init(CURL_GLOBAL_DEFAULT);
    if(curl_ret != CURLE_OK) {
        log_fatal("curl_global_init() failed: %s", curl_easy_strerror(curl_ret));
        goto cleanup;
    }

    ret = SF_STATUS_SUCCESS;

cleanup:
    return ret;
}

SF_STATUS STDCALL snowflake_global_term() {
    log_term();
    curl_global_cleanup();

    // Cleanup Constants
    SF_FREE(CA_BUNDLE_FILE);

    sf_alloc_map_to_log(SF_BOOLEAN_TRUE);
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_global_set_attribute(SF_GLOBAL_ATTRIBUTE type, const void *value) {
    switch (type) {
        case SF_GLOBAL_DISABLE_VERIFY_PEER:
            DISABLE_VERIFY_PEER = *(sf_bool *) value;
            break;
        case SF_GLOBAL_CA_BUNDLE_FILE:
            alloc_buffer_and_copy(&CA_BUNDLE_FILE, value);
            break;
        case SF_GLOBAL_SSL_VERSION:
            SSL_VERSION = *(int32 *) value;
            break;
        case SF_GLOBAL_DEBUG:
            DEBUG = *(sf_bool *) value;
            if (DEBUG) {
                log_set_quiet(SF_BOOLEAN_FALSE);
                log_set_level(LOG_TRACE);
            } else {
                log_set_quiet(SF_BOOLEAN_TRUE);
                log_set_level(LOG_INFO);
            }
            break;
        default:
            break;
    }
}

SF_CONNECT *STDCALL snowflake_init() {
    SF_CONNECT *sf = (SF_CONNECT *) SF_CALLOC(1, sizeof(SF_CONNECT));

    // Make sure memory was actually allocated
    if (sf) {
        // Initialize object with default values
        sf->host = NULL;
        sf->port = NULL;
        sf->user = NULL;
        sf->password = NULL;
        sf->database = NULL;
        sf->account = NULL;
        sf->role = NULL;
        sf->warehouse = NULL;
        sf->schema = NULL;
        alloc_buffer_and_copy(&sf->protocol, "https");
        sf->passcode = NULL;
        sf->passcode_in_password = SF_BOOLEAN_FALSE;
        sf->insecure_mode = SF_BOOLEAN_FALSE;
        sf->autocommit = SF_BOOLEAN_FALSE;
        sf->token = NULL;
        sf->master_token = NULL;
        sf->login_timeout = 120;
        sf->network_timeout = 0;
        sf->sequence_counter = 0;
        uuid4_generate(sf->request_id);
        clear_snowflake_error(&sf->error);
    }

    return sf;
}

void STDCALL snowflake_term(SF_CONNECT *sf) {
    // Ensure object is not null
    if (sf) {
        clear_snowflake_error(&sf->error);
        SF_FREE(sf->host);
        SF_FREE(sf->port);
        SF_FREE(sf->user);
        SF_FREE(sf->password);
        SF_FREE(sf->database);
        SF_FREE(sf->account);
        SF_FREE(sf->role);
        SF_FREE(sf->warehouse);
        SF_FREE(sf->schema);
        SF_FREE(sf->protocol);
        SF_FREE(sf->passcode);
        SF_FREE(sf->master_token);
        SF_FREE(sf->token);
    }
    SF_FREE(sf);
}

SF_STATUS STDCALL snowflake_connect(SF_CONNECT *sf) {
    sf_bool success = SF_BOOLEAN_FALSE;
    SF_JSON_ERROR json_error;
    if (!sf) {
        return SF_STATUS_ERROR;
    }
    // Reset error context
    clear_snowflake_error(&sf->error);
    cJSON *body = NULL;
    cJSON *data = NULL;
    cJSON *resp = NULL;
    char *s_body = NULL;
    char *s_resp = NULL;
    // Encoded URL to use with libcurl
    URL_KEY_VALUE url_params[] = {
            {"request_id=", sf->request_id, NULL, NULL, 0, 0},
            {"&databaseName=", sf->database, NULL, NULL, 0, 0},
            {"&schemaName=", sf->schema, NULL, NULL, 0, 0},
            {"&warehouse=", sf->warehouse, NULL, NULL, 0, 0},
            {"&roleName=", sf->role, NULL, NULL, 0, 0},
    };
    SF_STATUS ret = SF_STATUS_ERROR;

    if(is_string_empty(sf->user) || is_string_empty(sf->account)) {
        // Invalid connection
        log_error("Missing essential connection parameters. Either user or account (or both) are missing");
        SET_SNOWFLAKE_ERROR(&sf->error,
                            SF_ERROR_BAD_CONNECTION_PARAMS,
                            "Missing essential connection parameters. Either user or account (or both) are missing",
                            SF_SQLSTATE_UNABLE_TO_CONNECT);
        goto cleanup;
    }

    // Create body
    body = create_auth_json_body(sf, "C API", "C API", "0.1");
    log_debug("Created body");
    s_body = cJSON_Print(body);
    // TODO delete password before printing
    if (DEBUG) {
        log_trace("Here is constructed body:\n%s", s_body);
    }

    // Send request and get data
    if (request(sf, &resp, SESSION_URL, url_params, 5, s_body, NULL, POST_REQUEST_TYPE, &sf->error)) {
        s_resp = cJSON_Print(resp);
        log_trace("Here is JSON response:\n%s", s_resp);
        if ((json_error = json_copy_bool(&success, resp, "success")) != SF_JSON_ERROR_NONE ) {
            log_error("JSON error: %d", json_error);
            SET_SNOWFLAKE_ERROR(&sf->error, SF_ERROR_BAD_JSON, "No valid JSON response", SF_SQLSTATE_UNABLE_TO_CONNECT);
            goto cleanup;
        }
        if (!success) {
            cJSON *messageJson = cJSON_GetObjectItem(resp, "message");
            char *message = NULL;
            cJSON *codeJson = NULL;
            int64 code = -1;
            if (messageJson) {
                message = messageJson->valuestring;
            }
            codeJson = cJSON_GetObjectItem(resp, "code");
            if (codeJson) {
                code = (int64)atoi(codeJson->valuestring);
            } else {
                log_debug("no code element.");
            }

            SET_SNOWFLAKE_ERROR(&sf->error, code,
                                message ? message : "Query was not successful",
                                SF_SQLSTATE_UNABLE_TO_CONNECT);
            goto cleanup;
        }

        data = cJSON_GetObjectItem(resp, "data");
        if (!set_tokens(sf, data, "token", "masterToken", &sf->error)) {
            goto cleanup;
        }
    } else {
        log_error("No response");
        SET_SNOWFLAKE_ERROR(&sf->error, SF_ERROR_BAD_JSON, "No valid JSON response", SF_SQLSTATE_UNABLE_TO_CONNECT);
        goto cleanup;
    }

    /* we are done... */
    ret = SF_STATUS_SUCCESS;

cleanup:
    // Delete password and passcode for security's sake
    if (sf->password) {
        // Write over password in memory including null terminator
        memset(sf->password, 0, strlen(sf->password) + 1);
        SF_FREE(sf->password);
    }
    if (sf->passcode) {
        // Write over passcode in memory including null terminator
        memset(sf->passcode, 0, strlen(sf->passcode) + 1);
        SF_FREE(sf->passcode);
    }
    cJSON_Delete(body);
    cJSON_Delete(resp);
    SF_FREE(s_body);
    SF_FREE(s_resp);

    return ret;
}

SF_STATUS STDCALL snowflake_set_attr(
        SF_CONNECT *sf, SF_ATTRIBUTE type, const void *value) {
    if (!sf) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sf->error);
    switch (type) {
        case SF_CON_ACCOUNT:
            alloc_buffer_and_copy(&sf->account, value);
            break;
        case SF_CON_USER:
            alloc_buffer_and_copy(&sf->user, value);
            break;
        case SF_CON_PASSWORD:
            alloc_buffer_and_copy(&sf->password, value);
            break;
        case SF_CON_DATABASE:
            alloc_buffer_and_copy(&sf->database, value);
            break;
        case SF_CON_SCHEMA:
            alloc_buffer_and_copy(&sf->schema, value);
            break;
        case SF_CON_WAREHOUSE:
            alloc_buffer_and_copy(&sf->warehouse, value);
            break;
        case SF_CON_ROLE:
            alloc_buffer_and_copy(&sf->role, value);
            break;
        case SF_CON_HOST:
            alloc_buffer_and_copy(&sf->host, value);
            break;
        case SF_CON_PORT:
            alloc_buffer_and_copy(&sf->port, value);
            break;
        case SF_CON_PROTOCOL:
            alloc_buffer_and_copy(&sf->protocol, value);
            break;
        case SF_CON_PASSCODE:
            alloc_buffer_and_copy(&sf->passcode, value);
            break;
        case SF_CON_PASSCODE_IN_PASSWORD:
            sf->passcode_in_password = *((sf_bool *) value);
            break;
        case SF_CON_APPLICATION:
            // TODO Implement this
            break;
        case SF_CON_AUTHENTICATOR:
            // TODO Implement this
            break;
        case SF_CON_INSECURE_MODE:
            sf->insecure_mode = *((sf_bool *) value);
            break;
        case SF_SESSION_PARAMETER:
            // TODO Implement this
            break;
        case SF_CON_LOGIN_TIMEOUT:
            sf->login_timeout = *((int64 *) value);
            break;
        case SF_CON_NETWORK_TIMEOUT:
            sf->network_timeout = *((int64 *) value);
            break;
        case SF_CON_AUTOCOMMIT:
            sf->autocommit = *((sf_bool *) value);
            break;
        default:
            SET_SNOWFLAKE_ERROR(&sf->error, SF_ERROR_BAD_ATTRIBUTE_TYPE, "Invalid attribute type", SF_SQLSTATE_UNABLE_TO_CONNECT);
            return SF_STATUS_ERROR;
    }
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_get_attr(
        SF_CONNECT *sf, SF_ATTRIBUTE type, void **value) {
    if (!sf) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sf->error);
    //TODO Implement this
}

/**
 * Resets SNOWFLAKE_STMT parameters.
 *
 * @param sfstmt
 * @param free free allocated memory if true
 */
static void STDCALL _snowflake_stmt_reset(SF_STMT *sfstmt) {
    int64 i;
    clear_snowflake_error(&sfstmt->error);

    strncpy(sfstmt->sfqid, "", UUID4_LEN);
    uuid4_generate(sfstmt->request_id); /* TODO: move this to exec */

    if (sfstmt->sql_text) {
        SF_FREE(sfstmt->sql_text); /* SQL */
    }
    sfstmt->sql_text = NULL;

    if (sfstmt->raw_results) {
        cJSON_Delete(sfstmt->raw_results);
    }
    sfstmt->raw_results = NULL;

    if (sfstmt->params) {
        array_list_deallocate(sfstmt->params); /* binding parameters */
    }
    sfstmt->params = NULL;

    if (sfstmt->results) {
        array_list_deallocate(sfstmt->results); /* binding columns */
    }
    sfstmt->results = NULL;

    if (sfstmt->desc) {
        /* column metadata */
        for (i = 0; i < sfstmt->total_fieldcount; i++) {
            SF_FREE(sfstmt->desc[i]->name);
            SF_FREE(sfstmt->desc[i]);
        }
        SF_FREE(sfstmt->desc);
    }
    sfstmt->desc = NULL;

    if (sfstmt->stmt_attrs) {
        array_list_deallocate(sfstmt->stmt_attrs);
    }
    sfstmt->stmt_attrs = NULL;

    /* clear error handle */
    clear_snowflake_error(&sfstmt->error);

    sfstmt->total_rowcount = -1;
    sfstmt->total_fieldcount = -1;
    sfstmt->total_row_index = -1;
}

SF_STMT *STDCALL snowflake_stmt(SF_CONNECT *sf) {
    if (!sf) {
        return NULL;
    }

    SF_STMT *sfstmt = (SF_STMT *) SF_CALLOC(1, sizeof(SF_STMT));
    if (sfstmt) {
        _snowflake_stmt_reset(sfstmt);
        sfstmt->sequence_counter = ++sf->sequence_counter;
        sfstmt->connection = sf;
    }
    return sfstmt;
}

void STDCALL snowflake_stmt_term(SF_STMT *sfstmt) {
    if (sfstmt) {
        _snowflake_stmt_reset(sfstmt);
        SF_FREE(sfstmt);
    }
}

SF_STATUS STDCALL snowflake_bind_param(
    SF_STMT *sfstmt, SF_BIND_INPUT *sfbind) {
    if (!sfstmt) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sfstmt->error);
    if (sfstmt->params == NULL) {
        sfstmt->params = array_list_init();
    }
    array_list_set(sfstmt->params, sfbind, sfbind->idx);
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_bind_result(
    SF_STMT *sfstmt, SF_BIND_OUTPUT *sfbind) {
    if (!sfstmt) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sfstmt->error);
    if (sfstmt->results == NULL) {
        sfstmt->results = array_list_init();
    }
    array_list_set(sfstmt->results, sfbind, sfbind->idx);
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_query(
        SF_STMT *sfstmt, const char *command, size_t command_size) {
    if (!sfstmt) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sfstmt->error);
    if (snowflake_prepare(sfstmt, command, command_size) != SF_STATUS_SUCCESS) {
        return SF_STATUS_ERROR;
    }
    if (snowflake_execute(sfstmt) != SF_STATUS_SUCCESS) {
        return SF_STATUS_ERROR;
    }
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_fetch(SF_STMT *sfstmt) {
    if (!sfstmt) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sfstmt->error);
    SF_STATUS ret = SF_STATUS_ERROR;
    int64 i;
    cJSON *row = NULL;
    cJSON *raw_result;
    SF_BIND_OUTPUT *result;

    // If no more results, set return to SF_STATUS_EOL
    if (cJSON_GetArraySize(sfstmt->raw_results) == 0) {
        ret = SF_STATUS_EOL;
        goto cleanup;
    }

    // Check that we can write to the provided result bindings
    for (i = 0; i < sfstmt->total_fieldcount; i++) {
        result = array_list_get(sfstmt->results, i + 1);
        if (result == NULL) {
            continue;
        } else {
            if (result->type != sfstmt->desc[i]->c_type && result->type != SF_C_TYPE_STRING) {
                // TODO add error msg
                goto cleanup;
            }
        }
    }

    // Get next result row
    row = cJSON_DetachItemFromArray(sfstmt->raw_results, 0);

    // Write to results
    // TODO error checking for conversions during fetch
    for (i = 0; i < sfstmt->total_fieldcount; i++) {
        result = array_list_get(sfstmt->results, i + 1);
        if (result == NULL) {
            continue;
        } else {
            raw_result = cJSON_GetArrayItem(row, i);
            // TODO turn into switch statement
            if (result->type == SF_C_TYPE_INT8) {
                if (sfstmt->desc[i]->type == SF_TYPE_BOOLEAN) {
                    *(int8 *) result->value = cJSON_IsTrue(raw_result) ? SF_BOOLEAN_TRUE : SF_BOOLEAN_FALSE;
                } else {
                    // field is a char?
                    *(int8 *) result->value = (int8) raw_result->valuestring[0];
                }
            } else if (result->type == SF_C_TYPE_UINT8) {
                *(uint8 *) result->value = (uint8) raw_result->valuestring[0];
            } else if (result->type == SF_C_TYPE_INT64) {
                *(int64 *) result->value = (int64) strtoll(raw_result->valuestring, NULL, 10);
            } else if (result->type == SF_C_TYPE_UINT64) {
                *(uint64 *) result->value = (uint64) strtoull(raw_result->valuestring, NULL, 10);
            } else if (result->type == SF_C_TYPE_FLOAT64) {
                *(float64 *) result->value = (float64) strtod(raw_result->valuestring, NULL);
            } else if (result->type == SF_C_TYPE_STRING) {
                /* copy original data as is except Date/Time/Timestamp/Binary type */
                strncpy(result->value, raw_result->valuestring, result->max_length);
                result->len = strlen(raw_result->valuestring); /* TODO: what if null is included? */
            } else if (result->type == SF_C_TYPE_TIMESTAMP) {
                // TODO Do some timestamp stuff here
            } else {
                // TODO Create default case
            }
        }
    }

    ret = SF_STATUS_SUCCESS;

cleanup:
    cJSON_Delete(row);
    return ret;
}

SF_STATUS STDCALL snowflake_trans_begin(SF_CONNECT *sf) {
    if (!sf) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sf->error);
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_trans_commit(SF_CONNECT *sf) {
    if (!sf) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sf->error);
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_trans_rollback(SF_CONNECT *sf) {
    if (!sf) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sf->error);
    return SF_STATUS_SUCCESS;
}

int64 STDCALL snowflake_affected_rows(SF_STMT *sfstmt) {
    size_t i;
    int64 ret = -1;
    cJSON *row = NULL;
    cJSON *raw_row_result;
    clear_snowflake_error(&sfstmt->error);
    if (!sfstmt) {
        /* TODO: set error - set_snowflake_error */
        return ret;
    }
    if (cJSON_GetArraySize(sfstmt->raw_results) == 0) {
        /* no affected rows is determined. The potential cause is
         * the query is not DML. */
        /* TODO: set error - set_snowflake_error */
        return ret;
    }

    if (sfstmt->is_dml) {
        row = cJSON_DetachItemFromArray(sfstmt->raw_results, 0);
        ret = 0;
        for (i = 0; i < sfstmt->total_fieldcount; ++i) {
            raw_row_result = cJSON_GetArrayItem(row, i);
            ret += (int64) strtoll(raw_row_result->valuestring, NULL, 10);
        }
        cJSON_Delete(row);
    } else {
        ret = sfstmt->total_rowcount;
    }
    return ret;
}

SF_STATUS STDCALL snowflake_prepare(SF_STMT *sfstmt, const char *command, size_t command_size) {
    if (!sfstmt) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sfstmt->error);
    SF_STATUS ret = SF_STATUS_ERROR;
    size_t sql_text_size = 1; // Don't forget about null terminator
    if (!command) {
        goto cleanup;
    }
    _snowflake_stmt_reset(sfstmt);
    // Set sql_text to command
    if (command_size == 0) {
        log_debug("Command size is 0, using to strlen to find query length.");
        sql_text_size += strlen(command);
    } else {
        log_debug("Command size non-zero, setting as sql text size.");
        sql_text_size += command_size;
    }
    sfstmt->sql_text = (char *) SF_CALLOC(1, sql_text_size);
    memcpy(sfstmt->sql_text, command, sql_text_size - 1);
    // Null terminate
    sfstmt->sql_text[sql_text_size - 1] = '\0';

    ret = SF_STATUS_SUCCESS;

cleanup:
    return ret;
}

SF_STATUS STDCALL snowflake_execute(SF_STMT *sfstmt) {
    if (!sfstmt) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sfstmt->error);
    SF_STATUS ret = SF_STATUS_ERROR;
    SF_JSON_ERROR json_error;
    const char *error_msg;
    cJSON *body = NULL;
    cJSON *data = NULL;
    cJSON *rowtype = NULL;
    cJSON *resp = NULL;
    char *s_body = NULL;
    char *s_resp = NULL;
    sf_bool success = SF_BOOLEAN_FALSE;
    URL_KEY_VALUE url_params[] = {
            {"requestId=", sfstmt->request_id, NULL, NULL, 0, 0}
    };
    size_t i;
    cJSON *bindings = NULL;
    SF_BIND_INPUT *input;
    const char *type;
    char *value;

    // TODO Do error handing and checking and stuff
    if (sfstmt->params && sfstmt->params->used > 0) {
        bindings = cJSON_CreateObject();
        for (i = 0; i < sfstmt->params->used; i++) {
            cJSON *binding;
            input = (SF_BIND_INPUT *) array_list_get(sfstmt->params, i + 1);
            // TODO check if input is null and either set error or write msg to log
            type = snowflake_type_to_string(c_type_to_snowflake(input->c_type, SF_TYPE_TIMESTAMP_NTZ));
            value = value_to_string(input->value, input->len, input->c_type);
            binding = cJSON_CreateObject();
            char idxbuf[20];
            sprintf(idxbuf, "%ld", i + 1);
            cJSON_AddStringToObject(binding, "type", type);
            cJSON_AddStringToObject(binding, "value", value);
            cJSON_AddItemToObject(bindings, idxbuf, binding);
            SF_FREE(value);
        }
    }

    if (is_string_empty(sfstmt->connection->master_token) || is_string_empty(sfstmt->connection->token)) {
        log_error("Missing session token or Master token. Are you sure that snowflake_connect was successful?");
        SET_SNOWFLAKE_ERROR(&sfstmt->error, SF_ERROR_BAD_CONNECTION_PARAMS,
                            "Missing session or master token. Try running snowflake_connect.", SF_SQLSTATE_UNABLE_TO_CONNECT);
        goto cleanup;
    }

    // Create Body
    body = create_query_json_body(sfstmt->sql_text, sfstmt->sequence_counter);
    if (bindings != NULL) {
        /* binding parameters if exists */
        cJSON_AddItemToObject(body, "bindings", bindings);
    }
    s_body = cJSON_Print(body);
    log_debug("Created body");
    log_trace("Here is constructed body:\n%s", s_body);

    if (request(sfstmt->connection, &resp, QUERY_URL, url_params, 1, s_body, NULL, POST_REQUEST_TYPE, &sfstmt->error)) {
        s_resp = cJSON_Print(resp);
        log_trace("Here is JSON response:\n%s", s_resp);
        data = cJSON_GetObjectItem(resp, "data");
        if (json_copy_string_no_alloc(sfstmt->sfqid, data, "queryId", UUID4_LEN)) {
            log_debug("No valid sfqid found in response");
        }
        if ((json_error = json_copy_bool(&success, resp, "success")) == SF_JSON_ERROR_NONE && success) {
            // Set Database info
            if (json_copy_string(&sfstmt->connection->database, data, "finalDatabaseName")) {
                log_warn("No valid database found in response");
            }
            if (json_copy_string(&sfstmt->connection->schema, data, "finalSchemaName")) {
                log_warn("No valid schema found in response");
            }
            if (json_copy_string(&sfstmt->connection->warehouse, data, "finalWarehouseName")) {
                log_warn("No valid warehouse found in response");
            }
            if (json_copy_string(&sfstmt->connection->role, data, "finalRoleName")) {
                log_warn("No valid role found in response");
            }
            int64 stmt_type_id;
            if (json_copy_int(&stmt_type_id, data, "statementTypeId")) {
                /* failed to get statement type id */
                sfstmt->is_dml = SF_BOOLEAN_FALSE;
            } else {
                sfstmt->is_dml = detect_stmt_type(stmt_type_id);
            }
            rowtype = cJSON_GetObjectItem(data, "rowtype");
            if (cJSON_IsArray(rowtype)) {
                sfstmt->total_fieldcount = cJSON_GetArraySize(rowtype);
                sfstmt->desc = set_description(rowtype);
            }
            // Set results array
            if (json_detach_array_from_object(&sfstmt->raw_results, data, "rowset")) {
                log_error("No valid rowset found in response");
                SET_SNOWFLAKE_STMT_ERROR(&sfstmt->error, SF_ERROR_BAD_JSON,
                                    "Missing rowset from response. No results found.",
                                    SF_SQLSTATE_APP_REJECT_CONNECTION, sfstmt->sfqid);
                goto cleanup;
            }
            if (json_copy_int(&sfstmt->total_rowcount, data, "total")) {
                log_warn("No total count found in response. Reverting to using array size of results");
                sfstmt->total_rowcount = cJSON_GetArraySize(sfstmt->raw_results);
            }
        } else if (json_error != SF_JSON_ERROR_NONE) {
            JSON_ERROR_MSG(json_error, error_msg, "Success code");
            SET_SNOWFLAKE_STMT_ERROR(
              &sfstmt->error, SF_ERROR_BAD_JSON,
              error_msg, SF_SQLSTATE_APP_REJECT_CONNECTION, sfstmt->sfqid);
            goto cleanup;
        } else if (!success) {
            cJSON *messageJson = NULL;
            char *message = NULL;
            cJSON *codeJson = NULL;
            int64 code = -1;
            if (json_copy_string_no_alloc(sfstmt->error.sqlstate, data, "sqlState", SQLSTATE_LEN)) {
                log_debug("No valid sqlstate found in response");
            }
            messageJson = cJSON_GetObjectItem(resp, "message");
            if (messageJson) {
                message = messageJson->valuestring;
            }
            codeJson = cJSON_GetObjectItem(resp, "code");
            if (codeJson) {
                code = (int64)atoi(codeJson->valuestring);
            } else {
                log_debug("no code element.");
            }
            SET_SNOWFLAKE_STMT_ERROR(&sfstmt->error, code,
                                     message ? message : "Query was not successful",
                                     NULL, sfstmt->sfqid);
            goto cleanup;
        }
    } else {
        log_trace("Connection failed");
    }

    // Everything went well if we got to this point
    ret = SF_STATUS_SUCCESS;

cleanup:
    cJSON_Delete(body);
    cJSON_Delete(resp);
    SF_FREE(s_body);
    SF_FREE(s_resp);

    return ret;
}

SF_ERROR *STDCALL snowflake_error(SF_CONNECT *sf) {
    if (!sf) {
        return NULL;
    }
    return &sf->error;
}

SF_ERROR *STDCALL snowflake_stmt_error(SF_STMT *sfstmt) {
    if (!sfstmt) {
        return NULL;
    }
    return &sfstmt->error;
}

uint64 STDCALL snowflake_num_rows(SF_STMT *sfstmt) {
    // TODO fix int vs uint stuff
    if (!sfstmt) {
        // TODO change to -1?
        return 0;
    }
    return (uint64)sfstmt->total_rowcount;
}

uint64 STDCALL snowflake_num_fields(SF_STMT *sfstmt) {
    // TODO fix int vs uint stuff
    if (!sfstmt) {
        // TODO change to -1?
        return 0;
    }
    return (uint64)sfstmt->total_fieldcount;
}

uint64 STDCALL snowflake_param_count(SF_STMT *sfstmt) {
    if (!sfstmt) {
        // TODO change to -1?
        return 0;
    }
    return sfstmt->params->used;
}

const char *STDCALL snowflake_sfqid(SF_STMT *sfstmt) {
    if (!sfstmt) {
        return NULL;
    }
    return sfstmt->sfqid;
}

const char *STDCALL snowflake_sqlstate(SF_STMT *sfstmt) {
    if (!sfstmt) {
        return NULL;
    }
    return sfstmt->error.sqlstate;
}

SF_STATUS STDCALL snowflake_stmt_get_attr(
  SF_STMT *sfstmt, SF_STMT_ATTRIBUTE type, void *value) {
    if (!sfstmt) {
        return SF_STATUS_ERROR;
    }
    // TODO: get the value from SF_STMT.
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_stmt_set_attr(
  SF_STMT *sfstmt, SF_STMT_ATTRIBUTE type, const void *value) {
    if (!sfstmt) {
        return SF_STATUS_ERROR;
    }
    clear_snowflake_error(&sfstmt->error);
    /* TODO: need extra member in SF_STMT */
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_propagate_error(SF_CONNECT *sf, SF_STMT *sfstmt) {
    if (!sfstmt || !sf) {
        return SF_STATUS_ERROR;
    }
    if (sf->error.error_code) {
        /* if already error is set */
        SF_FREE(sf->error.msg);
    }
    memcpy(&sf->error, &sfstmt->error, sizeof(SF_ERROR));
    if (sfstmt->error.error_code) {
        /* any error */
        size_t len = strlen(sfstmt->error.msg);
        sf->error.msg = SF_CALLOC(len + 1, sizeof(char));
        strncpy(sf->error.msg, sfstmt->error.msg, len);
    }
    return SF_STATUS_SUCCESS;
}
