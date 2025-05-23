#include <sys/socket.h>

#include "dw_poll.h"
#include "dw_debug.h"

extern int use_wait_spinning;

// return value useful to return failure if we allocate memory here in the future
int dw_poll_init(dw_poll_t *p_poll, dw_poll_type_t type) {
    p_poll->poll_type = type;
    switch (p_poll->poll_type) {
    case DW_SELECT:
        p_poll->u.select_fds.n_rd_fd = 0;
        p_poll->u.select_fds.n_wr_fd = 0;
        break;
    case DW_POLL:
        p_poll->u.poll_fds.n_pollfds = 0;
        break;
    case DW_EPOLL:
        sys_check(p_poll->u.epoll_fds.epollfd = epoll_create1(0));
        break;
    default:
        check(0, "Wrong dw_poll_type");
    }
    return 0;
}

void dw_select_del_rd_pos(dw_poll_t *p_poll, int i) {
    p_poll->u.select_fds.n_rd_fd--;
    if (i < p_poll->u.select_fds.n_rd_fd) {
        p_poll->u.select_fds.rd_fd[i] =
            p_poll->u.select_fds.rd_fd[p_poll->u.select_fds.n_rd_fd];
        p_poll->u.select_fds.rd_flags[i] =
                p_poll->u.select_fds.rd_flags[p_poll->u.select_fds.n_rd_fd];
        p_poll->u.select_fds.rd_aux[i] =
            p_poll->u.select_fds.rd_aux[p_poll->u.select_fds.n_rd_fd];
    }
}

void dw_select_del_wr_pos(dw_poll_t *p_poll, int i) {
    p_poll->u.select_fds.n_wr_fd--;
    if (i < p_poll->u.select_fds.n_wr_fd) {
        p_poll->u.select_fds.wr_fd[i] =
            p_poll->u.select_fds.wr_fd[p_poll->u.select_fds.n_wr_fd];
        p_poll->u.select_fds.wr_flags[i] =
            p_poll->u.select_fds.wr_flags[p_poll->u.select_fds.n_wr_fd];
        p_poll->u.select_fds.wr_aux[i] =
            p_poll->u.select_fds.wr_aux[p_poll->u.select_fds.n_wr_fd];
    }
}

void dw_poll_del_pos(dw_poll_t *p_poll, int i) {
    p_poll->u.poll_fds.n_pollfds--;
    if (i < p_poll->u.poll_fds.n_pollfds) {
        p_poll->u.poll_fds.pollfds[i] = p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.n_pollfds];
        p_poll->u.poll_fds.flags[i] = p_poll->u.poll_fds.flags[p_poll->u.poll_fds.n_pollfds];
        p_poll->u.poll_fds.aux[i] = p_poll->u.poll_fds.aux[p_poll->u.poll_fds.n_pollfds];
    }
}

void dw_select_add_rd(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux) {
    p_poll->u.select_fds.rd_fd[p_poll->u.select_fds.n_rd_fd] = fd;
    p_poll->u.select_fds.rd_aux[p_poll->u.select_fds.n_rd_fd] = aux;
    p_poll->u.select_fds.rd_flags[p_poll->u.select_fds.n_rd_fd] = flags;
    p_poll->u.select_fds.n_rd_fd++;
}

void dw_select_add_wr(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux) {
    p_poll->u.select_fds.wr_fd[p_poll->u.select_fds.n_wr_fd] = fd;
    p_poll->u.select_fds.wr_aux[p_poll->u.select_fds.n_wr_fd] = aux;
    p_poll->u.select_fds.wr_flags[p_poll->u.select_fds.n_wr_fd] = flags;
    p_poll->u.select_fds.n_wr_fd++;
}

int dw_poll_add(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux) {
    dw_log("dw_poll_add(): fd=%d, p_poll=%p, flags=%08x, aux=%lu\n", fd, p_poll, flags, aux);
    int rv = 0;
    switch (p_poll->poll_type) {
    case DW_SELECT:
        if ((flags & DW_POLLIN && p_poll->u.select_fds.n_rd_fd == MAX_POLLFD)
            || (flags & DW_POLLOUT && p_poll->u.select_fds.n_wr_fd == MAX_POLLFD)) {
            dw_log("Exhausted number of possible fds in select()\n");
            return -1;
        }
        if (flags & DW_POLLIN)
            dw_select_add_rd(p_poll, fd, flags, aux);
        if (flags & DW_POLLOUT)
            dw_select_add_wr(p_poll, fd, flags, aux);
        break;
    case DW_POLL:
        if (p_poll->u.poll_fds.n_pollfds == MAX_POLLFD) {
            dw_log("Exhausted number of possible fds in poll()\n");
            return -1;
        }
        struct pollfd *pev = &p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.n_pollfds];
        p_poll->u.poll_fds.flags[p_poll->u.poll_fds.n_pollfds] = flags;
        p_poll->u.poll_fds.aux[p_poll->u.poll_fds.n_pollfds] = aux;
        p_poll->u.poll_fds.n_pollfds++;
        pev->fd = fd;
        pev->events = (flags & DW_POLLIN ? POLLIN : 0)
                      | (flags & DW_POLLOUT ? POLLOUT : 0);
        break;
    case DW_EPOLL: {
        struct epoll_event ev = (struct epoll_event) {
            .data.u64 = aux,
            .events = (flags & DW_POLLIN ? EPOLLIN : 0)
                      | (flags & DW_POLLOUT ? EPOLLOUT : 0)
                      | (flags & DW_POLLONESHOT ? EPOLLONESHOT : 0),
        };

        if ((rv = epoll_ctl(p_poll->u.epoll_fds.epollfd, EPOLL_CTL_ADD, fd, &ev)) < 0)
            perror("epoll_ctl() failed: ");
        break; }
    default:
        check(0, "Wrong dw_poll_type");
    }
    return rv;
}

/* This tolerates on purpose being called after a ONESHOT event, for which
 * poll and select will have deleted the fd in their user-space lists, whilst
 * epoll has it still registered, but with a clear events interest list
 */
int dw_poll_mod(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux) {
    dw_log("dw_poll_mod(): fd=%d, p_poll=%p, flags=%08x, aux=%lu\n", fd, p_poll, flags, aux);
    int rv = 0;
    switch (p_poll->poll_type) {
    case DW_SELECT: {
        int i;
        for (i = 0; i < p_poll->u.select_fds.n_rd_fd; i++)
            if (p_poll->u.select_fds.rd_fd[i] == fd)
                break;
        if (i < p_poll->u.select_fds.n_rd_fd && !(flags & DW_POLLIN))
            dw_select_del_rd_pos(p_poll, i);
        else if (i == p_poll->u.select_fds.n_rd_fd && (flags & DW_POLLIN))
            dw_select_add_rd(p_poll, fd, flags, aux);

        for (i = 0; i < p_poll->u.select_fds.n_wr_fd; i++)
            if (p_poll->u.select_fds.wr_fd[i] == fd)
                break;
        if (i < p_poll->u.select_fds.n_wr_fd && !(flags & DW_POLLOUT))
            dw_select_del_wr_pos(p_poll, i);
        else if (i == p_poll->u.select_fds.n_wr_fd && (flags & DW_POLLOUT))
            dw_select_add_wr(p_poll, fd, flags, aux);
        break; }
    case DW_POLL: {
        int i;
        for (i = 0; i < p_poll->u.poll_fds.n_pollfds; i++) {
            struct pollfd *pev = &p_poll->u.poll_fds.pollfds[i];
            if (pev->fd == fd) {
                pev->events = (flags & DW_POLLIN ? POLLIN : 0) | (flags & DW_POLLOUT ? POLLOUT : 0);
                dw_log("dw_poll_mod(): pev->events=%04x\n", pev->events);
                if (pev->events == 0)
                    dw_poll_del_pos(p_poll, i);
                break;
            }
        }
        if (i == p_poll->u.poll_fds.n_pollfds && flags != 0)
            rv = dw_poll_add(p_poll, fd, flags, aux);
        break; }
    case DW_EPOLL: {
        struct epoll_event ev = {
            .data.u64 = aux,
            .events = (flags & DW_POLLIN ? EPOLLIN : 0) | (flags & DW_POLLOUT ? EPOLLOUT : 0),
        };
        if (flags & (DW_POLLIN | DW_POLLOUT))
            rv = epoll_ctl(p_poll->u.epoll_fds.epollfd, EPOLL_CTL_MOD, fd, &ev);
        else
            rv = epoll_ctl(p_poll->u.epoll_fds.epollfd, EPOLL_CTL_DEL, fd, NULL);
        break; }
    default:
        check(0, "Wrong dw_poll_type");
    }
    return rv;
}

// return the number of file descriptors expected to iterate with dw_poll_next(),
// or -1 setting errno
int dw_poll_wait(dw_poll_t *p_poll) {
    int rv;
    switch (p_poll->poll_type) {
    case DW_SELECT:
        FD_ZERO(&p_poll->u.select_fds.rd_fds);
        FD_ZERO(&p_poll->u.select_fds.wr_fds);
        FD_ZERO(&p_poll->u.select_fds.ex_fds);
        int max_fd = 0;
        for (int i = 0; i < p_poll->u.select_fds.n_rd_fd; i++) {
            FD_SET(p_poll->u.select_fds.rd_fd[i], &p_poll->u.select_fds.rd_fds);
            if (p_poll->u.select_fds.rd_fd[i] > max_fd)
                max_fd = p_poll->u.select_fds.rd_fd[i];
        }
        for (int i = 0; i < p_poll->u.select_fds.n_wr_fd; i++) {
            FD_SET(p_poll->u.select_fds.wr_fd[i], &p_poll->u.select_fds.wr_fds);
            if (p_poll->u.select_fds.wr_fd[i] > max_fd)
                max_fd = p_poll->u.select_fds.wr_fd[i];
        }
        dw_log("select()ing: max_fd=%d\n", max_fd);
        struct timeval null_tout = { .tv_sec = 0, .tv_usec = 0 };
        rv = select(max_fd + 1, &p_poll->u.select_fds.rd_fds, &p_poll->u.select_fds.wr_fds, &p_poll->u.select_fds.ex_fds, use_wait_spinning ? &null_tout : NULL);
        // make sure we don't wastefully iterate if select() returned 0 fds ready or error
        p_poll->u.select_fds.iter = rv > 0 ? 0 : p_poll->u.select_fds.n_rd_fd + p_poll->u.select_fds.n_wr_fd;
        break;
    case DW_POLL:
        dw_log("poll()ing: n_pollfds=%d\n", p_poll->u.poll_fds.n_pollfds);
        rv = poll(p_poll->u.poll_fds.pollfds, p_poll->u.poll_fds.n_pollfds,
                  use_wait_spinning ? 0 : -1);
        // make sure we don't wastefully iterate if poll() returned 0 fds ready or error
        p_poll->u.poll_fds.iter = rv > 0 ? 0 : p_poll->u.poll_fds.n_pollfds;
        break;
    case DW_EPOLL:
        dw_log("epoll_wait()ing: epollfd=%d\n", p_poll->u.epoll_fds.epollfd);
        rv = epoll_wait(p_poll->u.epoll_fds.epollfd, p_poll->u.epoll_fds.events, MAX_POLLFD,
                        use_wait_spinning ? 0 : -1);
        p_poll->u.epoll_fds.iter = 0;
        if (rv >= 0)
            p_poll->u.epoll_fds.n_events = rv;
        break;
    default:
        check(0, "Wrong dw_poll_type");
    }
    return rv;
}

// returned fd is automatically removed from dw_poll if marked 1SHOT
int dw_poll_next(dw_poll_t *p_poll, dw_poll_flags *flags, uint64_t *aux) {
    switch (p_poll->poll_type) {
    case DW_SELECT:
        while (p_poll->u.select_fds.iter < p_poll->u.select_fds.n_rd_fd && !FD_ISSET(p_poll->u.select_fds.rd_fd[p_poll->u.select_fds.iter], &p_poll->u.select_fds.rd_fds))
            p_poll->u.select_fds.iter++;
        if (p_poll->u.select_fds.iter < p_poll->u.select_fds.n_rd_fd && FD_ISSET(p_poll->u.select_fds.rd_fd[p_poll->u.select_fds.iter], &p_poll->u.select_fds.rd_fds)) {
            *aux = p_poll->u.select_fds.rd_aux[p_poll->u.select_fds.iter];
            *flags = DW_POLLIN;

            if (p_poll->u.select_fds.rd_flags[p_poll->u.select_fds.iter] & DW_POLLONESHOT)
                // item iter replaced with last, so we need to check iter again
                dw_select_del_rd_pos(p_poll, p_poll->u.select_fds.iter);
            else
                p_poll->u.select_fds.iter++;
            return 1;
        }
        int n_rd = p_poll->u.select_fds.n_rd_fd;
        while (p_poll->u.select_fds.iter < n_rd + p_poll->u.select_fds.n_wr_fd && !FD_ISSET(p_poll->u.select_fds.wr_fd[p_poll->u.select_fds.iter - n_rd], &p_poll->u.select_fds.wr_fds))
            p_poll->u.select_fds.iter++;
        if (p_poll->u.select_fds.iter < n_rd + p_poll->u.select_fds.n_wr_fd && FD_ISSET(p_poll->u.select_fds.wr_fd[p_poll->u.select_fds.iter - n_rd], &p_poll->u.select_fds.wr_fds)) {
            *aux = p_poll->u.select_fds.wr_aux[p_poll->u.select_fds.iter - n_rd];
            *flags = DW_POLLOUT;
            if (p_poll->u.select_fds.wr_flags[p_poll->u.select_fds.iter - n_rd] & DW_POLLONESHOT)
                // item iter replaced with last, so we need to check iter again
                dw_select_del_wr_pos(p_poll, p_poll->u.select_fds.iter - n_rd);
            else
                p_poll->u.select_fds.iter++;
            return 1;
        }
        break;
    case DW_POLL:
        while (p_poll->u.poll_fds.iter < p_poll->u.poll_fds.n_pollfds && p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents == 0)
            p_poll->u.poll_fds.iter++;
        if (p_poll->u.poll_fds.iter < p_poll->u.poll_fds.n_pollfds && p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents != 0) {
            *flags = 0;
            *flags |= (p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents & POLLIN) ? DW_POLLIN : 0;
            *flags |= (p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents & POLLOUT) ? DW_POLLOUT : 0;
            *flags |= (p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents & POLLERR) ? DW_POLLERR : 0;
            *flags |= (p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents & POLLHUP) ? DW_POLLHUP : 0;
            *aux = p_poll->u.poll_fds.aux[p_poll->u.poll_fds.iter];
            if (p_poll->u.poll_fds.flags[p_poll->u.poll_fds.iter] & DW_POLLONESHOT)
                // item i replaced with last, so we need to check i again
                dw_poll_del_pos(p_poll, p_poll->u.poll_fds.iter);
            else
                p_poll->u.poll_fds.iter++;
            return 1;
        }
        break;
    case DW_EPOLL:
        if (p_poll->u.epoll_fds.iter < p_poll->u.epoll_fds.n_events && p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].events != 0) {
            *flags = 0;
            *flags |= (p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].events & EPOLLIN) ? DW_POLLIN : 0;
            *flags |= (p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].events & EPOLLOUT) ? DW_POLLOUT : 0;
            *flags |= (p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].events & EPOLLERR) ? DW_POLLERR : 0;
            *flags |= (p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].events & EPOLLHUP) ? DW_POLLHUP : 0;
            *aux = p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].data.u64;
            p_poll->u.epoll_fds.iter++;
            return 1;
        }
        break;
    default:
        check(0, "Wrong dw_poll_type");
    }
    return 0;
}
