/* add.c - ldap bdb2 back-end add routine */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>
#include <ac/string.h>

#include "slap.h"
#include "back-bdb2.h"
#include "proto-back-bdb2.h"

static int
bdb2i_back_add_internal(
    Backend	*be,
    Connection	*conn,
    Operation	*op,
    Entry	*e
)
{
	struct ldbminfo	*li = (struct ldbminfo *) be->be_private;
	char		*pdn;
	Entry		*p = NULL;
	int			rootlock = 0;
	int			rc = -1; 

	Debug(LDAP_DEBUG_ARGS, "==> bdb2i_back_add: %s\n", e->e_dn, 0, 0);

	/* nobody else can add until we lock our parent */
	ldap_pvt_thread_mutex_lock(&li->li_add_mutex);

	if ( ( bdb2i_dn2id( be, e->e_ndn ) ) != NOID ) {
		ldap_pvt_thread_mutex_unlock(&li->li_add_mutex);
		entry_free( e );
		send_ldap_result( conn, op, LDAP_ALREADY_EXISTS, "", "" );
		return( -1 );
	}

	if ( global_schemacheck && oc_schema_check( e ) != 0 ) {
		ldap_pvt_thread_mutex_unlock(&li->li_add_mutex);

		Debug( LDAP_DEBUG_TRACE, "entry failed schema check\n",
			0, 0, 0 );

		entry_free( e );
		send_ldap_result( conn, op, LDAP_OBJECT_CLASS_VIOLATION, "",
		    "" );
		return( -1 );
	}

	/*
	 * Get the parent dn and see if the corresponding entry exists.
	 * If the parent does not exist, only allow the "root" user to
	 * add the entry.
	 */

	if ( (pdn = dn_parent( be, e->e_ndn )) != NULL ) {
		char *matched = NULL;

		/* get parent with writer lock */
		if ( (p = bdb2i_dn2entry_w( be, pdn, &matched )) == NULL ) {
			ldap_pvt_thread_mutex_unlock(&li->li_add_mutex);
			Debug( LDAP_DEBUG_TRACE, "parent does not exist\n", 0,
			    0, 0 );
			send_ldap_result( conn, op, LDAP_NO_SUCH_OBJECT,
			    matched, "" );

			if ( matched != NULL ) {
				free( matched );
			}

			entry_free( e );
			free( pdn );
			return -1;
		}

		/* don't need the add lock anymore */
		ldap_pvt_thread_mutex_unlock(&li->li_add_mutex);

		free(pdn);

		if ( matched != NULL ) {
			free( matched );
		}

		if ( ! access_allowed( be, conn, op, p,
			"children", NULL, ACL_WRITE ) )
		{
			Debug( LDAP_DEBUG_TRACE, "no access to parent\n", 0,
			    0, 0 );
			send_ldap_result( conn, op, LDAP_INSUFFICIENT_ACCESS,
			    "", "" );

			/* free parent and writer lock */
			bdb2i_cache_return_entry_w( &li->li_cache, p ); 

			entry_free( e );
			return -1;
		}

	} else {
		/* no parent, must be adding entry to root */
		if ( ! be_isroot( be, op->o_ndn ) ) {
			ldap_pvt_thread_mutex_unlock(&li->li_add_mutex);
			Debug( LDAP_DEBUG_TRACE, "no parent & not root\n", 0,
			    0, 0 );
			send_ldap_result( conn, op, LDAP_INSUFFICIENT_ACCESS,
			    "", "" );

			entry_free( e );
			return -1;
		}

		/*
		 * no parent, acquire the root write lock
		 * and release the add lock.
		 */
		ldap_pvt_thread_mutex_lock(&li->li_root_mutex);
		rootlock = 1;
		ldap_pvt_thread_mutex_unlock(&li->li_add_mutex);
	}

	/* acquire required reader/writer lock */
	if (entry_rdwr_lock(e, 1)) {
		if( p != NULL) {
			/* free parent and writer lock */
			bdb2i_cache_return_entry_w( &li->li_cache, p ); 
		}

		if ( rootlock ) {
			/* release root lock */
			ldap_pvt_thread_mutex_unlock(&li->li_root_mutex);
		}

		Debug( LDAP_DEBUG_ANY, "add: could not lock entry\n",
			0, 0, 0 );

		entry_free(e);

		send_ldap_result( conn, op, LDAP_OPERATIONS_ERROR, "", "" );
		return( -1 );
	}

	e->e_id = bdb2i_next_id( be );

	/*
	 * Try to add the entry to the cache, assign it a new dnid
	 * This should only fail if the entry already exists.
	 */

	if ( bdb2i_cache_add_entry_lock( &li->li_cache, e, ENTRY_STATE_CREATING )
								!= 0 ) {
		if( p != NULL) {
			/* free parent and writer lock */
			bdb2i_cache_return_entry_w( &li->li_cache, p ); 
		}
		if ( rootlock ) {
			/* release root lock */
			ldap_pvt_thread_mutex_unlock(&li->li_root_mutex);
		}

		Debug( LDAP_DEBUG_ANY, "cache_add_entry_lock failed\n", 0, 0,
		    0 );
		bdb2i_next_id_return( be, e->e_id );
                
		entry_rdwr_unlock(e, 1);
		entry_free( e );

		send_ldap_result( conn, op, LDAP_ALREADY_EXISTS, "", "" );
		return( -1 );
	}

	/*
	 * add it to the id2children index for the parent
	 */

	if ( bdb2i_id2children_add( be, p, e ) != 0 ) {
		Debug( LDAP_DEBUG_TRACE, "bdb2i_id2children_add failed\n", 0,
		    0, 0 );
		send_ldap_result( conn, op, LDAP_OPERATIONS_ERROR, "", "" );

		goto return_results;
	}

	/*
	 * Add the entry to the attribute indexes, then add it to
	 * the id2children index, dn2id index, and the id2entry index.
	 */

	/* attribute indexes */
	if ( bdb2i_index_add_entry( be, e ) != 0 ) {
		Debug( LDAP_DEBUG_TRACE, "bdb2i_index_add_entry failed\n", 0,
		    0, 0 );
		send_ldap_result( conn, op, LDAP_OPERATIONS_ERROR, "", "" );

		goto return_results;
	}

	/* dn2id index */
	if ( bdb2i_dn2id_add( be, e->e_ndn, e->e_id ) != 0 ) {
		Debug( LDAP_DEBUG_TRACE, "bdb2i_dn2id_add failed\n", 0,
		    0, 0 );
		send_ldap_result( conn, op, LDAP_OPERATIONS_ERROR, "", "" );

		goto return_results;
	}

	/* id2entry index */
	if ( bdb2i_id2entry_add( be, e ) != 0 ) {
		Debug( LDAP_DEBUG_TRACE, "bdb2i_id2entry_add failed\n", 0,
		    0, 0 );
		(void) bdb2i_dn2id_delete( be, e->e_ndn );
		send_ldap_result( conn, op, LDAP_OPERATIONS_ERROR, "", "" );

		goto return_results;
	}

	send_ldap_result( conn, op, LDAP_SUCCESS, "", "" );
	rc = 0;

return_results:;
	if (p != NULL) {
		/* free parent and writer lock */
		bdb2i_cache_return_entry_w( &li->li_cache, p ); 

	}

	if ( rootlock ) {
		/* release root lock */
		ldap_pvt_thread_mutex_unlock(&li->li_root_mutex);
	}

	bdb2i_cache_set_state( &li->li_cache, e, 0 );

	/* free entry and writer lock */
	bdb2i_cache_return_entry_w( &li->li_cache, e ); 

	return( rc );
}


int
bdb2_back_add(
    Backend	*be,
    Connection	*conn,
    Operation	*op,
    Entry	*e
)
{
	DB_LOCK  lock;
	struct ldbminfo	*li = (struct ldbminfo *) be->be_private;

	struct   timeval  time1, time2;
	char     *elapsed_time;
	int      ret;

	gettimeofday( &time1, NULL );

	if ( bdb2i_enter_backend_w( &li->li_db_env, &lock ) != 0 ) {

		send_ldap_result( conn, op, LDAP_OPERATIONS_ERROR, "", "" );
		return( -1 );

	}

	/*  check, if a new default attribute index will be created,
		in which case we have to open the index file BEFORE TP  */
	if ( bdb2i_with_dbenv )
		bdb2i_check_default_attr_index_add( li, e );

	ret = bdb2i_back_add_internal( be, conn, op, e );

	(void) bdb2i_leave_backend( &li->li_db_env, lock );

	if ( bdb2i_do_timing ) {

		gettimeofday( &time2, NULL);
		elapsed_time = bdb2i_elapsed( time1, time2 );
		Debug( LDAP_DEBUG_ANY, "conn=%d op=%d ADD elapsed=%s\n",
				conn->c_connid, op->o_opid, elapsed_time );
		free( elapsed_time );

	}

	return( ret );
}


