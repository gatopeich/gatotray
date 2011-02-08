#define GATOTRAY_VERSION "gatotray v2.0"
/*
 * (c) 2011 by gatopeich, licensed under a Creative Commons Attribution 3.0
 * Unported License: http://creativecommons.org/licenses/by/3.0/
 * Briefly: Use it however suits you better and just give me due credit.
 *
 * Changelog:
 * v2.0 Added pref file and dialog.
 * v1.11 Experimenting with configurability.
 * v1.10 Added support for reading temperature from /sys (kernel>=2.6.26)
 * v1.9 Added support for /proc/acpi/thermal_zone/THRM/temperature
 * v1.8 Don't ever reduce history on app_icon size change.
 * v1.7 Don't fail/crash when cpufreq is not available.
 * v1.6 Show iowait in blue. Hide termo when unavailable.
 * v1.5 Count 'iowait' as busy CPU time.
 * v1.4 Fixed memory leak -- "g_free(tip)".
 *
 * TODO:
 * - Add "About" dialog with link to website.
 * - Refactor into headers+bodies.
 * - Refactor drawing code to reduce the chain pixmap-->pixbuf-->icon, specialy
 *   when transparency is enabled ... Check gdk_pixbuf_new_from_data() and
 *   gdk_pixbuf_from_pixdata().
 *
 */

#define _XOPEN_SOURCE
#include <sys/types.h>
#include <signal.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "cpu_usage.c"
#include "settings.c"
#include "gatotray.xpm"

#define SCALE 100

typedef struct {
    CPU_Usage cpu;
    int freq;
    int temp;
} CPUstatus;

CPUstatus* history = NULL;

int width = 0, hist_size = 0, timer = 0;

GdkPixmap *pixmap = NULL;
GtkStatusIcon *app_icon = NULL;

static void
popup_menu_cb(GtkStatusIcon *status_icon, guint button, guint time, GtkMenu* menu)
{
    gtk_menu_popup(menu, NULL, NULL, NULL, NULL, button, time);
}

GdkGC *gc = NULL;
GdkPoint Termometer[] = {{2,16},{2,2},{3,1},{4,1},{5,2},{5,16},{6,17},{6,19},{5,20},
    {2,20},{1,19},{1,17},{2,16}};
#define Termometer_tube_size 6 /* first points are the 'tube' */
#define Termometer_tube_start 2
#define Termometer_tube_end 16
#define Termometer_scale 22
GdkPoint termometer_tube[Termometer_tube_size];
GdkPoint termometer[sizeof(Termometer)/sizeof(*Termometer)];

void redraw(void)
{
    gdk_gc_set_rgb_fg_color(gc, &bg_color);
    gdk_draw_rectangle(pixmap, gc, TRUE, 0, 0, width, width);
    
    for(int i=0; i<width; i++)
    {
        CPUstatus* h = &history[width-1-i];
        unsigned shade = h->freq * 99 / SCALE;

        /* Or shade by temperature:
        shade = h->temp > 0 ? (h->temp < 100 ? h->temp : 99) : 0;
        gdk_gc_set_rgb_fg_color(gc, &temp_gradient[shade]);
        */

        /* Bottom blue strip for i/o waiting cycles: */
        int iow_size = h->cpu.iowait*width/SCALE;
        int bottom = width-iow_size;
        if( iow_size ) {
            gdk_gc_set_rgb_fg_color(gc, &iow_color);
            gdk_draw_line(pixmap, gc, i, bottom, i, width);
        }

        gdk_gc_set_rgb_fg_color(gc, &freq_gradient[shade]);
        gdk_draw_line(pixmap, gc, i, bottom-(h->cpu.usage*width/SCALE), i, bottom);
    }

    int T = history[0].temp;
    if(T) /* Hide if 0, meaning it could not be read */
    if( T<85 || (timer&1) ) /* Blink when hot! */
    {
        /* scale temp from 5~105 degrees Celsius to 0~100*/
        T = (T-5)*100/100;
        if(T<0) T=0;
        if(T>99) T=99;
        gdk_gc_set_rgb_fg_color(gc, &temp_gradient[T]);
        gdk_draw_polygon(pixmap, gc, TRUE, termometer, sizeof(termometer)/sizeof(*termometer));
        if( T<99 )
        {
            termometer_tube[0].y = (T*termometer[1].y+(99-T)*termometer[0].y)/99;
            termometer_tube[Termometer_tube_size-1].y = termometer_tube[0].y;
            gdk_gc_set_rgb_fg_color(gc, &bg_color);
            gdk_draw_polygon(pixmap, gc, TRUE, termometer_tube, Termometer_tube_size);
        }
        gdk_gc_set_rgb_fg_color(gc, &fg_color);
        gdk_draw_lines(pixmap, gc, termometer, sizeof(termometer)/sizeof(*termometer));
    }

    GdkPixbuf *pixbuf =
    gdk_pixbuf_get_from_drawable(NULL, pixmap, NULL, 0, 0, 0, 0, width, width);
    if(pref_transparent) {
        GdkPixbuf* new = gdk_pixbuf_add_alpha(pixbuf, TRUE, bg_color.red>>8
                                        , bg_color.green>>8, bg_color.blue>>8);
        g_object_unref(pixbuf);
        pixbuf = new;
    }
    gtk_status_icon_set_from_pixbuf(GTK_STATUS_ICON(app_icon), pixbuf);
    g_object_unref(pixbuf);
}

gboolean
resize_cb(GtkStatusIcon *app_icon, gint newsize, gpointer user_data)
{
    if(newsize > hist_size) {
        history = g_realloc(history, newsize*sizeof(*history));
        for(int i=hist_size; i<newsize; i++)
            history[i] = history[hist_size-1];
        hist_size = newsize;
    }
    width = newsize;

    if(pixmap) g_object_unref(pixmap);
    pixmap = gdk_pixmap_new(NULL, width, width, 24);
    
    if(gc)  g_object_unref(gc);
    gc = gdk_gc_new(pixmap);
    gdk_gc_set_line_attributes(gc, 1, GDK_LINE_SOLID, GDK_CAP_NOT_LAST, GDK_JOIN_MITER);

    for(int i=0; i<sizeof(termometer)/sizeof(*termometer); i++)
    {
        termometer[i].x = Termometer[i].x*newsize/Termometer_scale;
        termometer[i].y = Termometer[i].y*newsize/Termometer_scale;
        if(i<Termometer_tube_size) {
            termometer_tube[i].x = termometer[i].x;
            termometer_tube[i].y = termometer[i].y;
        }
    }
    
    redraw();
    return TRUE;
}

int
timeout_cb( gpointer data)
{
    timer++;
    int i = hist_size-1;
    while(i > 0) /* 'Smear' historic values */
    {
        #define smear(a,b) do{ \
        int _a = a, _b = b, _factor = i*2; \
        if( _a < _b ) _a++; \
        a = ( _a*(_factor-1) + _b )/_factor; \
        } while(0)

        smear(history[i].cpu.usage, history[i-1].cpu.usage);
        smear(history[i].cpu.iowait, history[i-1].cpu.iowait);
        smear(history[i].freq, history[i-1].freq);
        smear(history[i].temp, history[i-1].temp);

        #undef smear
        i--;
    }
    history[0].cpu = cpu_usage(SCALE);
    int freq = cpu_freq();
    history[0].freq = (freq - scaling_min_freq) * SCALE /
                        (scaling_max_freq-scaling_min_freq);
    history[0].temp = cpu_temperature();

    redraw();

    gchar* tip =
    g_strdup_printf("CPU %d%% busy @ %d MHz, %d%%wa\n"
                    "Temperature: %d C\n"
                    "(click for 'top')"
                    , history[0].cpu.usage*100/SCALE, freq/1000, history[0].cpu.iowait*100/SCALE
                    , history[0].temp);
    gtk_status_icon_set_tooltip(app_icon, tip);
    g_free(tip);

    return TRUE;
}

gboolean
icon_activate(GtkStatusIcon *app_icon, gpointer user_data)
{
    static GPid tops_pid = 0;

    if(tops_pid) {
        kill(tops_pid, SIGTERM);
        g_spawn_close_pid(tops_pid);
        tops_pid = 0;
    }
    else
    {
        #define BASE_COMMAND "xterm -n 'gatotray: top' -rv -geometry "
        char command[256] = BASE_COMMAND "73x12 top";
        GdkRectangle area;
        GtkOrientation orientation;
        if(gtk_status_icon_get_geometry(app_icon, NULL, &area, &orientation))
        {
            int x, y;
            if(orientation == GTK_ORIENTATION_HORIZONTAL) {
                x = area.x;
                y = area.y > area.height ? -1 : 0;
            } else {
                y = area.y;
                x = area.x > area.width ? -1 : 0;
            }
            g_snprintf(command, sizeof(command)
                , BASE_COMMAND "73x12%+d%+d top"
                , x, y);
        }
        char **myargv;
        g_shell_parse_argv(command, NULL, &myargv, NULL);
        g_spawn_async( NULL, myargv, NULL, G_SPAWN_SEARCH_PATH,
             NULL, NULL, &tops_pid, NULL);
        g_strfreev(myargv);
    }
    return TRUE;
}

int
main( int   argc, char *argv[] )
{
    gtk_init (&argc, &argv);
    GdkPixbuf* xpm = gdk_pixbuf_new_from_xpm_data(gatotray_xpm);
    gtk_window_set_default_icon(xpm);

    pref_init();
    //~ gchar* cs;
    //~ g_object_get(gtk_pref_get_default(), "gtk-color-scheme", &cs, NULL);
    //~ g_message("gtk-color-scheme: %s", cs);
    
    history = g_malloc(sizeof(*history));
    history[0].cpu = cpu_usage(SCALE);
    history[0].freq = 0;
    history[0].temp = cpu_temperature();
    hist_size = width = 1;

    app_icon = gtk_status_icon_new();
    resize_cb(app_icon, width, NULL);

    GtkWidget* menu = gtk_menu_new();
    GtkWidget* menuitem;

    menuitem = gtk_image_menu_item_new_from_stock(GTK_STOCK_ABOUT, NULL);
    gtk_menu_item_set_label(GTK_MENU_ITEM(menuitem), GATOTRAY_VERSION);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),menuitem);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    menuitem = gtk_image_menu_item_new_from_stock(GTK_STOCK_PREFERENCES, NULL);
    g_signal_connect(G_OBJECT (menuitem), "activate", show_pref_dialog, NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

    menuitem = gtk_image_menu_item_new_from_stock(GTK_STOCK_QUIT, NULL);
    g_signal_connect(G_OBJECT(menuitem), "activate", G_CALLBACK(gtk_main_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

    gtk_widget_show_all(menu);
    
    g_signal_connect(G_OBJECT(app_icon), "popup-menu", G_CALLBACK(popup_menu_cb), menu);
    g_signal_connect(G_OBJECT(app_icon), "size-changed", G_CALLBACK(resize_cb), NULL);
    g_signal_connect(G_OBJECT(app_icon), "activate", G_CALLBACK(icon_activate), NULL);
    gtk_status_icon_set_visible(app_icon, TRUE);

    g_timeout_add(1000, timeout_cb, NULL);

    gtk_main();

    return 0;
}
