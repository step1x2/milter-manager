/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *  Copyright (C) 2008  Kouhei Sutou <kou@cozmixng.org>
 *
 *  This library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this library.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <unistd.h>

#include "milter-test-utils.h"

#include <gcutter.h>


static gchar *base_dir = NULL;
const gchar *
milter_test_get_base_dir (void)
{
    const gchar *dir;

    if (base_dir)
        return base_dir;

    dir = g_getenv("BASE_DIR");
    if (!dir)
        dir = ".";

    if (g_path_is_absolute(dir)) {
        base_dir = g_strdup(dir);
    } else {
        gchar *current_dir;

        current_dir = g_get_current_dir();
        base_dir = g_build_filename(current_dir, dir, NULL);
        g_free(current_dir);
    }

    return base_dir;
}

#define BUFFER_SIZE 4096
void
milter_test_write_data_to_io_channel (GIOChannel *io_channel,
                                      const gchar *data, gsize data_size)
{
    gsize bytes_written;
    gsize rest_bytes = data_size;

    while (rest_bytes > 0) {
        gsize bytes;
        GIOStatus status;
        GError *error = NULL;

        g_main_context_iteration(NULL, FALSE);

        bytes = MIN(BUFFER_SIZE, rest_bytes);
        status = g_io_channel_write_chars(io_channel,
                                          data, bytes,
                                          &bytes_written,
                                          &error);
        gcut_assert_error(error);
        if (status == G_IO_STATUS_AGAIN)
            continue;

        cut_assert_operator_uint(bytes_written, >, 0);

        rest_bytes -= bytes_written;
        data += bytes_written;
        g_io_channel_flush(io_channel, &error);
        gcut_assert_error(error);
    }
}

gboolean
milter_test_read_data_from_io_channel (GIOChannel *channel,
                                       gchar **data, gsize *data_size)
{
    gboolean eof = FALSE;
    GString *read_string = g_string_new(NULL);

    while (!eof) {
        GIOStatus status;
        gchar stream[BUFFER_SIZE + 1];
        gsize length = 0;
        GError *error = NULL;

        status = g_io_channel_read_chars(channel, stream, BUFFER_SIZE,
                                         &length, &error);
        if (status == G_IO_STATUS_EOF) {
            eof = TRUE;
        }

        gcut_assert_error(error);

        g_string_append_len(read_string, stream, length);

        if (length <= 0)
            break;

        if (eof)
            break;
    }

    *data_size = read_string->len;
    *data = g_string_free(read_string, FALSE);

    return !eof;
}

gboolean
milter_test_decode_io_channel (MilterDecoder *decoder, GIOChannel *channel)
{
    gboolean eof = FALSE;

    while (!eof) {
        GIOStatus status;
        gchar stream[BUFFER_SIZE + 1];
        gsize length = 0;
        GError *error = NULL;

        status = g_io_channel_read_chars(channel, stream,
                                         BUFFER_SIZE, &length, &error);
        gcut_assert_error(error);

        if (status == G_IO_STATUS_EOF)
            eof = TRUE;

        if (length <= 0)
            break;

        milter_decoder_decode(decoder, stream, length, &error);
        gcut_assert_error(error);
    }

    return !eof;
}

static gboolean
decode_watch_func (GIOChannel *channel, GIOCondition condition, gpointer data)
{
    MilterDecoder *decoder = data;
    gboolean keep_callback = TRUE;

    if (condition & G_IO_IN ||
        condition & G_IO_PRI) {
        keep_callback = milter_test_decode_io_channel(decoder, channel);
    }

    if (condition & G_IO_ERR ||
        condition & G_IO_HUP ||
        condition & G_IO_NVAL) {
        gchar *message;

        message = milter_utils_inspect_io_condition_error(condition);
        cut_notify("%s", message);
        g_free(message);
        return FALSE;
    }

    return keep_callback;
}

guint
milter_test_io_add_decode_watch (GIOChannel *channel, MilterDecoder *decoder)
{
    return g_io_add_watch(channel,
                          G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                          decode_watch_func, decoder);
}

gboolean
milter_test_equal_symbols_table (GHashTable *table1, GHashTable *table2)
{
    return gcut_hash_table_equal(table1, table2,
                                 (GEqualFunc)gcut_list_equal_string);
}

static void
inspect_symbols_table_key (GString *string, gconstpointer data,
                           gpointer user_data)
{
    MilterCommand key = GPOINTER_TO_INT(data);

    g_string_append_printf(string, "'%c'", key);
}

static void
inspect_symbols_table_value (GString *string, gconstpointer data,
                             gpointer user_data)
{
    gchar *inspected;
    const GList *value = data;

    inspected = gcut_list_string_inspect(value);
    g_string_append_printf(string, "%s", inspected);
    g_free(inspected);
}

gchar *
milter_test_inspect_symbols_table (GHashTable *table)
{
    return gcut_hash_table_inspect(table,
                                   inspect_symbols_table_key,
                                   inspect_symbols_table_value,
                                   NULL);
}

static gboolean
cb_idle_check (gpointer data)
{
    gboolean *idle_emitted = data;

    *idle_emitted = TRUE;
    return FALSE;
}

void
milter_test_pump_all_events (void)
{
    gboolean idle_emitted = FALSE;

    g_idle_add(cb_idle_check, &idle_emitted);
    while (!idle_emitted) {
        g_main_context_iteration(NULL, TRUE);
    }
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
