/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2011 Artem Pavlenko
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#ifndef POSTGIS_CONNECTION_HPP
#define POSTGIS_CONNECTION_HPP

// mapnik
#include <mapnik/debug.hpp>
#include <mapnik/datasource.hpp>
#include <mapnik/timer.hpp>

// boost
#include <boost/make_shared.hpp>

// std
#include <sstream>
#include <iostream>
#include <sys/select.h>

extern "C" {
#include "libpq-fe.h"
}

#include "resultset.hpp"

class Connection
{
public:
    Connection(std::string const& connection_str,boost::optional<std::string> const& password)
        : cursorId(0),
          closed_(false),
          pending_(false),
          statement_timeout_(4000) // in milliseconds
    {
        std::string connect_with_pass = connection_str;
        if (password && !password->empty())
        {
            connect_with_pass += " password=" + *password;
        }
        conn_ = PQconnectdb(connect_with_pass.c_str());
        MAPNIK_LOG_DEBUG(postgis) << "postgis_connection: postgresql connection create - " << this;
        if (PQstatus(conn_) != CONNECTION_OK)
        {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nConnection string: '";
            err_msg += connection_str;
            err_msg += "'\n";
            MAPNIK_LOG_DEBUG(postgis) << "postgis_connection: creation failed, closing connection - " << this;
            close();
            throw mapnik::datasource_exception(err_msg);
        }
    }

    ~Connection()
    {
        if (! closed_)
        {
            PQfinish(conn_);
            MAPNIK_LOG_DEBUG(postgis) << "postgis_connection: postgresql connection closed - " << this;
            closed_ = true;
        }
    }

    bool execute(std::string const& sql) const
    {
#ifdef MAPNIK_STATS
        mapnik::progress_timer __stats__(std::clog, std::string("postgis_connection::execute ") + sql);
#endif

        int sent = PQsendQuery(conn_, sql.c_str());
        if ( ! sent ) return false;
        bool ok = false;
        while ( PGresult *result = PQgetResult(conn_) ) {
          ok = (PQresultStatus(result) == PGRES_COMMAND_OK);
          PQclear(result);
        }
        return ok;
    }

    boost::shared_ptr<ResultSet> executeQuery(std::string const& sql, int type = 0) const
    {
#ifdef MAPNIK_STATS
        mapnik::progress_timer __stats__(std::clog, std::string("postgis_connection::execute_query ") + sql);
#endif

        mapnik::timer timeout;

        PGresult* result = 0;
        int success;
        if (type == 1)
        {
            success = PQsendQueryParams(conn_, sql.c_str(), 0, 0, 0, 0, 0, 1);
        }
        else
        {
            success = PQsendQuery(conn_, sql.c_str());
        }

        int sock = PQsocket(conn_);
        if ( sock < 0 ) success = false;

        if ( ! success ) {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nin executeQuery Full sql was: '";
            err_msg += sql;
            err_msg += "'\n";
            throw mapnik::datasource_exception(err_msg);
        }

        bool ok = false;
        fd_set input_mask;
        struct timeval toutval;
        while ( true ) {
          do {

            success = PQconsumeInput(conn_);
            if ( ! success ) break;

            if ( PQisBusy(conn_) ) {
              FD_ZERO(&input_mask);
              FD_SET(sock, &input_mask);

              int msleft = statement_timeout_ - timeout.wall_clock_elapsed();
              toutval.tv_sec = 0;
              toutval.tv_usec = msleft*1000; // microseconds

              int ret = select(sock + 1, &input_mask, NULL, NULL, &toutval);
              if ( ret < 1 )
              {
                std::stringstream ss;
                ss << "Postgis Plugin: ";
                if ( ret == 0 ) {
                  ss << "timeout ";
                } else {
                  ss << "select: " << strerror(errno);
                }
                ss << "\nin executeQuery Full sql was: '";
                ss << sql;
                ss << "'\n";
                const_cast<Connection*>(this)->close();
                throw mapnik::datasource_exception(ss.str());
              }
            }
          } while ( PQisBusy(conn_) );
          if ( ! success ) {
            ok = false;
            break;
          }
          PGresult *tmp = PQgetResult(conn_);
          if ( ! tmp ) break;
          ok = (PQresultStatus(tmp) == PGRES_TUPLES_OK);
          if ( result ) PQclear(result);
          result = tmp;
        }
        if (!ok) 
        {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nin executeQuery Full sql was: '";
            err_msg += sql;
            err_msg += "'\n";
            if (result) PQclear(result);
            throw mapnik::datasource_exception(err_msg);
        }

        return boost::make_shared<ResultSet>(result);
    }

    std::string status() const
    {
        std::string status;
        if (conn_)
        {
            if ( isOK() ) return PQerrorMessage(conn_);
            else return "Bad connection";
        }
        else
        {
            status = "Uninitialized connection";
        }
        return status;
    }

    bool executeAsyncQuery(std::string const& sql, int type = 0)
    {
        int result = 0;
        if (type == 1)
        {
            result = PQsendQueryParams(conn_,sql.c_str(), 0, 0, 0, 0, 0, 1);
        }
        else
        {
            result = PQsendQuery(conn_, sql.c_str());
        }
        if (result != 1)
        {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nin executeAsyncQuery Full sql was: '";
            err_msg += sql;
            err_msg += "'\n";
            clearAsyncResult(PQgetResult(conn_));
            close();
            throw mapnik::datasource_exception(err_msg);
        }
        pending_ = true;
        return result;
    }


    boost::shared_ptr<ResultSet> getNextAsyncResult()
    {
        PGresult *result = PQgetResult(conn_);
        if( result && (PQresultStatus(result) != PGRES_TUPLES_OK))
        {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nin getNextAsyncResult";
            clearAsyncResult(result);
            // We need to guarde against losing the connection
            // (i.e db restart) so here we invalidate the full connection
            close();
            throw mapnik::datasource_exception(err_msg);
        }
       return boost::make_shared<ResultSet>(result);
    }

    boost::shared_ptr<ResultSet> getAsyncResult()
    {
        PGresult *result = PQgetResult(conn_);
        if ( !result || (PQresultStatus(result) != PGRES_TUPLES_OK))
        {
            std::string err_msg = "Postgis Plugin: ";
            err_msg += status();
            err_msg += "\nin getAsyncResult";
            clearAsyncResult(result);
            // We need to be guarded against losing the connection
            // (i.e db restart), we invalidate the full connection
            close();
            throw mapnik::datasource_exception(err_msg);
        }
        return boost::make_shared<ResultSet>(result);
    }

    std::string client_encoding() const
    {
        return PQparameterStatus(conn_, "client_encoding");
    }

    bool isOK() const
    {
        return (!closed_) && (PQstatus(conn_) != CONNECTION_BAD);
    }

    bool isPending() const
    {
        return pending_;
    }

    void close()
    {
        if (! closed_)
        {
            PQfinish(conn_);
            MAPNIK_LOG_DEBUG(postgis) << "postgis_connection: closing connection (close)- " << this;
            closed_ = true;
        }
    }

    std::string new_cursor_name()
    {
        std::ostringstream s;
        s << "mapnik_" << (cursorId++);
        return s.str();
    }

private:
    PGconn *conn_;
    int cursorId;
    bool closed_;
    bool pending_;
    int statement_timeout_; // statement timeout in seconds

    void clearAsyncResult(PGresult *result)
    {
        // Clear all pending results
        while(result)
        {
           PQclear(result);
           result = PQgetResult(conn_);
        }
        pending_ = false;
    }
};

#endif //CONNECTION_HPP
