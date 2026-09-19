#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#define APP_ID "io.github.linuxtaskmanager.TaskManager"
#define REFRESH_MS 1000
#define PROCESS_REFRESH_TICKS 2

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *stack;

    GtkWidget *process_box;
    GtkWidget *process_count;
    GtkWidget *selected_process_label;
    GtkWidget *advanced_switch;
    GtkWidget *end_task_button;
    gboolean advanced_view;
    gchar *selected_group_key;
    gchar *selected_name;
    uid_t selected_uid;
    GHashTable *expanded_groups;
    unsigned int refresh_tick;

    GtkWidget *cpu_value;
    GtkWidget *cpu_bar;
    GtkWidget *cpu_subtitle;
    GtkWidget *cpu_detail;

    GtkWidget *mem_value;
    GtkWidget *mem_bar;
    GtkWidget *mem_subtitle;
    GtkWidget *mem_detail;

    GtkWidget *gpu_value;
    GtkWidget *gpu_bar;
    GtkWidget *gpu_subtitle;
    GtkWidget *gpu_detail;

    GtkWidget *net_value;
    GtkWidget *net_bar;
    GtkWidget *net_subtitle;
    GtkWidget *net_detail;

    GtkWidget *uptime_value;
    GtkWidget *system_detail;

    unsigned long long prev_total_cpu;
    unsigned long long prev_idle_cpu;
    unsigned long long prev_net_rx;
    unsigned long long prev_net_tx;
    gint64 prev_net_time_us;
} AppState;

static AppState state = {0};

typedef struct {
    gchar *key;
    gchar *name;
    gchar *user;
    gchar *status;
    uid_t uid;
    GArray *pids;
    unsigned long long rss;
} ProcessGroup;

typedef struct {
    GtkWidget *revealer;
    GtkWidget *image;
    gchar *key;
} GroupToggleData;

typedef struct {
    gchar *name;
    uid_t uid;
    GArray *pids;
} ForceKillData;

typedef struct {
    gboolean found;
    char card[64];
    char pci_slot[64];
    char vendor[64];
    char device_id[64];
    char driver[128];
    char model[256];
    double busy;
    unsigned long long vram_total;
    unsigned long long vram_used;
} GpuInfo;

typedef struct {
    unsigned long long rx;
    unsigned long long tx;
    char busiest_if[IFNAMSIZ];
    char ip[INET6_ADDRSTRLEN];
} NetInfo;

static gchar *trim_copy(const char *s) {
    if (!s) return g_strdup("");
    gchar *copy = g_strdup(s);
    g_strstrip(copy);
    return copy;
}

static gchar *read_text_file(const char *path) {
    gchar *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &contents, &len, NULL)) return NULL;
    g_strstrip(contents);
    return contents;
}

static gchar *read_first_matching(const char *path, const char *prefix) {
    FILE *f = fopen(path, "r");
    if (!f) return g_strdup("Unknown");
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (g_str_has_prefix(line, prefix)) {
            char *p = strchr(line, ':');
            if (p) p++; else p = line + strlen(prefix);
            while (*p && g_ascii_isspace(*p)) p++;
            g_strchomp(p);
            fclose(f);
            return g_strdup(p);
        }
    }
    fclose(f);
    return g_strdup("Unknown");
}

static gchar *format_bytes(unsigned long long b) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    return g_strdup_printf(i == 0 ? "%.0f %s" : "%.1f %s", v, units[i]);
}

static gchar *format_rate(unsigned long long bytes_per_second) {
    gchar *base = format_bytes(bytes_per_second);
    gchar *out = g_strdup_printf("%s/s", base);
    g_free(base);
    return out;
}

static gboolean read_cpu_times(unsigned long long *total, unsigned long long *idle) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return FALSE;
    char tag[8];
    unsigned long long user = 0, nice = 0, systemv = 0, idlev = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
    int n = fscanf(f, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
                   tag, &user, &nice, &systemv, &idlev, &iowait, &irq, &softirq, &steal);
    fclose(f);
    if (n < 8 || strcmp(tag, "cpu") != 0) return FALSE;
    *idle = idlev + iowait;
    *total = user + nice + systemv + idlev + iowait + irq + softirq + (n >= 9 ? steal : 0);
    return TRUE;
}

static double get_cpu_usage(void) {
    unsigned long long total = 0, idle = 0;
    if (!read_cpu_times(&total, &idle)) return 0.0;
    if (state.prev_total_cpu == 0) {
        state.prev_total_cpu = total;
        state.prev_idle_cpu = idle;
        return 0.0;
    }
    unsigned long long dt = total - state.prev_total_cpu;
    unsigned long long di = idle - state.prev_idle_cpu;
    state.prev_total_cpu = total;
    state.prev_idle_cpu = idle;
    if (!dt) return 0.0;
    double usage = 100.0 * (double)(dt - di) / (double)dt;
    return CLAMP(usage, 0.0, 100.0);
}

static unsigned long long read_kb_value(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char name[64];
    unsigned long long value = 0;
    char unit[32];
    while (fscanf(f, "%63[^:]: %llu %31s\n", name, &value, unit) == 3) {
        if (strcmp(name, key) == 0) {
            fclose(f);
            return value * 1024ULL;
        }
    }
    fclose(f);
    return 0;
}

static gboolean get_memory(unsigned long long *total, unsigned long long *used,
                           unsigned long long *available, unsigned long long *cached,
                           unsigned long long *swap_total, unsigned long long *swap_used) {
    unsigned long long mt = read_kb_value("MemTotal");
    unsigned long long ma = read_kb_value("MemAvailable");
    unsigned long long cache = read_kb_value("Cached") + read_kb_value("SReclaimable");
    unsigned long long st = read_kb_value("SwapTotal");
    unsigned long long sf = read_kb_value("SwapFree");
    if (!mt) return FALSE;
    *total = mt;
    *available = ma;
    *used = mt > ma ? mt - ma : 0;
    *cached = cache;
    *swap_total = st;
    *swap_used = st > sf ? st - sf : 0;
    return TRUE;
}

static gboolean get_disk(unsigned long long *total, unsigned long long *used) {
    struct statvfs v;
    if (statvfs("/", &v) != 0) return FALSE;
    *total = (unsigned long long)v.f_blocks * v.f_frsize;
    unsigned long long avail = (unsigned long long)v.f_bavail * v.f_frsize;
    *used = *total > avail ? *total - avail : 0;
    return TRUE;
}

static gchar *read_proc_name(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    return read_text_file(path);
}

static gchar *read_proc_state(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return g_strdup("?");
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (g_str_has_prefix(line, "State:")) {
            char *p = line + 6;
            while (*p && g_ascii_isspace(*p)) p++;
            g_strchomp(p);
            fclose(f);
            return g_strdup(p);
        }
    }
    fclose(f);
    return g_strdup("?");
}

static gboolean read_proc_uid(pid_t pid, uid_t *uid_out) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return FALSE;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned long uid = 0;
        if (sscanf(line, "Uid:\t%lu", &uid) == 1 || sscanf(line, "Uid: %lu", &uid) == 1) {
            fclose(f);
            *uid_out = (uid_t)uid;
            return TRUE;
        }
    }
    fclose(f);
    return FALSE;
}

static unsigned long long read_proc_rss(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    unsigned long long kb = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "VmRSS: %llu kB", &kb) == 1) break;
    }
    fclose(f);
    return kb * 1024ULL;
}

static gchar *username_for_uid(uid_t uid) {
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name) return g_strdup(pw->pw_name);
    return g_strdup_printf("%u", (unsigned)uid);
}

static gboolean is_protected_process_name(const char *name) {
    if (!name || !*name) return TRUE;

    static const char *exact[] = {
        "systemd", "(sd-pam)", "init", "systemd-journald", "systemd-logind",
        "systemd-udevd", "systemd-resolved", "systemd-timesyncd", "systemd-oomd",
        "dbus-broker", "dbus-daemon", "polkitd", "NetworkManager",
        "gnome-shell", "plasmashell", "kwin_wayland", "kwin_x11", "ksmserver",
        "kded5", "kded6", "pipewire", "pipewire-pulse", "wireplumber", "pulseaudio",
        "xdg-desktop-portal", "xdg-document-portal", "xdg-permission-store",
        "dconf-service", "at-spi-bus-launcher", "at-spi2-registryd", "Xwayland",
        "gpg-agent", "ssh-agent", "gnome-keyring-daemon", "flatpak-session-helper",
        "kactivitymanagerd", "kglobalacceld", "powerdevil", "xembedsniproxy",
        "xfce4-session", "xfsettingsd", "xfce4-power-manager", "mate-session",
        "gdm", "gdm-wayland-session", "sddm", "sddm-helper"
    };
    for (guint i = 0; i < G_N_ELEMENTS(exact); i++) {
        if (g_strcmp0(name, exact[i]) == 0) return TRUE;
    }

    static const char *prefixes[] = {
        "systemd-", "kworker", "ksoftirqd", "migration/", "rcu_", "rcu-",
        "watchdog/", "irq/", "idle_inject/", "cpuhp/", "kcompactd", "kswapd",
        "gnome-session", "gsd-", "gvfs", "xdg-desktop-portal-", "kwin_", "kded",
        "baloo_", "tracker-miner", "evolution-source"
    };
    for (guint i = 0; i < G_N_ELEMENTS(prefixes); i++) {
        if (g_str_has_prefix(name, prefixes[i])) return TRUE;
    }
    return FALSE;
}

static gboolean should_hide_in_safe_view(pid_t pid, const char *name, uid_t uid) {
    if (uid != getuid()) return TRUE;
    if (pid <= 2 || pid == getpid()) return TRUE;
    if (is_protected_process_name(name)) return TRUE;
    return FALSE;
}

static gchar *make_group_key(uid_t uid, const char *name) {
    return g_strdup_printf("%u:%s", (unsigned)uid, name ? name : "");
}

static void process_group_free(gpointer data) {
    ProcessGroup *g = data;
    if (!g) return;
    g_free(g->key);
    g_free(g->name);
    g_free(g->user);
    g_free(g->status);
    if (g->pids) g_array_free(g->pids, TRUE);
    g_free(g);
}

static gint process_group_compare(gconstpointer a, gconstpointer b) {
    const ProcessGroup *ga = *(ProcessGroup * const *)a;
    const ProcessGroup *gb = *(ProcessGroup * const *)b;
    return g_utf8_collate(ga->name, gb->name);
}

static void clear_selected_process(void) {
    g_clear_pointer(&state.selected_group_key, g_free);
    g_clear_pointer(&state.selected_name, g_free);
    state.selected_uid = (uid_t)-1;
    if (state.selected_process_label)
        gtk_label_set_text(GTK_LABEL(state.selected_process_label), "No process selected");
    if (state.end_task_button)
        gtk_widget_set_sensitive(state.end_task_button, FALSE);
}

static void on_process_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    (void)user_data;
    if (!row) {
        clear_selected_process();
        return;
    }

    const char *key = g_object_get_data(G_OBJECT(row), "group-key");
    const char *name = g_object_get_data(G_OBJECT(row), "proc-name");
    uid_t uid = (uid_t)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "uid"));
    guint count = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "proc-count"));

    g_free(state.selected_group_key);
    g_free(state.selected_name);
    state.selected_group_key = g_strdup(key);
    state.selected_name = g_strdup(name);
    state.selected_uid = uid;

    gchar *label = count > 1
        ? g_strdup_printf("%s • %u processes", name ? name : "Process", count)
        : g_strdup_printf("%s", name ? name : "Process");
    gtk_label_set_text(GTK_LABEL(state.selected_process_label), label);
    gtk_widget_set_sensitive(state.end_task_button, TRUE);
    g_free(label);
}

static GArray *collect_group_pids(uid_t uid, const char *name) {
    GArray *pids = g_array_new(FALSE, FALSE, sizeof(pid_t));
    DIR *d = opendir("/proc");
    if (!d) return pids;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!g_ascii_isdigit(de->d_name[0])) continue;
        char *end = NULL;
        long v = strtol(de->d_name, &end, 10);
        if (*end != '\0' || v <= 0 || v > G_MAXINT) continue;
        pid_t pid = (pid_t)v;
        uid_t owner = (uid_t)-1;
        if (!read_proc_uid(pid, &owner) || owner != uid) continue;
        gchar *proc_name = read_proc_name(pid);
        gboolean match = proc_name && g_strcmp0(proc_name, name) == 0;
        g_free(proc_name);
        if (match) g_array_append_val(pids, pid);
    }
    closedir(d);
    return pids;
}

static void show_message(const char *message) {
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", message);
    gtk_alert_dialog_show(dlg, GTK_WINDOW(state.window));
    g_object_unref(dlg);
}

static void force_kill_data_free(ForceKillData *data) {
    if (!data) return;
    g_free(data->name);
    if (data->pids) g_array_free(data->pids, TRUE);
    g_free(data);
}

static gboolean force_kill_remaining(gpointer user_data) {
    ForceKillData *data = user_data;
    for (guint i = 0; i < data->pids->len; i++) {
        pid_t pid = g_array_index(data->pids, pid_t, i);
        if (pid <= 2 || pid == getpid()) continue;
        if (kill(pid, 0) != 0) continue;

        uid_t uid = (uid_t)-1;
        if (!read_proc_uid(pid, &uid) || uid != data->uid || uid != getuid()) continue;
        gchar *name = read_proc_name(pid);
        gboolean same = name && g_strcmp0(name, data->name) == 0;
        g_free(name);
        if (same && !is_protected_process_name(data->name))
            kill(pid, SIGKILL);
    }
    force_kill_data_free(data);
    return G_SOURCE_REMOVE;
}

static void on_end_task(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (!state.selected_name || !state.selected_group_key) return;

    if (is_protected_process_name(state.selected_name)) {
        show_message("This is a protected Linux/session process. It is visible in Advanced View, but Task Manager will not terminate it.");
        return;
    }

    GArray *pids = collect_group_pids(state.selected_uid, state.selected_name);
    if (pids->len == 0) {
        g_array_free(pids, TRUE);
        clear_selected_process();
        return;
    }

    guint sent = 0;
    int first_error = 0;
    for (guint i = 0; i < pids->len; i++) {
        pid_t pid = g_array_index(pids, pid_t, i);
        if (pid <= 2 || pid == getpid()) continue;
        if (kill(pid, SIGTERM) == 0) sent++;
        else if (!first_error) first_error = errno;
    }

    if (sent == 0) {
        gchar *msg = g_strdup_printf("Could not end %s: %s", state.selected_name,
                                     first_error ? g_strerror(first_error) : "permission denied");
        show_message(msg);
        g_free(msg);
        g_array_free(pids, TRUE);
        return;
    }

    if (state.selected_uid == getuid()) {
        ForceKillData *force = g_new0(ForceKillData, 1);
        force->name = g_strdup(state.selected_name);
        force->uid = state.selected_uid;
        force->pids = pids;
        g_timeout_add(850, force_kill_remaining, force);
    } else {
        g_array_free(pids, TRUE);
    }

    gchar *status = g_strdup_printf("Ending %s…", state.selected_name);
    gtk_label_set_text(GTK_LABEL(state.selected_process_label), status);
    g_free(status);
}

static void group_toggle_data_free(gpointer data) {
    GroupToggleData *d = data;
    if (!d) return;
    g_free(d->key);
    g_free(d);
}

static void on_group_toggle(GtkButton *button, gpointer user_data) {
    (void)user_data;
    GroupToggleData *d = g_object_get_data(G_OBJECT(button), "toggle-data");
    if (!d) return;
    gboolean open = !gtk_revealer_get_reveal_child(GTK_REVEALER(d->revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(d->revealer), open);
    gtk_image_set_from_icon_name(GTK_IMAGE(d->image), open ? "pan-down-symbolic" : "pan-end-symbolic");
    if (open)
        g_hash_table_replace(state.expanded_groups, g_strdup(d->key), GINT_TO_POINTER(1));
    else
        g_hash_table_remove(state.expanded_groups, d->key);
}

static GtkWidget *make_process_grid(const char *name, const char *pid_text, const char *user,
                                    const char *proc_state, unsigned long long rss,
                                    gboolean child, GtkWidget *leading) {
    GtkWidget *grid = gtk_grid_new();
    gtk_widget_set_margin_top(grid, child ? 5 : 8);
    gtk_widget_set_margin_bottom(grid, child ? 5 : 8);
    gtk_widget_set_margin_start(grid, child ? 34 : 12);
    gtk_widget_set_margin_end(grid, 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);

    GtkWidget *name_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_hexpand(name_box, TRUE);
    gtk_widget_set_size_request(name_box, 370, -1);
    if (leading) gtk_box_append(GTK_BOX(name_box), leading);
    else {
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(spacer, 26, 1);
        gtk_box_append(GTK_BOX(name_box), spacer);
    }
    GtkWidget *name_l = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(name_l), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(name_l), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name_l, TRUE);
    if (!child) gtk_widget_add_css_class(name_l, "process-name");
    else gtk_widget_add_css_class(name_l, "dim-label");
    gtk_box_append(GTK_BOX(name_box), name_l);

    GtkWidget *pid_l = gtk_label_new(pid_text);
    gtk_label_set_xalign(GTK_LABEL(pid_l), 0.0);
    gtk_widget_set_size_request(pid_l, 82, -1);
    if (child) gtk_widget_add_css_class(pid_l, "dim-label");

    GtkWidget *user_l = gtk_label_new(user);
    gtk_label_set_xalign(GTK_LABEL(user_l), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(user_l), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(user_l, 120, -1);
    if (child) gtk_widget_add_css_class(user_l, "dim-label");

    GtkWidget *state_l = gtk_label_new(proc_state);
    gtk_label_set_xalign(GTK_LABEL(state_l), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(state_l), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(state_l, 150, -1);
    if (child) gtk_widget_add_css_class(state_l, "dim-label");

    gchar *rss_s = format_bytes(rss);
    GtkWidget *rss_l = gtk_label_new(rss_s);
    gtk_label_set_xalign(GTK_LABEL(rss_l), 1.0);
    gtk_widget_set_size_request(rss_l, 110, -1);
    if (child) gtk_widget_add_css_class(rss_l, "dim-label");
    g_free(rss_s);

    gtk_grid_attach(GTK_GRID(grid), name_box, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), pid_l, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), user_l, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state_l, 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), rss_l, 4, 0, 1, 1);
    return grid;
}

static GtkWidget *make_group_row(ProcessGroup *group) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    guint count = group->pids->len;
    GtkWidget *leading = NULL;
    GtkWidget *revealer = NULL;
    GtkWidget *arrow_image = NULL;

    if (count > 1) {
        arrow_image = gtk_image_new_from_icon_name("pan-end-symbolic");
        GtkWidget *arrow = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(arrow), arrow_image);
        gtk_widget_add_css_class(arrow, "flat");
        gtk_widget_add_css_class(arrow, "disclosure-button");
        gtk_widget_set_tooltip_text(arrow, "Show grouped processes");
        leading = arrow;
    }

    gchar *display_name = count > 1
        ? g_strdup_printf("%s  (%u)", group->name, count)
        : g_strdup(group->name);
    gchar *pid_text = count == 1
        ? g_strdup_printf("%d", g_array_index(group->pids, pid_t, 0))
        : g_strdup("—");
    const char *status = count > 1 ? "Running" : group->status;
    GtkWidget *summary = make_process_grid(display_name, pid_text, group->user, status, group->rss, FALSE, leading);
    gtk_box_append(GTK_BOX(outer), summary);
    g_free(display_name);
    g_free(pid_text);

    if (count > 1) {
        revealer = gtk_revealer_new();
        gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
        gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 150);

        GtkWidget *children = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(children, "group-children");
        for (guint i = 0; i < group->pids->len; i++) {
            pid_t pid = g_array_index(group->pids, pid_t, i);
            gchar *p = g_strdup_printf("%d", pid);
            gchar *ps = read_proc_state(pid);
            unsigned long long rss = read_proc_rss(pid);
            GtkWidget *child_grid = make_process_grid(group->name, p, group->user, ps, rss, TRUE, NULL);
            gtk_box_append(GTK_BOX(children), child_grid);
            g_free(p);
            g_free(ps);
        }
        gtk_revealer_set_child(GTK_REVEALER(revealer), children);
        gtk_box_append(GTK_BOX(outer), revealer);

        gboolean open = g_hash_table_contains(state.expanded_groups, group->key);
        gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), open);
        gtk_image_set_from_icon_name(GTK_IMAGE(arrow_image), open ? "pan-down-symbolic" : "pan-end-symbolic");

        GroupToggleData *td = g_new0(GroupToggleData, 1);
        td->revealer = revealer;
        td->image = arrow_image;
        td->key = g_strdup(group->key);
        g_object_set_data_full(G_OBJECT(leading), "toggle-data", td, group_toggle_data_free);
        g_signal_connect(leading, "clicked", G_CALLBACK(on_group_toggle), NULL);
    }

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), outer);
    g_object_set_data_full(G_OBJECT(row), "group-key", g_strdup(group->key), g_free);
    g_object_set_data_full(G_OBJECT(row), "proc-name", g_strdup(group->name), g_free);
    g_object_set_data(G_OBJECT(row), "uid", GUINT_TO_POINTER((guint)group->uid));
    g_object_set_data(G_OBJECT(row), "proc-count", GUINT_TO_POINTER(count));
    return row;
}

static void refresh_processes(void) {
    gchar *wanted_key = g_strdup(state.selected_group_key);
    GtkListBoxRow *wanted_row = NULL;

    while (TRUE) {
        GtkWidget *child = gtk_widget_get_first_child(state.process_box);
        if (!child) break;
        gtk_list_box_remove(GTK_LIST_BOX(state.process_box), child);
    }

    GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, process_group_free);
    guint raw_visible = 0;
    guint hidden = 0;

    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!g_ascii_isdigit(de->d_name[0])) continue;
            char *end = NULL;
            long v = strtol(de->d_name, &end, 10);
            if (*end != '\0' || v <= 0 || v > G_MAXINT) continue;
            pid_t pid = (pid_t)v;
            uid_t uid = (uid_t)-1;
            if (!read_proc_uid(pid, &uid)) continue;

            gchar *name = read_proc_name(pid);
            if (!name || !*name) {
                g_free(name);
                continue;
            }

            if (!state.advanced_view && should_hide_in_safe_view(pid, name, uid)) {
                hidden++;
                g_free(name);
                continue;
            }

            raw_visible++;
            gchar *key = make_group_key(uid, name);
            ProcessGroup *group = g_hash_table_lookup(groups, key);
            if (!group) {
                group = g_new0(ProcessGroup, 1);
                group->key = g_strdup(key);
                group->name = g_strdup(name);
                group->uid = uid;
                group->user = username_for_uid(uid);
                group->status = read_proc_state(pid);
                group->pids = g_array_new(FALSE, FALSE, sizeof(pid_t));
                g_hash_table_insert(groups, g_strdup(key), group);
            }
            g_array_append_val(group->pids, pid);
            group->rss += read_proc_rss(pid);
            g_free(key);
            g_free(name);
        }
        closedir(d);
    }

    GPtrArray *ordered = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, groups);
    while (g_hash_table_iter_next(&iter, &key, &value))
        g_ptr_array_add(ordered, value);
    g_ptr_array_sort(ordered, process_group_compare);

    for (guint i = 0; i < ordered->len; i++) {
        ProcessGroup *group = g_ptr_array_index(ordered, i);
        GtkWidget *row = make_group_row(group);
        gtk_list_box_append(GTK_LIST_BOX(state.process_box), row);
        if (wanted_key && g_strcmp0(group->key, wanted_key) == 0)
            wanted_row = GTK_LIST_BOX_ROW(row);
    }

    if (wanted_row) {
        gtk_list_box_select_row(GTK_LIST_BOX(state.process_box), wanted_row);
    } else if (wanted_key) {
        clear_selected_process();
    }

    gchar *count_text;
    if (state.advanced_view) {
        count_text = g_strdup_printf("%u grouped entries • %u running processes • Advanced View",
                                     ordered->len, raw_visible);
    } else {
        count_text = g_strdup_printf("%u apps • duplicates grouped • %u system/background processes hidden",
                                     ordered->len, hidden);
    }
    gtk_label_set_text(GTK_LABEL(state.process_count), count_text);
    g_free(count_text);

    g_ptr_array_free(ordered, TRUE);
    g_hash_table_destroy(groups);
    g_free(wanted_key);
}

static void on_advanced_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    state.advanced_view = gtk_switch_get_active(sw);
    clear_selected_process();
    refresh_processes();
}

static gchar *cpu_frequency_string(void) {
    const char *paths[] = {
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq"
    };
    for (guint i = 0; i < G_N_ELEMENTS(paths); i++) {
        gchar *s = read_text_file(paths[i]);
        if (s) {
            double khz = g_ascii_strtod(s, NULL);
            g_free(s);
            if (khz > 0) return g_strdup_printf("%.2f GHz", khz / 1000000.0);
        }
    }
    gchar *mhz = read_first_matching("/proc/cpuinfo", "cpu MHz");
    double v = g_ascii_strtod(mhz, NULL);
    g_free(mhz);
    return v > 0 ? g_strdup_printf("%.2f GHz", v / 1000.0) : g_strdup("Unknown");
}

static gchar *vendor_name_from_id(const char *vendor) {
    if (!vendor) return g_strdup("Unknown");
    if (g_ascii_strcasecmp(vendor, "0x1002") == 0) return g_strdup("AMD");
    if (g_ascii_strcasecmp(vendor, "0x10de") == 0) return g_strdup("NVIDIA");
    if (g_ascii_strcasecmp(vendor, "0x8086") == 0) return g_strdup("Intel");
    return g_strdup(vendor);
}

static unsigned long long read_ull_file(const char *path) {
    gchar *s = read_text_file(path);
    if (!s) return 0;
    unsigned long long v = g_ascii_strtoull(s, NULL, 10);
    g_free(s);
    return v;
}

static gchar *spawn_capture(gchar **argv) {
    gchar *stdout_text = NULL;
    gchar *stderr_text = NULL;
    gint status = 0;
    GError *error = NULL;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                               &stdout_text, &stderr_text, &status, &error);
    g_free(stderr_text);
    if (!ok || status != 0) {
        g_clear_error(&error);
        g_free(stdout_text);
        return NULL;
    }
    g_clear_error(&error);
    return stdout_text;
}

static gchar *extract_property(const char *text, const char *property) {
    if (!text) return NULL;
    gchar **lines = g_strsplit(text, "\n", -1);
    gchar *result = NULL;
    for (guint i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], property)) {
            result = g_strdup(lines[i] + strlen(property));
            g_strstrip(result);
            break;
        }
    }
    g_strfreev(lines);
    return result;
}

static gchar *extract_contains_value(const char *text, const char *needle, char separator) {
    if (!text) return NULL;
    gchar **lines = g_strsplit(text, "\n", -1);
    gchar *result = NULL;
    for (guint i = 0; lines[i]; i++) {
        char *hit = strstr(lines[i], needle);
        if (!hit) continue;
        char *sep = strchr(hit, separator);
        if (!sep) continue;
        result = g_strdup(sep + 1);
        g_strstrip(result);
        if (*result) break;
        g_clear_pointer(&result, g_free);
    }
    g_strfreev(lines);
    return result;
}

static gchar *quoted_field(const char *line, int index) {
    if (!line) return NULL;
    int found = 0;
    const char *p = line;
    while ((p = strchr(p, '"')) != NULL) {
        const char *start = ++p;
        const char *end = strchr(start, '"');
        if (!end) break;
        if (found == index) return g_strndup(start, end - start);
        found++;
        p = end + 1;
    }
    return NULL;
}

static gchar *detect_gpu_model(const char *device_dir, const char *pci_slot, const char *vendor_id) {
    const char *sysfs_names[] = {"product_name", "marketing_name", "name"};
    for (guint i = 0; i < G_N_ELEMENTS(sysfs_names); i++) {
        gchar *path = g_build_filename(device_dir, sysfs_names[i], NULL);
        gchar *value = read_text_file(path);
        g_free(path);
        if (value && *value && g_ascii_strcasecmp(value, "unknown") != 0)
            return value;
        g_free(value);
    }

    gchar *program = g_find_program_in_path("vulkaninfo");
    if (program) {
        g_free(program);
        gchar *argv[] = {"vulkaninfo", "--summary", NULL};
        gchar *out = spawn_capture(argv);
        gchar *model = extract_contains_value(out, "deviceName", '=');
        g_free(out);
        if (model && *model) return model;
        g_free(model);
    }

    program = g_find_program_in_path("glxinfo");
    if (program) {
        g_free(program);
        gchar *argv[] = {"glxinfo", "-B", NULL};
        gchar *out = spawn_capture(argv);
        gchar *model = extract_contains_value(out, "Device:", ':');
        g_free(out);
        if (model && *model) return model;
        g_free(model);
    }

    program = g_find_program_in_path("udevadm");
    if (program) {
        g_free(program);
        gchar *argv[] = {"udevadm", "info", "--query=property", "--path", (gchar *)device_dir, NULL};
        gchar *out = spawn_capture(argv);
        gchar *model = extract_property(out, "ID_MODEL_FROM_DATABASE=");
        g_free(out);
        if (model && *model) return model;
        g_free(model);
    }

    program = g_find_program_in_path("lspci");
    if (pci_slot && *pci_slot && program) {
        g_free(program);
        gchar *argv[] = {"lspci", "-s", (gchar *)pci_slot, "-mm", NULL};
        gchar *out = spawn_capture(argv);
        gchar *model = quoted_field(out, 2);
        g_free(out);
        if (model && *model) return model;
        g_free(model);
    } else {
        g_free(program);
    }

    gchar *vendor = vendor_name_from_id(vendor_id);
    gchar *fallback = g_strdup_printf("%s GPU", vendor);
    g_free(vendor);
    return fallback;
}

static void get_gpu_info(GpuInfo *gpu) {
    memset(gpu, 0, sizeof *gpu);
    gpu->busy = -1.0;

    DIR *d = opendir("/sys/class/drm");
    if (!d) return;

    struct dirent *de;
    char chosen_device_dir[PATH_MAX] = {0};
    gboolean chosen_boot = FALSE;
    while ((de = readdir(d))) {
        if (!g_str_has_prefix(de->d_name, "card")) continue;
        const char *n = de->d_name + 4;
        if (!*n) continue;
        gboolean digits_only = TRUE;
        for (const char *p = n; *p; p++) if (!g_ascii_isdigit(*p)) digits_only = FALSE;
        if (!digits_only) continue;

        char device_dir[PATH_MAX];
        snprintf(device_dir, sizeof device_dir, "/sys/class/drm/%s/device", de->d_name);
        struct stat st;
        if (stat(device_dir, &st) != 0) continue;

        char boot_path[PATH_MAX];
        snprintf(boot_path, sizeof boot_path, "%s/boot_vga", device_dir);
        gchar *boot = read_text_file(boot_path);
        gboolean is_boot = boot && strcmp(boot, "1") == 0;
        g_free(boot);

        if (!chosen_device_dir[0] || (is_boot && !chosen_boot)) {
            g_strlcpy(chosen_device_dir, device_dir, sizeof chosen_device_dir);
            g_strlcpy(gpu->card, de->d_name, sizeof gpu->card);
            chosen_boot = is_boot;
        }
        if (chosen_boot) break;
    }
    closedir(d);
    if (!chosen_device_dir[0]) return;

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/vendor", chosen_device_dir);
    gchar *vendor = read_text_file(path);
    snprintf(path, sizeof path, "%s/device", chosen_device_dir);
    gchar *device = read_text_file(path);
    if (vendor) g_strlcpy(gpu->vendor, vendor, sizeof gpu->vendor);
    if (device) g_strlcpy(gpu->device_id, device, sizeof gpu->device_id);
    g_free(vendor);
    g_free(device);

    char real[PATH_MAX];
    if (realpath(chosen_device_dir, real)) {
        const char *base = strrchr(real, '/');
        if (base && base[1]) g_strlcpy(gpu->pci_slot, base + 1, sizeof gpu->pci_slot);
    }

    snprintf(path, sizeof path, "%s/driver", chosen_device_dir);
    char linkbuf[PATH_MAX];
    ssize_t len = readlink(path, linkbuf, sizeof linkbuf - 1);
    if (len > 0) {
        linkbuf[len] = '\0';
        const char *base = strrchr(linkbuf, '/');
        g_strlcpy(gpu->driver, base ? base + 1 : linkbuf, sizeof gpu->driver);
    } else {
        g_strlcpy(gpu->driver, "Unknown", sizeof gpu->driver);
    }

    static char cached_slot[64] = {0};
    static char cached_model[256] = {0};
    if (gpu->pci_slot[0] && g_strcmp0(cached_slot, gpu->pci_slot) == 0 && cached_model[0]) {
        g_strlcpy(gpu->model, cached_model, sizeof gpu->model);
    } else {
        gchar *model = detect_gpu_model(chosen_device_dir, gpu->pci_slot, gpu->vendor);
        if (model) {
            g_strlcpy(gpu->model, model, sizeof gpu->model);
            g_strlcpy(cached_model, model, sizeof cached_model);
            g_strlcpy(cached_slot, gpu->pci_slot, sizeof cached_slot);
            g_free(model);
        }
    }

    snprintf(path, sizeof path, "%s/gpu_busy_percent", chosen_device_dir);
    gchar *busy = read_text_file(path);
    if (busy) {
        gpu->busy = g_ascii_strtod(busy, NULL);
        g_free(busy);
    }
    snprintf(path, sizeof path, "%s/mem_info_vram_total", chosen_device_dir);
    gpu->vram_total = read_ull_file(path);
    snprintf(path, sizeof path, "%s/mem_info_vram_used", chosen_device_dir);
    gpu->vram_used = read_ull_file(path);
    gpu->found = TRUE;
}

static void get_network_info(NetInfo *net) {
    memset(net, 0, sizeof *net);
    FILE *f = fopen("/proc/net/dev", "r");
    if (f) {
        char line[512];
        int line_no = 0;
        unsigned long long best_total = 0;
        while (fgets(line, sizeof line, f)) {
            line_no++;
            if (line_no <= 2) continue;
            char ifname[IFNAMSIZ] = {0};
            unsigned long long rx = 0, tx = 0;
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = '\0';
            gchar *iftrim = trim_copy(line);
            g_strlcpy(ifname, iftrim, sizeof ifname);
            g_free(iftrim);
            if (strcmp(ifname, "lo") == 0) continue;
            if (sscanf(colon + 1,
                       " %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                       &rx, &tx) != 2) continue;
            net->rx += rx;
            net->tx += tx;
            if (rx + tx > best_total) {
                best_total = rx + tx;
                g_strlcpy(net->busiest_if, ifname, sizeof net->busiest_if);
            }
        }
        fclose(f);
    }

    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == 0) {
        for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || !*net->busiest_if) continue;
            if (strcmp(ifa->ifa_name, net->busiest_if) != 0) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &sa->sin_addr, net->ip, sizeof net->ip);
                break;
            }
        }
        freeifaddrs(ifaddr);
    }
}

static GtkWidget *info_row(const char *left, const char *right) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(box, 3);
    gtk_widget_set_margin_bottom(box, 3);
    GtkWidget *l = gtk_label_new(left);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_widget_add_css_class(l, "secondary-text");
    GtkWidget *r = gtk_label_new(right ? right : "—");
    gtk_label_set_xalign(GTK_LABEL(r), 1.0);
    gtk_label_set_selectable(GTK_LABEL(r), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(r), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), r);
    return box;
}

static GtkWidget *metric_card(const char *title, const char *icon_name, GtkWidget **value_out,
                              GtkWidget **subtitle_out, GtkWidget **bar_out) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_add_css_class(box, "metric-card");
    gtk_widget_set_hexpand(box, TRUE);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_add_css_class(icon, "metric-icon");
    GtkWidget *title_l = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_l), 0.0);
    gtk_widget_add_css_class(title_l, "card-title");
    gtk_box_append(GTK_BOX(head), icon);
    gtk_box_append(GTK_BOX(head), title_l);

    GtkWidget *value = gtk_label_new("0%");
    gtk_label_set_xalign(GTK_LABEL(value), 0.0);
    gtk_widget_add_css_class(value, "metric-big");

    GtkWidget *subtitle = gtk_label_new("Collecting data…");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(subtitle, "secondary-text");

    GtkWidget *bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), FALSE);

    gtk_box_append(GTK_BOX(box), head);
    gtk_box_append(GTK_BOX(box), value);
    gtk_box_append(GTK_BOX(box), subtitle);
    gtk_box_append(GTK_BOX(box), bar);
    *value_out = value;
    *subtitle_out = subtitle;
    *bar_out = bar;
    return box;
}

static void replace_box_contents(GtkWidget *box) {
    while (TRUE) {
        GtkWidget *child = gtk_widget_get_first_child(box);
        if (!child) break;
        gtk_box_remove(GTK_BOX(box), child);
    }
}

static void update_performance_details(double cpu, unsigned long long mt, unsigned long long mu,
                                       unsigned long long ma, unsigned long long cached,
                                       unsigned long long swap_total, unsigned long long swap_used,
                                       const GpuInfo *gpu, const NetInfo *net,
                                       unsigned long long rx_rate, unsigned long long tx_rate) {
    replace_box_contents(state.cpu_detail);
    gchar *freq = cpu_frequency_string();
    gchar *cpu_name = read_first_matching("/proc/cpuinfo", "model name");
    long threads = sysconf(_SC_NPROCESSORS_ONLN);
    gchar *usage = g_strdup_printf("%.1f%%", cpu);
    gchar *threads_s = g_strdup_printf("%ld", threads);
    gtk_box_append(GTK_BOX(state.cpu_detail), info_row("Processor", cpu_name));
    gtk_box_append(GTK_BOX(state.cpu_detail), info_row("Utilization", usage));
    gtk_box_append(GTK_BOX(state.cpu_detail), info_row("Current speed", freq));
    gtk_box_append(GTK_BOX(state.cpu_detail), info_row("Logical processors", threads_s));
    g_free(freq); g_free(cpu_name); g_free(usage); g_free(threads_s);

    replace_box_contents(state.mem_detail);
    gchar *mts = format_bytes(mt), *mus = format_bytes(mu), *mas = format_bytes(ma);
    gchar *cs = format_bytes(cached), *sts = format_bytes(swap_total), *sus = format_bytes(swap_used);
    gtk_box_append(GTK_BOX(state.mem_detail), info_row("Installed", mts));
    gtk_box_append(GTK_BOX(state.mem_detail), info_row("In use", mus));
    gtk_box_append(GTK_BOX(state.mem_detail), info_row("Available", mas));
    gtk_box_append(GTK_BOX(state.mem_detail), info_row("Cached", cs));
    gtk_box_append(GTK_BOX(state.mem_detail), info_row("Swap total", sts));
    gtk_box_append(GTK_BOX(state.mem_detail), info_row("Swap used", sus));
    g_free(mts); g_free(mus); g_free(mas); g_free(cs); g_free(sts); g_free(sus);

    replace_box_contents(state.gpu_detail);
    if (gpu->found) {
        gchar *vendor = vendor_name_from_id(gpu->vendor);
        gchar *id = g_strdup_printf("%s:%s", gpu->vendor[0] ? gpu->vendor : "?", gpu->device_id[0] ? gpu->device_id : "?");
        gchar *busy = gpu->busy >= 0 ? g_strdup_printf("%.0f%%", gpu->busy) : g_strdup("Unavailable");
        gchar *vt = gpu->vram_total ? format_bytes(gpu->vram_total) : g_strdup("Unavailable");
        gchar *vu = gpu->vram_total ? format_bytes(gpu->vram_used) : g_strdup("Unavailable");
        gtk_box_append(GTK_BOX(state.gpu_detail), info_row("GPU", gpu->model[0] ? gpu->model : "Unknown GPU"));
        gtk_box_append(GTK_BOX(state.gpu_detail), info_row("Vendor", vendor));
        gtk_box_append(GTK_BOX(state.gpu_detail), info_row("Driver", gpu->driver));
        gtk_box_append(GTK_BOX(state.gpu_detail), info_row("PCI slot", gpu->pci_slot[0] ? gpu->pci_slot : "Unavailable"));
        gtk_box_append(GTK_BOX(state.gpu_detail), info_row("PCI device ID", id));
        gtk_box_append(GTK_BOX(state.gpu_detail), info_row("Utilization", busy));
        gtk_box_append(GTK_BOX(state.gpu_detail), info_row("Dedicated memory", vt));
        gtk_box_append(GTK_BOX(state.gpu_detail), info_row("Dedicated memory used", vu));
        g_free(vendor); g_free(id); g_free(busy); g_free(vt); g_free(vu);
    } else {
        gtk_box_append(GTK_BOX(state.gpu_detail), info_row("GPU", "No DRM GPU detected"));
    }

    replace_box_contents(state.net_detail);
    gchar *rxs = format_rate(rx_rate), *txs = format_rate(tx_rate);
    gchar *rxt = format_bytes(net->rx), *txt = format_bytes(net->tx);
    gtk_box_append(GTK_BOX(state.net_detail), info_row("Interface", net->busiest_if[0] ? net->busiest_if : "None"));
    gtk_box_append(GTK_BOX(state.net_detail), info_row("IPv4", net->ip[0] ? net->ip : "Unavailable"));
    gtk_box_append(GTK_BOX(state.net_detail), info_row("Receive", rxs));
    gtk_box_append(GTK_BOX(state.net_detail), info_row("Send", txs));
    gtk_box_append(GTK_BOX(state.net_detail), info_row("Total received", rxt));
    gtk_box_append(GTK_BOX(state.net_detail), info_row("Total sent", txt));
    g_free(rxs); g_free(txs); g_free(rxt); g_free(txt);
}

static gboolean refresh_stats(gpointer user_data) {
    (void)user_data;
    double cpu = get_cpu_usage();
    gchar *cpu_s = g_strdup_printf("%.0f%%", cpu);
    gtk_label_set_text(GTK_LABEL(state.cpu_value), cpu_s);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state.cpu_bar), cpu / 100.0);
    gchar *freq = cpu_frequency_string();
    gchar *cpu_sub = g_strdup_printf("%s • %ld logical processors", freq, sysconf(_SC_NPROCESSORS_ONLN));
    gtk_label_set_text(GTK_LABEL(state.cpu_subtitle), cpu_sub);
    g_free(cpu_s); g_free(freq); g_free(cpu_sub);

    unsigned long long mt = 0, mu = 0, ma = 0, cached = 0, st = 0, su = 0;
    if (get_memory(&mt, &mu, &ma, &cached, &st, &su)) {
        double pct = mt ? 100.0 * (double)mu / (double)mt : 0.0;
        gchar *v = g_strdup_printf("%.0f%%", pct);
        gchar *u = format_bytes(mu), *t = format_bytes(mt);
        gchar *sub = g_strdup_printf("%s of %s in use", u, t);
        gtk_label_set_text(GTK_LABEL(state.mem_value), v);
        gtk_label_set_text(GTK_LABEL(state.mem_subtitle), sub);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state.mem_bar), CLAMP(pct / 100.0, 0.0, 1.0));
        g_free(v); g_free(u); g_free(t); g_free(sub);
    }

    GpuInfo gpu;
    get_gpu_info(&gpu);
    if (gpu.found) {
        if (gpu.busy >= 0) {
            gchar *v = g_strdup_printf("%.0f%%", gpu.busy);
            gtk_label_set_text(GTK_LABEL(state.gpu_value), v);
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state.gpu_bar), CLAMP(gpu.busy / 100.0, 0.0, 1.0));
            g_free(v);
        } else {
            gtk_label_set_text(GTK_LABEL(state.gpu_value), "N/A");
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state.gpu_bar), 0.0);
        }
        gchar *sub = g_strdup_printf("%s • %s", gpu.model[0] ? gpu.model : "Unknown GPU",
                                     gpu.driver[0] ? gpu.driver : "unknown driver");
        gtk_label_set_text(GTK_LABEL(state.gpu_subtitle), sub);
        g_free(sub);
    } else {
        gtk_label_set_text(GTK_LABEL(state.gpu_value), "N/A");
        gtk_label_set_text(GTK_LABEL(state.gpu_subtitle), "No DRM GPU detected");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state.gpu_bar), 0.0);
    }

    NetInfo net;
    get_network_info(&net);
    gint64 now = g_get_monotonic_time();
    unsigned long long rx_rate = 0, tx_rate = 0;
    if (state.prev_net_time_us > 0 && now > state.prev_net_time_us) {
        double seconds = (double)(now - state.prev_net_time_us) / 1000000.0;
        if (net.rx >= state.prev_net_rx) rx_rate = (unsigned long long)((net.rx - state.prev_net_rx) / seconds);
        if (net.tx >= state.prev_net_tx) tx_rate = (unsigned long long)((net.tx - state.prev_net_tx) / seconds);
    }
    state.prev_net_rx = net.rx;
    state.prev_net_tx = net.tx;
    state.prev_net_time_us = now;

    gchar *rx = format_rate(rx_rate), *tx = format_rate(tx_rate);
    gtk_label_set_text(GTK_LABEL(state.net_value), rx);
    gchar *net_sub = g_strdup_printf("Receive %s • Send %s", rx, tx);
    gtk_label_set_text(GTK_LABEL(state.net_subtitle), net_sub);
    double activity_hint = (double)(rx_rate + tx_rate) / (100.0 * 1024.0 * 1024.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state.net_bar), CLAMP(activity_hint, 0.0, 1.0));
    g_free(rx); g_free(tx); g_free(net_sub);

    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        unsigned long sec = si.uptime;
        unsigned long days = sec / 86400; sec %= 86400;
        unsigned long hours = sec / 3600; sec %= 3600;
        unsigned long mins = sec / 60;
        gchar *s = g_strdup_printf("%lud %luh %lum", days, hours, mins);
        gtk_label_set_text(GTK_LABEL(state.uptime_value), s);
        g_free(s);
    }

    update_performance_details(cpu, mt, mu, ma, cached, st, su, &gpu, &net, rx_rate, tx_rate);

    state.refresh_tick++;
    if (state.refresh_tick % PROCESS_REFRESH_TICKS == 0)
        refresh_processes();
    return G_SOURCE_CONTINUE;
}

static GtkWidget *build_process_tab(void) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(root, "page");
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 20);
    gtk_widget_set_margin_end(root, 20);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *heading_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(heading_box, TRUE);
    GtkWidget *heading = gtk_label_new("Processes");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_widget_add_css_class(heading, "page-heading");
    state.process_count = gtk_label_new("Loading processes…");
    gtk_label_set_xalign(GTK_LABEL(state.process_count), 0.0);
    gtk_widget_add_css_class(state.process_count, "secondary-text");
    gtk_box_append(GTK_BOX(heading_box), heading);
    gtk_box_append(GTK_BOX(heading_box), state.process_count);

    GtkWidget *advanced_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(advanced_box, GTK_ALIGN_CENTER);
    GtkWidget *advanced_label = gtk_label_new("Advanced View");
    state.advanced_switch = gtk_switch_new();
    gtk_widget_set_tooltip_text(state.advanced_switch, "Reveal system services and background infrastructure");
    g_signal_connect(state.advanced_switch, "notify::active", G_CALLBACK(on_advanced_toggled), NULL);
    gtk_box_append(GTK_BOX(advanced_box), advanced_label);
    gtk_box_append(GTK_BOX(advanced_box), state.advanced_switch);

    gtk_box_append(GTK_BOX(top), heading_box);
    gtk_box_append(GTK_BOX(top), advanced_box);
    gtk_box_append(GTK_BOX(root), top);

    GtkWidget *surface = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(surface, "surface-card");
    gtk_widget_set_vexpand(surface, TRUE);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(toolbar, 12);
    gtk_widget_set_margin_end(toolbar, 12);
    gtk_widget_set_margin_top(toolbar, 10);
    gtk_widget_set_margin_bottom(toolbar, 10);
    state.selected_process_label = gtk_label_new("No process selected");
    gtk_label_set_xalign(GTK_LABEL(state.selected_process_label), 0.0);
    gtk_widget_set_hexpand(state.selected_process_label, TRUE);
    gtk_widget_add_css_class(state.selected_process_label, "secondary-text");
    state.end_task_button = gtk_button_new_with_label("End task");
    gtk_widget_add_css_class(state.end_task_button, "destructive-action");
    gtk_widget_set_sensitive(state.end_task_button, FALSE);
    g_signal_connect(state.end_task_button, "clicked", G_CALLBACK(on_end_task), NULL);
    gtk_box_append(GTK_BOX(toolbar), state.selected_process_label);
    gtk_box_append(GTK_BOX(toolbar), state.end_task_button);

    GtkWidget *header = gtk_grid_new();
    gtk_widget_add_css_class(header, "table-header");
    gtk_grid_set_column_spacing(GTK_GRID(header), 16);
    gtk_widget_set_margin_start(header, 12);
    gtk_widget_set_margin_end(header, 12);
    gtk_widget_set_margin_top(header, 8);
    gtk_widget_set_margin_bottom(header, 8);
    const char *heads[] = {"Name", "PID", "User", "Status", "Memory"};
    int widths[] = {370, 82, 120, 150, 110};
    for (int i = 0; i < 5; i++) {
        GtkWidget *l = gtk_label_new(heads[i]);
        gtk_label_set_xalign(GTK_LABEL(l), i == 4 ? 1.0 : 0.0);
        gtk_widget_set_size_request(l, widths[i], -1);
        if (i == 0) gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_add_css_class(l, "table-heading");
        gtk_grid_attach(GTK_GRID(header), l, i, 0, 1, 1);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    state.process_box = gtk_list_box_new();
    gtk_widget_add_css_class(state.process_box, "process-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state.process_box), GTK_SELECTION_SINGLE);
    g_signal_connect(state.process_box, "row-selected", G_CALLBACK(on_process_row_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), state.process_box);

    gtk_box_append(GTK_BOX(surface), toolbar);
    gtk_box_append(GTK_BOX(surface), header);
    gtk_box_append(GTK_BOX(surface), scroll);
    gtk_box_append(GTK_BOX(root), surface);
    return root;
}

static GtkWidget *detail_panel(const char *title, GtkWidget **content_out) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_add_css_class(box, "detail-card");
    GtkWidget *title_l = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_l), 0.0);
    gtk_widget_add_css_class(title_l, "card-title");
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(box), title_l);
    gtk_box_append(GTK_BOX(box), content);
    *content_out = content;
    return box;
}

static GtkWidget *build_performance_tab(void) {
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(root, "page");
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 22);
    gtk_widget_set_margin_start(root, 20);
    gtk_widget_set_margin_end(root, 20);

    GtkWidget *heading = gtk_label_new("Performance");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_widget_add_css_class(heading, "page-heading");
    gtk_box_append(GTK_BOX(root), heading);

    GtkWidget *metrics = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(metrics), 12);
    gtk_grid_set_column_spacing(GTK_GRID(metrics), 12);
    gtk_grid_set_column_homogeneous(GTK_GRID(metrics), TRUE);

    GtkWidget *cpu = metric_card("CPU", "utilities-system-monitor-symbolic", &state.cpu_value, &state.cpu_subtitle, &state.cpu_bar);
    GtkWidget *mem = metric_card("Memory", "media-flash-symbolic", &state.mem_value, &state.mem_subtitle, &state.mem_bar);
    GtkWidget *gpu = metric_card("GPU", "video-display-symbolic", &state.gpu_value, &state.gpu_subtitle, &state.gpu_bar);
    GtkWidget *net = metric_card("Network", "network-wired-symbolic", &state.net_value, &state.net_subtitle, &state.net_bar);
    gtk_grid_attach(GTK_GRID(metrics), cpu, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(metrics), mem, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(metrics), gpu, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(metrics), net, 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(root), metrics);

    GtkWidget *detail_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(detail_grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(detail_grid), 12);
    gtk_grid_set_column_homogeneous(GTK_GRID(detail_grid), TRUE);
    GtkWidget *cpu_d = detail_panel("CPU details", &state.cpu_detail);
    GtkWidget *mem_d = detail_panel("Memory details", &state.mem_detail);
    GtkWidget *gpu_d = detail_panel("GPU details", &state.gpu_detail);
    GtkWidget *net_d = detail_panel("Network details", &state.net_detail);
    gtk_grid_attach(GTK_GRID(detail_grid), cpu_d, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(detail_grid), mem_d, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(detail_grid), gpu_d, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(detail_grid), net_d, 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(root), detail_grid);

    GtkWidget *sys = detail_panel("System", &state.system_detail);
    struct utsname u;
    uname(&u);
    const char *hostname = g_get_host_name();
    unsigned long long dt = 0, du = 0;
    get_disk(&dt, &du);
    gchar *disk_total = format_bytes(dt);
    gchar *disk_used = format_bytes(du);
    gchar *disk = g_strdup_printf("%s used of %s", disk_used, disk_total);
    gtk_box_append(GTK_BOX(state.system_detail), info_row("Hostname", hostname));
    gtk_box_append(GTK_BOX(state.system_detail), info_row("Operating system", u.sysname));
    gtk_box_append(GTK_BOX(state.system_detail), info_row("Kernel", u.release));
    gtk_box_append(GTK_BOX(state.system_detail), info_row("Architecture", u.machine));
    gtk_box_append(GTK_BOX(state.system_detail), info_row("Root filesystem", disk));
    GtkWidget *upt = info_row("Uptime", "Starting…");
    state.uptime_value = gtk_widget_get_last_child(upt);
    gtk_box_append(GTK_BOX(state.system_detail), upt);
    g_free(disk_total); g_free(disk_used); g_free(disk);
    gtk_box_append(GTK_BOX(root), sys);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), root);
    return scroll;
}

static void on_nav_toggled(GtkToggleButton *button, gpointer user_data) {
    if (!gtk_toggle_button_get_active(button)) return;
    const char *page = user_data;
    gtk_stack_set_visible_child_name(GTK_STACK(state.stack), page);
}

static GtkWidget *make_nav_button(const char *label, const char *icon_name, const char *page,
                                  GtkToggleButton *group) {
    GtkWidget *button = gtk_toggle_button_new();
    if (group) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button), group);
    gtk_widget_add_css_class(button, "nav-button");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    GtkWidget *text = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), text);
    gtk_button_set_child(GTK_BUTTON(button), box);
    g_signal_connect(button, "toggled", G_CALLBACK(on_nav_toggled), (gpointer)page);
    return button;
}

static void install_css(void) {
    /*
     * Do not use desktop-theme background variables here. Transparent KDE/GTK
     * themes can intentionally define @window_bg_color/@view_bg_color with an
     * alpha channel, which made Task Manager itself translucent. These colors
     * are owned by the app and are deliberately 100% opaque.
     */
    const char *css =
        "@define-color tm_bg #111318;"
        "@define-color tm_sidebar #15181d;"
        "@define-color tm_surface #1b1f26;"
        "@define-color tm_surface_alt #20252d;"
        "@define-color tm_hover #282e38;"
        "@define-color tm_border #323844;"
        "@define-color tm_text #f3f5f7;"
        "@define-color tm_muted #a9b0bb;"
        "@define-color tm_accent #4b8df8;"
        "@define-color tm_accent_hover #64a0ff;"
        "@define-color tm_danger #d84b4b;"
        "@define-color tm_danger_hover #ed5b5b;"

        "window.task-manager-window,"
        "window.task-manager-window.background,"
        ".app-shell, stack, stack > *, .page,"
        "scrolledwindow, scrolledwindow viewport {"
        "  background-color: @tm_bg;"
        "  background-image: none;"
        "  color: @tm_text;"
        "  opacity: 1;"
        "}"
        ".app-shell { background-color: @tm_bg; }"
        ".sidebar {"
        "  background-color: @tm_sidebar;"
        "  background-image: none;"
        "  color: @tm_text;"
        "  border-right: 1px solid @tm_border;"
        "  padding: 12px;"
        "  opacity: 1;"
        "}"
        ".brand { font-size: 20px; font-weight: 800; margin: 8px 8px 18px 8px; color: @tm_text; }"
        ".page-heading { font-size: 24px; font-weight: 800; color: @tm_text; }"
        ".card-title { font-size: 15px; font-weight: 750; color: @tm_text; }"
        ".metric-big { font-size: 32px; font-weight: 800; color: @tm_text; }"
        ".secondary-text { color: @tm_muted; opacity: 1; }"
        ".table-heading { font-size: 12px; font-weight: 700; color: @tm_muted; opacity: 1; }"
        ".process-name { font-weight: 650; color: @tm_text; }"

        ".surface-card, .metric-card, .detail-card {"
        "  background-color: @tm_surface;"
        "  background-image: none;"
        "  color: @tm_text;"
        "  border: 1px solid @tm_border;"
        "  border-radius: 12px;"
        "  opacity: 1;"
        "}"
        ".metric-card, .detail-card { padding: 16px; }"
        ".surface-card { padding: 0; }"
        ".table-header {"
        "  background-color: @tm_surface_alt;"
        "  background-image: none;"
        "  border-top: 1px solid @tm_border;"
        "  border-bottom: 1px solid @tm_border;"
        "  color: @tm_text;"
        "  opacity: 1;"
        "}"
        ".process-list { background-color: @tm_surface; background-image: none; color: @tm_text; opacity: 1; }"
        ".process-list row {"
        "  background-color: @tm_surface;"
        "  background-image: none;"
        "  color: @tm_text;"
        "  border-bottom: 1px solid @tm_border;"
        "  opacity: 1;"
        "}"
        ".process-list row:hover { background-color: @tm_hover; }"
        ".process-list row:selected { background-color: @tm_accent; color: white; }"
        ".process-list row:selected .secondary-text { color: white; }"
        ".group-children {"
        "  background-color: @tm_surface_alt;"
        "  background-image: none;"
        "  border-top: 1px solid @tm_border;"
        "  opacity: 1;"
        "}"
        ".disclosure-button { min-width: 26px; min-height: 26px; padding: 0; }"

        ".nav-button {"
        "  background-color: transparent;"
        "  background-image: none;"
        "  color: @tm_text;"
        "  border: 0;"
        "  border-radius: 8px;"
        "  padding: 9px 10px;"
        "  margin-bottom: 4px;"
        "  box-shadow: none;"
        "}"
        ".nav-button:hover { background-color: @tm_hover; }"
        ".nav-button:checked { background-color: @tm_accent; color: white; }"

        "button.destructive-action {"
        "  background-color: @tm_danger;"
        "  background-image: none;"
        "  color: white;"
        "  border: 0;"
        "  box-shadow: none;"
        "}"
        "button.destructive-action:hover { background-color: @tm_danger_hover; }"
        "button.destructive-action:disabled { background-color: @tm_surface_alt; color: @tm_muted; }"

        "progressbar trough {"
        "  min-height: 8px;"
        "  border-radius: 8px;"
        "  background-color: @tm_surface_alt;"
        "  background-image: none;"
        "}"
        "progressbar progress {"
        "  min-height: 8px;"
        "  border-radius: 8px;"
        "  background-color: @tm_accent;"
        "  background-image: none;"
        "}"
        "scrollbar trough { background-color: @tm_bg; background-image: none; }"
        "scrollbar slider { background-color: #59616e; min-width: 8px; min-height: 8px; border-radius: 8px; }";

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER + 100);
    g_object_unref(provider);
}

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    if (state.window) {
        gtk_window_present(GTK_WINDOW(state.window));
        return;
    }

    state.app = app;
    state.selected_uid = (uid_t)-1;
    state.advanced_view = FALSE;
    state.expanded_groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    install_css();

    state.window = gtk_application_window_new(app);
    gtk_widget_add_css_class(state.window, "task-manager-window");
    gtk_widget_set_opacity(state.window, 1.0);
    gtk_window_set_title(GTK_WINDOW(state.window), "Task Manager");
    gtk_window_set_default_size(GTK_WINDOW(state.window), 1180, 780);

    GtkWidget *shell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(shell, "app-shell");

    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 190, -1);

    GtkWidget *brand = gtk_label_new("Task Manager");
    gtk_label_set_xalign(GTK_LABEL(brand), 0.0);
    gtk_widget_add_css_class(brand, "brand");
    gtk_box_append(GTK_BOX(sidebar), brand);

    GtkWidget *proc_nav = make_nav_button("Processes", "view-list-symbolic", "processes", NULL);
    GtkWidget *perf_nav = make_nav_button("Performance", "utilities-system-monitor-symbolic", "performance", GTK_TOGGLE_BUTTON(proc_nav));
    gtk_box_append(GTK_BOX(sidebar), proc_nav);
    gtk_box_append(GTK_BOX(sidebar), perf_nav);

    GtkWidget *safe_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_margin_start(safe_box, 8);
    gtk_widget_set_margin_end(safe_box, 8);
    gtk_widget_set_margin_top(safe_box, 14);
    gtk_widget_set_valign(safe_box, GTK_ALIGN_END);
    gtk_widget_set_vexpand(safe_box, TRUE);
    GtkWidget *safe_icon = gtk_image_new_from_icon_name("security-high-symbolic");
    GtkWidget *safe_label = gtk_label_new("Safe process view");
    gtk_label_set_xalign(GTK_LABEL(safe_label), 0.0);
    gtk_widget_add_css_class(safe_label, "secondary-text");
    gtk_box_append(GTK_BOX(safe_box), safe_icon);
    gtk_box_append(GTK_BOX(safe_box), safe_label);
    gtk_box_append(GTK_BOX(sidebar), safe_box);

    state.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(state.stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(state.stack), 130);
    gtk_widget_set_hexpand(state.stack, TRUE);
    gtk_widget_set_vexpand(state.stack, TRUE);
    gtk_stack_add_named(GTK_STACK(state.stack), build_process_tab(), "processes");
    gtk_stack_add_named(GTK_STACK(state.stack), build_performance_tab(), "performance");

    gtk_box_append(GTK_BOX(shell), sidebar);
    gtk_box_append(GTK_BOX(shell), state.stack);
    gtk_window_set_child(GTK_WINDOW(state.window), shell);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(proc_nav), TRUE);
    refresh_processes();
    refresh_stats(NULL);
    g_timeout_add(REFRESH_MS, refresh_stats, NULL);
    gtk_window_present(GTK_WINDOW(state.window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
