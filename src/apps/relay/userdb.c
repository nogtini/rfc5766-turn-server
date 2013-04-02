/*
 * Copyright (C) 2013 Citrix Systems
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <ifaddrs.h>
#include <getopt.h>
#include <locale.h>
#include <libgen.h>

#if !defined(TURN_NO_THREADS)
#include <pthread.h>
#endif

#if !defined(TURN_NO_PQ)
#include <libpq-fe.h>
#endif

#if !defined(TURN_NO_MYSQL)
#include <mysql.h>
#endif

#include <signal.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "userdb.h"

#include "ns_turn_utils.h"

#include "ns_turn_server.h"
#include "ns_turn_maps.h"

#include "apputils.h"

//////////// USER DB //////////////////////////////

TURN_USERDB_TYPE userdb_type=TURN_USERDB_TYPE_FILE;
char userdb[1025]="\0";

size_t users_number = 0;
int use_lt_credentials = 0;
int anon_credentials = 0;

int use_auth_secret_with_timestamp = 0;
char static_auth_secret[1025]="\0";
turn_time_t auth_secret_timestamp_expiration_time = DEFAULT_AUTH_SECRET_EXPIRATION_TIME;

turn_user_db *users = NULL;

s08bits global_realm[1025]="\0";

/////////// USER DB CHECK //////////////////

static int convert_string_key_to_binary(char* keysource, hmackey_t key) {
	if(strlen(keysource)!=(2*sizeof(hmackey_t))) {
		return -1;
	} else {
		char is[3];
		size_t i;
		unsigned int v;
		is[2]=0;
		for(i=0;i<sizeof(hmackey_t);i++) {
			is[0]=keysource[i*2];
			is[1]=keysource[i*2+1];
			sscanf(is,"%02x",&v);
			key[i]=(unsigned char)v;
		}
		return 0;
	}
}


static int is_pqsql_userdb(void)
{
#if !defined(TURN_NO_PQ)
	return (userdb_type == TURN_USERDB_TYPE_PQ);
#else
	return 0;
#endif
}

static int is_mysql_userdb(void)
{
#if !defined(TURN_NO_MYSQL)
	return (userdb_type == TURN_USERDB_TYPE_MYSQL);
#else
	return 0;
#endif
}

#if !defined(TURN_NO_PQ)
static PGconn *get_pqdb_connection(void)
{
	static PGconn *pqdbconnection = NULL;
	if(pqdbconnection) {
		ConnStatusType status = PQstatus(pqdbconnection);
		if(status != CONNECTION_OK) {
			PQfinish(pqdbconnection);
			pqdbconnection = NULL;
		}
	}
	if(!pqdbconnection && is_pqsql_userdb()) {
		char *errmsg=NULL;
		PQconninfoOption *co = PQconninfoParse(userdb, &errmsg);
		if(!co) {
			if(errmsg) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot open DB connection <%s>, connection string format error: %s\n",userdb,errmsg);
				free(errmsg);
			} else {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot open DB connection: <%s>, unknown connection string format error\n",userdb);
			}
		} else {
			PQconninfoFree(co);
			if(errmsg)
				free(errmsg);
			pqdbconnection = PQconnectdb(userdb);
			if(!pqdbconnection) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot open DB connection: <%s>, runtime error\n",userdb);
			} else {
				ConnStatusType status = PQstatus(pqdbconnection);
				if(status != CONNECTION_OK) {
					PQfinish(pqdbconnection);
					pqdbconnection = NULL;
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot open DB connection: <%s>, runtime error\n",userdb);
				} else {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "DB connection success: %s\n",userdb);
				}
			}
		}
	}
	return pqdbconnection;
}
#endif

#if !defined(TURN_NO_MYSQL)

struct _Myconninfo {
	char *host;
	char *dbname;
	char *user;
	char *password;
	unsigned int port;
};

typedef struct _Myconninfo Myconninfo;

static void MyconninfoFree(Myconninfo *co) {
	if(co) {
		if(co->host) free(co->host);
		if(co->dbname) free(co->dbname);
		if(co->user) free(co->user);
		if(co->password) free(co->password);
		ns_bzero(co,sizeof(Myconninfo));
	}
}

static Myconninfo *MyconninfoParse(char *userdb, char **errmsg)
{
	Myconninfo *co = (Myconninfo*)malloc(sizeof(Myconninfo));
	ns_bzero(co,sizeof(Myconninfo));
	if(userdb) {
		char *s0=strdup(userdb);
		char *s = s0;

		while(s && *s) {

			while(*s && (*s==' ')) ++s;
			char *snext = strstr(s," ");
			if(snext) {
				*snext = 0;
				++snext;
			}

			char* seq = strstr(s,"=");
			if(!seq) {
				MyconninfoFree(co);
				co = NULL;
				if(errmsg) {
					*errmsg = strdup(s);
				}
				break;
			}

			*seq = 0;
			if(!strcmp(s,"host"))
				co->host = strdup(seq+1);
			else if(!strcmp(s,"ip"))
				co->host = strdup(seq+1);
			else if(!strcmp(s,"addr"))
				co->host = strdup(seq+1);
			else if(!strcmp(s,"ipaddr"))
				co->host = strdup(seq+1);
			else if(!strcmp(s,"dbname"))
				co->dbname = strdup(seq+1);
			else if(!strcmp(s,"db"))
				co->dbname = strdup(seq+1);
			else if(!strcmp(s,"database"))
				co->dbname = strdup(seq+1);
			else if(!strcmp(s,"user"))
				co->user = strdup(seq+1);
			else if(!strcmp(s,"uname"))
				co->user = strdup(seq+1);
			else if(!strcmp(s,"name"))
				co->user = strdup(seq+1);
			else if(!strcmp(s,"username"))
				co->user = strdup(seq+1);
			else if(!strcmp(s,"password"))
				co->password = strdup(seq+1);
			else if(!strcmp(s,"pwd"))
				co->password = strdup(seq+1);
			else if(!strcmp(s,"passwd"))
				co->password = strdup(seq+1);
			else if(!strcmp(s,"secret"))
				co->password = strdup(seq+1);
			else if(!strcmp(s,"port"))
				co->port = (unsigned int)atoi(seq+1);
			else if(!strcmp(s,"p"))
				co->port = (unsigned int)atoi(seq+1);
			else {
				MyconninfoFree(co);
				co = NULL;
				if(errmsg) {
					*errmsg = strdup(s);
				}
				break;
			}

			s = snext;
		}

		free(s0);
	}
	return co;
}

static MYSQL *get_mydb_connection(void)
{
	static MYSQL *mydbconnection = NULL;

	if(mydbconnection) {
		if(mysql_ping(mydbconnection)) {
			mysql_close(mydbconnection);
			mydbconnection=NULL;
		}
	}

	if(!mydbconnection && is_mysql_userdb()) {
		char *errmsg=NULL;
		Myconninfo *co=MyconninfoParse(userdb, &errmsg);
		if(!co) {
			if(errmsg) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot open DB connection <%s>, connection string format error: %s\n",userdb,errmsg);
				free(errmsg);
			} else {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot open DB connection <%s>, connection string format error\n",userdb);
			}
		} else if(errmsg) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot open DB connection <%s>, connection string format error: %s\n",userdb,errmsg);
			free(errmsg);
			MyconninfoFree(co);
		} else if(!(co->dbname)) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Database name is not provided: <%s>\n",userdb);
			MyconninfoFree(co);
		} else {
			mydbconnection = mysql_init(NULL);
			if(!mydbconnection) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot initialize DB connection\n");
			} else {
				MYSQL *conn = mysql_real_connect(mydbconnection, co->host, co->user, co->password, co->dbname, co->port, NULL, CLIENT_IGNORE_SIGPIPE);
				if(!conn) {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot open DB connection: <%s>, runtime error\n",userdb);
					mysql_close(mydbconnection);
					mydbconnection=NULL;
				} else if(mysql_select_db(mydbconnection, co->dbname)) {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot connect to DB: %s\n",co->dbname);
					mysql_close(mydbconnection);
					mydbconnection=NULL;
				} else {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "DB connection success: %s\n",userdb);
				}
			}
			MyconninfoFree(co);
		}
	}
	return mydbconnection;
}
#endif

static int get_auth_secret(char *auth_secret)
{
	int ret = -1;

	if(static_auth_secret[0]) {
		STRCPY(auth_secret,static_auth_secret);
		ret=0;
		return ret;
	}

#if !defined(TURN_NO_PQ)
	PGconn * pqc = get_pqdb_connection();
	if(pqc) {
		char statement[1025];
		STRCPY(statement,"select value from turn_secret");
		PGresult *res = PQexec(pqc, statement);

		if(!res || (PQresultStatus(res) != PGRES_TUPLES_OK) || (PQntuples(res)!=1)) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error retrieving DB information: %s\n",PQerrorMessage(pqc));
		} else {
			char *kval = PQgetvalue(res,0,0);
			if(kval) {
				STRCPY(auth_secret,kval);
				ret = 0;
			}
		}

		if(res) {
			PQclear(res);
		}
	}
#endif

#if !defined(TURN_NO_MYSQL)
	MYSQL * myc = get_mydb_connection();
	if(myc) {
		char statement[1025];
		STRCPY(statement,"select value from turn_secret");
		int res = mysql_query(myc, statement);
		if(res) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error retrieving DB information: %s\n",mysql_error(myc));
		} else {
			MYSQL_RES *mres = mysql_store_result(myc);
			if(!mres) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error retrieving DB information: %s\n",mysql_error(myc));
			} else if(mysql_field_count(myc)==1) {
				MYSQL_ROW row = mysql_fetch_row(mres);
				if(row && row[0]) {
					unsigned long *lengths = mysql_fetch_lengths(mres);
					if(lengths) {
						size_t sz = lengths[0];
						ns_bcopy(row[0],auth_secret,sz);
						auth_secret[sz]=0;
						ret = 0;
					}
				}
				mysql_free_result(mres);
			}
		}
	}
#endif

	return ret;
}

#if !defined(SHA_DIGEST_LENGTH)
#define SHA_DIGEST_LENGTH (20)
#endif

int get_user_key(u08bits *uname, hmackey_t key)
{
	int ret = -1;

	if(use_auth_secret_with_timestamp) {

		turn_time_t ctime = (turn_time_t) time(NULL);
		turn_time_t ts = 0;
		char auth_secret[1025]="\0";

		if(get_auth_secret(auth_secret)<0 || !(auth_secret[0]))
			return ret;

		char *col = strstr((char*)uname,":");

		if(col) {
			ts = (turn_time_t)atol(col+1);
		} else {
			ts = (turn_time_t)atol((char*)uname);
		}

		if(!turn_time_before((ctime + auth_secret_timestamp_expiration_time), ts)) {

			u08bits hmac[1025]="\0";
			unsigned int hmac_len = SHA_DIGEST_LENGTH;

			if(calculate_hmac(uname, strlen((char*)uname), auth_secret, strlen(auth_secret), hmac, &hmac_len)>=0) {
				size_t pwd_length = 0;
				char *pwd = base64_encode(hmac,hmac_len,&pwd_length);

				if(pwd && pwd_length>0) {
					if(stun_produce_integrity_key_str((u08bits*)uname, (u08bits*)global_realm, (u08bits*)pwd, key)>=0) {
						ret = 0;
					}
					free(pwd);
				}
			}
		}

		return ret;
	}

	ur_string_map_value_type ukey = NULL;
	ur_string_map_lock(users->static_accounts);
	if(ur_string_map_get(users->static_accounts, (ur_string_map_key_type)uname, &ukey)) {
		ret = 0;
	} else {
		ur_string_map_lock(users->dynamic_accounts);
		if(ur_string_map_get(users->dynamic_accounts, (ur_string_map_key_type)uname, &ukey)) {
			ret = 0;
		}
		ur_string_map_unlock(users->dynamic_accounts);
	}
	ur_string_map_unlock(users->static_accounts);

	if(ret==0) {
		ns_bcopy(ukey,key,sizeof(hmackey_t));
		return 0;
	} else {
#if !defined(TURN_NO_PQ)
	PGconn * pqc = get_pqdb_connection();
	if(pqc) {
		char statement[1025];
		sprintf(statement,"select hmackey from turnusers_lt where name='%s'",uname);
		PGresult *res = PQexec(pqc, statement);

		if(!res || (PQresultStatus(res) != PGRES_TUPLES_OK) || (PQntuples(res)!=1)) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error retrieving DB information: %s\n",PQerrorMessage(pqc));
		} else {
			char *kval = PQgetvalue(res,0,0);
			if(kval) {
				if(convert_string_key_to_binary(kval, key)<0) {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong key: %s, user %s\n",kval,uname);
				} else {
					ret = 0;
				}
			} else {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong hmackey data for user %s: %s\n",uname,kval);
			}
		}

		if(res) {
			PQclear(res);
		}
	}
#endif
#if !defined(TURN_NO_MYSQL)
	MYSQL * myc = get_mydb_connection();
	if(myc) {
		char statement[1025];
		sprintf(statement,"select hmackey from turnusers_lt where name='%s'",uname);
		int res = mysql_query(myc, statement);
		if(res) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error retrieving DB information: %s\n",mysql_error(myc));
		} else {
			MYSQL_RES *mres = mysql_store_result(myc);
			if(!mres) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error retrieving DB information: %s\n",mysql_error(myc));
			} else if(mysql_field_count(myc)!=1) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Unknown error retrieving DB information: %s\n",statement);
			} else {
				MYSQL_ROW row = mysql_fetch_row(mres);
				if(row && row[0]) {
					unsigned long *lengths = mysql_fetch_lengths(mres);
					if(lengths) {
						size_t sz = sizeof(hmackey_t)*2;
						if(lengths[0]!=sz) {
							TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong hmackey data for user %s, size in DB is %lu\n",uname,lengths[0]);
						} else {
							char kval[sizeof(hmackey_t)+sizeof(hmackey_t)+1];
							ns_bcopy(row[0],kval,sz);
							kval[sz]=0;
							if(convert_string_key_to_binary(kval, key)<0) {
								TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong key: %s, user %s\n",kval,uname);
							} else {
								ret = 0;
							}
						}
					}
				}
				mysql_free_result(mres);
			}
		}
	}
#endif
	}

	return ret;
}

u08bits *start_user_check(turnserver_id id, u08bits *uname, get_username_resume_cb resume, ioa_net_data *in_buffer, void *ctx, int *postpone_reply)
{
	UNUSED_ARG(uname);
	UNUSED_ARG(resume);
	UNUSED_ARG(in_buffer);
	UNUSED_ARG(ctx);

	*postpone_reply = 1;

	struct auth_message am;
	ns_bzero(&am,sizeof(am));
	am.id = id;
	STRCPY(am.username,uname);
	am.resume_func = resume;
	memcpy(&(am.in_buffer),in_buffer,sizeof(am.in_buffer));
	in_buffer->nbh = NULL;
	am.ctx = ctx;

	send_auth_message_to_auth_server(&am);

	return NULL;
}

static u08bits *get_real_username(u08bits *user) {
	u08bits *ret = (u08bits*)strdup((char*)user);
	if(use_auth_secret_with_timestamp) {
		char *col=strstr((char*)ret,":");
		if(col) {
			*col=0;
		}
	}
	return ret;
}

int check_new_allocation_quota(u08bits *user)
{
	int ret = 0;
	if (user) {
		u08bits *username = get_real_username(user);
		ur_string_map_lock(users->alloc_counters);
		if (users->total_quota && (users->total_current_allocs >= users->total_quota)) {
			ret = -1;
		} else {
			ur_string_map_value_type value = 0;
			if (!ur_string_map_get(users->alloc_counters, (ur_string_map_key_type) username, &value)) {
				value = (ur_string_map_value_type) 1;
				ur_string_map_put(users->alloc_counters, (ur_string_map_key_type) username, value);
				++(users->total_current_allocs);
			} else {
				if ((users->user_quota) && ((size_t) value >= users->user_quota)) {
					ret = -1;
				} else {
					value = (ur_string_map_value_type)(((size_t)value) + 1);
					ur_string_map_put(users->alloc_counters, (ur_string_map_key_type) username, value);
					++(users->total_current_allocs);
				}
			}
		}
		free(username);
		ur_string_map_unlock(users->alloc_counters);
	}
	return ret;
}

void release_allocation_quota(u08bits *user)
{
	if (user) {
		u08bits *username = get_real_username(user);
		ur_string_map_lock(users->alloc_counters);
		ur_string_map_value_type value = 0;
		ur_string_map_get(users->alloc_counters, (ur_string_map_key_type) username, &value);
		if (value) {
			value = (ur_string_map_value_type)(((size_t)value) - 1);
			ur_string_map_put(users->alloc_counters, (ur_string_map_key_type) username, value);
		}
		if (users->total_current_allocs)
			--(users->total_current_allocs);
		ur_string_map_unlock(users->alloc_counters);
		free(username);
	}
}

//////////////////////////////////

void read_userdb_file(void)
{
	static char *full_path_to_userdb_file = NULL;
	static int first_read = 1;
	static turn_time_t mtime = 0;

	if(userdb_type != TURN_USERDB_TYPE_FILE)
		return;
	if(use_auth_secret_with_timestamp)
		return;

	FILE *f = NULL;

	if(full_path_to_userdb_file) {
		struct stat sb;
		if(stat(full_path_to_userdb_file,&sb)<0) {
			perror("File statistics");
		} else {
			turn_time_t newmtime = (turn_time_t)(sb.st_mtime);
			if(mtime == newmtime)
				return;
			mtime = newmtime;

		}
	}

	if (!full_path_to_userdb_file)
		full_path_to_userdb_file = find_config_file(userdb, first_read);

	if (full_path_to_userdb_file)
		f = fopen(full_path_to_userdb_file, "r");

	if (f) {

		char sbuf[1025];

		ur_string_map_lock(users->dynamic_accounts);
		ur_string_map_clean(users->dynamic_accounts);

		for (;;) {
			char *s = fgets(sbuf, sizeof(sbuf) - 1, f);
			if (!s)
				break;
			s = skip_blanks(s);
			if (s[0] == '#')
				continue;
			if (!s[0])
				continue;
			size_t slen = strlen(s);
			while (slen && (s[slen - 1] == 10 || s[slen - 1] == 13))
				s[--slen] = 0;
			if (slen)
				add_user_account(s,1);
		}

		ur_string_map_unlock(users->dynamic_accounts);

		fclose(f);

	} else if (first_read)
		TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "WARNING: Cannot find userdb file: %s: going without dynamic user database.\n", userdb);

	first_read = 0;
}

int add_user_account(char *user, int dynamic)
{
	if(user && !use_auth_secret_with_timestamp) {
		char *s = strstr(user, ":");
		if(!s || (s==user) || (strlen(s)<2)) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong user account: %s\n",user);
		} else {
			size_t ulen = s-user;
			char *uname = (char*)malloc(sizeof(char)*(ulen+1));
			strncpy(uname,user,ulen);
			uname[ulen]=0;
			if(SASLprep((u08bits*)uname)<0) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong user name: %s\n",user);
				free(uname);
				return -1;
			}
			s = skip_blanks(s+1);
			hmackey_t *key = (hmackey_t*)malloc(sizeof(hmackey_t));
			if(strstr(s,"0x")==s) {
				char *keysource = s + 2;
				if(convert_string_key_to_binary(keysource, *key)<0) {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong key: %s\n",s);
					free(uname);
					free(key);
					return -1;
				}
			} else {
				stun_produce_integrity_key_str((u08bits*)uname, (u08bits*)global_realm, (u08bits*)s, *key);
			}
			if(dynamic) {
				ur_string_map_lock(users->dynamic_accounts);
				ur_string_map_put(users->dynamic_accounts, (ur_string_map_key_type)uname, (ur_string_map_value_type)*key);
				ur_string_map_unlock(users->dynamic_accounts);
			} else {
				ur_string_map_lock(users->static_accounts);
				ur_string_map_put(users->static_accounts, (ur_string_map_key_type)uname, (ur_string_map_value_type)*key);
				ur_string_map_unlock(users->static_accounts);
			}
			users_number++;
			free(uname);
			return 0;
		}
	}

	return -1;
}

////////////////// Admin /////////////////////////

int adminuser(u08bits *user, u08bits *realm, u08bits *pwd, int kcommand, int acommand , int dcommand)
{
	hmackey_t key;
	char skey[sizeof(hmackey_t)*2+1];

	if(!dcommand) {
		stun_produce_integrity_key_str(user, realm, pwd, key);
		size_t i = 0;
		char *s=skey;
		for(i=0;i<sizeof(hmackey_t);i++) {
			sprintf(s,"%02x",(unsigned int)key[i]);
			s+=2;
		}
	}

	if(kcommand) {

		printf("0x%s\n",skey);

	} else if(is_pqsql_userdb()){
#if !defined(TURN_NO_PQ)
		char statement[1025];
		PGconn *pqc = get_pqdb_connection();
		if(pqc) {
			if(dcommand) {
				sprintf(statement,"delete from turnusers_lt where name='%s'",user);
				PGresult *res = PQexec(pqc, statement);
				if(res) {
					PQclear(res);
				}
			}

			if(acommand) {
				sprintf(statement,"insert into turnusers_lt values('%s','%s')",user,skey);
				PGresult *res = PQexec(pqc, statement);
				if(!res || (PQresultStatus(res) != PGRES_COMMAND_OK)) {
					if(res) {
						PQclear(res);
					}
					sprintf(statement,"update turnusers_lt set hmackey='%s' where name='%s'",skey,user);
					res = PQexec(pqc, statement);
					if(!res || (PQresultStatus(res) != PGRES_COMMAND_OK)) {
						TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error inserting/updating user key information: %s\n",PQerrorMessage(pqc));
					}
				}
				if(res) {
					PQclear(res);
				}
			}
		}
#endif
	} else if(is_mysql_userdb()){
#if !defined(TURN_NO_MYSQL)
		char statement[1025];
		MYSQL * myc = get_mydb_connection();
		if(myc) {
			if(dcommand) {
				sprintf(statement,"delete from turnusers_lt where name='%s'",user);
				int res = mysql_query(myc, statement);
				if(res) {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error deleting user key information: %s\n",mysql_error(myc));
				}
			}

			if(acommand) {
				sprintf(statement,"insert into turnusers_lt values('%s','%s')",user,skey);
				int res = mysql_query(myc, statement);
				if(res) {
					sprintf(statement,"update turnusers_lt set hmackey='%s' where name='%s'",skey,user);
					res = mysql_query(myc, statement);
					if(res) {
						TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error inserting/updating user key information: %s\n",mysql_error(myc));
					}
				}
			}
		}
#endif
	} else {

		char *full_path_to_userdb_file = find_config_file(userdb, 1);
		FILE *f = full_path_to_userdb_file ? fopen(full_path_to_userdb_file,"r") : NULL;
		int found = 0;
		char us[1025];
		size_t i = 0;
		char **content = NULL;
		size_t csz = 0;

		strcpy(us, (char*) user);
		strcpy(us + strlen(us), ":");

		if (!f) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "File %s not found, will be created.\n",userdb);
		} else {

			char sarg[1025];
			char sbuf[1025];

			for (;;) {
				char *s0 = fgets(sbuf, sizeof(sbuf) - 1, f);
				if (!s0)
					break;

				size_t slen = strlen(s0);
				while (slen && (s0[slen - 1] == 10 || s0[slen - 1] == 13))
					s0[--slen] = 0;

				char *s = skip_blanks(s0);

				if (s[0] == '#')
					goto add_and_cont;
				if (!s[0])
					goto add_and_cont;

				strcpy(sarg, s);
				if (strstr(sarg, us) == sarg) {
					if (dcommand)
						continue;

					if (found)
						continue;
					found = 1;
					strcpy(us, (char*) user);
					strcpy(us + strlen(us), ":0x");
					for (i = 0; i < sizeof(key); i++) {
						sprintf(
										us + strlen(us),
										"%02x",
										(unsigned int) key[i]);
					}

					s0 = us;
				}

				add_and_cont:
				content = (char**)realloc(content, sizeof(char*) * (++csz));
				content[csz - 1] = strdup(s0);
			}

			fclose(f);
		}

		if(!found && acommand) {
			strcpy(us,(char*)user);
			strcpy(us+strlen(us),":0x");
			for(i=0;i<sizeof(key);i++) {
				sprintf(us+strlen(us),"%02x",(unsigned int)key[i]);
			}
			content = (char**)realloc(content,sizeof(char*)*(++csz));
			content[csz-1]=strdup(us);
		}

		if(!full_path_to_userdb_file)
			full_path_to_userdb_file=strdup(userdb);

		char *dir = (char*)malloc(strlen(full_path_to_userdb_file)+21);
		strcpy(dir,full_path_to_userdb_file);
		size_t dlen = strlen(dir);
		while(dlen) {
			if(dir[dlen-1]=='/')
				break;
			dir[--dlen]=0;
		}
		strcpy(dir+strlen(dir),".tmp_userdb");

		f = fopen(dir,"w");
		if(!f) {
			perror("file open");
			exit(-1);
		}

		for(i=0;i<csz;i++)
			fprintf(f,"%s\n",content[i]);

		fclose(f);

		rename(dir,full_path_to_userdb_file);
	}

	return 0;
}


///////////////////////////////