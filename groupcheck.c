/*
 * groupcheck is a minimal polkit replacement for group-based authentication.
 * Copyright (c) 2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#define LINE_BUF_SIZE 512
#define MAX_NAME_SIZE 256
#define MAX_GROUPS 10

/* file parser results */

struct line_data {
    char buf[LINE_BUF_SIZE];
    char *id;
    int n_groups;
    char *groups[MAX_GROUPS];
};

/* D-Bus message analysis */

enum subject_kind {
    SUBJECT_KIND_UNKNOWN = 0,
    SUBJECT_KIND_UNIX_PROCESS,
    SUBJECT_KIND_UNIX_SESSION,
    SUBJECT_KIND_SYSTEM_BUS_NAME,
};

struct subject_unix_session {
    char session_id[MAX_NAME_SIZE];
};

struct subject_unix_process {
    uint32_t pid;
    uint64_t start_time;
};

struct subject_system_bus {
    char system_bus_name[MAX_NAME_SIZE];
};

struct subject {
    enum subject_kind kind;
    union {
        struct subject_unix_session s;
        struct subject_unix_process p;
        struct subject_system_bus b;
    } data;
};

#define STAT_NAME_SIZE 32
#define STAT_DATA_SIZE 256

static int verify_start_time(struct subject *subject)
{
    /* Get the pid start time from /proc/stat and compare it with the value in
     * the request. Return -1 if no match. */

    char namebuf[STAT_NAME_SIZE];
    char databuf[STAT_DATA_SIZE];
    int r;
    FILE *f;
    char *p, *endp = NULL;
    int i;
    uint64_t start_time;

    r = snprintf(namebuf, STAT_NAME_SIZE, "/proc/%d/stat", subject->data.p.pid);
    if (r < 0 || r >= STAT_NAME_SIZE)
        return -EINVAL;

    f = fopen(namebuf, "r");

    if (f == NULL)
        return -EINVAL;

    p = fgets(databuf, STAT_DATA_SIZE, f);
    if (p == NULL)
        return -EINVAL;

    /* read the 22th field, which is the process start time in jiffies */

    /* skip over the "comm" field that has parentheses */
    p = strchr(p, ')');

    if (*p == '\0')
        return -EINVAL;

    /* That was the second field. Then skip over 19 more (20 spaces). */

    for (i = 0; i < 20; i++) {
        p = strchr(p, ' ');
        if (*p == '\0')
            return -EINVAL;
    }

    start_time = strtoul(p, &endp, 10);
    if (endp != NULL)
        return -EINVAL;

    if (start_time != subject->data.p.start_time)
        return -EINVAL;

    /* start times match */
    return 0;
}

static bool check_allowed(sd_bus *bus, struct line_data *data,
        struct subject *subject, const char *action_id)
{
    struct line_data *line;
    char **groups = NULL;
    int n_groups = 0;
    int r;
    sd_bus_creds *creds = NULL;
    gid_t primary_gid;
    uint64_t mask = SD_BUS_CREDS_SUPPLEMENTARY_GIDS | SD_BUS_CREDS_AUGMENT
            | SD_BUS_CREDS_PID | SD_BUS_CREDS_GID;
    const gid_t *gids = NULL;
    int n_gids = 0;
    pid_t ruid, euid;

    /* find first the corresponding group data from the policy */

    line = data;

    while (line->id) {
        if (strcmp(line->id, action_id) == 0) {
            groups = line->groups;
            n_groups = line->n_groups;
            break;
        }
        line++;
    }

    if (!groups)
        return false;

    /* check which groups the subject belongs to */

    switch (subject->kind) {
    case SUBJECT_KIND_UNIX_PROCESS:

#if 0
        if (subject->data.p.pid == 0) {
            /* We don't authenticate requests coming from root to protect
             * against attacks where the process exec()s a binary that is
             * setuid root after asking for permissions. This is not needed if
             * the root doesn't belong to any special groups though. It's the
             * responsibility of the system administrator to make sure that
             * there aren't any other UIDs that have setuid() binaries and
             * belong to administrator groups. */
            goto end;
        }
#endif

        mask = _SD_BUS_CREDS_ALL;
        r = sd_bus_creds_new_from_pid(&creds, subject->data.p.pid, mask);
        if (r < 0)
            goto end;

        r = verify_start_time(subject);
        if (r < 0)
            goto end;

        r = sd_bus_creds_get_uid(creds, &ruid);
        if (r < 0)
            goto end;

        r = sd_bus_creds_get_euid(creds, &euid);
        if (r < 0)
            goto end;

        /* We want the real uid to be the same as the effective uid. This helps
         * to make sure that the original caller hasn't used exec() to start
         * a setuid() process for which the effective user might belong to a
         * different set of groups. */

        if (euid != ruid)
            goto end;

        n_gids = sd_bus_creds_get_supplementary_gids(creds, &gids);
        if (n_gids < 0)
            goto end;

        r = sd_bus_creds_get_gid(creds, &primary_gid);
        if (r < 0)
            goto end;

        break;

    case SUBJECT_KIND_SYSTEM_BUS_NAME:
        r = sd_bus_get_name_creds(bus, subject->data.b.system_bus_name, mask, &creds);
        if (r < 0)
            goto end;

        r = sd_bus_creds_get_uid(creds, &ruid);
        if (r < 0)
            goto end;

        r = sd_bus_creds_get_euid(creds, &euid);
        if (r < 0)
            goto end;

        if (euid != ruid)
            goto end;

        n_gids = sd_bus_creds_get_supplementary_gids(creds, &gids);
        if (n_gids < 0)
            goto end;

        r = sd_bus_creds_get_gid(creds, &primary_gid);
        if (r < 0)
            goto end;

        break;

    default:
        /* not supported yet */
        break;
    }

    if (gids) {
        int i, j;
        struct group *grp;

        /* match the groups */

        for (i = 0; i < n_groups; i++) {
            grp = getgrnam(groups[i]);
            if (grp == NULL)
                continue;

            for (j = 0; j < n_gids; j++) {

                if (gids[j] == primary_gid) {
                    /* We only include supplementary gids in the check, not the
                       primary gid. This is to make it more difficult for
                       processes to exec a setgid process to gain elevated
                       group access. */
                       continue;
                }

                if (gids[j] == grp->gr_gid) {
                    sd_bus_creds_unref(creds);
                    /* the subject belongs to one of the groups defined in policy */
                    return true;
                }
            }
        }
    }

end:
    sd_bus_creds_unref(creds);
    return false;
}

static int parse_subject(sd_bus_message *m, struct subject *subject)
{
    int r;
    const char *contents;
    const char *subject_kind;

    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "sa{sv}");
    if (r < 0)
        return r;

    r = sd_bus_message_read(m, "s", &subject_kind);
    if (r < 0)
        return r;

    /* There are three known subject types in polkit: proces, session, and
     * D-Bus name. We parse them all, but support only process and D-Bus based
     * authentication. */

    if (strcmp(subject_kind, "unix-process") == 0)
        subject->kind = SUBJECT_KIND_UNIX_PROCESS;
    else if (strcmp(subject_kind, "unix-session") == 0)
        subject->kind = SUBJECT_KIND_UNIX_SESSION;
    else if (strcmp(subject_kind, "system-bus-name") == 0)
        subject->kind = SUBJECT_KIND_SYSTEM_BUS_NAME;
    else
        return -EINVAL;

    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0)
        return r;

    while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *subject_detail_key;

        r = sd_bus_message_read(m, "s", &subject_detail_key);
        if (r < 0)
            return r;

        r = sd_bus_message_peek_type(m, NULL, &contents);
        if (r < 0)
            return r;

        switch (subject->kind) {
        case SUBJECT_KIND_UNIX_PROCESS:
            if (strcmp(subject_detail_key, "pid") == 0) {
                r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                if (r < 0)
                    return r;

                r = sd_bus_message_read(m, "u", &subject->data.p.pid);
                if (r < 0)
                    return r;

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                    return r;
            }
            else if (strcmp(subject_detail_key, "start-time") == 0) {
                r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                if (r < 0)
                    return r;

                r = sd_bus_message_read(m, "t", &subject->data.p.start_time);
                if (r < 0)
                    return r;

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                    return r;
            }
            break;
        case SUBJECT_KIND_UNIX_SESSION:
            if (strcmp(subject_detail_key, "session-id") == 0) {
                const char *value;

                r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                if (r < 0)
                    return r;

                r = sd_bus_message_read(m, "s", &value);
                if (r < 0)
                    return r;

                if (strlen(value) >= MAX_NAME_SIZE)
                    return r;

                strncpy(subject->data.s.session_id, value, MAX_NAME_SIZE);

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                    return r;
            }
            break;
        case SUBJECT_KIND_SYSTEM_BUS_NAME:
            if (strcmp(subject_detail_key, "name") == 0) {
                const char *value;

                r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                if (r < 0)
                    return r;

                r = sd_bus_message_read(m, "s", &value);
                if (r < 0)
                    return r;

                if (strlen(value) >= MAX_NAME_SIZE)
                    return -EINVAL;

                strncpy(subject->data.s.session_id, value, MAX_NAME_SIZE);

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                    return r;
            }
            break;
        default:
            return -EINVAL;
        }

        /* dict entry */
        r = sd_bus_message_exit_container(m);
        if (r < 0)
            return r;
    }

    /* array */
    r = sd_bus_message_exit_container(m);
    if (r < 0)
        return r;

    /* struct */
    r = sd_bus_message_exit_container(m);
    if (r < 0)
        return r;

    return 0;
}

static void print_decision(struct subject *subject, const char *action_id, bool allowed)
{
    if (subject == NULL || action_id == NULL)
        return;

    switch (subject->kind) {
    case SUBJECT_KIND_UNIX_PROCESS:
        fprintf(stdout, "Unix process (pid: %d, start time: %lu) %sallowed to do action-id %s\n",
                subject->data.p.pid, subject->data.p.start_time, allowed ? "" : "NOT ", action_id);
        break;
    case SUBJECT_KIND_UNIX_SESSION:
        fprintf(stdout, "Unix session (session id: %s) %sallowed to do action-id %s\n",
                subject->data.s.session_id, allowed ? "" : "NOT ", action_id);
        break;
    case SUBJECT_KIND_SYSTEM_BUS_NAME:
        fprintf(stdout, "System bus name %s %sallowed to do action-id %s\n",
                subject->data.b.system_bus_name, allowed ? "" : "NOT ", action_id);
        break;
    default:
        break;
    }
}

static int method_check_authorization(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    int r;
    uint32_t authorization_flags;
    const char *action_id;
    const char *cancellation_id;
    struct subject subject = { 0 };
    sd_bus_message *reply = NULL;
    bool allowed;
    struct line_data *data = userdata;

    /*
        ‣ Type=method_call  Endian=l  Flags=0  Version=1  Priority=0 Cookie=2860
          Sender=:1.0  Destination=org.freedesktop.PolicyKit1  Path=/org/freedesktop/PolicyKit1/Authority  Interface=org.freedesktop.PolicyKit1.Authority  Member=CheckAuthorization
          UniqueName=:1.0
          MESSAGE "(sa{sv})sa{ss}us" {
                  STRUCT "sa{sv}" {
                          STRING "system-bus-name";
                          ARRAY "{sv}" {
                                  DICT_ENTRY "sv" {
                                          STRING "name";
                                          VARIANT "s" {
                                                  STRING ":1.174";
                                          };
                                  };
                          };
                  };
                  STRING "org.freedesktop.systemd1.reload-daemon";
                  ARRAY "{ss}" {
                  };
                  UINT32 1;
                  STRING "";
          };
    */

    /* fprintf(stdout, "Incoming CheckAuthorization message!\n"); */

    r = parse_subject(m, &subject);
    if (r < 0) {
        fprintf(stderr, "Failed to parse subject\n");
        return r;
    }

    r = sd_bus_message_read(m, "s", &action_id);
    if (r < 0) {
        fprintf(stderr, "Failed to read action_id\n");
        return r;
    }

    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{ss}");
    if (r < 0)
        return r;

    while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "ss")) > 0) {
        const char *key;
        const char *value;

        r = sd_bus_message_read(m, "s", &key);
        if (r < 0)
            return r;

        r = sd_bus_message_read(m, "s", &value);
        if (r < 0)
            return r;

        /* dict entry */
        r = sd_bus_message_exit_container(m);
        if (r < 0)
            return r;
    }

    /* array */
    r = sd_bus_message_exit_container(m);
    if (r < 0)
        return r;

    r = sd_bus_message_read(m, "u", &authorization_flags);
    if (r < 0)
        return r;

    r = sd_bus_message_read(m, "s", &cancellation_id);
    if (r < 0)
        return r;

    /* make decision about whether the request should be allowed or not */

    allowed = check_allowed(sd_bus_message_get_bus(m), data, &subject, action_id);

    print_decision(&subject, action_id, allowed);

    /* construct the reply */
    r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0)
        goto end;

    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "bba{ss}");
    if (r < 0)
        goto end;

    r = sd_bus_message_append(reply, "bb", allowed, false);
    if (r < 0)
        goto end;

    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{ss}");
    if (r < 0)
        goto end;

    /* array */
    r = sd_bus_message_close_container(reply);
    if (r < 0)
        goto end;

    /* struct */
    r = sd_bus_message_close_container(reply);
    if (r < 0)
        goto end;

    r = sd_bus_send(NULL, reply, NULL);

end:
    sd_bus_message_unref(reply);
    return r;
}

static int method_cancel_check_authorization(sd_bus_message *m, void *userdata,
        sd_bus_error *ret_error)
{
    /* we are synchronic at the moment, so no possibility of really having time to cancel anything */
    return sd_bus_reply_method_return(m, "");
}

static int method_enumerate_actions(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    int r;
    const char *locale;
    sd_bus_message *reply = NULL;
    struct line_data *line;

    line = userdata;

    r = sd_bus_message_read(m, "s", &locale);
    if (r < 0)
        return r;

    r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0)
        goto end;

    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(ssssssuuua{ss})");
    if (r < 0)
        goto end;

    while (line->id) {
        r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "ssssssuuua{ss}");
        if (r < 0)
            goto end;

        /* just report the id and that authorization is required for all users */
        r = sd_bus_message_append(reply, "ssssssuuu", line->id, "", "", "", "", "", 1, 1, 1);
        if (r < 0)
            goto end;

        r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{ss}");
        if (r < 0)
            goto end;

        /* array */
        r = sd_bus_message_close_container(reply);
        if (r < 0)
            goto end;

        /* struct */
        r = sd_bus_message_close_container(reply);
        if (r < 0)
            goto end;

        line++;
    }

    /* array */
    r = sd_bus_message_close_container(reply);
    if (r < 0)
        goto end;

    r = sd_bus_send(NULL, reply, NULL);

end:
    sd_bus_message_unref(reply);
    return r;
}

static int property_backend_name(sd_bus *bus, const char *path,
        const char *interface, const char *property, sd_bus_message *reply,
        void *userdata, sd_bus_error *error)
{
    return sd_bus_message_append(reply, "s", "groupcheck");
}

static int property_backend_version(sd_bus *bus, const char *path,
        const char *interface, const char *property, sd_bus_message *reply,
        void *userdata, sd_bus_error *error)
{
    return sd_bus_message_append(reply, "s", "0.1");
}

static int property_backend_features(sd_bus *bus, const char *path,
        const char *interface, const char *property, sd_bus_message *reply,
        void *userdata, sd_bus_error *error)
{
    /* we don't support temprorary authorizations */
    return sd_bus_message_append(reply, "u", 0);
}

static const sd_bus_vtable polkit_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("CheckAuthorization", "(sa{sv})sa{ss}us", "(bba{ss})", method_check_authorization, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("CancelCheckAuthorization", "s", "", method_cancel_check_authorization, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("EnumerateActions", "s", "a(ssssssuuua{ss})", method_enumerate_actions, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("BackendName", "s", property_backend_name, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("BackendVersion", "s", property_backend_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("BackendFeatures", "u", property_backend_features, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

static int parse_line(struct line_data *data)
{
    char *p;
    bool has_equals = false;
    bool group_begins = true;

    memset(data->groups, 0, MAX_GROUPS*sizeof(char *));
    data->n_groups = 0;

    /* data->buf has already been initialized with the raw data */

    p = data->id = data->buf;

    while (*p && p != data->buf + sizeof(data->buf)) {
        if (*p == '=') {
            has_equals = true;
            *p = '\0';
            p++;
            break;
        }
        p++;
    }

    if (!has_equals) {
        fprintf(stderr, "Error parsing configuration file.\n");
        return -EINVAL;
    }

    if (*p != '"') {
        fprintf(stderr, "Error parsing configuration file.\n");
        return -EINVAL;
    }

    if (p != data->buf + sizeof(data->buf))
        p++;

    while (*p && p != data->buf + sizeof(data->buf)) {
        if (group_begins) {
            if (data->n_groups >= MAX_GROUPS) {
                fprintf(stderr, "Error: too many groups defined.\n");
                return -EINVAL;
            }
            data->groups[data->n_groups++] = p;
            group_begins = false;
            continue;
        }

        if (*p == ',') {
            group_begins = true;
            *p = '\0';
        }
        else if (*p == '"') {
            /* done parsing the line */
            *p = '\0';
            return 0;
        }
        p++;
    }

    fprintf(stderr, "Error parsing configuration file.\n");
    return -EINVAL;
}

static struct line_data * load_file(const char *filename)
{
    FILE *f;
    char buf[LINE_BUF_SIZE];
    int n_lines = 0;
    int r, i;
    struct line_data *data = NULL;

    f = fopen(filename, "r");

    if (f == NULL)
        return NULL;

    /* The configuration file must be of following format. No whitespaces
     * are allowed except for newlines. First part of the line is the action-id.
     * It is followed by an equation mark and then the comma-separated list of
     * groups inside double quotation marks. Comments are lines starting with
     * '#' character.

       org.freedesktop.login1.reboot="adm,wheel"
       # reboot allowed only for adm group
       org.freedesktop.login1.reboot="adm"

     */

    /* allocate memory for storing the data */
    while (fgets(buf, sizeof(buf), f)) {
        if (strlen(buf) == 0) {
            /* '\0' in line */
            continue;
        }
        else if (buf[0] == '#') {
            /* a comment */
            continue;
        }
        else if (buf[0] == '\n') {
            /* a newline */
            continue;
        }

        data = realloc(data, sizeof(struct line_data)*(n_lines+1));
        memcpy(data[n_lines].buf, buf, LINE_BUF_SIZE);
        n_lines++;
    }

    /* allocate one more line item to be a sentinel and zero it */
    data = realloc(data, sizeof(struct line_data)*(n_lines+1));
    memset(&data[n_lines], 0, sizeof(struct line_data));

    /* parse the lines */
    for (i = 0; i < n_lines; i++) {
        r = parse_line(&data[i]);
        if (r < 0) {
            fclose(f);
            free(data);
            return NULL;
        }
    }

    fclose(f);

    return data;
}

static const char *find_policy_file()
{
    struct stat s;
    const char *dynamic_conf = "/etc/groupcheck.policy";
    const char *default_conf = "/usr/share/defaults/etc/groupcheck.policy";

    if (stat(dynamic_conf, &s) == 0)
        return dynamic_conf;
    else if (stat(default_conf, &s) == 0)
        return default_conf;

    return NULL;
}

int main(int argc, char *argv[])
{
    sd_event *e = NULL;
    sd_bus *bus = NULL;
    sd_bus_slot *slot = NULL;
    struct line_data *data = NULL;
    int r = -1;
    const char *policy_file;

    policy_file = find_policy_file();
    if (!policy_file) {
        fprintf(stderr, "Error finding policy data file.\n");
        goto end;
    }

    data = load_file(policy_file);
    if (!data) {
        fprintf(stderr, "Error loading policy data.\n");
        goto end;
    }

    r = sd_event_default(&e);
    if (r < 0) {
        fprintf(stderr, "Error initializing default event: %s\n", strerror(-r));
        goto end;
    }

    r = sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "Error connecting to bus: %s\n", strerror(-r));
        goto end;
    }

    r = sd_bus_add_object_vtable(bus, &slot,
            "/org/freedesktop/PolicyKit1/Authority",
            "org.freedesktop.PolicyKit1.Authority", polkit_vtable, data);
    if (r < 0) {
        fprintf(stderr, "Error creating D-Bus object: %s\n", strerror(-r));
        goto end;
    }

    r = sd_bus_request_name(bus, "org.freedesktop.PolicyKit1", 0);
    if (r < 0) {
        fprintf(stderr, "Error requesting service name: %s\n", strerror(-r));
        goto end;
    }

    r = sd_bus_attach_event(bus, e, 0);
    if (r < 0) {
        fprintf(stderr, "Error attaching bus to event loop: %s\n", strerror(-r));
        goto end;
    }

    r = sd_event_loop(e);
    if (r < 0) {
        fprintf(stderr, "Exited from event loop with error: %s\n", strerror(-r));
    }

end:
    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);
    sd_event_unref(e);

    free(data);

    fprintf(stdout, "Exiting daemon.\n");

    if (r < 0) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
