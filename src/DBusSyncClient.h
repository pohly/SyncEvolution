/* This is an implementation of EvolutionSyncClient
 * that is a DBus service. Internally it uses a 
 * SyncevoDBusServer GObject to handle the DBus side
 * of things.
 * */


#ifndef INCL_DBUSSYNCCLIENT
#define INCL_DBUSSYNCCLIENT

#include <config.h>

#include <synthesis/sync_declarations.h>
#include "EvolutionSyncClient.h"

#include <string>
#include <set>
#include <map>

class DBusSyncClient : public EvolutionSyncClient {

public:
	DBusSyncClient(const string &server,
				   const map<string, int> &source_map,
				   void (*progress) (const char *source,int type,int extra1,int extra2,int extra3,gpointer data) = NULL,
				   void (*server_message) (const char *message,gpointer data) = NULL,
				   char* (*need_password) (const char *message,gpointer data) = NULL,
				   gboolean (*check_for_suspend)(gpointer data) = NULL,
				   gpointer userdata = NULL);

	~DBusSyncClient();

protected:
	virtual void prepare(const std::vector<EvolutionSyncSource *> &sources);

	virtual string askPassword(const string &descr);

	virtual void displayServerMessage(const string &message);

	virtual void displaySyncProgress(sysync::TProgressEventEnum type,
	                                 int32_t extra1, int32_t extra2, int32_t extra3);

	virtual void displaySourceProgress(sysync::TProgressEventEnum type,
	                                   EvolutionSyncSource &source,
	                                   int32_t extra1, int32_t extra2, int32_t extra3);

	virtual bool checkForSuspend();

private:
	map<string, int> m_source_map;
	gpointer m_userdata;

	void (*m_progress) (const char *source,int type,int extra1,int extra2,int extra3,gpointer data);
	void (*m_server_message) (const char *message,gpointer data);
	char* (*m_need_password) (const char *message,gpointer data);
	gboolean (*m_check_for_suspend) (gpointer data);

	static set<string> getSyncSources (const map<string, int> &source_map)
	{
		set<string> sources;
		map<string,int>::const_iterator iter;

		for (iter = source_map.begin (); iter != source_map.end (); iter++)
			sources.insert (iter->first);

		return sources;
	}
};


#endif
