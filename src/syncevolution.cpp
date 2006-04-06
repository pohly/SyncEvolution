/*
 * Copyright (C) 2005 Patrick Ohly
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <config.h>

#include <base/Log.h>
#include <posix/base/posixlog.h>

#include <iostream>
using namespace std;

#include <libgen.h>

#include "EvolutionContactSource.h"
#include "EvolutionSyncClient.h"

/**
 * list all known data sources of a certain type
 */
static void listSources( EvolutionSyncSource &syncSource, const string &header )
{
    cout << header << ":\n";
    EvolutionSyncSource::sources sources = syncSource.getSyncBackends();

    for( EvolutionSyncSource::sources::const_iterator it = sources.begin();
         it != sources.end();
         it++ ) {
        cout << it->m_name << ": " << it->m_uri << "\n";
    }
}

int main( int argc, char **argv )
{
    setLogFile("-");
    LOG.reset();
    LOG.setLevel(LOG_LEVEL_INFO);
    resetError();

    // Expand PATH to cover the directory we were started from?
    // This might be needed to find normalize_vcard.
    char *exe = strdup(argv[0]);
    if (strchr(exe, '/') ) {
        char *dir = dirname(exe);
        string path;
        char *oldpath = getenv("PATH");
        if (oldpath) {
            path += oldpath;
            path += ":";
        }
        path += dir;
        setenv("PATH", path.c_str(), 1);
    }
    free(exe);

    try {
        if ( argc != 2 ) {
            EvolutionContactSource contactSource( string( "list" ) );

            listSources( contactSource, "address books" );

            fprintf( stderr, "\nusage: %s <server>\n", argv[0] );
        } else {
            EvolutionSyncClient client(argv[1]);
            client.sync(SYNC_NONE, true);
        }
        return 0;
    } catch ( int sync4jerror ) {
        LOG.error( lastErrorMsg );
    } catch ( string *errmsg ) {
        LOG.error( errmsg->c_str() );
        delete errmsg;
    } catch ( const char *errmsg ) {
        LOG.error( errmsg );
    } catch ( const string error ) {
        LOG.error( error.c_str() );
    } catch ( std::ios_base::failure error ) {
        LOG.error( error.what() );
    } catch (...) {
        LOG.error( "unknown error" );
    }

    return 1;
}
