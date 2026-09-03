#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "gmail_api.h"

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --import <file.json>       Import client_secret JSON from Google Cloud Console\n");
    printf("  --client-id <id>           Specify OAuth Client ID\n");
    printf("  --client-secret <secret>   Specify OAuth Client Secret\n");
    printf("  --code <code>              Exchange manual authorization code\n");
    printf("  --auth                     Run browser OAuth loopback flow\n");
    printf("  --check                    Test checking inbox and fetching counts\n");
    printf("  --list-labels              List all labels in the authorized account\n");
    printf("  --help                     Show this help message\n");
}

int main(int argc, char *argv[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    GmailState state;
    gmail_state_init(&state, NULL, NULL);

    /* Try loading existing GKrellM config */
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/.gkrellm2/user-config", getenv("HOME"));
    FILE *f = fopen(config_path, "r");
    if (!f) {
        snprintf(config_path, sizeof(config_path), "%s/.gkrellm2/user_config", getenv("HOME"));
        f = fopen(config_path, "r");
    }
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char kw[64], item[128], val[384];
            if (sscanf(line, "%s %s %[^\n]", kw, item, val) >= 2) {
                if (strcmp(kw, "gkrellm_gmail") == 0) {
                    if (strcmp(item, "client_id") == 0) g_strlcpy(state.client_id, val, sizeof(state.client_id));
                    else if (strcmp(item, "client_secret") == 0) g_strlcpy(state.client_secret, val, sizeof(state.client_secret));
                    else if (strcmp(item, "refresh_token") == 0) {
                        g_strlcpy(state.refresh_token, val, sizeof(state.refresh_token));
                        state.authorized = TRUE;
                    }
                    else if (strcmp(item, "email_address") == 0) g_strlcpy(state.email_address, val, sizeof(state.email_address));
                    else if (strcmp(item, "labels") == 0) g_strlcpy(state.labels_str, val, sizeof(state.labels_str));
                }
            }
        }
        fclose(f);
    }

    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--import") == 0 && i + 1 < argc) {
            char err[256] = {0};
            if (gmail_load_client_secrets_file(argv[++i], state.client_id, sizeof(state.client_id),
                                               state.client_secret, sizeof(state.client_secret),
                                               err, sizeof(err))) {
                printf("Successfully loaded client credentials:\n");
                printf("  Client ID: %s\n", state.client_id);
            } else {
                fprintf(stderr, "Failed to import JSON: %s\n", err);
                return 1;
            }
        } else if (strcmp(argv[i], "--client-id") == 0 && i + 1 < argc) {
            g_strlcpy(state.client_id, argv[++i], sizeof(state.client_id));
        } else if (strcmp(argv[i], "--client-secret") == 0 && i + 1 < argc) {
            g_strlcpy(state.client_secret, argv[++i], sizeof(state.client_secret));
        } else if (strcmp(argv[i], "--code") == 0 && i + 1 < argc) {
            char err[256] = {0};
            if (gmail_exchange_auth_code(&state, argv[++i], err, sizeof(err))) {
                printf("Authorization code exchanged successfully!\n");
                printf("Refresh Token: %s\n", state.refresh_token);
            } else {
                fprintf(stderr, "Auth code exchange failed: %s\n", err);
                return 1;
            }
        } else if (strcmp(argv[i], "--auth") == 0) {
            if (!state.client_id[0] || !state.client_secret[0]) {
                fprintf(stderr, "Error: Please specify --client-id and --client-secret (or --import JSON) first.\n");
                return 1;
            }
            printf("Starting browser OAuth flow on http://127.0.0.1:%d ...\n", DEFAULT_OAUTH_PORT);
            if (gmail_start_oauth_flow(&state, DEFAULT_OAUTH_PORT)) {
                printf("Waiting for authorization in browser...\n");
                while (state.auth_in_progress) {
                    g_usleep(200000);
                }
                if (state.authorized) {
                    printf("Authorization successful!\n");
                    printf("Refresh Token: %s\n", state.refresh_token);
                } else {
                    fprintf(stderr, "Authorization failed: %s\n", state.auth_error);
                    return 1;
                }
            }
        } else if (strcmp(argv[i], "--check") == 0) {
            if (!state.refresh_token[0]) {
                fprintf(stderr, "Error: Not authorized. Run with --auth or specify credentials first.\n");
                return 1;
            }
            printf("Checking Gmail account...\n");
            if (gmail_fetch_all(&state)) {
                printf("Connected Account: %s\n", state.email_address);
                printf("Unread Messages in monitored labels: %d\n", state.total_unread);
                printf("Total Messages in INBOX: %d\n", state.inbox_total);
                printf("Total Messages in Mailbox: %d\n", state.total_messages);
                GList *l;
                for (l = state.monitored_labels_stats; l != NULL; l = l->next) {
                    GmailLabelStats *st = (GmailLabelStats *)l->data;
                    printf("  - %s: %d unread, %d total\n", st->name, st->messages_unread, st->messages_total);
                }
            } else {
                fprintf(stderr, "Check failed: %s\n", state.last_status_msg);
                return 1;
            }
        } else if (strcmp(argv[i], "--list-labels") == 0) {
            char err[256] = {0};
            if (!state.access_token[0] && state.refresh_token[0]) {
                gmail_refresh_access_token(&state, err, sizeof(err));
            }
            GList *labels = gmail_fetch_labels_list(&state, err, sizeof(err));
            if (labels) {
                printf("Available Labels in Gmail Account:\n");
                GList *l;
                for (l = labels; l != NULL; l = l->next) {
                    GmailLabelStats *st = (GmailLabelStats *)l->data;
                    printf("  %-25s (ID: %-20s, Type: %s)\n", st->name, st->id, st->type);
                }
                gmail_free_label_stats_list(labels);
            } else {
                fprintf(stderr, "Failed to list labels: %s\n", err);
                return 1;
            }
        }
    }

    if (argc == 1) {
        print_usage(argv[0]);
    }

    return 0;
}
