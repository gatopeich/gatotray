/*
 * process_browser.c - Full GUI process browser for gatotray
 *
 * Provides a GTK window showing live system status and a sortable
 * process list, with non-modal click-through pop-ups for:
 *   - Individual process details (CPU, mem, I/O, network, threads, FDs)
 *   - TCP connection details (bytes sent/received, RTT)
 *
 * Public API:
 *   pb_show_main()  – open (or raise) the main process browser window
 *   pb_close_all()  – close every open browser / pop-up window
 *
 * This file is #included from gatotray.c after cpu_usage.c, net_stats.c,
 * settings.c, and top_procs.c, so all their symbols are visible here.
 */

/* ================================================================
 * Window tracking – pb_all_windows keeps every open browser window
 * so that pb_close_all() can destroy them all in one shot.
 * ================================================================ */

static GSList   *pb_all_windows  = NULL;
static GtkWidget *pb_main_window = NULL; /* singleton main window */

static void pb_on_window_destroy(GtkWidget *w, gpointer unused)
{
    (void)unused;
    pb_all_windows = g_slist_remove(pb_all_windows, w);
    if (w == pb_main_window)
        pb_main_window = NULL;
}

/* Register a window for tracking; it will be removed automatically
 * when destroyed. */
static void pb_track(GtkWidget *w)
{
    pb_all_windows = g_slist_prepend(pb_all_windows, w);
    g_signal_connect(w, "destroy", G_CALLBACK(pb_on_window_destroy), NULL);
}

/* Close every tracked window. */
void pb_close_all(void)
{
    /* Copy the list first; each destroy callback modifies pb_all_windows. */
    GSList *copy = g_slist_copy(pb_all_windows);
    for (GSList *l = copy; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_slist_free(copy);
}

/* ================================================================
 * Connection detail pop-up
 * Shows a snapshot of TCP sockets held by one process.
 * ================================================================ */

static void show_connection_detail(unsigned pid, const char *comm)
{
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gchar *title = g_strdup_printf("Connections: %s  [PID %u]", comm, pid);
    gtk_window_set_title(GTK_WINDOW(win), title);
    g_free(title);
    gtk_window_set_default_size(GTK_WINDOW(win), 580, 300);
    pb_track(win);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* Header */
    gchar *hdr = g_strdup_printf(
        "<b>TCP connections for %s (PID %u)</b>\n"
        "<small>Snapshot taken at time of opening – close and reopen to refresh.</small>",
        comm, pid);
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), hdr);
    g_free(hdr);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), FALSE);
    gtk_misc_set_alignment(GTK_MISC(lbl), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);

    /* Build the connection list model: inode | bytes_sent | bytes_recv | RTT */
    enum { CC_INODE, CC_SENT, CC_RECV, CC_RTT, N_CC };
    GtkListStore *store = gtk_list_store_new(N_CC,
        G_TYPE_UINT,   /* CC_INODE */
        G_TYPE_STRING, /* CC_SENT  */
        G_TYPE_STRING, /* CC_RECV  */
        G_TYPE_STRING  /* CC_RTT   */
    );

    int found = 0;
    for (int i = 0; i < n_inode_map; i++) {
        if (inode_map[i].pid != pid) continue;
        SockStat *ss = sock_hash_lookup(inode_map[i].inode);
        if (!ss) continue;
        found++;

        gchar *sent = g_strdup_printf("%llu KB",
            (unsigned long long)(ss->bytes_acked    / 1024));
        gchar *recv = g_strdup_printf("%llu KB",
            (unsigned long long)(ss->bytes_received / 1024));
        gchar *rtt  = ss->rtt_us
            ? g_strdup_printf("%.1f ms", ss->rtt_us / 1000.0)
            : g_strdup("N/A");

        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it,
            CC_INODE, ss->inode,
            CC_SENT,  sent,
            CC_RECV,  recv,
            CC_RTT,   rtt,
            -1);
        g_free(sent); g_free(recv); g_free(rtt);
    }

    if (!found) {
        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it,
            CC_INODE, (guint)0,
            CC_SENT,  "—",
            CC_RECV,  "—",
            CC_RTT,   "No TCP connections found for this process",
            -1);
    }

    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(tv), TRUE);

    const gchar *col_titles[N_CC] = { "Socket Inode", "Bytes Sent", "Bytes Recv", "RTT" };
    for (int c = 0; c < N_CC; c++) {
        GtkCellRenderer *cr = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            col_titles[c], cr, "text", c, NULL);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    gtk_widget_show_all(win);
}

/* ================================================================
 * Process detail pop-up
 * Shows live-updating resource stats for a single process.
 * ================================================================ */

typedef struct {
    unsigned  pid;
    char      comm[32];
    GtkWidget *window;
    guint     timer_id;
    GtkLabel  *lbl_cpu, *lbl_avg, *lbl_io, *lbl_mem;
    GtkLabel  *lbl_fds, *lbl_socks, *lbl_thrs, *lbl_net;
    GtkLabel  *lbl_rtt;
} ProcDetailData;

/* Update all stat labels for the detail window. */
static void proc_detail_update(ProcDetailData *d)
{
    /* Find the process in the live list by PID. */
    ProcessInfo *p = NULL;
    for (ProcessInfo *pi = top_procs; pi; pi = pi->next) {
        if (pi->pid == d->pid) { p = pi; break; }
    }

    if (!p) {
        gtk_label_set_text(d->lbl_cpu,   "(process exited)");
        gtk_label_set_text(d->lbl_avg,   "—");
        gtk_label_set_text(d->lbl_io,    "—");
        gtk_label_set_text(d->lbl_mem,   "—");
        gtk_label_set_text(d->lbl_fds,   "—");
        gtk_label_set_text(d->lbl_socks, "—");
        gtk_label_set_text(d->lbl_thrs,  "—");
        gtk_label_set_text(d->lbl_net,   "—");
        gtk_label_set_text(d->lbl_rtt,   "—");
        return;
    }

    gchar *s;
    s = g_strdup_printf("%.2g%%", p->cpu > 0.005f ? p->cpu : 0.0f);
    gtk_label_set_text(d->lbl_cpu, s); g_free(s);

    s = g_strdup_printf("%.2g%%", p->average_cpu > 0.005f ? p->average_cpu : 0.0f);
    gtk_label_set_text(d->lbl_avg, s); g_free(s);

    s = g_strdup_printf("%.2g%%", p->io_wait > 0.005f ? p->io_wait : 0.0f);
    gtk_label_set_text(d->lbl_io, s); g_free(s);

    float gb = p->rss * PAGE_GB();
    s = g_strdup_printf("%.3g GB", gb);
    gtk_label_set_text(d->lbl_mem, s); g_free(s);

    s = g_strdup_printf("%u", p->fd_count);
    gtk_label_set_text(d->lbl_fds, s); g_free(s);

    s = g_strdup_printf("%u", p->socket_count);
    gtk_label_set_text(d->lbl_socks, s); g_free(s);

    s = g_strdup_printf("%u", p->thread_count);
    gtk_label_set_text(d->lbl_thrs, s); g_free(s);

    if (p->net_rx_KBps || p->net_tx_KBps)
        s = g_strdup_printf("down %d  up %d  KB/s",
            p->net_rx_KBps, p->net_tx_KBps);
    else
        s = g_strdup("—");
    gtk_label_set_text(d->lbl_net, s); g_free(s);

    s = p->min_rtt_us
        ? g_strdup_printf("%.1f ms", p->min_rtt_us / 1000.0f)
        : g_strdup("—");
    gtk_label_set_text(d->lbl_rtt, s); g_free(s);
}

static gboolean proc_detail_timer_cb(gpointer user_data)
{
    ProcDetailData *d = (ProcDetailData *)user_data;
    if (!d->window || !GTK_IS_WIDGET(d->window))
        return FALSE; /* window was destroyed – stop timer */
    proc_detail_update(d);
    return TRUE;
}

static void proc_detail_on_destroy(GtkWidget *w, gpointer user_data)
{
    (void)w;
    ProcDetailData *d = (ProcDetailData *)user_data;
    if (d->timer_id) {
        g_source_remove(d->timer_id);
        d->timer_id = 0;
    }
    g_free(d);
}

static void on_view_connections_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    ProcDetailData *d = (ProcDetailData *)user_data;
    show_connection_detail(d->pid, d->comm);
}

/* Helper: append a (label | value) row to a 2-column GtkTable. */
static GtkLabel *detail_add_row(GtkWidget *table, int row,
    const gchar *label_text)
{
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_misc_set_alignment(GTK_MISC(lbl), 1.0f, 0.5f);
    gtk_table_attach(GTK_TABLE(table), lbl, 0, 1, row, row + 1,
        GTK_FILL, GTK_FILL, 0, 0);

    GtkWidget *val = gtk_label_new("—");
    gtk_misc_set_alignment(GTK_MISC(val), 0.0f, 0.5f);
    gtk_table_attach(GTK_TABLE(table), val, 1, 2, row, row + 1,
        (GtkAttachOptions)(GTK_FILL | GTK_EXPAND), GTK_FILL, 0, 0);

    return GTK_LABEL(val);
}

static void show_process_detail(unsigned pid, const char *comm)
{
    ProcDetailData *d = g_new0(ProcDetailData, 1);
    d->pid = pid;
    g_strlcpy(d->comm, comm, sizeof(d->comm));

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    d->window = win;

    gchar *title = g_strdup_printf("Process: %s  [PID %u]", comm, pid);
    gtk_window_set_title(GTK_WINDOW(win), title);
    g_free(title);
    gtk_window_set_default_size(GTK_WINDOW(win), 420, 360);
    pb_track(win);
    g_signal_connect(win, "destroy", G_CALLBACK(proc_detail_on_destroy), d);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* Header */
    gchar *hdr = g_strdup_printf(
        "<big><b>%s</b></big>  <small>(PID %u)</small>", comm, pid);
    GtkWidget *hdr_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr_lbl), hdr);
    g_free(hdr);
    gtk_misc_set_alignment(GTK_MISC(hdr_lbl), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(vbox), hdr_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_hseparator_new(), FALSE, FALSE, 0);

    /* Stats table */
    GtkWidget *table = gtk_table_new(9, 2, FALSE);
    gtk_table_set_col_spacings(GTK_TABLE(table), 16);
    gtk_table_set_row_spacings(GTK_TABLE(table), 4);
    gtk_box_pack_start(GTK_BOX(vbox), table, FALSE, FALSE, 0);

    d->lbl_cpu   = detail_add_row(table, 0, "CPU (current):");
    d->lbl_avg   = detail_add_row(table, 1, "CPU (average):");
    d->lbl_io    = detail_add_row(table, 2, "I/O wait:");
    d->lbl_mem   = detail_add_row(table, 3, "Memory:");
    d->lbl_fds   = detail_add_row(table, 4, "File descriptors:");
    d->lbl_socks = detail_add_row(table, 5, "Sockets:");
    d->lbl_thrs  = detail_add_row(table, 6, "Threads:");
    d->lbl_net   = detail_add_row(table, 7, "Network I/O:");
    d->lbl_rtt   = detail_add_row(table, 8, "Min RTT:");

    proc_detail_update(d); /* initial fill */

    gtk_box_pack_start(GTK_BOX(vbox), gtk_hseparator_new(), FALSE, FALSE, 0);

    /* Button to open connection detail */
    GtkWidget *btn = gtk_button_new_with_label("View TCP Connection Details...");
    g_signal_connect(btn, "clicked",
        G_CALLBACK(on_view_connections_clicked), d);
    gtk_box_pack_start(GTK_BOX(vbox), btn, FALSE, FALSE, 0);

    /* Start live-update timer */
    d->timer_id = g_timeout_add(refresh_interval_ms,
        proc_detail_timer_cb, d);

    gtk_widget_show_all(win);
}

/* ================================================================
 * Main process browser window (singleton)
 * ================================================================ */

/* Column indices for the process list model */
enum ProcCol {
    PC_PID = 0,  /* G_TYPE_UINT   */
    PC_COMM,     /* G_TYPE_STRING */
    PC_CPU,      /* G_TYPE_FLOAT  – current CPU % */
    PC_AVG_CPU,  /* G_TYPE_FLOAT  – lifetime-average CPU % */
    PC_IO,       /* G_TYPE_FLOAT  – I/O-wait % */
    PC_MEM_GB,   /* G_TYPE_FLOAT  – RSS in GB */
    PC_FDS,      /* G_TYPE_UINT   */
    PC_SOCKETS,  /* G_TYPE_UINT   */
    PC_THREADS,  /* G_TYPE_UINT   */
    PC_NET_RX,   /* G_TYPE_INT    – KB/s */
    PC_NET_TX,   /* G_TYPE_INT    – KB/s */
    PC_RTT_MS,   /* G_TYPE_FLOAT  – min RTT in ms, 0 = N/A */
    N_PC
};

typedef struct {
    GtkWidget    *window;
    GtkListStore *store;
    GtkLabel     *status_lbl;
    guint         timer_id;
} MainBrowserData;

static MainBrowserData *pb_main_data = NULL;

/* Rebuild the system-status label and the whole process list. */
static void pb_main_update(MainBrowserData *mbd)
{
    /* --- System status line --- */
    MemInfo mi = mem_info();
    int cpu_pct = PERCENT(history[0].cpu.usage);
    int io_pct  = PERCENT(history[0].cpu.iowait);
    const char *cpu_icon = cpu_pct > CPU_HIGH_THRESHOLD ? "High" : "Normal";
    gchar *status;
    if (mi.Total_MB)
        status = g_strdup_printf(
            "CPU: %d%% busy (%s)  |  I/O wait: %d%%  |  "
            "RAM: %d / %d MB free  |  "
            "Processes: %d total, %d active",
            cpu_pct, cpu_icon, io_pct,
            mi.Available_MB, mi.Total_MB,
            procs_total, procs_active);
    else
        status = g_strdup_printf(
            "CPU: %d%% busy (%s)  |  I/O wait: %d%%  |  "
            "Processes: %d total, %d active",
            cpu_pct, cpu_icon, io_pct,
            procs_total, procs_active);
    gtk_label_set_text(mbd->status_lbl, status);
    g_free(status);

    /* --- Rebuild the process list --- */
    GtkListStore *store = mbd->store;
    gtk_list_store_clear(store);

    for (ProcessInfo *p = top_procs; p; p = p->next) {
        if (p->pid == 0) continue;
        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        float gb     = p->rss * PAGE_GB();
        float rtt_ms = p->min_rtt_us ? p->min_rtt_us / 1000.0f : 0.0f;
        gtk_list_store_set(store, &it,
            PC_PID,     (guint)p->pid,
            PC_COMM,    p->comm,
            PC_CPU,     p->cpu,
            PC_AVG_CPU, p->average_cpu,
            PC_IO,      p->io_wait,
            PC_MEM_GB,  gb,
            PC_FDS,     (guint)p->fd_count,
            PC_SOCKETS, (guint)p->socket_count,
            PC_THREADS, (guint)p->thread_count,
            PC_NET_RX,  (gint)p->net_rx_KBps,
            PC_NET_TX,  (gint)p->net_tx_KBps,
            PC_RTT_MS,  rtt_ms,
            -1);
    }
}

static gboolean pb_main_timer_cb(gpointer user_data)
{
    MainBrowserData *mbd = (MainBrowserData *)user_data;
    if (!mbd->window || !GTK_IS_WIDGET(mbd->window))
        return FALSE;
    pb_main_update(mbd);
    return TRUE;
}

static void pb_main_on_destroy(GtkWidget *w, gpointer user_data)
{
    (void)w;
    MainBrowserData *mbd = (MainBrowserData *)user_data;
    if (mbd->timer_id) {
        g_source_remove(mbd->timer_id);
        mbd->timer_id = 0;
    }
    /* pb_on_window_destroy already removed w from pb_all_windows and
     * cleared pb_main_window (it runs first because pb_track connected
     * its handler before we connect this one). */
    pb_main_data = NULL;
    g_free(mbd);
}

/* ---- Cell-data formatter callbacks ---- */

static void cell_fmt_float_pct(GtkTreeViewColumn *col, GtkCellRenderer *cell,
    GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)col;
    gfloat val;
    gtk_tree_model_get(model, iter, GPOINTER_TO_INT(data), &val, -1);
    gchar *s = val > 0.005f ? g_strdup_printf("%.2g%%", val) : g_strdup("0");
    g_object_set(cell, "text", s, NULL);
    g_free(s);
}

static void cell_fmt_float_gb(GtkTreeViewColumn *col, GtkCellRenderer *cell,
    GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)col;
    gfloat val;
    gtk_tree_model_get(model, iter, GPOINTER_TO_INT(data), &val, -1);
    gchar *s = val > 0.0f ? g_strdup_printf("%.3g GB", val) : g_strdup("0");
    g_object_set(cell, "text", s, NULL);
    g_free(s);
}

static void cell_fmt_uint(GtkTreeViewColumn *col, GtkCellRenderer *cell,
    GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)col;
    guint val;
    gtk_tree_model_get(model, iter, GPOINTER_TO_INT(data), &val, -1);
    gchar *s = g_strdup_printf("%u", val);
    g_object_set(cell, "text", s, NULL);
    g_free(s);
}

static void cell_fmt_kbps(GtkTreeViewColumn *col, GtkCellRenderer *cell,
    GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)col;
    gint val;
    gtk_tree_model_get(model, iter, GPOINTER_TO_INT(data), &val, -1);
    gchar *s = val ? g_strdup_printf("%d KB/s", val) : g_strdup("—");
    g_object_set(cell, "text", s, NULL);
    g_free(s);
}

static void cell_fmt_rtt(GtkTreeViewColumn *col, GtkCellRenderer *cell,
    GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)col; (void)data;
    gfloat val;
    gtk_tree_model_get(model, iter, PC_RTT_MS, &val, -1);
    gchar *s = val > 0.0f ? g_strdup_printf("%.1f ms", val) : g_strdup("—");
    g_object_set(cell, "text", s, NULL);
    g_free(s);
}

/* Append a column that uses a cell-data-func and sorts by col_id. */
static void pb_add_col(GtkTreeView *tv, const gchar *title,
    int col_id, GtkTreeCellDataFunc fmt)
{
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, title);
    gtk_tree_view_column_pack_start(col, cr, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, cr, fmt,
        GINT_TO_POINTER(col_id), NULL);
    gtk_tree_view_column_set_sort_column_id(col, col_id);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 56);
    gtk_tree_view_append_column(tv, col);
}

/* Called when the user double-clicks (or presses Enter on) a process row. */
static void pb_row_activated(GtkTreeView *tv, GtkTreePath *path,
    GtkTreeViewColumn *col, gpointer data)
{
    (void)col; (void)data;
    GtkTreeModel *model = gtk_tree_view_get_model(tv);
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(model, &it, path))
        return;
    guint  pid;
    gchar *comm = NULL;
    gtk_tree_model_get(model, &it, PC_PID, &pid, PC_COMM, &comm, -1);
    if (pid > 0)
        show_process_detail(pid, comm ? comm : "?");
    g_free(comm);
}

/* Open (or raise) the main process browser window. */
void pb_show_main(void)
{
    if (pb_main_window) {
        gtk_window_present(GTK_WINDOW(pb_main_window));
        return;
    }

    MainBrowserData *mbd = g_new0(MainBrowserData, 1);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    pb_main_window  = win;
    mbd->window     = win;

    gtk_window_set_title(GTK_WINDOW(win), "gatotray — Process Browser");
    gtk_window_set_default_size(GTK_WINDOW(win), 1050, 620);

    /* pb_track registers pb_on_window_destroy (clears pb_main_window).
     * pb_main_on_destroy cleans up mbd. Both run on destroy. */
    pb_track(win);
    g_signal_connect(win, "destroy", G_CALLBACK(pb_main_on_destroy), mbd);

    /* ---- Layout ---- */
    GtkWidget *vbox = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* System status frame */
    GtkWidget *frame = gtk_frame_new("System Status");
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    GtkWidget *status_lbl = gtk_label_new("Loading…");
    gtk_misc_set_alignment(GTK_MISC(status_lbl), 0.0f, 0.5f);
    gtk_container_set_border_width(GTK_CONTAINER(frame), 4);
    gtk_container_add(GTK_CONTAINER(frame), status_lbl);
    mbd->status_lbl = GTK_LABEL(status_lbl);

    /* Hint row */
    GtkWidget *hint = gtk_label_new(
        "Double-click a row to open process details.  "
        "Click a column header to sort.");
    gtk_misc_set_alignment(GTK_MISC(hint), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(vbox), hint, FALSE, FALSE, 0);

    /* ---- Process list model ---- */
    GtkListStore *store = gtk_list_store_new(N_PC,
        G_TYPE_UINT,   /* PC_PID     */
        G_TYPE_STRING, /* PC_COMM    */
        G_TYPE_FLOAT,  /* PC_CPU     */
        G_TYPE_FLOAT,  /* PC_AVG_CPU */
        G_TYPE_FLOAT,  /* PC_IO      */
        G_TYPE_FLOAT,  /* PC_MEM_GB  */
        G_TYPE_UINT,   /* PC_FDS     */
        G_TYPE_UINT,   /* PC_SOCKETS */
        G_TYPE_UINT,   /* PC_THREADS */
        G_TYPE_INT,    /* PC_NET_RX  */
        G_TYPE_INT,    /* PC_NET_TX  */
        G_TYPE_FLOAT   /* PC_RTT_MS  */
    );
    mbd->store = store;

    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store); /* view holds the only reference from here */
    gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(tv), TRUE);
    gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(tv), TRUE);
    g_signal_connect(tv, "row-activated", G_CALLBACK(pb_row_activated), NULL);

    /* PID column */
    pb_add_col(GTK_TREE_VIEW(tv), "PID",       PC_PID,     cell_fmt_uint);
    /* Name column (plain text, no data-func needed) */
    {
        GtkCellRenderer *cr = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            "Name", cr, "text", PC_COMM, NULL);
        gtk_tree_view_column_set_sort_column_id(col, PC_COMM);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_min_width(col, 100);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }
    pb_add_col(GTK_TREE_VIEW(tv), "CPU%",      PC_CPU,     cell_fmt_float_pct);
    pb_add_col(GTK_TREE_VIEW(tv), "Avg CPU%",  PC_AVG_CPU, cell_fmt_float_pct);
    pb_add_col(GTK_TREE_VIEW(tv), "I/O wait%", PC_IO,      cell_fmt_float_pct);
    pb_add_col(GTK_TREE_VIEW(tv), "Memory",    PC_MEM_GB,  cell_fmt_float_gb);
    pb_add_col(GTK_TREE_VIEW(tv), "FDs",       PC_FDS,     cell_fmt_uint);
    pb_add_col(GTK_TREE_VIEW(tv), "Sockets",   PC_SOCKETS, cell_fmt_uint);
    pb_add_col(GTK_TREE_VIEW(tv), "Threads",   PC_THREADS, cell_fmt_uint);
    pb_add_col(GTK_TREE_VIEW(tv), "Net down",  PC_NET_RX,  cell_fmt_kbps);
    pb_add_col(GTK_TREE_VIEW(tv), "Net up",    PC_NET_TX,  cell_fmt_kbps);
    pb_add_col(GTK_TREE_VIEW(tv), "RTT",       PC_RTT_MS,  cell_fmt_rtt);

    /* Default sort: CPU% descending */
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store),
        PC_CPU, GTK_SORT_DESCENDING);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    /* Initial data load */
    pb_main_update(mbd);

    /* Live-update timer */
    mbd->timer_id = g_timeout_add(refresh_interval_ms, pb_main_timer_cb, mbd);
    pb_main_data  = mbd;

    gtk_widget_show_all(win);
}
