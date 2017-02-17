#ifndef VERSION
#define VERSION "3.0"
#endif
#define GATOTRAY_VERSION "gatotray v" VERSION
/*
 * (c) 2011 by gatopeich, licensed under a Creative Commons Attribution 3.0
 * Unported License: http://creativecommons.org/licenses/by/3.0/
 * Briefly: Use it however suits you better and just give me due credit.
 *
 * Changelog:
 * v3.1 Fix bugs in freq handling after change in SCALE
 * v3.0 Screensaver mode, higher SCALE factor for better history accuracy
 * v2.2 Added config for top command and location of temperature & frequency
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

#include "cpu_usage.c"
#include "settings.c"
#include "gatotray.xpm"

#define SCALE (1<<15)

typedef struct {
    CPU_Usage cpu;
    int freq;
    int temp;
} CPUstatus;

CPUstatus* history = NULL;

int width = 0, hist_size = 0, timer = 0;

GdkPixmap *pixmap = NULL;
GtkStatusIcon *app_icon = NULL;
GdkWindow *screensaver = NULL;
GString* info_text = NULL;
gchar* abs_argv0;

static void
popup_menu_cb(GtkStatusIcon *status_icon, guint button, guint time, GtkMenu* menu)
{
    gtk_menu_popup(menu, NULL, NULL, NULL, NULL, button, time);
}

GdkGC *gc = NULL;
GdkPoint Termometer[] = {{2,16},{2,2},{3,1},{4,1},{5,2},{5,16},{6,17},{6,19},{5,20},
    {2,20},{1,19},{1,17},{2,16}};
#define Termometer_tube_points 6 /* first points are the 'tube' */
#define Termometer_tube_start 2
#define Termometer_tube_end 16
#define Termometer_scale 21
GdkPoint termometer_tube[Termometer_tube_points];
GdkPoint termometer[G_N_ELEMENTS(Termometer)];

void redraw(void)
{
    if (screensaver)
    {
        int w = gdk_window_get_width(screensaver), h = gdk_window_get_height(screensaver);
        
        cairo_t *screen = gdk_cairo_create(screensaver);
        cairo_surface_t *double_buffer = cairo_surface_create_similar_image(
            cairo_get_target(screen), CAIRO_FORMAT_RGB24, w, h);
        cairo_t *cr = cairo_create (double_buffer);

        if (bg_color.red || bg_color.green || bg_color.blue) {
            gdk_cairo_set_source_color(cr, &bg_color);
            cairo_paint(cr);
        }

        const float _1 = 1.0/65535;
        cairo_move_to(cr, 0, h);
        cairo_pattern_t *pattern = cairo_pattern_create_linear(0,h,w,h);
        float d_w = (w-1)*1.0/(width-1), d_h = (h-1)*1.0/SCALE, d_o = 1.0/width;
        GdkColor* shade = {0};
        for(int i=0; i<width; i++) {
            CPUstatus* st = &history[width-1-i];
            cairo_line_to(cr, i*d_w, h-(d_h * st->cpu.usage));
            shade = &freq_gradient[MIN(MAX(0, st->freq*MAX_SHADE/SCALE), MAX_SHADE)];
            cairo_pattern_add_color_stop_rgba(pattern, i*d_o, _1*shade->red, _1*shade->green, _1*shade->blue, 0.7);
        }
        cairo_rel_line_to(cr, d_w, 0);
        cairo_line_to(cr, w, h);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, _1*shade->red, _1*shade->green, _1*shade->blue);
        cairo_stroke_preserve(cr);
        cairo_set_source(cr, pattern);
        cairo_fill(cr);

        cairo_move_to(cr, 0, h);
        for(int i=0; i<width; i++)
            cairo_line_to(cr, i*d_w, h-(d_h * history[width-1-i].cpu.iowait));
        cairo_rel_line_to(cr, d_w, 0);
        cairo_set_source_rgb(cr, _1*iow_color.red, _1*iow_color.green, _1*iow_color.blue);
        cairo_stroke_preserve(cr);
        cairo_line_to(cr, w, h);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, _1*iow_color.red, _1*iow_color.green, _1*iow_color.blue, 0.5);
        cairo_fill(cr);

        PangoContext *pango = pango_cairo_create_context(cr);
        PangoLayout *pl = pango_layout_new (pango);
        pango_layout_set_width (pl, w * PANGO_SCALE);
        pango_layout_set_alignment (pl, PANGO_ALIGN_CENTER);
        pango_layout_set_text (pl, info_text ? info_text->str : GATOTRAY_VERSION, -1);
        gdk_cairo_set_source_color(cr, &fg_color);
        pango_cairo_show_layout(cr, pl);
        g_object_unref(pl);
        g_object_unref(pango);

        cairo_destroy(cr); // Draw now!
        cairo_set_source_surface(screen, double_buffer, 0, 0);
        cairo_paint(screen);
        cairo_destroy(screen);
        cairo_surface_destroy(double_buffer);
    }
    else
    {
        gdk_gc_set_rgb_fg_color(gc, &bg_color);
        gdk_draw_rectangle(pixmap, gc, TRUE, 0, 0, width, width);

        for(int i=0; i<width; i++)
        {
            CPUstatus* h = &history[width-1-i];
            GdkColor* shade = &freq_gradient[MIN(MAX(0, h->freq*MAX_SHADE/SCALE), MAX_SHADE)];

            /* Or shade by temperature:
            GdkColor* shade = &temp_gradient[MIN(MAX(0, h->temp*MAX_SHADE/SCALE, SCALE)];
            */

            /* Bottom blue strip for i/o waiting cycles: */
            int iow_size = h->cpu.iowait*width/SCALE;
            int bottom = width-iow_size;
            if( iow_size ) {
                gdk_gc_set_rgb_fg_color(gc, &iow_color);
                gdk_draw_line(pixmap, gc, i, bottom, i, width);
            }

            gdk_gc_set_rgb_fg_color(gc, shade);
            gdk_draw_line(pixmap, gc, i, bottom-(h->cpu.usage*width/SCALE), i, bottom);
        }

        int T;
        if (pref_thermometer && (T=history[0].temp)) /* if temp=0, it could not be read */
        if ( T<pref_temp_alarm || (timer&1) ) /* Blink when hot! */
        {
            /* scale temp from 5~105 degrees Celsius to 0~GRADIENT_SIZE*/
            T = MIN(MAX(0, (T-5)*MAX_SHADE/100), MAX_SHADE);
            gdk_gc_set_rgb_fg_color(gc, &temp_gradient[T]);
            gdk_draw_polygon(pixmap, gc, TRUE, termometer, G_N_ELEMENTS(termometer));
            if( T<MAX_SHADE )
            {
                termometer_tube[0].y = (T*termometer[1].y+(MAX_SHADE-T)*termometer[0].y)/MAX_SHADE;
                termometer_tube[Termometer_tube_points-1].y = termometer_tube[0].y;
                gdk_gc_set_rgb_fg_color(gc, &bg_color);
                gdk_draw_polygon(pixmap, gc, TRUE, termometer_tube, Termometer_tube_points);
            }
            gdk_gc_set_rgb_fg_color(gc, &fg_color);
            gdk_draw_lines(pixmap, gc, termometer, G_N_ELEMENTS(termometer));
        }

        GdkPixbuf *pixbuf = gdk_pixbuf_get_from_drawable(NULL, pixmap, NULL, 0, 0, 0, 0, width, width);
        if (screensaver)
        {
            int w = gdk_window_get_width(screensaver), h = gdk_window_get_height(screensaver);
            int size = MIN(w,h), x = (w-size)/2, y = (h-size)/2;
            GdkPixbuf* scaled = gdk_pixbuf_scale_simple (pixbuf, size, size, GDK_INTERP_TILES);
            PangoContext *pango = gdk_pango_context_get_for_screen (gdk_window_get_screen(screensaver));
            PangoLayout *pl = pango_layout_new (pango);
            pango_layout_set_width (pl, size * PANGO_SCALE);
            pango_layout_set_alignment (pl, PANGO_ALIGN_CENTER);
            pango_layout_set_text (pl, info_text ? info_text->str : GATOTRAY_VERSION, -1);
            gdk_gc_set_rgb_fg_color(gc, &fg_color);
            gdk_draw_pixbuf (screensaver, NULL, scaled, 0,0, x,y, -1,-1, GDK_RGB_DITHER_NONE,0,0);
            gdk_draw_layout (screensaver, gc, x,y, pl);
            g_object_unref(scaled);
            g_object_unref(pl);
            g_object_unref(pango);
        } else {
            if (pref_transparent) { // TODO: Draw directly with alpha!
                GdkPixbuf* new = gdk_pixbuf_add_alpha(pixbuf, TRUE
                    , bg_color.red>>8, bg_color.green>>8, bg_color.blue>>8);
                g_object_unref(pixbuf);
                pixbuf = new;
            }
            gtk_status_icon_set_from_pixbuf(GTK_STATUS_ICON(app_icon), pixbuf);
        }
        g_object_unref(pixbuf);
    }
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

    if (!screensaver) {
        if (pixmap) g_object_unref(pixmap);
        pixmap = gdk_pixmap_new(NULL, width, width, 24);        
    }

    if (gc)  g_object_unref(gc);
    gc = gdk_gc_new(pixmap);
    gdk_gc_set_line_attributes(gc, 1, GDK_LINE_SOLID, GDK_CAP_NOT_LAST, GDK_JOIN_MITER);

    for(int i=0; i<G_N_ELEMENTS(termometer); i++)
    {
        termometer[i].x = Termometer[i].x*newsize/Termometer_scale;
        termometer[i].y = Termometer[i].y*newsize/Termometer_scale;
        if(i<Termometer_tube_points) {
            termometer_tube[i].x = termometer[i].x;
            termometer_tube[i].y = termometer[i].y;
        }
    }

    redraw();
    return TRUE;
}

int
timeout_cb (gpointer data)
{
    timer++;
    for(int i = hist_size-1; i > 0; i--)
    {
        // Persistence 'P' is higher for farther history points, so that they take
        // longer to blend with newer data. Ideally we have:
        // - High P (~1.0) at the end of the history
        // - Low P (~0) at the most recent point
        // - P grows fast on the first half, then slower on the second.
        // Best formula I found so far is: P = (c+1) - c(c+1)/(x+c)
        // Since (1/x) is the log derivative, I call this a "pseudo-logarithmic time scale"
        // Examples:
        // - Linear: P = x;
        // - Cuadratic: P = x(2-x) == x*(2*_1-x)/_1
        // ... a-(b/(x+c)): a = b/c ; a = 1 + (b/(1+c))
        // b/c = 1+ b/(1+c) :: b = c + b/(1+1/c) :: b/c = c+1 :: { b = c(c+1), a = c+1 }
        // a = c+1, b = a*c
        // c = 1/4 --> a = 5/4, b = 5/16 ===> 5/4 - (5/(16x+4))
        // - P = (c+1) - (c(c+1)/(x+c)) := ((x+c)(c+1)-c(c+1)) / (x+c) := (c+1)x/(x+c)
        // - Log-dev: P = (c+1)x/(c+x)
        // Taking C as a (negative) power of 2 makes all this math fast & accurate with fixed-point
        const int _1 = 1<<15; // For Q15 fixed-point operation
        int x = _1 * i / hist_size, C = _1/4, P = (_1+C)*x/(C+x);
        #define blend(dst, src) { dst = (P*dst + (_1-P)*src) / _1; }

        // Linear
        //const int _1 = hist_size, P = i;
        //#define blend(dst, src) { dst = (P*dst + (_1-P)*src + (_1/2)) / _1; }

        // Simplest blending:
        // #define blend(dst, src) { dst = (dst + src + 1)/2; }

        blend(history[i].cpu.usage, history[i-1].cpu.usage);
        blend(history[i].cpu.iowait, history[i-1].cpu.iowait);
        blend(history[i].freq, history[i-1].freq);
        blend(history[i].temp, history[i-1].temp);
        #undef blend
    }
    history[0].cpu = cpu_usage(SCALE);
    int freq = cpu_freq(); // Frequency in MHz
    history[0].freq = scaling_max_freq > scaling_min_freq ?
        (freq - scaling_min_freq) * SCALE / (scaling_max_freq-scaling_min_freq) : 0;
    // printf("freq = %d, min~max = %d~%d, normalized = %d%%\n", freq, scaling_min_freq, scaling_max_freq, history[0].freq*GRADIENT_SIZE/SCALE);

    history[0].temp = cpu_temperature();

    const MemInfo mi = mem_info();

    if (!info_text)
        info_text = g_string_new(NULL);
    g_string_printf(info_text, GATOTRAY_VERSION "\nCPU %d%% busy, %d%% on I/O-wait @ %d MHz"
        , history[0].cpu.usage*100/SCALE, history[0].cpu.iowait*100/SCALE, freq);
    if (mi.Total)
        g_string_append_printf (info_text, "\nFree RAM: %d%% of %d MB"
            , ((mi.Available?mi.Available:mi.Free)*100/mi.Total), mi.Total>>10);
    if (history[0].temp)
        g_string_append_printf (info_text, "\nTemperature: %d°C", history[0].temp);

    // Tooltip should not be refreshed too often, otherwise it never shows
    static gint64 last_tooltip_update = 0;
    if (app_icon && (g_get_monotonic_time()-last_tooltip_update) > G_USEC_PER_SEC) {
        gtk_status_icon_set_tooltip (app_icon, info_text->str);
        last_tooltip_update = g_get_monotonic_time();
    }

    redraw();

    // Re-add every time to handle changes in refresh_rate
    g_timeout_add(refresh_rate, timeout_cb, NULL);
    return FALSE;
}

void
open_website()
{
    g_spawn_command_line_async("xdg-open https://bitbucket.org/gatopeich/gatotray", NULL);
}

void
install_screensaver()
{
    gchar* cmd = g_strdup_printf(
        "sh -c \"(echo programs: %s -root;echo mode: _1;echo selected: 0) >> %s/.xscreensaver"
        " && xscreensaver-command -demo\"", abs_argv0, g_get_home_dir());
    g_message("%s",cmd);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
}

GRegex* regex_position;
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
        gchar* pos;
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
            pos = g_strdup_printf("%+d%+d", x, y);
        }
        else pos = g_strdup("");
        if (!regex_position) regex_position = g_regex_new("{position}",0,0,NULL);
        gchar* command = g_regex_replace_literal(regex_position, pref_custom_command, -1, 0, pos, 0, NULL);
        g_free(pos);
        char **argv;
        g_shell_parse_argv(command, NULL, &argv, NULL);
        g_free(command);
        g_spawn_async( NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &tops_pid, NULL);
        g_strfreev(argv);
    }
    return TRUE;
}

int
main( int argc, char *argv[] )
{
    abs_argv0 = NULL;
    if (!g_file_test(argv[0], G_FILE_TEST_EXISTS)||g_file_test(argv[0], G_FILE_TEST_IS_DIR))
        abs_argv0 = g_find_program_in_path(argv[0]);
    if (!abs_argv0) {
        GFile* gf = g_file_new_for_commandline_arg(argv[0]);
        abs_argv0 = g_file_get_path(gf);
        g_object_unref(gf);
    }

    gtk_init (&argc, &argv);
    GdkPixbuf* xpm = gdk_pixbuf_new_from_xpm_data(gatotray_xpm);
    gtk_window_set_default_icon(xpm);

    pref_init();

    history = g_malloc(sizeof(*history));
    history[0].cpu = cpu_usage(SCALE);
    history[0].freq = 0;
    history[0].temp = cpu_temperature();
    hist_size = width = 1;

    gchar** envp = g_get_environ();
    const gchar* wid = g_environ_getenv(envp,"XSCREENSAVER_WINDOW");
    if (wid || g_str_has_suffix(argv[0], "xgatotray")
            || (argc>1 && g_str_has_prefix(argv[1], "-root"))) {
        if (wid)
            screensaver = gdk_window_foreign_new(g_ascii_strtoull(wid, NULL, 16));
        else {
            // screensaver = GDK_WINDOW(gdk_get_default_root_window());
            GdkWindowAttr attr = {
                "xgatotray", 0, 0,0,400,300, GDK_INPUT_OUTPUT,NULL,NULL,GDK_WINDOW_TOPLEVEL
                , NULL, NULL, NULL, FALSE, GDK_WINDOW_TYPE_HINT_NORMAL
            };
            screensaver = gdk_window_new(NULL, &attr, GDK_WA_TITLE);
            // gdk_window_fullscreen(GDK_WINDOW(screensaver));
            gdk_window_show(GDK_WINDOW(screensaver));
        }
        resize_cb(NULL, width = 4*Termometer_scale, NULL);
    } else {
        app_icon = gtk_status_icon_new();
        resize_cb(app_icon, width, NULL);

        GtkWidget* menu = gtk_menu_new();
        GtkWidget* menuitem;

        menuitem = gtk_image_menu_item_new_from_stock(GTK_STOCK_PREFERENCES, NULL);
        g_signal_connect(G_OBJECT (menuitem), "activate", show_pref_dialog, NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

        menuitem = gtk_image_menu_item_new_from_stock(GTK_STOCK_FULLSCREEN, NULL);
        gtk_menu_item_set_label(GTK_MENU_ITEM(menuitem), "Use as screensaver");
        g_signal_connect(G_OBJECT (menuitem), "activate", install_screensaver, NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),menuitem);

        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());

        menuitem = gtk_image_menu_item_new_from_stock(GTK_STOCK_ABOUT, NULL);
        gtk_menu_item_set_label(GTK_MENU_ITEM(menuitem), "Open " GATOTRAY_VERSION " website");
        g_signal_connect(G_OBJECT (menuitem), "activate", open_website, NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),menuitem);

        menuitem = gtk_image_menu_item_new_from_stock(GTK_STOCK_QUIT, NULL);
        g_signal_connect(G_OBJECT(menuitem), "activate", G_CALLBACK(gtk_main_quit), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

        gtk_widget_show_all(menu);

        g_signal_connect(G_OBJECT(app_icon), "popup-menu", G_CALLBACK(popup_menu_cb), menu);
        g_signal_connect(G_OBJECT(app_icon), "size-changed", G_CALLBACK(resize_cb), NULL);
        g_signal_connect(G_OBJECT(app_icon), "activate", G_CALLBACK(icon_activate), NULL);
        gtk_status_icon_set_visible(app_icon, TRUE);
        gtk_status_icon_set_tooltip(app_icon, GATOTRAY_VERSION);
    }
    g_free(envp);
    g_timeout_add(refresh_rate, timeout_cb, NULL);
    gtk_main();
    return 0;
}
