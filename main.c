#include <gtk/gtk.h>
#include <glib.h>
#include <strings.h>
#include <string.h>
#include <glib.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>
#include <spice/macros.h>
#include <red_replay_qxl.h>

#include "basic_event_loop.h"

typedef struct SpiceStruct
{
    SpiceCoreInterface* core;
    SpiceServer* server;
    SpiceReplay* replay;
    QXLWorker* qxl_worker;
    gboolean started;
    QXLInstance display_sin;
    GAsyncQueue* aqueue;
    int slow;
    pid_t client_pid;
    gboolean paused;
    int runCmdCount;
} SpiceStruct;

typedef struct GuiStruct
{
    GtkWindow* window;
    GtkWidget* recordFileEntry;
    GtkWidget* portEntry;
    GtkWidget* clientEntry;
    GtkWidget* slowEntry;
    GtkWidget* slowCB;
    GtkWidget* runBtn;
    GtkWidget* pauseBtn;
    GtkWidget* contBtn;
    GtkWidget* nextBtn;
    GtkWidget* nextnBtn;
    GtkWidget* skipEntry;
    GtkTextBuffer* textBuffer;
} GuiStruct;

#define MEM_SLOT_GROUP_ID 0
#define MAX_SURFACE_NUM 1024

static QXLDevMemSlot slot = { .slot_group_id = MEM_SLOT_GROUP_ID,
                              .slot_id = 0,
                              .generation = 0,
                              .virt_start = 0,
                              .virt_end = ~0,
                              .addr_delta = 0,
                              .qxl_ram_size = ~0, };

static SpiceStruct spiceObj;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static guint fill_source_id = 0;

static void show_message_dialog(GtkWindow *parent, gchar* msg)
{
    GtkWidget *dialog = gtk_message_dialog_new (parent,
                                 GTK_DIALOG_DESTROY_WITH_PARENT,
                                 GTK_MESSAGE_ERROR,
                                 GTK_BUTTONS_CLOSE,
                                 "%s",
                                 msg);
    gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
}

static void replay_channel_event(int event, SpiceChannelEventInfo *info)
{
    if (info->type == SPICE_CHANNEL_DISPLAY &&
        event == SPICE_CHANNEL_EVENT_INITIALIZED) {
        spiceObj.started = TRUE;
    }
}

static void attach_worker(QXLInstance* qin, QXLWorker* _qxl_worker)
{
    static int count = 0;
    if (++count > 1) {
        g_warning("%s ignored\n", __func__);
        return;
    }
    g_debug("%s\n", __func__);
    spiceObj.qxl_worker = _qxl_worker;
    spice_qxl_add_memslot(qin, &slot);
    spice_server_vm_start(spiceObj.server);
}

static void set_compression_level(QXLInstance* qin, int level)
{
    g_debug("%s\n", __func__);
}

static void set_mm_time(QXLInstance* qin, uint32_t mm_time)
{
}

static void get_init_info(QXLInstance* qin, QXLDevInitInfo* info)
{
    bzero(info, sizeof(*info));
    info->num_memslots = 1;
    info->num_memslots_groups = 1;
    info->memslot_id_bits = 1;
    info->memslot_gen_bits = 1;
    info->n_surfaces = MAX_SURFACE_NUM;
}

static gboolean fill_queue_idle(gpointer user_data)
{
    gboolean keep = FALSE;
    int i;

    if(spiceObj.paused) {
        for(i=0; i<spiceObj.runCmdCount; i++) {
            QXLCommandExt* cmd = spice_replay_next_cmd(spiceObj.replay, spiceObj.qxl_worker);
            if (!cmd) {
                g_async_queue_push(spiceObj.aqueue, GINT_TO_POINTER(-1));
                goto end;
            }

            if (spiceObj.slow)
                g_usleep(spiceObj.slow);

            g_async_queue_push(spiceObj.aqueue, cmd);
        }
        spiceObj.runCmdCount = 0;
    } else {
        while (g_async_queue_length(spiceObj.aqueue) < 50) {
            QXLCommandExt* cmd = spice_replay_next_cmd(spiceObj.replay, spiceObj.qxl_worker);
            if (!cmd) {
                g_async_queue_push(spiceObj.aqueue, GINT_TO_POINTER(-1));
                goto end;
            }

            if (spiceObj.slow)
                g_usleep(spiceObj.slow);

            g_async_queue_push(spiceObj.aqueue, cmd);
        }
    }

end:
    if (!keep) {
        pthread_mutex_lock(&mutex);
        fill_source_id = 0;
        pthread_mutex_unlock(&mutex);
    }
    spice_qxl_wakeup(&spiceObj.display_sin);

    return keep;
}

static void fill_queue(void)
{
    pthread_mutex_lock(&mutex);

    if (!spiceObj.started)
        goto end;

    if (fill_source_id != 0)
        goto end;

    fill_source_id = g_idle_add(fill_queue_idle, NULL);

end:
    pthread_mutex_unlock(&mutex);
}

static void end_replay(void)
{
    int child_status;

    /* FIXME: wait threads and end cleanly */
    spice_replay_free(spiceObj.replay);

    if (spiceObj.client_pid) {
        kill(spiceObj.client_pid, SIGINT);
        waitpid(spiceObj.client_pid, &child_status, 0);
    }
}

// called from spice_server thread (i.e. red_worker thread)
static int get_command(QXLInstance* qin, QXLCommandExt* ext)
{
    QXLCommandExt* cmd;

    if (g_async_queue_length(spiceObj.aqueue) == 0) {
        /* could use a gcondition ? */
        fill_queue();
        return FALSE;
    }

    cmd = g_async_queue_try_pop(spiceObj.aqueue);
    if (GPOINTER_TO_INT(cmd) == -1) {
        end_replay();
        return FALSE;
    }

    *ext = *cmd;

    return TRUE;
}

static int req_cmd_notification(QXLInstance* qin)
{
    if (!spiceObj.started)
        return TRUE;

    //g_printerr("id: %d, queue length: %d", fill_source_id, g_async_queue_length(spiceObj.aqueue));

    return TRUE;
}

static void release_resource(QXLInstance* qin, struct QXLReleaseInfoExt release_info)
{
    spice_replay_free_cmd(spiceObj.replay, (QXLCommandExt*)release_info.info->id);
}

static int get_cursor_command(QXLInstance* qin, struct QXLCommandExt* ext)
{
    return FALSE;
}

static int req_cursor_notification(QXLInstance* qin)
{
    return TRUE;
}

static void notify_update(QXLInstance* qin, uint32_t update_id)
{
}

static int flush_resources(QXLInstance* qin)
{
    return TRUE;
}

static QXLInterface display_sif = {
    .base = {
        .type = SPICE_INTERFACE_QXL,
        .description = "replay",
        .major_version = SPICE_INTERFACE_QXL_MAJOR,
        .minor_version = SPICE_INTERFACE_QXL_MINOR
    },
    .attache_worker = attach_worker,
    .set_compression_level = set_compression_level,
    .set_mm_time = set_mm_time,
    .get_init_info = get_init_info,
    .get_command = get_command,
    .req_cmd_notification = req_cmd_notification,
    .release_resource = release_resource,
    .get_cursor_command = get_cursor_command,
    .req_cursor_notification = req_cursor_notification,
    .notify_update = notify_update,
    .flush_resources = flush_resources,
};

static gboolean start_client(const gchar *cmd, GError **error)
{
    gboolean retval;
    gint argc;
    gchar **argv = NULL;


    if (!g_shell_parse_argv(cmd, &argc, &argv, error))
        return FALSE;

    retval = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                           NULL, NULL, &spiceObj.client_pid, error);
    g_strfreev(argv);

    return retval;
}

static void runReplay(GtkWidget* widget, gpointer data)
{
    GuiStruct* guiObj = (GuiStruct*)data;
    const gchar* file = gtk_entry_get_text(GTK_ENTRY(guiObj->recordFileEntry));
    FILE* fd = fopen(file, "r");
    if (fd == NULL) {
        show_message_dialog(guiObj->window, "open record file failed!");
        return;
    }
    if (fcntl(fileno(fd), FD_CLOEXEC) < 0) {
        show_message_dialog(guiObj->window, "fcntl record file failed!");
        return;
    }
    const gchar* port = gtk_entry_get_text(GTK_ENTRY(guiObj->portEntry));
    int pt = atoi(port);
    if(pt<1 && pt>65535) {
        show_message_dialog(guiObj->window, "port is not a number!");
        return;
    }
    if(gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(guiObj->slowCB))) {
        const gchar* sl = gtk_entry_get_text(GTK_ENTRY(guiObj->slowEntry));
        spiceObj.slow = atoi(sl);
    } else {
        spiceObj.slow = 0;
    }

    spiceObj.client_pid = 0;
    spiceObj.paused = FALSE;
    spiceObj.runCmdCount = 0;
    spiceObj.replay = spice_replay_new(fd, MAX_SURFACE_NUM);

    spiceObj.aqueue = g_async_queue_new();
    spiceObj.core = basic_event_loop_init();
    spiceObj.core->channel_event = replay_channel_event;

    spiceObj.server = spice_server_new();
    spice_server_set_image_compression(spiceObj.server, SPICE_IMAGE_COMPRESSION_AUTO_GLZ);
    spice_server_set_port(spiceObj.server, pt);
    spice_server_set_noauth(spiceObj.server);

    spice_server_init(spiceObj.server, spiceObj.core);

    spiceObj.display_sin.base.sif = &display_sif.base;
    spice_server_add_interface(spiceObj.server, &spiceObj.display_sin.base);
    const gchar *client = gtk_entry_get_text(GTK_ENTRY(guiObj->clientEntry));
    gboolean wait = FALSE;
    GError *error = NULL;
    if(strlen(client)) {
        start_client(client, &error);
        wait = TRUE;
    }
    if (!wait) {
        spiceObj.started = TRUE;
        fill_queue();
    }
}

static void pauseReplay(GtkWidget* widget, gpointer data)
{
    spiceObj.paused = TRUE;
}

static void contReplay(GtkWidget* widget, gpointer data)
{
    spiceObj.paused = FALSE;
    fill_queue();
}

static void nextReplay(GtkWidget* widget, gpointer data)
{
    spiceObj.runCmdCount = 1;
    fill_queue();
}

static void nextnReplay(GtkWidget* widget, gpointer data)
{
    GuiStruct* guiObj = (GuiStruct*)data;
    const gchar *count = gtk_entry_get_text(GTK_ENTRY(guiObj->skipEntry));
    int c = atoi(count);
    if(c<1 || c>500) {
        show_message_dialog(guiObj->window, "next n is not in [1, 500]");
        return;
    }
    spiceObj.runCmdCount = c;
    fill_queue();
}


int main(int argc, char* argv[])
{
    GtkWidget* window;
    GuiStruct guiObj;

    gtk_init(&argc, &argv);
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    guiObj.window = GTK_WINDOW(window);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget* vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* frame1 = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame1), GTK_SHADOW_IN);
    gtk_widget_set_size_request(vpaned, 800, 600);
    gtk_container_add(GTK_CONTAINER(box), frame1);
    GtkWidget* btnBox = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(box), btnBox);

    gtk_paned_pack1(GTK_PANED(vpaned), box, TRUE, FALSE);
    gtk_widget_set_size_request(frame1, 800, 90);
    GtkWidget* sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_paned_pack2(GTK_PANED(vpaned), sc, FALSE, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(sc), 2);
    gtk_widget_set_size_request(sc, 800, 510);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);

    gtk_container_add(GTK_CONTAINER(frame1), grid);
    GtkWidget* lbl1 = gtk_label_new("Record File:");
    gtk_grid_attach(GTK_GRID(grid), lbl1, 0, 0, 1, 1);

    guiObj.recordFileEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(guiObj.recordFileEntry), "/home/coolper/test.spice");
    gtk_grid_attach(GTK_GRID(grid), guiObj.recordFileEntry, 1, 0, 5, 1);

    GtkWidget* lbl2 = gtk_label_new("Port:");
    gtk_grid_attach(GTK_GRID(grid), lbl2, 0, 1, 1, 1);

    guiObj.portEntry = gtk_entry_new();
    gtk_widget_set_size_request(guiObj.portEntry, 40, 24);
    gtk_entry_set_text(GTK_ENTRY(guiObj.portEntry), "5900");
    gtk_grid_attach(GTK_GRID(grid), guiObj.portEntry, 1, 1, 1, 1);

    GtkWidget* lbl3 = gtk_label_new("Client:");
    gtk_grid_attach(GTK_GRID(grid), lbl3, 2, 1, 1, 1);

    guiObj.clientEntry = gtk_entry_new();
    gtk_widget_set_size_request(guiObj.clientEntry, 100, 24);
    //gtk_entry_set_text(GTK_ENTRY(guiObj.clientEntry), "remote-viewer spice://localhost:5900");
    gtk_entry_set_text(GTK_ENTRY(guiObj.clientEntry), "spicec -h localhost -p 5900");
    gtk_grid_attach(GTK_GRID(grid), guiObj.clientEntry, 3, 1, 1, 1);

    guiObj.slowCB = gtk_check_button_new_with_label("Slow");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(guiObj.slowCB), TRUE);
    gtk_grid_attach(GTK_GRID(grid), guiObj.slowCB, 4, 1, 1, 1);

    guiObj.slowEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(guiObj.slowEntry), "100");
    gtk_grid_attach(GTK_GRID(grid), guiObj.slowEntry, 5, 1, 1, 1);

    guiObj.runBtn = gtk_button_new_with_label("Run");
    g_signal_connect(guiObj.runBtn, "clicked", G_CALLBACK(runReplay), &guiObj);
    gtk_container_add(GTK_CONTAINER(btnBox), guiObj.runBtn);

    guiObj.pauseBtn = gtk_button_new_with_label("Pause");
    g_signal_connect(guiObj.pauseBtn, "clicked", G_CALLBACK(pauseReplay), &guiObj);
    gtk_container_add(GTK_CONTAINER(btnBox), guiObj.pauseBtn);

    guiObj.contBtn = gtk_button_new_with_label("Cont");
    g_signal_connect(guiObj.contBtn, "clicked", G_CALLBACK(contReplay), &guiObj);
    gtk_container_add(GTK_CONTAINER(btnBox), guiObj.contBtn);

    guiObj.nextBtn = gtk_button_new_with_label("Next");
    g_signal_connect(guiObj.nextBtn, "clicked", G_CALLBACK(nextReplay), &guiObj);
    gtk_container_add(GTK_CONTAINER(btnBox), guiObj.nextBtn);

    guiObj.nextnBtn = gtk_button_new_with_label("Next-N");
    g_signal_connect(guiObj.nextnBtn, "clicked", G_CALLBACK(nextnReplay), &guiObj);
    gtk_container_add(GTK_CONTAINER(btnBox), guiObj.nextnBtn);

    guiObj.skipEntry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(guiObj.skipEntry), "10");
    gtk_container_add(GTK_CONTAINER(btnBox), guiObj.skipEntry);

    guiObj.textBuffer = gtk_text_buffer_new(NULL);
    GtkWidget* text_view = gtk_text_view_new_with_buffer(guiObj.textBuffer);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD);
    gtk_container_add(GTK_CONTAINER(sc), text_view);

    gtk_container_add(GTK_CONTAINER(window), vpaned);
    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
