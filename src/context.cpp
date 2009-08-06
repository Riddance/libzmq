/*
    Copyright (c) 2007-2009 FastMQ Inc.

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the Lesser GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Lesser GNU General Public License for more details.

    You should have received a copy of the Lesser GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../include/zmq.h"

#include "context.hpp"
#include "i_api.hpp"
#include "app_thread.hpp"
#include "io_thread.hpp"
#include "platform.hpp"
#include "err.hpp"
#include "pipe.hpp"

#if defined ZMQ_HAVE_WINDOWS
#include "windows.h"
#endif

zmq::context_t::context_t (int app_threads_, int io_threads_)
{
#ifdef ZMQ_HAVE_WINDOWS
    //  Intialise Windows sockets. Note that WSAStartup can be called multiple
    //  times given that WSACleanup will be called for each WSAStartup.
    WORD version_requested = MAKEWORD (2, 2);
    WSADATA wsa_data;
    int rc = WSAStartup (version_requested, &wsa_data);
    zmq_assert (rc == 0);
    zmq_assert (LOBYTE (wsa_data.wVersion) == 2 &&
        HIBYTE (wsa_data.wVersion) == 2);
#endif

    //  Create application thread proxies.
    for (int i = 0; i != app_threads_; i++) {
        app_thread_t *app_thread = new app_thread_t (this, i);
        zmq_assert (app_thread);
        app_threads.push_back (app_thread);
        signalers.push_back (app_thread->get_signaler ());
    }

    //  Create I/O thread objects.
    for (int i = 0; i != io_threads_; i++) {
        io_thread_t *io_thread = new io_thread_t (this, i + app_threads_);
        zmq_assert (io_thread);
        io_threads.push_back (io_thread);
        signalers.push_back (io_thread->get_signaler ());
    }

    //  Create command pipe matrix.
    command_pipes = new command_pipe_t [signalers.size () * signalers.size ()];
    zmq_assert (command_pipes);

    //  Launch I/O threads.
    for (int i = 0; i != io_threads_; i++)
        io_threads [i]->start ();
}

zmq::context_t::~context_t ()
{
    //  Close all application theads, sockets, io_objects etc.
    for (app_threads_t::size_type i = 0; i != app_threads.size (); i++)
        delete app_threads [i];

    //  Ask I/O threads to terminate. If stop signal wasn't sent to I/O
    //  thread subsequent invocation of destructor would hang-up.
    for (io_threads_t::size_type i = 0; i != io_threads.size (); i++)
        io_threads [i]->stop ();

    //  Wait till I/O threads actually terminate.
    for (io_threads_t::size_type i = 0; i != io_threads.size (); i++)
        delete io_threads [i];

    delete [] command_pipes;

#ifdef ZMQ_HAVE_WINDOWS
    //  On Windows, uninitialise socket layer.
    int rc = WSACleanup ();
    wsa_assert (rc != SOCKET_ERROR);
#endif
}

int zmq::context_t::thread_slot_count ()
{
    return signalers.size ();
}

zmq::i_api *zmq::context_t::create_socket (int type_)
{
    threads_sync.lock ();
    app_thread_t *thread = choose_app_thread ();
    if (!thread) {
        threads_sync.unlock ();
        return NULL;
    }

    zmq_assert (false);
    i_api *s = NULL;
    //i_api *s = thread->create_socket (type_);

    threads_sync.unlock ();
    return s;
}

zmq::app_thread_t *zmq::context_t::choose_app_thread ()
{
    //  Check whether thread ID is already assigned. If so, return it.
    for (app_threads_t::size_type i = 0; i != app_threads.size (); i++)
        if (app_threads [i]->is_current ())
            return app_threads [i];

    //  Check whether there's an unused thread slot in the cotext.
    for (app_threads_t::size_type i = 0; i != app_threads.size (); i++)
        if (app_threads [i]->make_current ())
            return app_threads [i];

    //  Thread limit was exceeded.
    errno = EMFILE;
    return NULL;
}

zmq::io_thread_t *zmq::context_t::choose_io_thread (uint64_t taskset_)
{
    zmq_assert (io_threads.size () > 0);

    //  Find the I/O thread with minimum load.
    int min_load = io_threads [0]->get_load ();
    io_threads_t::size_type result = 0;
    for (io_threads_t::size_type i = 1; i != io_threads.size (); i++) {
        if (!taskset_ || (taskset_ & (uint64_t (1) << i))) {
            int load = io_threads [i]->get_load ();
            if (load < min_load) {
                min_load = load;
                result = i;
            }
        }
    }

    return io_threads [result];
}
