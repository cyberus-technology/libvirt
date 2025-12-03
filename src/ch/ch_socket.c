/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_monitor.h: header file for managing Cloud-Hypervisor interactions
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "ch_socket.h"


#define VIR_FROM_THIS VIR_FROM_CH

#define PKT_TIMEOUT_MS 500 /* ms */

static char *
chSocketRecv(int sock, bool use_timeout)
{
    struct pollfd pfds[1];
    g_autofree char *buf = NULL;
    size_t buf_len = 1024;
    int timeout = PKT_TIMEOUT_MS;
    int ret;

    buf = g_new0(char, buf_len);

    pfds[0].fd = sock;
    pfds[0].events = POLLIN;

    if (!use_timeout)
        timeout = -1;

    do {
        ret = poll(pfds, G_N_ELEMENTS(pfds), timeout);
    } while (ret < 0 && errno == EINTR);

    if (ret <= 0) {
        if (ret < 0) {
            virReportSystemError(errno, _("Poll on sock %1$d failed"), sock);
        } else if (ret == 0) {
            virReportSystemError(errno, _("Poll on sock %1$d timed out"), sock);
        }
        return NULL;
    }

    do {
        ret = recv(sock, buf, buf_len - 1, 0);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        virReportSystemError(errno, _("recv on sock %1$d failed"), sock);
        return NULL;
    }

    return g_steal_pointer(&buf);
}
#undef PKT_TIMEOUT_MS

int
chSocketProcessHttpResponse(int sock, bool use_poll_timeout)
{
    g_autofree char *response = NULL;
    int http_res;

    response = chSocketRecv(sock, use_poll_timeout);
    if (response == NULL) {
        return -1;
    }

    /* Parse the HTTP response code */
    if (sscanf(response, "HTTP/1.%*d %d", &http_res) != 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("Failed to parse HTTP response code"));
        return -1;
    }
    if (http_res != 204 && http_res != 200) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                        _("Unexpected response from CH: %1$s"), response);
        return -1;
    }

    return 0;
}

int
chCloseFDs(int *fds, size_t nfds)
{
    size_t i;
    for (i = 0; i < nfds; i++) {
        VIR_FORCE_CLOSE(fds[i]);
    }
    return 0;
}
