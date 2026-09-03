#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include "../gmail_api.h"

#define COLOR_GREEN "\033[32m"
#define COLOR_RED   "\033[31m"
#define COLOR_CYAN  "\033[36m"
#define COLOR_RESET "\033[0m"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_START(name) do { \
    tests_run++; \
    printf("  [RUN]  %s ... ", name); \
    fflush(stdout); \
} while(0)

#define TEST_PASS() do { \
    tests_passed++; \
    printf(COLOR_GREEN "PASS" COLOR_RESET "\n"); \
} while(0)

#define TEST_FAIL(msg) do { \
    printf(COLOR_RED "FAIL: %s" COLOR_RESET "\n", msg); \
} while(0)

/* Test 1: JSON Client Secrets file parsing */
static void test_client_secrets_parsing(void) {
    TEST_START("Google client_secret.json parser");

    /* Case A: Standard installed app format */
    const char *json_installed =
        "{\"installed\":{"
        "\"client_id\":\"123456789-abcdef.apps.googleusercontent.com\","
        "\"project_id\":\"test-project\","
        "\"auth_uri\":\"https://accounts.google.com/o/oauth2/auth\","
        "\"token_uri\":\"https://oauth2.googleapis.com/token\","
        "\"client_secret\":\"GOCSPX-Secret12345\""
        "}}";

    char tmp_file[] = "/tmp/test_client_secret_XXXXXX.json";
    int fd = mkstemps(tmp_file, 5);
    assert(fd >= 0);
    write(fd, json_installed, strlen(json_installed));
    close(fd);

    char cid[256] = {0}, csec[256] = {0}, err[256] = {0};
    gboolean ok = gmail_load_client_secrets_file(tmp_file, cid, sizeof(cid), csec, sizeof(csec), err, sizeof(err));
    unlink(tmp_file);

    if (!ok || strcmp(cid, "123456789-abcdef.apps.googleusercontent.com") != 0 ||
        strcmp(csec, "GOCSPX-Secret12345") != 0) {
        TEST_FAIL("Failed to parse standard installed client_secret.json");
        return;
    }

    /* Case B: Web application format */
    const char *json_web =
        "{\"web\":{"
        "\"client_id\":\"web-client-id.apps.googleusercontent.com\","
        "\"client_secret\":\"Web-Secret-XYZ\""
        "}}";

    char tmp_file2[] = "/tmp/test_client_secret_web_XXXXXX.json";
    fd = mkstemps(tmp_file2, 5);
    assert(fd >= 0);
    write(fd, json_web, strlen(json_web));
    close(fd);

    cid[0] = csec[0] = err[0] = 0;
    ok = gmail_load_client_secrets_file(tmp_file2, cid, sizeof(cid), csec, sizeof(csec), err, sizeof(err));
    unlink(tmp_file2);

    if (!ok || strcmp(cid, "web-client-id.apps.googleusercontent.com") != 0 ||
        strcmp(csec, "Web-Secret-XYZ") != 0) {
        TEST_FAIL("Failed to parse web format client_secret.json");
        return;
    }

    /* Case C: Invalid file path */
    cid[0] = csec[0] = err[0] = 0;
    ok = gmail_load_client_secrets_file("/path/to/nonexistent/file.json", cid, sizeof(cid), csec, sizeof(csec), err, sizeof(err));
    if (ok || err[0] == 0) {
        TEST_FAIL("Should return FALSE on nonexistent file");
        return;
    }

    TEST_PASS();
}

/* Test 2: State Initialization & Cleanup */
static void test_state_init_and_cleanup(void) {
    TEST_START("GmailState initialization and cleanup");

    GmailState state;
    gmail_state_init(&state, NULL, NULL);

    if (state.poll_interval != DEFAULT_POLL_INTERVAL ||
        strcmp(state.labels_str, DEFAULT_LABELS) != 0 ||
        !state.total_is_inbox ||
        state.authorized ||
        state.total_unread != 0) {
        TEST_FAIL("Default values not set correctly in gmail_state_init");
        gmail_state_cleanup(&state);
        return;
    }

    /* Test disconnect */
    g_strlcpy(state.client_id, "test-id", sizeof(state.client_id));
    g_strlcpy(state.refresh_token, "test-token", sizeof(state.refresh_token));
    state.authorized = TRUE;
    state.total_unread = 10;

    gmail_disconnect_account(&state);

    if (state.authorized || state.refresh_token[0] != '\0' || state.total_unread != 0) {
        TEST_FAIL("gmail_disconnect_account did not clear tokens or state");
        gmail_state_cleanup(&state);
        return;
    }

    gmail_state_cleanup(&state);
    TEST_PASS();
}

/* Test 3: JSON Profile and Label responses parser */
static void test_json_parsing_logic(void) {
    TEST_START("Gmail REST API JSON response parsing");

    /* Mock Profile Response */
    const char *mock_profile =
        "{"
        "\"emailAddress\":\"alice.smith@gmail.com\","
        "\"messagesTotal\":14250,"
        "\"threadsTotal\":8100,"
        "\"historyId\":\"1029384\""
        "}";

    JsonParser *parser = json_parser_new();
    gboolean parsed = json_parser_load_from_data(parser, mock_profile, -1, NULL);
    if (!parsed) {
        TEST_FAIL("Failed to parse mock profile JSON");
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);
    const char *email = json_object_get_string_member(obj, "emailAddress");
    gint64 total_msgs = json_object_get_int_member(obj, "messagesTotal");

    if (strcmp(email, "alice.smith@gmail.com") != 0 || total_msgs != 14250) {
        TEST_FAIL("Profile JSON fields extracted incorrectly");
        g_object_unref(parser);
        return;
    }
    g_object_unref(parser);

    /* Mock Label Detail Response */
    const char *mock_label =
        "{"
        "\"id\":\"INBOX\","
        "\"name\":\"INBOX\","
        "\"messagesTotal\":342,"
        "\"messagesUnread\":7,"
        "\"threadsTotal\":210,"
        "\"threadsUnread\":5"
        "}";

    parser = json_parser_new();
    parsed = json_parser_load_from_data(parser, mock_label, -1, NULL);
    if (!parsed) {
        TEST_FAIL("Failed to parse mock label JSON");
        g_object_unref(parser);
        return;
    }

    root = json_parser_get_root(parser);
    obj = json_node_get_object(root);
    gint64 unread = json_object_get_int_member(obj, "messagesUnread");
    gint64 total = json_object_get_int_member(obj, "messagesTotal");

    if (unread != 7 || total != 342) {
        TEST_FAIL("Label JSON counts extracted incorrectly");
        g_object_unref(parser);
        return;
    }
    g_object_unref(parser);

    TEST_PASS();
}

/* Test 4: Labels tokenization and list management */
static void test_label_tokenization(void) {
    TEST_START("Label list parsing, tokenization and deduplication");

    const char *input_str = "INBOX, Work, Urgent; Alerts \t INBOX, Work";
    gchar **tokens = g_strsplit_set(input_str, ",; \t\n", -1);
    assert(tokens != NULL);

    GList *list = NULL;
    int i;
    for (i = 0; tokens[i] != NULL; i++) {
        gchar *tok = g_strstrip(tokens[i]);
        if (!*tok) continue;

        gboolean exists = FALSE;
        GList *l;
        for (l = list; l != NULL; l = l->next) {
            GmailLabelStats *st = (GmailLabelStats *)l->data;
            if (g_ascii_strcasecmp(st->name, tok) == 0) {
                exists = TRUE;
                break;
            }
        }
        if (!exists) {
            GmailLabelStats *st = g_new0(GmailLabelStats, 1);
            g_strlcpy(st->name, tok, sizeof(st->name));
            list = g_list_append(list, st);
        }
    }
    g_strfreev(tokens);

    /* Should contain exactly 4 unique labels: INBOX, Work, Urgent, Alerts */
    if (g_list_length(list) != 4) {
        TEST_FAIL("Expected 4 unique labels after deduplication");
        gmail_free_label_stats_list(list);
        return;
    }

    const char *expected[] = { "INBOX", "Work", "Urgent", "Alerts" };
    GList *l;
    int idx = 0;
    for (l = list; l != NULL; l = l->next, idx++) {
        GmailLabelStats *st = (GmailLabelStats *)l->data;
        if (g_ascii_strcasecmp(st->name, expected[idx]) != 0) {
            TEST_FAIL("Deduplicated label order or name mismatch");
            gmail_free_label_stats_list(list);
            return;
        }
    }

    gmail_free_label_stats_list(list);
    TEST_PASS();
}

/* Test 5: OAuth Token JSON Response parser */
static void test_oauth_token_json_parsing(void) {
    TEST_START("OAuth 2.0 Token Response (access_token, refresh_token, expires_in)");

    const char *mock_token_resp =
        "{"
        "\"access_token\":\"ya29.a0AfH6SM...MockAccessToken\","
        "\"expires_in\":3599,"
        "\"refresh_token\":\"1//0gMockRefreshToken...\","
        "\"scope\":\"https://www.googleapis.com/auth/gmail.readonly\","
        "\"token_type\":\"Bearer\""
        "}";

    JsonParser *parser = json_parser_new();
    gboolean ok = json_parser_load_from_data(parser, mock_token_resp, -1, NULL);
    if (!ok) {
        TEST_FAIL("Failed to parse token response JSON");
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);

    if (!json_object_has_member(obj, "access_token") ||
        !json_object_has_member(obj, "refresh_token") ||
        !json_object_has_member(obj, "expires_in")) {
        TEST_FAIL("Token JSON missing expected members");
        g_object_unref(parser);
        return;
    }

    const char *at = json_object_get_string_member(obj, "access_token");
    const char *rt = json_object_get_string_member(obj, "refresh_token");
    gint64 exp = json_object_get_int_member(obj, "expires_in");

    if (strcmp(at, "ya29.a0AfH6SM...MockAccessToken") != 0 ||
        strcmp(rt, "1//0gMockRefreshToken...") != 0 ||
        exp != 3599) {
        TEST_FAIL("Extracted token values do not match mock data");
        g_object_unref(parser);
        return;
    }

    g_object_unref(parser);
    TEST_PASS();
}

int main(void) {
    printf(COLOR_CYAN "=== Running GKrellM Gmail Plugin Test Suite ===" COLOR_RESET "\n\n");

    test_client_secrets_parsing();
    test_state_init_and_cleanup();
    test_json_parsing_logic();
    test_label_tokenization();
    test_oauth_token_json_parsing();

    printf("\n" COLOR_CYAN "Test Results: %d/%d passed (%d%%)" COLOR_RESET "\n",
           tests_passed, tests_run, (tests_passed * 100) / (tests_run ? tests_run : 1));

    return (tests_passed == tests_run) ? 0 : 1;
}
